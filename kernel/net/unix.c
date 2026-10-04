/*
 * AF_UNIX sockets: SOCK_STREAM, SOCK_SEQPACKET and SOCK_DGRAM; filesystem and abstract
 * names; socketpair; SCM_RIGHTS file descriptor passing; SO_PEERCRED. This is the IPC that
 * Wayland, D-Bus and X11 are built on.
 *
 * Data is queued as chunks (one per send for datagram types, up to 64 KiB for streams).
 * Passed files ride on the chunk that starts the message; a stream recv never merges data
 * across a chunk carrying files, so they arrive with the first byte sent alongside them.
 * Blocking uses the global poll_wq like pipes/poll.
 */
#include <kernel/vfs.h>
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

static struct list_node bound_socks = LIST_INIT(bound_socks);
static const struct file_ops unix_fops;

static struct usock *usock_new(int type) {
    struct usock *s = kzalloc(sizeof *s);
    if (!s) return nullptr;
    s->type = type;
    list_init(&s->rxq); list_init(&s->backlog);
    s->cred = (struct ucred_k){ curproc ? curproc->pid : 0, curproc ? curproc->uid : 0, curproc ? curproc->gid : 0 };
    return s;
}

static void chunk_free(struct chunk *c) {
    for (int i = 0; i < c->nfds; i++) if (c->fds[i]) vfs_close(c->fds[i]);
    kfree(c->fds);
    kfree(c);
}

static void usock_destroy(struct usock *s);

static void disconnect(struct usock *s) {
    if (s->peer) { s->peer->peer = nullptr; s->peer->peer_gone = true; s->peer = nullptr; }
}

static void usock_destroy(struct usock *s) {
    disconnect(s);
    list_for_each_safe(it, tmp, &s->rxq) { list_del(it); chunk_free(list_entry(it, struct chunk, node)); }
    list_for_each_safe(it, tmp, &s->backlog) {               /* never-accepted connections */
        struct usock *c = list_entry(it, struct usock, bl_node);
        list_del(it);
        usock_destroy(c);
    }
    if (s->bound) list_del(&s->all_node);
    if (s->bind_inode) iput(s->bind_inode);
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

static struct usock *sock_of(int fd, int *err) {
    struct file *f = fd_get(fd);
    if (!f) { *err = -EBADF; return nullptr; }
    if (f->fops != &unix_fops) { *err = -ENOTSOCK; return nullptr; }
    return f->priv;
}
#define GET_SOCK(s, fd) int __e; struct usock *s = sock_of(fd, &__e); if (!s) return __e
static bool nonblock(int fd, int flags) { struct file *f = fd_get(fd); return (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK)); }

#define WAIT_FOR(nb, cond) ({ int __r = 0; \
    while (!(cond)) { \
        if (nb) { __r = -EAGAIN; break; } \
        uint64_t __fl = arch_irq_save(); \
        if (!(cond)) __r = wait_event(&poll_wq); \
        arch_irq_restore(__fl); \
        if (__r) break; \
    } __r; })

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

static struct usock *lookup_name(const struct sockaddr_un_k *a, int len, int *err) {
    if (is_abstract(a, len)) {
        list_for_each(it, &bound_socks) {
            struct usock *s = list_entry(it, struct usock, all_node);
            if (!s->bind_inode && s->namelen == len && !memcmp(s->name.path, a->path, len - 2)) return s;
        }
        *err = -ECONNREFUSED;
        return nullptr;
    }
    struct inode *ino;
    int r = vfs_lookup(a->path, true, &ino);
    if (r) { *err = r; return nullptr; }
    struct usock *found = nullptr;
    if (S_ISSOCK(ino->mode))
        list_for_each(it, &bound_socks) {
            struct usock *s = list_entry(it, struct usock, all_node);
            if (s->bind_inode == ino) { found = s; break; }
        }
    iput(ino);
    if (!found) *err = -ECONNREFUSED;
    return found;
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

/* ------------------------------------------------------------- syscalls */
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
    a->peer = b; b->peer = a;
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
    if (is_abstract(&a, alen)) {
        int e;
        if (lookup_name(&a, alen, &e)) return -EADDRINUSE;
    } else {
        r = vfs_mknod_at(nullptr, a.path, S_IFSOCK | (0777 & ~curproc->umask), 0);
        if (r == -EEXIST) return -EADDRINUSE;
        if (r) return r;
        r = vfs_lookup(a.path, false, &s->bind_inode);
        if (r) return r;
    }
    s->name = a; s->namelen = alen; s->bound = true;
    list_add_tail(&bound_socks, &s->all_node);
    return 0;
}

