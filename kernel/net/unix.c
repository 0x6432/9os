/*
 * AF_UNIX sockets: SOCK_STREAM, SOCK_SEQPACKET and SOCK_DGRAM; filesystem and abstract
 * names; socketpair; SCM_RIGHTS file descriptor passing; SO_PEERCRED. This is the IPC that
 * Wayland, D-Bus and X11 are built on.
 *
 * Data is queued as chunks (one per send for datagram types, up to 64 KiB for streams).
 * Passed files ride on the chunk that starts the message; a stream recv never merges data
 * across a chunk carrying files, so they arrive with the first byte sent alongside them.
 * Blocking uses the global poll_wq like pipes/poll (see the locking notes below).
 */
#include <kernel/vfs.h>
#include <kernel/cred.h>
#include <kernel/syscall.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/mm.h>
#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/cpu.h>

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define MSG_PEEK 0x2
#define MSG_CTRUNC 0x8
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define MAX_PASS_FDS 253
#define SOCK_BUF (256 * 1024)
#define CHUNK_MAX (64 * 1024)

enum { SS_UNCONN, SS_LISTEN, SS_CONNECTED };

struct sockaddr_un_k { uint16_t family; char path[108]; };
struct ucred_k { int32_t pid; uint32_t uid, gid; };

struct chunk {
    struct list_node node;
    size_t len, off;
    int nfds;
    struct file **fds;
    struct sockaddr_un_k from; int fromlen;
    struct ucred_k cred;
    uint8_t data[];
};

struct usock {
    int type, state;
    struct usock *peer;            /* connected peer (stream/seqpacket), cleared when it closes */
    bool peer_gone, shut_rd, shut_wr, peer_shut_wr, nonblock_dummy;
    struct list_node rxq; size_t rxbytes;
    struct list_node backlog; int nbacklog, maxbacklog;   /* listener: queued server sockets */
    struct list_node bl_node;
    struct list_node all_node;     /* bound sockets */
    bool bound; struct sockaddr_un_k name; int namelen;
    struct inode *bind_inode;      /* filesystem name */
    struct sockaddr_un_k dst; int dstlen;  /* dgram default destination */
    struct ucred_k cred, peercred;
    bool passcred;
    struct file *file;             /* owning file (nullptr while unaccepted in a backlog) */
};

/*
 * Locking: every usock field, every receive queue, the backlogs and bound_socks are guarded by
 * unix_lock, an IRQ-off lock spun with spin_lock_ipi() (user copies, i.e. the mm lock, happen
 * under it). The data path (send/recv family, read/write) runs without the BKL and holds a file
 * reference on its own socket; a peer is only dereferenced under unix_lock and re-resolved
 * after every sleep, since it can be destroyed as soon as the lock is dropped. Setup syscalls
 * still run under the BKL and take unix_lock around the state they share with the data path.
 * Never under unix_lock: the BKL, VFS lookups, vfs_close()/iput() (chunk_free), fd_alloc().
 * Sleeps sample poll_seq under the lock and use poll_wait_seq() after dropping it; every state
 * change is followed by poll_notify(), so wakeups cannot be lost.
 * Lock order: BKL -> unix_lock -> mm lock -> pt/buddy/slab -> sched_lock.
 */
static const struct lock_class unix_class = { "unix_lock", LR_UNIX, false };
static spinlock_t unix_lock = SPINLOCK_INIT_CLASS(&unix_class);
static uint64_t ulock(void) { uint64_t f = arch_irq_save(); spin_lock_ipi(&unix_lock); return f; }
static void uunlock(uint64_t f) { spin_unlock(&unix_lock); arch_irq_restore(f); }

static struct list_node bound_socks = LIST_INIT(bound_socks);
static const struct file_ops unix_fops;

static struct usock *usock_new(int type) {
    struct usock *s = kzalloc(sizeof *s);
    if (!s) return nullptr;
    s->type = type;
    list_init(&s->rxq); list_init(&s->backlog);
    const struct cred *c = current_cred();      /* SO_PEERCRED/SCM_CREDENTIALS: effective ids */
    s->cred = (struct ucred_k){ curproc ? curproc->pid : 0, c->euid, c->egid };
    return s;
}

static void chunk_free(struct chunk *c) {
    for (int i = 0; i < c->nfds; i++) if (c->fds[i]) vfs_close(c->fds[i]);
    kfree(c->fds);
    kfree(c);
}
static void free_chunks(struct list_node *dead) {      /* outside unix_lock */
    list_for_each_safe(it, tmp, dead) { list_del(it); chunk_free(list_entry(it, struct chunk, node)); }
}

static void disconnect(struct usock *s) {               /* unix_lock held */
    if (s->peer) { s->peer->peer = nullptr; s->peer->peer_gone = true; s->peer = nullptr; }
}

