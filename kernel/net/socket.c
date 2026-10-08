/*
 * Socket system calls (M32): dispatch on the address family at creation and on the socket
 * file's operations afterwards. AF_UNIX lives in unix.c (whose entry points still resolve the
 * fd themselves); AF_INET and AF_PACKET in inet.c. AF_INET6 and AF_NETLINK are not
 * implemented and fail with EAFNOSUPPORT, which makes musl, BusyBox and most programs fall
 * back to IPv4 and /proc.
 */
#include <kernel/net.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/uaccess.h>

extern const struct file_ops unix_fops, inet_fops;
int64_t unix_sys_socket(int, int, int);
int64_t unix_sys_socketpair(int, int, int, int *);
int64_t unix_sys_bind(int, const void *, int);
int64_t unix_sys_listen(int, int);
int64_t unix_sys_connect(int, const void *, int);
int64_t unix_sys_accept4(int, void *, int *, int);
int64_t unix_sys_getsockname(int, void *, int *);
int64_t unix_sys_getpeername(int, void *, int *);
int64_t unix_sys_shutdown(int, int);
int64_t unix_sys_setsockopt(int, int, int, const void *, int);
int64_t unix_sys_getsockopt(int, int, int, void *, int *);
int64_t unix_sys_sendmsg(int, const struct msghdr_k *, int);
int64_t unix_sys_recvmsg(int, struct msghdr_k *, int);
int64_t unix_sys_sendto(int, const void *, size_t, int, const void *, int);
int64_t unix_sys_recvfrom(int, void *, size_t, int, void *, int *);
int inet_socket(int domain, int type, int proto);
int inet_bind(struct file *f, const void *uaddr, int len);
int inet_connect(struct file *f, const void *uaddr, int len);
int inet_listen(struct file *f, int backlog);
int inet_accept(struct file *f, void *uaddr, int *ulen, int flags);
int inet_getname(struct file *f, void *uaddr, int *ulen, bool peer);
int inet_shutdown(struct file *f, int how);
int inet_setsockopt(struct file *f, int level, int name, const void *uval, int len);
int inet_getsockopt(struct file *f, int level, int name, void *uval, int *ulen);
int64_t inet_sendmsg(struct file *f, struct msghdr_k *m, int flags);
int64_t inet_recvmsg(struct file *f, struct msghdr_k *m, int flags);

enum { K_BAD, K_UNIX, K_INET };
/* referenced socket file (dropped with vfs_close) and its kind */
static int sock_kind(int fd, struct file **out) {
    struct file *f = fd_get_ref(fd);
    *out = f;
    if (!f) return -EBADF;
    if (f->fops == &unix_fops) return K_UNIX;
    if (f->fops == &inet_fops && f->priv) return K_INET;
    vfs_close(f);
    *out = nullptr;
    return -ENOTSOCK;
}
#define DISPATCH(fd, unix_call, inet_call) ({ \
    struct file *f_; int k_ = sock_kind(fd, &f_); int64_t r_; \
    if (k_ < 0) r_ = k_; \
    else { r_ = k_ == K_UNIX ? (int64_t)(unix_call) : (int64_t)(inet_call); vfs_close(f_); } \
    r_; })

int64_t sys_socket(int domain, int type, int proto) {
    if (type & ~(0xf | 04000 | 02000000)) return -EINVAL;
    switch (domain) {
    case AF_UNIX: return unix_sys_socket(domain, type, proto);
    case AF_INET: case AF_PACKET: return inet_socket(domain, type, proto);
    default: return -EAFNOSUPPORT;
    }
}
int64_t sys_socketpair(int domain, int type, int proto, int *sv) {
    if (domain == AF_UNIX) return unix_sys_socketpair(domain, type, proto, sv);
    return domain == AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
}
int64_t sys_bind(int fd, const void *a, int l) { return DISPATCH(fd, unix_sys_bind(fd, a, l), inet_bind(f_, a, l)); }
int64_t sys_listen(int fd, int b) { return DISPATCH(fd, unix_sys_listen(fd, b), inet_listen(f_, b)); }
int64_t sys_connect(int fd, const void *a, int l) { return DISPATCH(fd, unix_sys_connect(fd, a, l), inet_connect(f_, a, l)); }
int64_t sys_accept4(int fd, void *a, int *l, int fl) { return DISPATCH(fd, unix_sys_accept4(fd, a, l, fl), inet_accept(f_, a, l, fl)); }
int64_t sys_accept(int fd, void *a, int *l) { return sys_accept4(fd, a, l, 0); }
int64_t sys_getsockname(int fd, void *a, int *l) { return DISPATCH(fd, unix_sys_getsockname(fd, a, l), inet_getname(f_, a, l, false)); }
int64_t sys_getpeername(int fd, void *a, int *l) { return DISPATCH(fd, unix_sys_getpeername(fd, a, l), inet_getname(f_, a, l, true)); }
int64_t sys_shutdown(int fd, int how) { return DISPATCH(fd, unix_sys_shutdown(fd, how), inet_shutdown(f_, how)); }
int64_t sys_setsockopt(int fd, int lv, int n, const void *v, int l) { return DISPATCH(fd, unix_sys_setsockopt(fd, lv, n, v, l), inet_setsockopt(f_, lv, n, v, l)); }
int64_t sys_getsockopt(int fd, int lv, int n, void *v, int *l) { return DISPATCH(fd, unix_sys_getsockopt(fd, lv, n, v, l), inet_getsockopt(f_, lv, n, v, l)); }