int64_t sys_listen(int fd, int backlog) {
    GET_SOCK(s, fd);
    if (s->type == SOCK_DGRAM) return -EOPNOTSUPP;
    if (!s->bound) return -EINVAL;              /* no autobind */
    s->state = SS_LISTEN;
    s->maxbacklog = backlog < 1 ? 1 : MIN(backlog, 128);
    return 0;
}

int64_t sys_connect(int fd, const void *uaddr, int len) {
    GET_SOCK(s, fd);
    struct sockaddr_un_k a; int alen;
    int r = addr_in(uaddr, len, &a, &alen);
    if (r) return r;
    int err;
    if (s->type == SOCK_DGRAM) {
        struct usock *t = lookup_name(&a, alen, &err);
        if (!t) return err;
        s->dst = a; s->dstlen = alen;
        s->state = SS_CONNECTED;
        return 0;
    }
    if (s->state == SS_CONNECTED) return -EISCONN;
    if (s->state == SS_LISTEN) return -EINVAL;
    bool nb = nonblock(fd, 0);
    struct usock *l;
    for (;;) {
        l = lookup_name(&a, alen, &err);
        if (!l) return err;
        if (l->type != s->type) return -EPROTOTYPE;
        if (l->state != SS_LISTEN) return -ECONNREFUSED;
        if (l->nbacklog < l->maxbacklog) break;
        if (nb) return -EAGAIN;
        uint64_t fl = arch_irq_save();
        int w = wait_event(&poll_wq);
        arch_irq_restore(fl);
        if (w) return w;
    }
    struct usock *srv = usock_new(s->type);
    if (!srv) return -ENOMEM;
    srv->cred = l->cred;
    srv->name = l->name; srv->namelen = l->namelen;
    srv->peer = s; s->peer = srv;
    srv->state = s->state = SS_CONNECTED;
    srv->peercred = s->cred; s->peercred = l->cred;
    list_add_tail(&l->backlog, &srv->bl_node);
    l->nbacklog++;
    s->dst = a; s->dstlen = alen;
    poll_notify();
    return 0;
}

int64_t sys_accept4(int fd, void *uaddr, int *ulen, int flags) {
    GET_SOCK(l, fd);
    if (l->state != SS_LISTEN) return -EINVAL;
    int r = WAIT_FOR(nonblock(fd, 0), !list_empty(&l->backlog));
    if (r) return r;
    struct usock *srv = list_entry(l->backlog.next, struct usock, bl_node);
    list_del(&srv->bl_node);
    l->nbacklog--;
    poll_notify();
    int nfd = sock_file(srv, flags);
    if (nfd < 0) { usock_destroy(srv); return nfd; }
    struct usock *p = srv->peer;
    if (uaddr && ulen) {
        r = put_name(uaddr, ulen, p && p->bound ? &p->name : nullptr, p && p->bound ? p->namelen : 0);
        if (r) { fd_close(nfd); return r; }
    }
    return nfd;
}
int64_t sys_accept(int fd, void *uaddr, int *ulen) { return sys_accept4(fd, uaddr, ulen, 0); }