/* unlink s (and its never-accepted connections) under the lock, free everything after it */
static void usock_destroy(struct usock *s) {
    struct list_node dead = LIST_INIT(dead), srvs = LIST_INIT(srvs);
    uint64_t f = ulock();
    disconnect(s);
    list_for_each_safe(it, tmp, &s->rxq) { list_del(it); list_add_tail(&dead, it); }
    list_for_each_safe(it, tmp, &s->backlog) {
        struct usock *c = list_entry(it, struct usock, bl_node);
        list_del(it);
        disconnect(c);
        list_for_each_safe(jt, t2, &c->rxq) { list_del(jt); list_add_tail(&dead, jt); }
        list_add_tail(&srvs, &c->bl_node);
    }
    s->nbacklog = 0;
    if (s->bound) { list_del(&s->all_node); s->bound = false; }
    uunlock(f);
    free_chunks(&dead);
    list_for_each_safe(it, tmp, &srvs) { list_del(it); kfree(list_entry(it, struct usock, bl_node)); }
    if (s->bind_inode) {
        bool took = !bkl_held();
        if (took) bkl_enter();
        iput(s->bind_inode);
        if (took) bkl_exit();
    }
    kfree(s);
    poll_notify();
}

static int sock_file(struct usock *s, int flags) {
    struct inode *i = inode_alloc(S_IFSOCK | 0777);
    if (!i) return -ENOMEM;
    i->fops = &unix_fops;
    struct file *f = file_open_inode(i, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
    iput(i);
    if (!f) return -ENOMEM;
    f->priv = s; s->file = f;
    int fd = fd_alloc(f, 0, flags & SOCK_CLOEXEC);
    if (fd < 0) { f->priv = nullptr; s->file = nullptr; vfs_close(f); }
    return fd;
}

/* BKL syscalls: borrowed file */
static struct usock *sock_of(int fd, int *err) {
    struct file *f = fd_get(fd);
    if (!f) { *err = -EBADF; return nullptr; }
    if (f->fops != &unix_fops) { *err = -ENOTSOCK; return nullptr; }
    return f->priv;
}
#define GET_SOCK(s, fd) int __e; struct usock *s = sock_of(fd, &__e); if (!s) return __e
static bool nonblock(int fd, int flags) { struct file *f = fd_get(fd); return (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK)); }

/* lock-free syscalls: referenced file, dropped with vfs_close() */
static struct file *sock_file_ref(int fd, int *err) {
    struct file *f = fd_get_ref(fd);
    if (!f) { *err = -EBADF; return nullptr; }
    if (f->fops != &unix_fops || !f->priv) { vfs_close(f); *err = -ENOTSOCK; return nullptr; }
    return f;
}
static bool file_nonblock(struct file *f, int flags) { return (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK)); }

/* sleep until poll_notify() runs after 'seq' (sampled under unix_lock) */
static int usleep_seq(uint64_t seq) { return poll_wait_seq(seq, UINT64_MAX); }

/* ------------------------------------------------------------- names */
static int addr_in(const void *uaddr, int len, struct sockaddr_un_k *a, int *alen) {
    if (len < 2 || len > (int)sizeof *a) return -EINVAL;
    memset(a, 0, sizeof *a);
    if (copy_from_user(a, uaddr, len)) return -EFAULT;
    if (a->family != AF_UNIX) return -EAFNOSUPPORT;
    *alen = len;
    if (len > 2 && a->path[0]) {                          /* path name: normalise length */
        size_t l = strnlen(a->path, sizeof a->path - 1);
        *alen = 2 + l + 1;
    }
    return 0;
}

static bool is_abstract(const struct sockaddr_un_k *a, int len) { return len > 2 && a->path[0] == 0; }

/* Name resolution is split: the filesystem part (BKL) yields an inode reference, the match
 * against bound_socks happens under unix_lock. */
static int name_inode(const struct sockaddr_un_k *a, int len, struct inode **ino) {
    *ino = nullptr;
    if (is_abstract(a, len)) return 0;
    bool took = !bkl_held();
    if (took) bkl_enter();
    int r = vfs_lookup(a->path, true, ino);
    if (!r && !S_ISSOCK((*ino)->mode)) { iput(*ino); *ino = nullptr; r = -ECONNREFUSED; }
    if (!r && (r = inode_permission(*ino, MAY_WRITE)) == -EROFS) r = 0;   /* connecting = writing */
    if (r && *ino) { iput(*ino); *ino = nullptr; }
    if (took) bkl_exit();
    return r;
}
static void name_inode_put(struct inode *ino) {
    if (!ino) return;
    bool took = !bkl_held();
    if (took) bkl_enter();
    iput(ino);
    if (took) bkl_exit();
}
static struct usock *match_locked(const struct sockaddr_un_k *a, int len, struct inode *ino) {
    list_for_each(it, &bound_socks) {
        struct usock *s = list_entry(it, struct usock, all_node);
        if (ino ? s->bind_inode == ino
                : (!s->bind_inode && s->namelen == len && !memcmp(s->name.path, a->path, len - 2))) return s;
    }
    return nullptr;
}