/* msghdr with a kernel copy of the iovec array */
static int msg_in(const struct msghdr_k *um, struct msghdr_k *m, struct iovec_k **iov) {
    if (copy_from_user(m, um, sizeof *m)) return -EFAULT;
    if (m->iovlen > 1024) return -EMSGSIZE;
    *iov = kmalloc(sizeof(struct iovec_k) * (m->iovlen ? m->iovlen : 1));
    if (!*iov) return -ENOMEM;
    if (m->iovlen && copy_from_user(*iov, m->iov, sizeof(struct iovec_k) * m->iovlen)) { kfree(*iov); return -EFAULT; }
    for (size_t i = 0; i < m->iovlen; i++)
        if (!access_ok((*iov)[i].base, (*iov)[i].len)) { kfree(*iov); return -EFAULT; }
    m->iov = *iov;
    return 0;
}

static int64_t inet_sendmsg_u(struct file *f, const struct msghdr_k *um, int flags) {
    struct msghdr_k m; struct iovec_k *iov;
    int r = msg_in(um, &m, &iov);
    if (r) return r;
    int64_t ret = inet_sendmsg(f, &m, flags);
    kfree(iov);
    return ret;
}
static int64_t inet_recvmsg_u(struct file *f, struct msghdr_k *um, int flags) {
    struct msghdr_k m; struct iovec_k *iov;
    int r = msg_in(um, &m, &iov);
    if (r) return r;
    int64_t ret = inet_recvmsg(f, &m, flags);
    if (ret >= 0) {
        uint32_t nl = m.namelen; size_t cl = m.controllen; int fl = m.flags;
        if (copy_to_user(&um->namelen, &nl, sizeof nl) || copy_to_user(&um->controllen, &cl, sizeof cl) ||
            copy_to_user(&um->flags, &fl, sizeof fl)) ret = -EFAULT;
    }
    kfree(iov);
    return ret;
}
static int64_t inet_sendto_u(struct file *f, const void *buf, size_t len, int flags, const void *a, int al) {
    if (!access_ok(buf, len)) return -EFAULT;
    struct iovec_k iov = { (void *)buf, len };
    struct msghdr_k m = { .name = (void *)a, .namelen = a ? (uint32_t)al : 0, .iov = &iov, .iovlen = 1 };
    return inet_sendmsg(f, &m, flags);
}
static int64_t inet_recvfrom_u(struct file *f, void *buf, size_t len, int flags, void *a, int *al) {
    if (!access_ok(buf, len)) return -EFAULT;
    struct iovec_k iov = { buf, len };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    if (a && al) {
        int l;
        if (copy_from_user(&l, al, sizeof l)) return -EFAULT;
        if (l < 0) return -EINVAL;
        m.name = a; m.namelen = (uint32_t)l;
    }
    int64_t r = inet_recvmsg(f, &m, flags);
    if (r >= 0 && a && al) { int n = (int)m.namelen; if (copy_to_user(al, &n, sizeof n)) r = -EFAULT; }
    return r;
}

int64_t sys_sendmsg(int fd, const struct msghdr_k *um, int flags) { return DISPATCH(fd, unix_sys_sendmsg(fd, um, flags), inet_sendmsg_u(f_, um, flags)); }
int64_t sys_recvmsg(int fd, struct msghdr_k *um, int flags) { return DISPATCH(fd, unix_sys_recvmsg(fd, um, flags), inet_recvmsg_u(f_, um, flags)); }
int64_t sys_sendto(int fd, const void *b, size_t n, int fl, const void *a, int al) {
    return DISPATCH(fd, unix_sys_sendto(fd, b, n, fl, a, al), inet_sendto_u(f_, b, n, fl, a, al));
}
int64_t sys_recvfrom(int fd, void *b, size_t n, int fl, void *a, int *al) {
    return DISPATCH(fd, unix_sys_recvfrom(fd, b, n, fl, a, al), inet_recvfrom_u(f_, b, n, fl, a, al));
}

/* sendmmsg/recvmmsg: loops over the single-message calls (musl's DNS resolver uses neither,
 * but some tools do) */
struct mmsghdr_k { struct msghdr_k hdr; uint32_t len, pad; };
int64_t sys_sendmmsg(int fd, struct mmsghdr_k *v, unsigned n, int flags) {
    unsigned i;
    for (i = 0; i < n && i < 1024; i++) {
        int64_t r = sys_sendmsg(fd, &v[i].hdr, flags);
        if (r < 0) return i ? (int64_t)i : r;
        uint32_t l = (uint32_t)r;
        if (copy_to_user(&v[i].len, &l, sizeof l)) return i ? (int64_t)i : -EFAULT;
    }
    return i;
}
int64_t sys_recvmmsg(int fd, struct mmsghdr_k *v, unsigned n, int flags, void *timeout) {
    (void)timeout;
    unsigned i;
    for (i = 0; i < n && i < 1024; i++) {
        int fl = (flags & ~0x10000) | (i && (flags & 0x10000) ? 0x40 : 0);   /* MSG_WAITFORONE: then MSG_DONTWAIT */
        int64_t r = sys_recvmsg(fd, &v[i].hdr, fl);
        if (r < 0) return i ? (int64_t)i : r;
        uint32_t l = (uint32_t)r;
        if (copy_to_user(&v[i].len, &l, sizeof l)) return i ? (int64_t)i : -EFAULT;
    }
    return i;
}