int64_t sys_getsockname(int fd, void *uaddr, int *ulen) {
    GET_SOCK(s, fd);
    return put_name(uaddr, ulen, s->namelen ? &s->name : nullptr, s->namelen);
}
int64_t sys_getpeername(int fd, void *uaddr, int *ulen) {
    GET_SOCK(s, fd);
    if (s->type == SOCK_DGRAM) {
        if (!s->dstlen) return -ENOTCONN;
        return put_name(uaddr, ulen, &s->dst, s->dstlen);
    }
    if (s->state != SS_CONNECTED) return -ENOTCONN;
    if (s->dstlen) return put_name(uaddr, ulen, &s->dst, s->dstlen);     /* client: the listener's name */
    struct usock *p = s->peer;
    return put_name(uaddr, ulen, p && p->bound ? &p->name : nullptr, p && p->bound ? p->namelen : 0);
}

int64_t sys_shutdown(int fd, int how) {
    GET_SOCK(s, fd);
    if (how < 0 || how > 2) return -EINVAL;
    if (how != 1) s->shut_rd = true;
    if (how != 0) { s->shut_wr = true; if (s->peer) s->peer->peer_shut_wr = true; }
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
        s->passcred = v;
    }
    return 0;   /* buffer sizes, timeouts etc. are accepted and ignored */
}