static int put_name(void *uaddr, int *ulen, const struct sockaddr_un_k *a, int alen) {
    if (!uaddr || !ulen) return 0;
    int len;
    if (copy_from_user(&len, ulen, sizeof len)) return -EFAULT;
    if (len < 0) return -EINVAL;
    struct sockaddr_un_k tmp = { .family = AF_UNIX };
    int n = 2;
    if (a && alen > 2) { tmp = *a; n = alen; }
    if (copy_to_user(uaddr, &tmp, MIN(len, n))) return -EFAULT;
    return copy_to_user(ulen, &n, sizeof n) ? -EFAULT : 0;
}

/* ------------------------------------------------------------- syscalls (BKL) */
int64_t sys_socket(int domain, int type, int proto) {
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    int t = type & 0xf;
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET) return -EPROTONOSUPPORT;
    struct usock *s = usock_new(t);
    if (!s) return -ENOMEM;
    int fd = sock_file(s, type);
    if (fd < 0) usock_destroy(s);
    return fd;
}

int64_t sys_socketpair(int domain, int type, int proto, int *usv) {
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    int t = type & 0xf;
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET) return -EPROTONOSUPPORT;
    struct usock *a = usock_new(t), *b = usock_new(t);
    if (!a || !b) { kfree(a); kfree(b); return -ENOMEM; }
    a->peer = b; b->peer = a;                     /* not reachable by anyone else yet */
    a->state = b->state = SS_CONNECTED;
    a->peercred = b->cred; b->peercred = a->cred;
    int sv[2];
    sv[0] = sock_file(a, type);
    if (sv[0] < 0) { usock_destroy(a); usock_destroy(b); return sv[0]; }
    sv[1] = sock_file(b, type);
    if (sv[1] < 0) { fd_close(sv[0]); usock_destroy(b); return sv[1]; }
    if (copy_to_user(usv, sv, sizeof sv)) { fd_close(sv[0]); fd_close(sv[1]); return -EFAULT; }
    return 0;
}

int64_t sys_bind(int fd, const void *uaddr, int len) {
    GET_SOCK(s, fd);
    if (s->bound) return -EINVAL;
    struct sockaddr_un_k a; int alen;
    int r = addr_in(uaddr, len, &a, &alen);
    if (r) return r;
    if (alen <= 2) return -EINVAL;            /* autobind not supported */
    struct inode *ino = nullptr;
    if (!is_abstract(&a, alen)) {
        r = vfs_mknod_at(nullptr, a.path, S_IFSOCK | (0777 & ~curproc->umask), 0);
        if (r == -EEXIST) return -EADDRINUSE;
        if (r) return r;
        r = vfs_lookup(a.path, false, &ino);
        if (r) return r;
    }
    uint64_t f = ulock();
    if (s->bound || (!ino && match_locked(&a, alen, nullptr))) {
        bool was = s->bound;
        uunlock(f);
        if (ino) iput(ino);
        return was ? -EINVAL : -EADDRINUSE;
    }
    s->bind_inode = ino;
    s->name = a; s->namelen = alen; s->bound = true;
    list_add_tail(&bound_socks, &s->all_node);
    uunlock(f);
    return 0;
}

int64_t sys_listen(int fd, int backlog) {
    GET_SOCK(s, fd);
    if (s->type == SOCK_DGRAM) return -EOPNOTSUPP;
    uint64_t f = ulock();
    int r = 0;
    if (!s->bound) r = -EINVAL;                   /* no autobind */
    else if (s->state == SS_CONNECTED) r = -EINVAL;
    else { s->state = SS_LISTEN; s->maxbacklog = backlog < 1 ? 1 : MIN(backlog, 128); }
    uunlock(f);
    if (!r) poll_notify();                        /* blocked connectors re-check */
    return r;
}

int64_t sys_connect(int fd, const void *uaddr, int len) {
    GET_SOCK(s, fd);
    struct sockaddr_un_k a; int alen;
    int r = addr_in(uaddr, len, &a, &alen);
    if (r) return r;
    struct inode *ino;
    if ((r = name_inode(&a, alen, &ino))) return r;
    if (s->type == SOCK_DGRAM) {
        uint64_t f = ulock();
        if (match_locked(&a, alen, ino)) { s->dst = a; s->dstlen = alen; s->state = SS_CONNECTED; }
        else r = -ECONNREFUSED;
        uunlock(f);
        name_inode_put(ino);
        return r;
    }
    bool nb = nonblock(fd, 0);
    struct usock *srv = usock_new(s->type);
    if (!srv) { name_inode_put(ino); return -ENOMEM; }
    for (;;) {
        uint64_t f = ulock();
        struct usock *l = match_locked(&a, alen, ino);
        if (s->state == SS_CONNECTED) r = -EISCONN;
        else if (s->state == SS_LISTEN) r = -EINVAL;
        else if (!l) r = -ECONNREFUSED;
        else if (l->type != s->type) r = -EPROTOTYPE;
        else if (l->state != SS_LISTEN) r = -ECONNREFUSED;
        else if (l->nbacklog < l->maxbacklog) {
            srv->cred = l->cred;
            srv->name = l->name; srv->namelen = l->namelen;
            srv->peer = s; s->peer = srv;
            srv->state = s->state = SS_CONNECTED;
            srv->peercred = s->cred; s->peercred = l->cred;
            list_add_tail(&l->backlog, &srv->bl_node);
            l->nbacklog++;
            s->dst = a; s->dstlen = alen;
            uunlock(f);
            name_inode_put(ino);
            poll_notify();
            return 0;
        } else if (nb) r = -EAGAIN;
        else {
            uint64_t seq = poll_seq_read();
            uunlock(f);
            if ((r = usleep_seq(seq))) break;
            continue;
        }
        uunlock(f);
        break;
    }
    kfree(srv);
    name_inode_put(ino);
    return r;
}