int64_t sys_getsockopt(int fd, int level, int name, void *uval, int *ulen) {
    GET_SOCK(s, fd);
    int len;
    if (copy_from_user(&len, ulen, sizeof len)) return -EFAULT;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    int v;
    switch (name) {
    case SO_PEERCRED: {
        if (s->state != SS_CONNECTED && s->type != SOCK_DGRAM) return -ENOTCONN;
        struct ucred_k c = s->peercred;
        int n = MIN(len, (int)sizeof c);
        if (copy_to_user(uval, &c, n) || copy_to_user(ulen, &n, sizeof n)) return -EFAULT;
        return 0;
    }
    case SO_TYPE: v = s->type; break;
    case SO_ERROR: v = 0; break;
    case SO_SNDBUF: case SO_RCVBUF: v = SOCK_BUF; break;
    case SO_PASSCRED: v = s->passcred; break;
    case SO_ACCEPTCONN: v = s->state == SS_LISTEN; break;
    case SO_PROTOCOL: v = 0; break;
    case SO_DOMAIN: v = AF_UNIX; break;
    default: return -ENOPROTOOPT;
    }
    int n = MIN(len, 4);
    if (copy_to_user(uval, &v, n) || copy_to_user(ulen, &n, sizeof n)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------- data path */
struct iovec_k { void *base; size_t len; };
struct msghdr_k { void *name; uint32_t namelen, _p0; struct iovec_k *iov; size_t iovlen; void *control; size_t controllen; int flags, _p1; };
struct cmsghdr_k { size_t len; int level, type; };
#define CMSG_ALIGN_K(n) (((n) + 7) & ~7ul)

static struct usock *dest_of(struct usock *s, const struct sockaddr_un_k *a, int alen, int *err) {
    if (s->type == SOCK_DGRAM) {
        if (a) return lookup_name(a, alen, err);
        if (!s->dstlen) { *err = -ENOTCONN; return nullptr; }
        struct usock *t = lookup_name(&s->dst, s->dstlen, err);
        if (!t) *err = -ECONNREFUSED;
        return t;
    }
    if (s->state != SS_CONNECTED) { *err = -ENOTCONN; return nullptr; }
    if (!s->peer || s->shut_wr || s->peer->shut_rd) { *err = -EPIPE; return nullptr; }
    return s->peer;
}

/* On success the fds array (if any) is owned by the queue and *consumed is set. */
static int64_t do_send(int fd, struct usock *s, struct iovec_k *iov, size_t niov, struct file **fds, int nfds,
                       const struct sockaddr_un_k *to, int tolen, int flags, bool *consumed) {
    size_t total = 0;
    for (size_t i = 0; i < niov; i++) total += iov[i].len;
    bool nb = nonblock(fd, flags);
    bool dgram = s->type != SOCK_STREAM;
    if (dgram && total > SOCK_BUF) return -EMSGSIZE;
    size_t sent = 0, vi = 0, vo = 0;
    bool first = true;
    while (first || sent < total) {
        int err = 0;
        struct usock *d = dest_of(s, to, tolen, &err);
        if (!d) {
            if (err == -EPIPE && !(flags & MSG_NOSIGNAL)) signal_send(curproc, SIGPIPE);
            return sent ? (int64_t)sent : err;
        }
        size_t want = dgram ? total : MIN(total - sent, (size_t)CHUNK_MAX);
        if (d->rxbytes + want > SOCK_BUF && d->rxbytes) {
            if (nb) return sent ? (int64_t)sent : -EAGAIN;
            uint64_t fl = arch_irq_save();
            int w = 0;
            if (d->rxbytes + want > SOCK_BUF && d->rxbytes) w = wait_event(&poll_wq);
            arch_irq_restore(fl);
            if (w) return sent ? (int64_t)sent : w;
            continue;                                        /* re-resolve: peer may be gone */
        }
        struct chunk *c = kmalloc(sizeof *c + want);
        if (!c) return sent ? (int64_t)sent : -ENOMEM;
        memset(c, 0, sizeof *c);
        size_t got = 0;
        while (got < want) {                                 /* gather from the iovecs */
            size_t n = MIN(want - got, iov[vi].len - vo);
            if (n && copy_from_user(c->data + got, (uint8_t *)iov[vi].base + vo, n)) { kfree(c); return sent ? (int64_t)sent : -EFAULT; }
            got += n; vo += n;
            if (vo == iov[vi].len) { vi++; vo = 0; }
        }
        c->len = want;
        c->cred = s->cred;
        if (s->bound) { c->from = s->name; c->fromlen = s->namelen; }
        if (first && nfds) { c->fds = fds; c->nfds = nfds; fds = nullptr; if (consumed) *consumed = true; }
        list_add_tail(&d->rxq, &c->node);
        d->rxbytes += want;
        sent += want;
        first = false;
        poll_notify();
    }
    return sent;
}

static unsigned rx_ready(struct usock *s) {
    if (!list_empty(&s->rxq)) return 1;
    if (s->type == SOCK_DGRAM) return 0;
    return s->peer_gone || s->peer_shut_wr || s->shut_rd ? 2 : 0;   /* EOF */
}

/* deliver passed files into the caller's fd table as an SCM_RIGHTS cmsg */
static int put_rights(struct chunk *c, struct msghdr_k *m, size_t *ctl_used, int flags) {
    if (!c->nfds) return 0;
    size_t room = m->controllen > *ctl_used ? m->controllen - *ctl_used : 0;
    size_t hdr = sizeof(struct cmsghdr_k);
    int fit = room > hdr ? (int)((room - hdr) / sizeof(int)) : 0;
    int n = MIN(fit, c->nfds);
    int fdv[MAX_PASS_FDS];
    int k = 0;
    for (; k < n; k++) {
        int nfd = fd_alloc(c->fds[k], 0, flags & MSG_CMSG_CLOEXEC);
        if (nfd < 0) break;
        c->fds[k] = nullptr;                              /* reference moved into the fd table */
        fdv[k] = nfd;
    }
    if (k < c->nfds) m->flags |= MSG_CTRUNC;
    for (int j = k; j < c->nfds; j++) if (c->fds[j]) { vfs_close(c->fds[j]); c->fds[j] = nullptr; }
    c->nfds = 0;
    if (!k) return 0;
    struct cmsghdr_k h = { hdr + k * sizeof(int), SOL_SOCKET, SCM_RIGHTS };
    uint8_t *dst = (uint8_t *)m->control + *ctl_used;
    if (copy_to_user(dst, &h, sizeof h) || copy_to_user(dst + hdr, fdv, k * sizeof(int))) return -EFAULT;
    *ctl_used += CMSG_ALIGN_K(h.len);
    return 0;
}

static int put_creds(struct chunk *c, struct msghdr_k *m, size_t *ctl_used) {
    size_t hdr = sizeof(struct cmsghdr_k), need = hdr + sizeof(struct ucred_k);
    if (m->controllen < *ctl_used + need) { m->flags |= MSG_CTRUNC; return 0; }
    struct cmsghdr_k h = { need, SOL_SOCKET, SCM_CREDENTIALS };
    uint8_t *dst = (uint8_t *)m->control + *ctl_used;
    if (copy_to_user(dst, &h, sizeof h) || copy_to_user(dst + hdr, &c->cred, sizeof c->cred)) return -EFAULT;
    *ctl_used += CMSG_ALIGN_K(need);
    return 0;
}

static int64_t do_recv(int fd, struct usock *s, struct msghdr_k *m, int flags) {
    if (s->state == SS_LISTEN) return -EINVAL;
    if (s->type != SOCK_DGRAM && s->state != SS_CONNECTED) return -ENOTCONN;
    int r = WAIT_FOR(nonblock(fd, flags), rx_ready(s));
    if (r) return r;
    m->flags = 0;
    size_t ctl_used = 0, total = 0;
    for (size_t i = 0; i < m->iovlen; i++) total += m->iov[i].len;
    if (list_empty(&s->rxq)) { m->controllen = 0; return 0; }       /* EOF */
    struct chunk *first = list_entry(s->rxq.next, struct chunk, node);
    if (m->name) {
        struct sockaddr_un_k a = { .family = AF_UNIX }; int n = 2;
        if (first->fromlen) { a = first->from; n = first->fromlen; }
        if (copy_to_user(m->name, &a, MIN((size_t)m->namelen, (size_t)n))) return -EFAULT;
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
            if (n && copy_to_user((uint8_t *)m->iov[vi].base + vo, c->data + c->off + t, n)) return done ? (int64_t)done : -EFAULT;
            t += n; vo += n;
            if (vo == m->iov[vi].len) { vi++; vo = 0; }
        }
        done += take;
        if (!peek) {
            if (c->nfds && put_rights(c, m, &ctl_used, flags)) return -EFAULT;
            if (c == first && s->passcred && put_creds(c, m, &ctl_used)) return -EFAULT;
        }
        if (s->type != SOCK_STREAM) {
            if (take < avail) m->flags |= MSG_TRUNC;
            if (!peek) { list_del(&c->node); s->rxbytes -= c->len - c->off; chunk_free(c); }
            break;
        }
        if (!peek) {
            c->off += take; s->rxbytes -= take;
            if (c->off == c->len) { list_del(&c->node); chunk_free(c); }
        }
        if (done == total) break;
    }
    m->controllen = ctl_used;
    poll_notify();
    if (s->type != SOCK_STREAM && (flags & MSG_TRUNC)) return first ? (int64_t)first->len : (int64_t)done;
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
    GET_SOCK(s, fd);
    struct msghdr_k m; struct iovec_k *iov;
    int r = read_msghdr(um, &m, &iov);
    if (r) return r;
    struct file *fds[MAX_PASS_FDS]; int nfds = 0;
    struct sockaddr_un_k to; int tolen = 0;
    int64_t ret = 0;
    if (m.name && m.namelen) { ret = addr_in(m.name, m.namelen, &to, &tolen); if (ret) goto out; }
    if (m.control && m.controllen >= sizeof(struct cmsghdr_k)) {
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
                    struct file *f = fd_get(v[i]);
                    if (!f || nfds >= MAX_PASS_FDS) { ret = f ? -ETOOMANYREFS : -EBADF; break; }
                    fds[nfds++] = file_get(f);
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
    ret = do_send(fd, s, iov, m.iovlen, fv, nfds, tolen ? &to : nullptr, tolen, flags, &consumed);
    if (consumed) nfds = 0;                        /* references now travel with the message */
    else kfree(fv);
out:
    for (int i = 0; i < nfds; i++) vfs_close(fds[i]);
    kfree(iov);
    return ret;
}

int64_t sys_recvmsg(int fd, struct msghdr_k *um, int flags) {
    GET_SOCK(s, fd);
    struct msghdr_k m; struct iovec_k *iov;
    int r = read_msghdr(um, &m, &iov);
    if (r) return r;
    void *uiov = nullptr;
    { struct msghdr_k raw; copy_from_user(&raw, um, sizeof raw); uiov = raw.iov; }
    int64_t ret = do_recv(fd, s, &m, flags);
    if (ret >= 0) {
        m.iov = uiov;
        if (copy_to_user(um, &m, sizeof m)) ret = -EFAULT;
    }
    kfree(iov);
    return ret;
}

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags, const void *uaddr, int alen) {
    GET_SOCK(s, fd);
    struct iovec_k iov = { (void *)buf, len };
    struct sockaddr_un_k to; int tolen = 0;
    if (uaddr && alen) { int r = addr_in(uaddr, alen, &to, &tolen); if (r) return r; }
    return do_send(fd, s, &iov, 1, nullptr, 0, tolen ? &to : nullptr, tolen, flags, nullptr);
}

int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags, void *uaddr, int *ulen) {
    GET_SOCK(s, fd);
    struct iovec_k iov = { buf, len };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    int64_t r;
    if (uaddr && ulen) {
        m.name = uaddr;                         /* do_recv copies the sender's name out */
        int l; if (copy_from_user(&l, ulen, sizeof l)) return -EFAULT;
        m.namelen = l;
        r = do_recv(fd, s, &m, flags);
        if (r >= 0) { int n = m.namelen; if (copy_to_user(ulen, &n, sizeof n)) return -EFAULT; }
        return r;
    }
    return do_recv(fd, s, &m, flags);
}