int64_t sys_accept4(int fd, void *uaddr, int *ulen, int flags) {
    GET_SOCK(l, fd);
    bool nb = nonblock(fd, 0);
    struct usock *srv;
    for (;;) {
        uint64_t f = ulock();
        if (l->state != SS_LISTEN) { uunlock(f); return -EINVAL; }
        if (!list_empty(&l->backlog)) {
            srv = list_entry(l->backlog.next, struct usock, bl_node);
            list_del(&srv->bl_node);
            l->nbacklog--;
            uunlock(f);
            break;
        }
        if (nb) { uunlock(f); return -EAGAIN; }
        uint64_t seq = poll_seq_read();
        uunlock(f);
        int r = usleep_seq(seq);
        if (r) return r;
    }
    poll_notify();
    int nfd = sock_file(srv, flags);
    if (nfd < 0) { usock_destroy(srv); return nfd; }
    if (uaddr && ulen) {
        struct sockaddr_un_k pn; int pl = 0;
        uint64_t f = ulock();
        struct usock *p = srv->peer;
        if (p && p->bound) { pn = p->name; pl = p->namelen; }
        uunlock(f);
        int r = put_name(uaddr, ulen, pl ? &pn : nullptr, pl);
        if (r) { fd_close(nfd); return r; }
    }
    return nfd;
}
int64_t sys_accept(int fd, void *uaddr, int *ulen) { return sys_accept4(fd, uaddr, ulen, 0); }

int64_t sys_getsockname(int fd, void *uaddr, int *ulen) {
    GET_SOCK(s, fd);
    uint64_t f = ulock();
    struct sockaddr_un_k n = s->name; int nl = s->namelen;
    uunlock(f);
    return put_name(uaddr, ulen, nl ? &n : nullptr, nl);
}
int64_t sys_getpeername(int fd, void *uaddr, int *ulen) {
    GET_SOCK(s, fd);
    struct sockaddr_un_k n; int nl = 0, r = 0;
    uint64_t f = ulock();
    if (s->type == SOCK_DGRAM) { if (!s->dstlen) r = -ENOTCONN; else { n = s->dst; nl = s->dstlen; } }
    else if (s->state != SS_CONNECTED) r = -ENOTCONN;
    else if (s->dstlen) { n = s->dst; nl = s->dstlen; }            /* client: the listener's name */
    else if (s->peer && s->peer->bound) { n = s->peer->name; nl = s->peer->namelen; }
    uunlock(f);
    if (r) return r;
    return put_name(uaddr, ulen, nl ? &n : nullptr, nl);
}

int64_t sys_shutdown(int fd, int how) {
    GET_SOCK(s, fd);
    if (how < 0 || how > 2) return -EINVAL;
    uint64_t f = ulock();
    if (how != 1) s->shut_rd = true;
    if (how != 0) { s->shut_wr = true; if (s->peer) s->peer->peer_shut_wr = true; }
    uunlock(f);
    poll_notify();
    return 0;
}

#define SO_DEBUG 1
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL 38
#define SO_DOMAIN 39

int64_t sys_setsockopt(int fd, int level, int name, const void *uval, int len) {
    GET_SOCK(s, fd);
    if (level == SOL_SOCKET && name == SO_PASSCRED && len >= 4) {
        int v; if (copy_from_user(&v, uval, 4)) return -EFAULT;
        uint64_t f = ulock();
        s->passcred = v;
        uunlock(f);
    }
    return 0;   /* buffer sizes, timeouts etc. are accepted and ignored */
}