/* ------------------------------------------------------------- file ops */
static ssize_t u_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct usock *s = f->priv;
    struct iovec_k iov = { buf, n };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    return do_recv(-1, s, &m, f->flags & O_NONBLOCK ? MSG_DONTWAIT : 0);
}
static ssize_t u_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct usock *s = f->priv;
    struct iovec_k iov = { (void *)buf, n };
    return do_send(-1, s, &iov, 1, nullptr, 0, nullptr, 0, f->flags & O_NONBLOCK ? MSG_DONTWAIT : 0, nullptr);
}
static unsigned u_poll(struct file *f) {
    struct usock *s = f->priv;
    unsigned r = 0;
    if (s->state == SS_LISTEN) return list_empty(&s->backlog) ? 0 : POLLIN | POLLRDNORM;
    unsigned rx = rx_ready(s);
    if (rx) r |= POLLIN | POLLRDNORM;
    if (s->type != SOCK_DGRAM && s->state == SS_CONNECTED && (s->peer_gone || (s->shut_rd && s->shut_wr))) r |= POLLHUP;
    if (s->type != SOCK_DGRAM && (s->peer_gone || s->peer_shut_wr)) r |= 0x2000;   /* POLLRDHUP */
    if (s->type == SOCK_DGRAM) r |= POLLOUT | POLLWRNORM;
    else if (s->state == SS_CONNECTED && s->peer && s->peer->rxbytes < SOCK_BUF) r |= POLLOUT | POLLWRNORM;
    else if (s->state == SS_CONNECTED && !s->peer) r |= POLLERR;
    return r;
}
static int u_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct usock *s = f->priv;
    if (cmd == 0x541B) {                                    /* FIONREAD */
        int v = s->type == SOCK_STREAM ? (int)s->rxbytes
              : (list_empty(&s->rxq) ? 0 : (int)list_entry(s->rxq.next, struct chunk, node)->len);
        return copy_to_user((void *)arg, &v, sizeof v) ? -EFAULT : 0;
    }
    return -ENOTTY;
}
static void u_release(struct file *f) {
    struct usock *s = f->priv;
    if (s) usock_destroy(s);
}
static const struct file_ops unix_fops = { .read = u_read, .write = u_write, .poll = u_poll, .ioctl = u_ioctl, .release = u_release };