int64_t sys_getsockopt(int fd, int level, int name, void *uval, int *ulen) {
    GET_SOCK(s, fd);
    int len;
    if (copy_from_user(&len, ulen, sizeof len)) return -EFAULT;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    uint64_t f = ulock();
    int state = s->state, passcred = s->passcred;
    struct ucred_k c = s->peercred;
    uunlock(f);
    int v;
    switch (name) {
    case SO_PEERCRED: {
        if (state != SS_CONNECTED && s->type != SOCK_DGRAM) return -ENOTCONN;
        int n = MIN(len, (int)sizeof c);
        if (copy_to_user(uval, &c, n) || copy_to_user(ulen, &n, sizeof n)) return -EFAULT;
        return 0;
    }
    case SO_TYPE: v = s->type; break;
    case SO_ERROR: v = 0; break;
    case SO_SNDBUF: case SO_RCVBUF: v = SOCK_BUF; break;
    case SO_PASSCRED: v = passcred; break;
    case SO_ACCEPTCONN: v = state == SS_LISTEN; break;
    case SO_PROTOCOL: v = 0; break;
    case SO_DOMAIN: v = AF_UNIX; break;
    default: return -ENOPROTOOPT;
    }
    int n = MIN(len, 4);
    if (copy_to_user(uval, &v, n) || copy_to_user(ulen, &n, sizeof n)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------- data path (no BKL) */
struct iovec_k { void *base; size_t len; };
struct msghdr_k { void *name; uint32_t namelen, _p0; struct iovec_k *iov; size_t iovlen; void *control; size_t controllen; int flags, _p1; };
struct cmsghdr_k { size_t len; int level, type; };
#define CMSG_ALIGN_K(n) (((n) + 7) & ~7ul)

/* unix_lock held; ino is the resolved filesystem name for path destinations */
static struct usock *dest_locked(struct usock *s, const struct sockaddr_un_k *a, int alen, struct inode *ino, int *err) {
    if (s->type == SOCK_DGRAM) {
        if (!a && !s->dstlen) { *err = -ENOTCONN; return nullptr; }
        struct usock *t = match_locked(a ? a : &s->dst, a ? alen : s->dstlen, ino);
        if (!t) *err = -ECONNREFUSED;
        return t;
    }
    if (s->state != SS_CONNECTED) { *err = -ENOTCONN; return nullptr; }
    if (!s->peer || s->shut_wr || s->peer->shut_rd) { *err = -EPIPE; return nullptr; }
    return s->peer;
}

static void raise_sigpipe(void) {
    bool took = !bkl_held();
    if (took) bkl_enter();
    signal_send(curproc, SIGPIPE);
    if (took) bkl_exit();
}

/* Chunks are allocated and filled from user memory before taking unix_lock; on success the
 * fds array (if any) is owned by the queue and *consumed is set. */
static int64_t do_send(struct file *file, struct usock *s, struct iovec_k *iov, size_t niov, struct file **fds, int nfds,
                       const struct sockaddr_un_k *to, int tolen, int flags, bool *consumed) {
    size_t total = 0;
    for (size_t i = 0; i < niov; i++) total += iov[i].len;
    bool nb = file_nonblock(file, flags);
    bool dgram = s->type != SOCK_STREAM;
    if (dgram && total > SOCK_BUF) return -EMSGSIZE;
    /* datagram destinations that are filesystem names are resolved once, outside the lock */
    struct inode *ino = nullptr;
    if (s->type == SOCK_DGRAM) {
        struct sockaddr_un_k d0; int d0len = 0;
        if (to) { d0 = *to; d0len = tolen; }
        else { uint64_t f = ulock(); d0 = s->dst; d0len = s->dstlen; uunlock(f); }
        if (d0len) { int r = name_inode(&d0, d0len, &ino); if (r) return r == -ENOENT ? -ENOENT : r; }
    }
    size_t sent = 0, vi = 0, vo = 0;
    bool first = true;
    int64_t ret = 0;
    while (first || sent < total) {
        size_t want = dgram ? total : MIN(total - sent, (size_t)CHUNK_MAX);
        struct chunk *c = kmalloc(sizeof *c + want);
        if (!c) { ret = -ENOMEM; break; }
        memset(c, 0, sizeof *c);
        size_t got = 0;
        while (got < want) {                                 /* gather from the iovecs */
            size_t n = MIN(want - got, iov[vi].len - vo);
            if (n && copy_from_user(c->data + got, (uint8_t *)iov[vi].base + vo, n)) { kfree(c); c = nullptr; break; }
            got += n; vo += n;
            if (vo == iov[vi].len) { vi++; vo = 0; }
        }
        if (!c) { ret = -EFAULT; break; }
        c->len = want;
        for (;;) {
            int err = 0;
            uint64_t f = ulock();
            struct usock *d = dest_locked(s, to, tolen, ino, &err);
            if (!d) {
                uunlock(f);
                if (err == -EPIPE && !(flags & MSG_NOSIGNAL)) raise_sigpipe();
                ret = err;
                break;
            }
            if (d->rxbytes + want > SOCK_BUF && d->rxbytes) {
                if (nb) { uunlock(f); ret = -EAGAIN; break; }
                uint64_t seq = poll_seq_read();
                uunlock(f);
                int w = usleep_seq(seq);
                if (w) { ret = w; break; }
                continue;                                    /* re-resolve: peer may be gone */
            }
            c->cred = s->cred;
            if (s->bound) { c->from = s->name; c->fromlen = s->namelen; }
            if (first && nfds) { c->fds = fds; c->nfds = nfds; fds = nullptr; if (consumed) *consumed = true; }
            list_add_tail(&d->rxq, &c->node);
            d->rxbytes += want;
            uunlock(f);
            c = nullptr;
            poll_notify();
            break;
        }
        if (c) { kfree(c); break; }                          /* not queued: ret holds the error */
        sent += want;
        first = false;
    }
    name_inode_put(ino);
    return sent ? (int64_t)sent : ret;
}

static unsigned rx_ready(struct usock *s) {          /* unix_lock held */
    if (!list_empty(&s->rxq)) return 1;
    if (s->type == SOCK_DGRAM) return 0;
    return s->peer_gone || s->peer_shut_wr || s->shut_rd ? 2 : 0;   /* EOF */
}

/* deliver passed files into the caller's fd table as an SCM_RIGHTS cmsg; runs outside
 * unix_lock, and under the BKL because fd_alloc() is serialised by it */
static int put_rights(struct file **fv, int nfv, struct msghdr_k *m, size_t *ctl_used, int flags) {
    if (!nfv) return 0;
    size_t room = m->controllen > *ctl_used ? m->controllen - *ctl_used : 0;
    size_t hdr = sizeof(struct cmsghdr_k);
    int fit = room > hdr ? (int)((room - hdr) / sizeof(int)) : 0;
    int n = MIN(fit, nfv);
    int fdv[MAX_PASS_FDS];
    int k = 0;
    bool took = !bkl_held();
    if (took) bkl_enter();
    for (; k < n; k++) {
        int nfd = fd_alloc(fv[k], 0, flags & MSG_CMSG_CLOEXEC);
        if (nfd < 0) break;
        fv[k] = nullptr;                                  /* reference moved into the fd table */
        fdv[k] = nfd;
    }
    if (took) bkl_exit();
    if (k < nfv) m->flags |= MSG_CTRUNC;
    for (int j = k; j < nfv; j++) if (fv[j]) { vfs_close(fv[j]); fv[j] = nullptr; }
    if (!k) return 0;
    struct cmsghdr_k h = { hdr + k * sizeof(int), SOL_SOCKET, SCM_RIGHTS };
    uint8_t *dst = (uint8_t *)m->control + *ctl_used;
    if (copy_to_user(dst, &h, sizeof h) || copy_to_user(dst + hdr, fdv, k * sizeof(int))) return -EFAULT;
    *ctl_used += CMSG_ALIGN_K(h.len);
    return 0;
}

static int put_creds(const struct ucred_k *cred, struct msghdr_k *m, size_t *ctl_used) {
    size_t hdr = sizeof(struct cmsghdr_k), need = hdr + sizeof(struct ucred_k);
    if (m->controllen < *ctl_used + need) { m->flags |= MSG_CTRUNC; return 0; }
    struct cmsghdr_k h = { need, SOL_SOCKET, SCM_CREDENTIALS };
    uint8_t *dst = (uint8_t *)m->control + *ctl_used;
    if (copy_to_user(dst, &h, sizeof h) || copy_to_user(dst + hdr, cred, sizeof *cred)) return -EFAULT;
    *ctl_used += CMSG_ALIGN_K(need);
    return 0;
}

static int64_t do_recv(struct file *file, struct usock *s, struct msghdr_k *m, int flags) {
    bool nb = file_nonblock(file, flags);
    uint64_t f;
    for (;;) {
        f = ulock();
        if (s->state == SS_LISTEN) { uunlock(f); return -EINVAL; }
        if (s->type != SOCK_DGRAM && s->state != SS_CONNECTED) { uunlock(f); return -ENOTCONN; }
        if (rx_ready(s)) break;                               /* keep the lock */
        if (nb) { uunlock(f); return -EAGAIN; }
        uint64_t seq = poll_seq_read();
        uunlock(f);
        int r = usleep_seq(seq);
        if (r) return r;
    }
    m->flags = 0;
    size_t ctl_used = 0, total = 0;
    for (size_t i = 0; i < m->iovlen; i++) total += m->iov[i].len;
    if (list_empty(&s->rxq)) { uunlock(f); m->controllen = 0; return 0; }       /* EOF */
    struct chunk *first = list_entry(s->rxq.next, struct chunk, node);
    size_t first_len = first->len;
    struct list_node dead = LIST_INIT(dead);
    struct file *fv[MAX_PASS_FDS]; int nfv = 0;
    struct ucred_k cred = first->cred;
    bool want_cred = false;
    int64_t err = 0;
    if (m->name) {
        struct sockaddr_un_k a = { .family = AF_UNIX }; int n = 2;
        if (first->fromlen) { a = first->from; n = first->fromlen; }
        if (copy_to_user(m->name, &a, MIN((size_t)m->namelen, (size_t)n))) { uunlock(f); return -EFAULT; }
        m->namelen = n;
    }
    size_t done = 0, vi = 0, vo = 0;
    bool peek = flags & MSG_PEEK;
    list_for_each_safe(it, tmp, &s->rxq) {
        struct chunk *c = list_entry(it, struct chunk, node);
        if (c != first && (c->nfds || s->type != SOCK_STREAM)) break;   /* message / fd boundary */
        size_t avail = c->len - c->off, take = MIN(avail, total - done);
        size_t t = 0;
        while (t < take) {
            size_t n = MIN(take - t, m->iov[vi].len - vo);
            if (n && copy_to_user((uint8_t *)m->iov[vi].base + vo, c->data + c->off + t, n)) { err = -EFAULT; break; }
            t += n; vo += n;
            if (vo == m->iov[vi].len) { vi++; vo = 0; }
        }
        if (err) break;
        done += take;
        if (!peek) {
            if (c->nfds) {                                    /* detach; installed after unlock */
                for (int i = 0; i < c->nfds; i++) if (c->fds[i]) fv[nfv++] = c->fds[i];
                c->nfds = 0;
            }
            if (c == first && s->passcred) want_cred = true;
        }
        if (s->type != SOCK_STREAM) {
            if (take < avail) m->flags |= MSG_TRUNC;
            if (!peek) { list_del(&c->node); s->rxbytes -= c->len - c->off; list_add_tail(&dead, &c->node); }
            break;
        }
        if (!peek) {
            c->off += take; s->rxbytes -= take;
            if (c->off == c->len) { list_del(&c->node); list_add_tail(&dead, &c->node); }
        }
        if (done == total) break;
    }
    uunlock(f);
    poll_notify();
    free_chunks(&dead);
    if (err && !done) {
        for (int i = 0; i < nfv; i++) vfs_close(fv[i]);
        return err;
    }
    if (put_rights(fv, nfv, m, &ctl_used, flags)) return -EFAULT;
    if (want_cred && put_creds(&cred, m, &ctl_used)) return -EFAULT;
    m->controllen = ctl_used;
    if (s->type != SOCK_STREAM && (flags & MSG_TRUNC)) return (int64_t)first_len;
    return done;
}

static int read_msghdr(const struct msghdr_k *um, struct msghdr_k *m, struct iovec_k **iov) {
    if (copy_from_user(m, um, sizeof *m)) return -EFAULT;
    if (m->iovlen > 1024) return -EMSGSIZE;
    *iov = kmalloc(sizeof(struct iovec_k) * (m->iovlen ? m->iovlen : 1));
    if (!*iov) return -ENOMEM;
    if (m->iovlen && copy_from_user(*iov, m->iov, sizeof(struct iovec_k) * m->iovlen)) { kfree(*iov); return -EFAULT; }
    m->iov = *iov;
    return 0;
}

int64_t sys_sendmsg(int fd, const struct msghdr_k *um, int flags) {
    int e;
    struct file *sf = sock_file_ref(fd, &e);
    if (!sf) return e;
    struct usock *s = sf->priv;
    struct msghdr_k m; struct iovec_k *iov;
    int r = read_msghdr(um, &m, &iov);
    if (r) { vfs_close(sf); return r; }
    struct file *fds[MAX_PASS_FDS]; int nfds = 0;
    struct sockaddr_un_k to; int tolen = 0;
    int64_t ret = 0;
    if (m.name && m.namelen) { ret = addr_in(m.name, m.namelen, &to, &tolen); if (ret) goto out; }
    if (m.control && m.controllen >= sizeof(struct cmsghdr_k)) {
        if (m.controllen > 65536) { ret = -ENOMEM; goto out; }
        uint8_t *ctl = kmalloc(m.controllen);
        if (!ctl) { ret = -ENOMEM; goto out; }
        if (copy_from_user(ctl, m.control, m.controllen)) { kfree(ctl); ret = -EFAULT; goto out; }
        for (size_t off = 0; off + sizeof(struct cmsghdr_k) <= m.controllen; ) {
            struct cmsghdr_k *h = (struct cmsghdr_k *)(ctl + off);
            if (h->len < sizeof *h || off + h->len > m.controllen) break;
            if (h->level == SOL_SOCKET && h->type == SCM_RIGHTS) {
                int n = (h->len - sizeof *h) / sizeof(int);
                int *v = (int *)(h + 1);
                for (int i = 0; i < n; i++) {
                    if (nfds >= MAX_PASS_FDS) { ret = -ETOOMANYREFS; break; }
                    struct file *f = fd_get_ref(v[i]);          /* referenced: safe without the BKL */
                    if (!f) { ret = -EBADF; break; }
                    fds[nfds++] = f;
                }
            }
            if (ret) break;
            off += CMSG_ALIGN_K(h->len);
        }
        kfree(ctl);
        if (ret) goto out;
    }
    struct file **fv = nullptr;
    if (nfds) {
        fv = kmalloc(sizeof(struct file *) * nfds);
        if (!fv) { ret = -ENOMEM; goto out; }
        memcpy(fv, fds, sizeof(struct file *) * nfds);
    }
    bool consumed = false;
    ret = do_send(sf, s, iov, m.iovlen, fv, nfds, tolen ? &to : nullptr, tolen, flags, &consumed);
    if (consumed) nfds = 0;                        /* references now travel with the message */
    else kfree(fv);
out:
    for (int i = 0; i < nfds; i++) vfs_close(fds[i]);
    kfree(iov);
    vfs_close(sf);
    return ret;
}

int64_t sys_recvmsg(int fd, struct msghdr_k *um, int flags) {
    int e;
    struct file *sf = sock_file_ref(fd, &e);
    if (!sf) return e;
    struct msghdr_k m; struct iovec_k *iov;
    int r = read_msghdr(um, &m, &iov);
    if (r) { vfs_close(sf); return r; }
    void *uiov = nullptr;
    { struct msghdr_k raw; if (!copy_from_user(&raw, um, sizeof raw)) uiov = raw.iov; }
    int64_t ret = do_recv(sf, sf->priv, &m, flags);
    if (ret >= 0) {
        m.iov = uiov;
        if (copy_to_user(um, &m, sizeof m)) ret = -EFAULT;
    }
    kfree(iov);
    vfs_close(sf);
    return ret;
}

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags, const void *uaddr, int alen) {
    int e;
    struct file *sf = sock_file_ref(fd, &e);
    if (!sf) return e;
    struct iovec_k iov = { (void *)buf, len };
    struct sockaddr_un_k to; int tolen = 0;
    int64_t r = 0;
    if (uaddr && alen) r = addr_in(uaddr, alen, &to, &tolen);
    if (!r) r = do_send(sf, sf->priv, &iov, 1, nullptr, 0, tolen ? &to : nullptr, tolen, flags, nullptr);
    vfs_close(sf);
    return r;
}

int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags, void *uaddr, int *ulen) {
    int e;
    struct file *sf = sock_file_ref(fd, &e);
    if (!sf) return e;
    struct iovec_k iov = { buf, len };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    int64_t r;
    if (uaddr && ulen) {
        m.name = uaddr;                         /* do_recv copies the sender's name out */
        int l;
        if (copy_from_user(&l, ulen, sizeof l)) r = -EFAULT;
        else {
            m.namelen = l;
            r = do_recv(sf, sf->priv, &m, flags);
            if (r >= 0) { int n = m.namelen; if (copy_to_user(ulen, &n, sizeof n)) r = -EFAULT; }
        }
    } else r = do_recv(sf, sf->priv, &m, flags);
    vfs_close(sf);
    return r;
}

/* ------------------------------------------------------------- file ops */
static ssize_t u_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct usock *s = f->priv;
    struct iovec_k iov = { buf, n };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    return do_recv(f, s, &m, 0);
}
static ssize_t u_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct usock *s = f->priv;
    struct iovec_k iov = { (void *)buf, n };
    return do_send(f, s, &iov, 1, nullptr, 0, nullptr, 0, 0, nullptr);
}
static unsigned u_poll(struct file *f) {
    struct usock *s = f->priv;
    unsigned r = 0;
    uint64_t fl = ulock();
    if (s->state == SS_LISTEN) { r = list_empty(&s->backlog) ? 0 : POLLIN | POLLRDNORM; uunlock(fl); return r; }
    unsigned rx = rx_ready(s);
    if (rx) r |= POLLIN | POLLRDNORM;
    if (s->type != SOCK_DGRAM && s->state == SS_CONNECTED && (s->peer_gone || (s->shut_rd && s->shut_wr))) r |= POLLHUP;
    if (s->type != SOCK_DGRAM && (s->peer_gone || s->peer_shut_wr)) r |= 0x2000;   /* POLLRDHUP */
    if (s->type == SOCK_DGRAM) r |= POLLOUT | POLLWRNORM;
    else if (s->state == SS_CONNECTED && s->peer && s->peer->rxbytes < SOCK_BUF) r |= POLLOUT | POLLWRNORM;
    else if (s->state == SS_CONNECTED && !s->peer) r |= POLLERR;
    uunlock(fl);
    return r;
}
static int u_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct usock *s = f->priv;
    if (cmd == 0x541B) {                                    /* FIONREAD */
        uint64_t fl = ulock();
        int v = s->type == SOCK_STREAM ? (int)s->rxbytes
              : (list_empty(&s->rxq) ? 0 : (int)list_entry(s->rxq.next, struct chunk, node)->len);
        uunlock(fl);
        return copy_to_user((void *)arg, &v, sizeof v) ? -EFAULT : 0;
    }
    return -ENOTTY;
}
static void u_release(struct file *f) {
    struct usock *s = f->priv;
    if (s) usock_destroy(s);
}
static const struct file_ops unix_fops = { .nobkl = true, .read = u_read, .write = u_write, .poll = u_poll, .ioctl = u_ioctl, .release = u_release };
