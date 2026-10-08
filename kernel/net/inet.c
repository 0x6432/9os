/*
 * The socket layer for AF_INET (TCP, UDP, raw IP, ICMP ping sockets) and AF_PACKET.
 *
 * Every protocol object is a struct sock guarded by net_mutex. A socket file holds the sock
 * in f->priv; the data path (send/recv/read/write/poll) runs without the BKL. Blocking follows
 * the global poll scheme: sample poll_seq under net_mutex, drop it, poll_wait_seq(); every
 * state change calls sock_changed(), which recomputes the socket's poll mask (read without
 * locks by ->poll, so poll/epoll never block on net_mutex) and runs poll_notify().
 * User memory is copied outside net_mutex where it is convenient (bounce buffers), else under
 * it (a page fault taking the mm lock under the sleeping mutex is fine).
 */
#include <kernel/net.h>
#include <kernel/vfs.h>
#include <kernel/cred.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/printk.h>
#include <kernel/syscall.h>

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_PACKET 10
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_DONTROUTE 0x4
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000
#define POLLRDHUP 0x2000
#define BOUNCE 16384

bool tcp_abort_ret(struct sock *s, int err);
int arp_ioctl(uint64_t cmd, void *r);
int route_add(uint32_t dst, uint32_t mask, uint32_t gw, struct netdev *d, int metric, unsigned flags);
int route_del(uint32_t dst, uint32_t mask, uint32_t gw, struct netdev *d, int metric);
void eth_xmit_raw(struct netdev *d, struct pkt *p);
const struct file_ops inet_fops;

struct sock_filter_k { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct bpf_prog_k { int len; struct sock_filter_k *insns; };

/* ------------------------------------------------------------------ sock objects */
static bool linked(struct sock *s) { return s->node.next && s->node.next != &s->node; }
static void link_to(struct sock *s, struct list_node *l) { if (!linked(s)) list_add_tail(l, &s->node); }
static void unlink_sock(struct sock *s) { if (linked(s)) list_del(&s->node); list_init(&s->node); }

int sysctl_somaxconn = 4096;

static struct sock *sock_alloc(int family, int type, int proto) {
    struct sock *s = kzalloc(sizeof *s);
    if (!s) return nullptr;
    s->family = family; s->type = type; s->protocol = proto;
    list_init(&s->node);
    list_init(&s->rxq);
    list_init(&s->ooo);
    list_init(&s->children);
    s->rcvbuf = type == SOCK_STREAM ? 131072 : 212992;
    s->sndbuf = type == SOCK_STREAM ? 131072 : 212992;
    s->ttl = sysctl_ip_default_ttl;
    s->uid = current_cred()->euid;
    if (type == SOCK_STREAM) tcp_sock_init(s);
    return s;
}

void sock_free(struct sock *s) {
    unlink_sock(s);
    pkt_queue_purge(&s->rxq);
    pkt_queue_purge(&s->ooo);
    kfree(s->snd.buf);
    kfree(s->rcv.buf);
    struct bpf_prog_k *f = (struct bpf_prog_k *)s->filter;
    if (f) { kfree(f->insns); kfree(f); }
    kfree(s);
    poll_notify();
}

struct sock *sock_new_child(struct sock *l) {
    struct sock *c = sock_alloc(AF_INET, SOCK_STREAM, l->protocol);
    if (!c) return nullptr;
    c->rcvbuf = l->rcvbuf; c->sndbuf = l->sndbuf;
    c->nodelay = l->nodelay; c->keepalive = l->keepalive; c->reuseaddr = l->reuseaddr;
    c->keepidle = l->keepidle; c->keepintvl = l->keepintvl; c->keepcnt = l->keepcnt;
    c->ttl = l->ttl; c->tos = l->tos; c->bound_dev = l->bound_dev; c->uid = l->uid;
    c->linger_on = l->linger_on; c->linger_s = l->linger_s;
    c->bound = true;
    if (!ring_alloc(&c->snd, (size_t)c->sndbuf) || !ring_alloc(&c->rcv, (size_t)c->rcvbuf)) { sock_free(c); return nullptr; }
    c->parent = l;
    return c;
}

unsigned sock_poll_mask(struct sock *s) {
    unsigned m = 0;
    if (s->type == SOCK_STREAM) {
        if (s->state == TCP_LISTEN) {
            list_for_each(it, &s->children)
                if (list_entry(it, struct sock, child_node)->accepted_ready) return POLLIN | POLLRDNORM;
            return 0;
        }
        if (s->err) m |= POLLERR;
        if (s->rcv.len || s->peer_fin) m |= POLLIN | POLLRDNORM;
        if (s->peer_fin || s->shut_rd) m |= POLLIN | POLLRDNORM | POLLRDHUP;
        if (s->state == TCP_CLOSE) return m | POLLHUP | POLLIN | POLLRDNORM | POLLRDHUP | POLLOUT | POLLWRNORM;
        if (s->shut_wr && s->peer_fin) m |= POLLHUP;
        if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECV) return m;
        if (s->shut_wr) m |= POLLOUT | POLLWRNORM;
        else if ((s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) && ring_space(&s->snd) &&
                 ring_space(&s->snd) * 2 >= s->snd.len) m |= POLLOUT | POLLWRNORM;
        return m;
    }
    if (s->err) m |= POLLERR;
    if (!list_empty(&s->rxq) || s->shut_rd) m |= POLLIN | POLLRDNORM;
    if (s->shut_rd && s->shut_wr) m |= POLLHUP;
    return m | POLLOUT | POLLWRNORM;
}

void sock_changed(struct sock *s) {
    __atomic_store_n(&s->pollmask, sock_poll_mask(s), __ATOMIC_RELEASE);
    poll_notify();
}

/* classic BPF (SO_ATTACH_FILTER), enough for udhcpc and tcpdump-style filters */
static uint32_t bpf_run(const struct bpf_prog_k *f, const uint8_t *d, size_t len) {
    uint32_t A = 0, X = 0, M[16] = { 0 };
    for (int pc = 0; pc < f->len; pc++) {
        const struct sock_filter_k *i = &f->insns[pc];
        uint32_t k = i->k, off;
        switch (i->code) {
        case 0x00: A = k; break;                                              /* ld #k */
        case 0x01: X = k; break;                                              /* ldx #k */
        case 0x20: off = k; goto ldw;                                         /* ld [k] */
        case 0x28: off = k; goto ldh;
        case 0x30: off = k; goto ldb;
        case 0x40: off = X + k; goto ldw;                                     /* ld [x+k] */
        case 0x48: off = X + k; goto ldh;
        case 0x50: off = X + k; goto ldb;
        ldw: if (off + 4 > len) return 0; A = (uint32_t)d[off] << 24 | d[off + 1] << 16 | d[off + 2] << 8 | d[off + 3]; break;
        ldh: if (off + 2 > len) return 0; A = (uint32_t)(d[off] << 8 | d[off + 1]); break;
        ldb: if (off + 1 > len) return 0; A = d[off]; break;
        case 0x80: A = (uint32_t)len; break;                                  /* ld len */
        case 0x81: X = (uint32_t)len; break;
        case 0xb1: if (k >= len) return 0; X = (uint32_t)(d[k] & 0xf) * 4; break;  /* ldx 4*([k]&0xf) */
        case 0x60: A = M[k & 15]; break;
        case 0x61: X = M[k & 15]; break;
        case 0x02: M[k & 15] = A; break;
        case 0x03: M[k & 15] = X; break;
        case 0x04: A += k; break; case 0x0c: A += X; break;
        case 0x14: A -= k; break; case 0x1c: A -= X; break;
        case 0x24: A *= k; break; case 0x2c: A *= X; break;
        case 0x34: if (!k) return 0; A /= k; break; case 0x3c: if (!X) return 0; A /= X; break;
        case 0x44: A |= k; break; case 0x4c: A |= X; break;
        case 0x54: A &= k; break; case 0x5c: A &= X; break;
        case 0x64: A <<= (k & 31); break; case 0x6c: A <<= (X & 31); break;
        case 0x74: A >>= (k & 31); break; case 0x7c: A >>= (X & 31); break;
        case 0x84: A = -A; break;
        case 0x05: pc += (int)k; break;                                       /* ja */
        case 0x15: pc += A == k ? i->jt : i->jf; break;
        case 0x1d: pc += A == X ? i->jt : i->jf; break;
        case 0x25: pc += A > k ? i->jt : i->jf; break;
        case 0x2d: pc += A > X ? i->jt : i->jf; break;
        case 0x35: pc += A >= k ? i->jt : i->jf; break;
        case 0x3d: pc += A >= X ? i->jt : i->jf; break;
        case 0x45: pc += (A & k) ? i->jt : i->jf; break;
        case 0x4d: pc += (A & X) ? i->jt : i->jf; break;
        case 0x06: return k;                                                  /* ret #k */
        case 0x16: return A;
        case 0x07: X = A; break;                                              /* tax */
        case 0x87: A = X; break;                                              /* txa */
        default: return 0;
        }
    }
    return 0;
}

void sock_queue_rx(struct sock *s, struct pkt *p) {
    struct bpf_prog_k *f = (struct bpf_prog_k *)s->filter;
    if (f) {
        const uint8_t *d = p->data; size_t len = p->len;
        if (s->family == AF_PACKET && s->type == SOCK_DGRAM && len >= ETH_HLEN) { d += ETH_HLEN; len -= ETH_HLEN; }
        uint32_t keep = bpf_run(f, d, len);
        if (!keep) { pkt_free(p); return; }
    }
    if (s->rxbytes + p->len > (size_t)s->rcvbuf || s->shut_rd) { pkt_free(p); return; }
    list_add_tail(&s->rxq, &p->node);
    s->rxbytes += p->len;
    sock_changed(s);
}

/* ------------------------------------------------------------------ ports */
bool inet_port_in_use(int proto, uint32_t laddr, uint16_t port, struct sock *self, bool reuse) {
    struct list_node *l = proto == IPPROTO_TCP ? &tcp_socks : proto == IPPROTO_UDP ? &udp_socks : &raw_socks;
    list_for_each(it, l) {
        struct sock *o = list_entry(it, struct sock, node);
        if (o == self || o->lport != port) continue;
        if (proto == IPPROTO_ICMP && o->type != SOCK_DGRAM) continue;
        if (o->laddr && laddr && o->laddr != laddr) continue;
        if (proto == IPPROTO_TCP && reuse && o->reuseaddr && o->state != TCP_LISTEN) continue;
        if (proto == IPPROTO_TCP && reuse && o->state == TCP_TIME_WAIT) continue;
        if (proto == IPPROTO_UDP && reuse && o->reuseaddr) continue;
        if (reuse && self && self->reuseport && o->reuseport && o->uid == self->uid) continue;
        return true;
    }
    return false;
}

uint16_t inet_ephemeral_port(int proto, uint32_t laddr) {
    uint32_t lo = 32768, n = 61000 - 32768;
    uint32_t start = (uint32_t)(random_u64() % n);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t port = htons((uint16_t)(lo + (start + i) % n));
        if (!inet_port_in_use(proto, laddr, port, nullptr, false)) return port;
    }
    return 0;
}

static int ipproto_of(struct sock *s) {
    return s->type == SOCK_STREAM ? IPPROTO_TCP : s->type == SOCK_DGRAM && s->protocol != IPPROTO_ICMP ? IPPROTO_UDP : IPPROTO_ICMP;
}
static struct list_node *table_of(struct sock *s) {
    if (s->family == AF_PACKET) return &packet_socks;
    return s->type == SOCK_STREAM ? &tcp_socks : ipproto_of(s) == IPPROTO_UDP ? &udp_socks : &raw_socks;
}

static int autobind(struct sock *s) {
    if (s->bound || (s->type == SOCK_RAW)) return 0;
    uint16_t p = inet_ephemeral_port(ipproto_of(s), s->laddr);
    if (!p) return -EAGAIN;
    s->lport = p;
    s->bound = true;
    link_to(s, table_of(s));
    return 0;
}

/* ------------------------------------------------------------------ helpers */
static int lock_wait(uint64_t deadline) {           /* net_mutex held; dropped while sleeping */
    uint64_t seq = poll_seq_read();
    mutex_unlock(&net_mutex);
    int r = 0;
    uint64_t now = time_ns();
    if (deadline != UINT64_MAX && now >= deadline) r = -ETIMEDOUT;
    else r = poll_wait_seq(seq, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
    mutex_lock(&net_mutex);
    if (r == -ETIMEDOUT) return -EAGAIN;
    return r == -EINTR ? -EINTR : 0;
}
static uint64_t deadline_of(uint64_t timeo) { return timeo ? time_ns() + timeo : UINT64_MAX; }
static bool nonblock(struct file *f, int flags) { return (flags & MSG_DONTWAIT) || (f->flags & O_NONBLOCK); }

static void raise_sigpipe(void) {
    bool took = !bkl_held();
    if (took) bkl_enter();
    signal_send(curproc, SIGPIPE);
    if (took) bkl_exit();
}

static int get_sin(const void *uaddr, int len, struct sockaddr_in_k *a) {
    if (len < (int)sizeof(uint16_t)) return -EINVAL;
    memset(a, 0, sizeof *a);
    if (copy_from_user(a, uaddr, MIN((size_t)len, sizeof *a))) return -EFAULT;
    if (a->family == 0 /* AF_UNSPEC */) return 1;
    if (len < 16) return -EINVAL;
    if (a->family != AF_INET) return -EAFNOSUPPORT;
    return 0;
}
static int put_name(void *uaddr, int *ulen, const void *a, int alen) {
    if (!uaddr || !ulen) return 0;
    int l;
    if (copy_from_user(&l, ulen, sizeof l)) return -EFAULT;
    if (l < 0) return -EINVAL;
    if (copy_to_user(uaddr, a, (size_t)MIN(l, alen)) || copy_to_user(ulen, &alen, sizeof alen)) return -EFAULT;
    return 0;
}

static size_t iov_total(const struct iovec_k *iov, size_t n) {
    size_t t = 0;
    for (size_t i = 0; i < n; i++) t += iov[i].len;
    return t;
}
/* copy between a kernel buffer and the iovec at byte offset off */
static int iov_xfer(const struct iovec_k *iov, size_t niov, size_t off, void *k, size_t n, bool to_user) {
    uint8_t *kb = k;
    for (size_t i = 0; i < niov && n; i++) {
        if (off >= iov[i].len) { off -= iov[i].len; continue; }
        size_t c = MIN(n, iov[i].len - off);
        uint8_t *u = (uint8_t *)iov[i].base + off;
        if (to_user ? copy_to_user(u, kb, c) : copy_from_user(kb, u, c)) return -EFAULT;
        kb += c; n -= c; off = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ files */
static int sock_install(struct sock *s, int flags, struct file **out) {
    struct inode *i = inode_alloc(S_IFSOCK | 0777);
    if (!i) return -ENOMEM;
    i->fops = &inet_fops;
    struct file *f = file_open_inode(i, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
    s->ino = i->ino;
    iput(i);
    if (!f) return -ENOMEM;
    *out = f;
    return 0;
}

int inet_socket(int domain, int type, int proto) {
    int t = type & 0xf;
    if (domain == AF_PACKET) {
        if (t != SOCK_RAW && t != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
        if (!capable(CAP_NET_RAW)) return -EPERM;
    } else if (t == SOCK_STREAM) {
        if (proto && proto != IPPROTO_TCP) return -EPROTONOSUPPORT;
        proto = IPPROTO_TCP;
    } else if (t == SOCK_DGRAM) {
        if (proto && proto != IPPROTO_UDP && proto != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        if (!proto) proto = IPPROTO_UDP;
    } else if (t == SOCK_RAW) {
        if (!capable(CAP_NET_RAW)) return -EPERM;
        if (proto <= 0 || proto > 255) return -EPROTONOSUPPORT;
    } else return -ESOCKTNOSUPPORT;
    struct sock *s = sock_alloc(domain, t, proto);
    if (!s) return -ENOMEM;
    if (t == SOCK_RAW && domain == AF_INET && proto == IPPROTO_RAW) s->hdrincl = true;
    struct file *f;
    int r = sock_install(s, type, &f);
    if (r) { kfree(s); return r; }
    mutex_lock(&net_mutex);
    f->priv = s; s->file = f;
    if (domain == AF_PACKET) { s->pproto = ntohs((uint16_t)proto); link_to(s, &packet_socks); }
    else if (t == SOCK_RAW) link_to(s, &raw_socks);
    sock_changed(s);
    mutex_unlock(&net_mutex);
    int fd = fd_alloc(f, 0, type & SOCK_CLOEXEC);
    if (fd < 0) vfs_close(f);
    return fd;
}

static void inet_release(struct file *f) {
    struct sock *s = f->priv;
    if (!s) return;
    mutex_lock(&net_mutex);
    f->priv = nullptr;
    if (s->type == SOCK_STREAM && s->family == AF_INET) tcp_close(s);
    else sock_free(s);
    mutex_unlock(&net_mutex);
    poll_notify();
}

/* ------------------------------------------------------------------ bind / connect / listen / accept */
int inet_bind(struct file *f, const void *uaddr, int len) {
    struct sock *s = f->priv;
    if (s->family == AF_PACKET) {
        struct sockaddr_ll_k ll;
        if (len < 12) return -EINVAL;
        memset(&ll, 0, sizeof ll);
        if (copy_from_user(&ll, uaddr, MIN((size_t)len, sizeof ll))) return -EFAULT;
        if (ll.family != AF_PACKET) return -EINVAL;
        mutex_lock(&net_mutex);
        int r = 0;
        if (ll.ifindex && !netdev_by_index(ll.ifindex)) r = -ENODEV;
        else {
            s->ifindex = ll.ifindex;
            if (ll.protocol) s->pproto = ntohs(ll.protocol);
            s->bound = true;
            link_to(s, &packet_socks);
        }
        mutex_unlock(&net_mutex);
        return r;
    }
    struct sockaddr_in_k a;
    int r = get_sin(uaddr, len, &a);
    if (r < 0) return r;
    if (r == 1) { if (a.addr) return -EAFNOSUPPORT; a.family = AF_INET; }   /* AF_UNSPEC + ANY: Linux compat */
    if (s->type != SOCK_RAW && a.port && ntohs(a.port) < 1024 && !capable(CAP_NET_BIND_SERVICE)) return -EACCES;
    mutex_lock(&net_mutex);
    if (s->bound && s->type != SOCK_RAW) { r = -EINVAL; goto out; }
    if (s->type == SOCK_STREAM && s->state != TCP_CLOSE) { r = -EINVAL; goto out; }
    if (a.addr && a.addr != INADDR_BROADCAST && !net_is_local_addr(a.addr) && !ipv4_is_multicast(a.addr)) { r = -EADDRNOTAVAIL; goto out; }
    if (s->type == SOCK_RAW) { s->laddr = a.addr; goto out; }
    int proto = ipproto_of(s);
    if (a.port) {
        if (inet_port_in_use(proto, a.addr, a.port, s, s->reuseaddr || s->reuseport)) { r = -EADDRINUSE; goto out; }
        s->lport = a.port;
    } else if (!(s->lport = inet_ephemeral_port(proto, a.addr))) { r = -EADDRINUSE; goto out; }
    s->laddr = a.addr;
    s->bound = true;
    link_to(s, table_of(s));
out:
    mutex_unlock(&net_mutex);
    return r;
}

int inet_connect(struct file *f, const void *uaddr, int len) {
    struct sock *s = f->priv;
    if (s->family == AF_PACKET) return -EOPNOTSUPP;
    struct sockaddr_in_k a;
    int r = get_sin(uaddr, len, &a);
    if (r < 0) return r;
    mutex_lock(&net_mutex);
    if (s->type != SOCK_STREAM) {
        if (r == 1) {                                   /* AF_UNSPEC: dissolve the association */
            s->connected = false; s->raddr = 0; s->rport = 0;
        } else {
            if (s->type == SOCK_DGRAM && ipproto_of(s) == IPPROTO_UDP && !a.port) { r = -EINVAL; goto out; }
            uint32_t dst = a.addr ? a.addr : INADDR_LOOPBACK;
            struct netdev *d; uint32_t nh, src;
            r = ip_route(dst, s->bound_dev, &d, &nh, &src);
            if (r) goto out;
            if (!s->laddr && dst != INADDR_BROADCAST) s->laddr = src;
            s->raddr = dst; s->rport = a.port;
            s->connected = true;
            s->err = 0;
            r = autobind(s);
        }
        sock_changed(s);
        goto out;
    }
    if (r == 1) { r = -EAFNOSUPPORT; goto out; }
    switch (s->state) {
    case TCP_SYN_SENT: r = nonblock(f, 0) ? -EALREADY : 0; if (r) goto out; goto wait;
    case TCP_CLOSE: break;
    case TCP_LISTEN: r = -EINVAL; goto out;
    default: r = -EISCONN; goto out;
    }
    if (s->was_connected) { r = s->err ? -s->err : -EISCONN; s->err = 0; goto out; }
    s->raddr = a.addr ? a.addr : INADDR_LOOPBACK;
    s->rport = a.port;
    if (!a.port) { r = -ECONNREFUSED; goto out; }
    if (!s->bound) {
        struct netdev *d; uint32_t nh, src;
        r = ip_route(s->raddr, s->bound_dev, &d, &nh, &src);
        if (r) goto out;
        uint16_t p = inet_ephemeral_port(IPPROTO_TCP, 0);
        if (!p) { r = -EADDRNOTAVAIL; goto out; }
        s->lport = p;
        s->bound = true;
    }
    unlink_sock(s);                                    /* tcp_connect links it */
    r = tcp_connect(s);
    if (r) { link_to(s, &tcp_socks); goto out; }
    if (nonblock(f, 0)) { r = -EINPROGRESS; goto out; }
wait:
    {
        uint64_t dl = deadline_of(s->sndtimeo);
        while (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECV) {
            r = lock_wait(dl);
            if (r == -EAGAIN) { r = -EINPROGRESS; goto out; }
            if (r) goto out;
        }
        if (s->state == TCP_CLOSE || (s->err && !s->was_connected)) { r = s->err ? -s->err : -ECONNREFUSED; s->err = 0; }
        else r = 0;
    }
out:
    mutex_unlock(&net_mutex);
    return r;
}

int inet_listen(struct file *f, int backlog) {
    struct sock *s = f->priv;
    if (s->type != SOCK_STREAM || s->family != AF_INET) return -EOPNOTSUPP;
    mutex_lock(&net_mutex);
    int r = 0;
    if (s->state != TCP_CLOSE && s->state != TCP_LISTEN) r = -EINVAL;
    else if (s->was_connected) r = -EINVAL;
    else {
        if (!s->bound) {
            s->lport = inet_ephemeral_port(IPPROTO_TCP, 0);
            s->bound = true;
        }
        unlink_sock(s);
        r = tcp_listen(s, backlog > 0 ? MIN(backlog, sysctl_somaxconn) : 1);
        if (r) link_to(s, &tcp_socks);
    }
    mutex_unlock(&net_mutex);
    return r;
}

int inet_accept(struct file *f, void *uaddr, int *ulen, int flags) {
    struct sock *s = f->priv;
    if (s->type != SOCK_STREAM || s->family != AF_INET) return -EOPNOTSUPP;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    struct sock dummy;
    struct file *nf;
    int r = sock_install(&dummy, flags, &nf);
    if (r) return r;
    uint64_t ino = dummy.ino;
    mutex_lock(&net_mutex);
    struct sock *c = nullptr;
    uint64_t dl = deadline_of(s->rcvtimeo);
    for (;;) {
        if (s->state != TCP_LISTEN) { r = -EINVAL; break; }
        list_for_each(it, &s->children) {
            struct sock *x = list_entry(it, struct sock, child_node);
            if (x->accepted_ready) { c = x; break; }
        }
        if (c) break;
        if (nonblock(f, 0)) { r = -EAGAIN; break; }
        r = lock_wait(dl);
        if (r) break;
    }
    struct sockaddr_in_k peer = { AF_INET, 0, 0, { 0 } };
    if (c) {
        list_del(&c->child_node);
        s->nchildren--;
        c->parent = nullptr;
        c->accepted_ready = false;
        c->file = nf; nf->priv = c;
        c->ino = ino;
        peer.port = c->rport; peer.addr = c->raddr;
        sock_changed(c);
        sock_changed(s);
    }
    mutex_unlock(&net_mutex);
    if (!c) { vfs_close(nf); return r; }
    r = put_name(uaddr, ulen, &peer, sizeof peer);
    if (r) { vfs_close(nf); return r; }
    int fd = fd_alloc(nf, 0, flags & SOCK_CLOEXEC);
    if (fd < 0) vfs_close(nf);
    return fd;
}

int inet_getname(struct file *f, void *uaddr, int *ulen, bool peer) {
    struct sock *s = f->priv;
    mutex_lock(&net_mutex);
    if (s->family == AF_PACKET) {
        struct sockaddr_ll_k ll = { AF_PACKET, htons(s->pproto), s->ifindex, ARPHRD_ETHER, 0, 6, { 0 } };
        struct netdev *d = s->ifindex ? netdev_by_index(s->ifindex) : nullptr;
        if (d) { memcpy(ll.addr, d->hwaddr, 6); ll.hatype = (uint16_t)d->type; }
        mutex_unlock(&net_mutex);
        if (peer) return -EOPNOTSUPP;
        return put_name(uaddr, ulen, &ll, 18);
    }
    struct sockaddr_in_k a = { AF_INET, 0, 0, { 0 } };
    int r = 0;
    if (peer) {
        bool conn = s->type == SOCK_STREAM ? (s->state != TCP_CLOSE && s->state != TCP_LISTEN && s->state != TCP_SYN_SENT) : s->connected;
        if (!conn) r = -ENOTCONN;
        a.addr = s->raddr; a.port = s->rport;
    } else { a.addr = s->laddr; a.port = s->type == SOCK_RAW ? 0 : s->lport; }
    mutex_unlock(&net_mutex);
    return r ? r : put_name(uaddr, ulen, &a, sizeof a);
}

int inet_shutdown(struct file *f, int how) {
    struct sock *s = f->priv;
    if (how < 0 || how > 2) return -EINVAL;
    mutex_lock(&net_mutex);
    int r = 0;
    if (s->type == SOCK_STREAM && s->family == AF_INET) {
        if (s->state == TCP_CLOSE || s->state == TCP_SYN_SENT) r = -ENOTCONN;
        if (s->state == TCP_LISTEN) { s->shut_rd = true; tcp_shutdown(s, 2); }
        else if (!r) {
            if (how != 1) s->shut_rd = true;
            if (how != 0 && !s->shut_wr) { s->shut_wr = true; tcp_shutdown(s, how); }
        }
    } else {
        if (!s->connected && s->family == AF_INET) r = -ENOTCONN;
        if (how != 1) s->shut_rd = true;
        if (how != 0) s->shut_wr = true;
    }
    sock_changed(s);
    mutex_unlock(&net_mutex);
    return r;
}

/* ------------------------------------------------------------------ data path */
struct icmphdr_k { uint8_t type, code; uint16_t check, id, seq; };

static int64_t tcp_sendmsg(struct file *f, struct sock *s, const struct iovec_k *iov, size_t niov, int flags) {
    size_t total = iov_total(iov, niov), done = 0;
    bool nb = nonblock(f, flags);
    uint8_t *kb = kmalloc(MIN(total ? total : 1, (size_t)BOUNCE));
    if (!kb) return -ENOMEM;
    int64_t err = 0;
    uint64_t dl = deadline_of(s->sndtimeo);
    mutex_lock(&net_mutex);
    while (done < total || (!total && !done)) {
        size_t chunk = MIN(total - done, (size_t)BOUNCE);
        mutex_unlock(&net_mutex);
        int cr = iov_xfer(iov, niov, done, kb, chunk, false);
        mutex_lock(&net_mutex);
        if (cr) { err = cr; break; }
        size_t off = 0;
        for (;;) {
            if (s->err) { err = -s->err; s->err = 0; break; }
            if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECV) {
                if (nb) { err = -EAGAIN; break; }
                int w = lock_wait(dl);
                if (w) { err = w; break; }
                continue;
            }
            if (s->shut_wr || (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)) {
                err = s->was_connected || s->shut_wr ? -EPIPE : -ENOTCONN;
                break;
            }
            if (off == chunk) break;
            size_t n = ring_put(&s->snd, kb + off, chunk - off);
            if (n) { off += n; tcp_output(s); sock_changed(s); continue; }
            if (nb) { err = -EAGAIN; break; }
            int w = lock_wait(dl);
            if (w) { err = w; break; }
        }
        done += off;
        if (err || !total) break;
    }
    mutex_unlock(&net_mutex);
    kfree(kb);
    if (err == -EPIPE && !(flags & MSG_NOSIGNAL)) raise_sigpipe();
    if (done) return (int64_t)done;
    return err;
}

static int64_t tcp_recvmsg(struct file *f, struct sock *s, const struct iovec_k *iov, size_t niov, int flags) {
    if (flags & MSG_OOB) return -EINVAL;
    size_t total = iov_total(iov, niov), done = 0;
    bool nb = nonblock(f, flags);
    uint8_t *kb = kmalloc(MIN(total ? total : 1, (size_t)BOUNCE));
    if (!kb) return -ENOMEM;
    int64_t err = 0;
    uint64_t dl = deadline_of(s->rcvtimeo);
    mutex_lock(&net_mutex);
    if (s->state == TCP_LISTEN) err = -ENOTCONN;
    else if (s->state == TCP_CLOSE && !s->was_connected && !s->err) err = -ENOTCONN;
    while (!err && done < total) {
        size_t avail = s->rcv.len > ((flags & MSG_PEEK) ? done : 0) ? s->rcv.len - ((flags & MSG_PEEK) ? done : 0) : 0;
        if (avail) {
            size_t n = ring_get(&s->rcv, kb, MIN(total - done, (size_t)BOUNCE), (flags & MSG_PEEK) ? done : 0, !(flags & MSG_PEEK));
            if (!(flags & MSG_PEEK)) tcp_recv_window_update(s);
            sock_changed(s);
            mutex_unlock(&net_mutex);
            int cr = iov_xfer(iov, niov, done, kb, n, true);
            mutex_lock(&net_mutex);
            if (cr) { err = cr; break; }
            done += n;
            continue;
        }
        if (done && !(flags & MSG_WAITALL)) break;
        if (done && (flags & MSG_PEEK)) break;
        if (s->err) { if (!done) { err = -s->err; s->err = 0; } break; }
        if (s->peer_fin || s->shut_rd || s->state == TCP_CLOSE) break;           /* EOF */
        if (nb) { if (!done) err = -EAGAIN; break; }
        int w = lock_wait(dl);
        if (w) { if (!done) err = w; break; }
    }
    mutex_unlock(&net_mutex);
    kfree(kb);
    return done ? (int64_t)done : err;
}

/* fill the source address of a received datagram */
static void dgram_from(struct sock *s, struct pkt *p, struct msghdr_k *m, size_t *alen) {
    if (s->family == AF_PACKET) {
        struct sockaddr_ll_k ll = { AF_PACKET, htons(p->proto), p->dev ? p->dev->index : 0, (uint16_t)(p->dev ? p->dev->type : 1),
                                    p->pkttype, 6, { 0 } };
        memcpy(ll.addr, p->nh + 6, 6);
        *alen = 18;
        if (m->name && m->namelen) copy_to_user(m->name, &ll, MIN((size_t)m->namelen, (size_t)18));
        return;
    }
    struct iphdr *h = (struct iphdr *)p->nh;
    struct sockaddr_in_k a = { AF_INET, 0, h->saddr, { 0 } };
    if (s->type == SOCK_DGRAM && ipproto_of(s) == IPPROTO_UDP) a.port = ((uint16_t *)p->th)[0];
    *alen = sizeof a;
    if (m->name && m->namelen) copy_to_user(m->name, &a, MIN((size_t)m->namelen, sizeof a));
}

static int64_t dgram_recvmsg(struct file *f, struct sock *s, struct msghdr_k *m, int flags) {
    if (flags & MSG_OOB) return -EINVAL;
    bool nb = nonblock(f, flags);
    uint64_t dl = deadline_of(s->rcvtimeo);
    struct pkt *p = nullptr;
    int64_t r = 0;
    mutex_lock(&net_mutex);
    for (;;) {
        if (!list_empty(&s->rxq)) {
            struct pkt *q = list_first(&s->rxq, struct pkt, node);
            if (flags & MSG_PEEK) p = pkt_clone(q);
            else { list_del(&q->node); s->rxbytes -= q->len; p = q; }
            if (!p) r = -ENOMEM;
            sock_changed(s);
            break;
        }
        if (s->err) { r = -s->err; s->err = 0; sock_changed(s); break; }
        if (s->shut_rd) break;
        if (nb) { r = -EAGAIN; break; }
        int w = lock_wait(dl);
        if (w) { r = w; break; }
    }
    mutex_unlock(&net_mutex);
    if (!p) { m->namelen = 0; m->controllen = 0; return r; }
    const uint8_t *d = p->data; size_t len = p->len;
    if (s->family == AF_PACKET && s->type == SOCK_DGRAM) { d += ETH_HLEN; len -= ETH_HLEN; }
    size_t total = iov_total(m->iov, m->iovlen), n = MIN(total, len);
    r = iov_xfer(m->iov, m->iovlen, 0, (void *)d, n, true);
    size_t alen = 0;
    if (!r) {
        dgram_from(s, p, m, &alen);
        m->namelen = (uint32_t)alen;
        m->controllen = 0;
        m->flags = n < len ? MSG_TRUNC : 0;
        r = (flags & MSG_TRUNC) ? (int64_t)len : (int64_t)n;
    }
    pkt_free(p);
    return r;
}

static int64_t dgram_sendmsg(struct file *f, struct sock *s, struct msghdr_k *m, int flags) {
    size_t len = iov_total(m->iov, m->iovlen);
    struct sockaddr_in_k to = { 0 };
    struct sockaddr_ll_k ll = { 0 };
    bool have_to = false;
    if (m->name && m->namelen) {
        if (s->family == AF_PACKET) {
            if (m->namelen < 12) return -EINVAL;
            if (copy_from_user(&ll, m->name, MIN((size_t)m->namelen, sizeof ll))) return -EFAULT;
        } else {
            int r = get_sin(m->name, (int)m->namelen, &to);
            if (r < 0) return r;
            if (r == 0) have_to = true;
        }
    }
    if (len > 65535) return -EMSGSIZE;
    uint8_t *kb = kmalloc(len ? len : 1);
    if (!kb) return -ENOMEM;
    int64_t r = iov_xfer(m->iov, m->iovlen, 0, kb, len, false);
    if (r) { kfree(kb); return r; }
    mutex_lock(&net_mutex);
    if (s->shut_wr) { r = -EPIPE; goto out; }
    if (s->family == AF_PACKET) {
        int ifi = ll.ifindex ? ll.ifindex : s->ifindex;
        struct netdev *d = ifi ? netdev_by_index(ifi) : nullptr;
        if (!d) { r = -ENXIO; goto out; }
        if (!(d->flags & IFF_UP)) { r = -ENETDOWN; goto out; }
        size_t hdr = s->type == SOCK_DGRAM ? ETH_HLEN : 0;
        if (len + hdr > (size_t)d->mtu + ETH_HLEN) { r = -EMSGSIZE; goto out; }
        if (s->type == SOCK_RAW && len < ETH_HLEN) { r = -EINVAL; goto out; }
        struct pkt *p = pkt_alloc(len + hdr);
        if (!p) { r = -ENOBUFS; goto out; }
        p->len = len + hdr;
        if (hdr) {
            uint16_t proto = ll.protocol ? ntohs(ll.protocol) : s->pproto;
            static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            memcpy(p->data, m->name && ll.halen ? ll.addr : bc, 6);
            memcpy(p->data + 6, d->hwaddr, 6);
            p->data[12] = (uint8_t)(proto >> 8); p->data[13] = (uint8_t)proto;
        }
        memcpy(p->data + hdr, kb, len);
        eth_xmit_raw(d, p);
        r = (int64_t)len;
        goto out;
    }
    uint32_t daddr; uint16_t dport;
    if (have_to) { daddr = to.addr ? to.addr : INADDR_LOOPBACK; dport = to.port; }
    else if (s->connected) { daddr = s->raddr; dport = s->rport; }
    else { r = -EDESTADDRREQ; goto out; }
    if (s->connected && s->err) { r = -s->err; s->err = 0; sock_changed(s); goto out; }
    if (s->type == SOCK_DGRAM && ipproto_of(s) == IPPROTO_UDP) {
        if (!dport) { r = -EINVAL; goto out; }
        if ((r = autobind(s))) goto out;
        r = udp_send(s, kb, len, daddr, dport);
        goto out;
    }
    struct ip_opts o = { (uint8_t)s->ttl, (uint8_t)s->tos, false, s->bound_dev };
    if (s->type == SOCK_DGRAM) {                          /* ICMP ping socket */
        if (len < sizeof(struct icmphdr_k)) { r = -EINVAL; goto out; }
        struct icmphdr_k *ic = (struct icmphdr_k *)kb;
        if (ic->type != 8 || ic->code) { r = -EINVAL; goto out; }
        if ((r = autobind(s))) goto out;
        ic->id = s->lport;
        ic->check = 0;
        ic->check = htons(csum_fold(csum_partial(kb, len, 0)));
    } else if (s->hdrincl) {
        struct pkt *p = pkt_alloc(len);
        if (!p) { r = -ENOBUFS; goto out; }
        memcpy(p->data, kb, len); p->len = len;
        r = ip_output_hdrincl(p, s->bound_dev);
        if (!r) r = (int64_t)len;
        goto out;
    }
    if (ipv4_is_multicast(daddr) == false && daddr == INADDR_BROADCAST && !s->broadcast) { r = -EACCES; goto out; }
    struct pkt *p = pkt_alloc(len);
    if (!p) { r = -ENOBUFS; goto out; }
    memcpy(p->data, kb, len); p->len = len;
    uint32_t src = s->laddr && !ipv4_is_multicast(s->laddr) ? s->laddr : 0;
    r = ip_output(p, src, daddr, (uint8_t)(s->type == SOCK_DGRAM ? IPPROTO_ICMP : s->protocol), &o);
    if (!r) r = (int64_t)len;
out:
    mutex_unlock(&net_mutex);
    kfree(kb);
    if (r == -EPIPE && !(flags & MSG_NOSIGNAL)) raise_sigpipe();
    return r;
}

int64_t inet_sendmsg(struct file *f, struct msghdr_k *m, int flags) {
    struct sock *s = f->priv;
    if (s->type == SOCK_STREAM && s->family == AF_INET) return tcp_sendmsg(f, s, m->iov, m->iovlen, flags);
    return dgram_sendmsg(f, s, m, flags);
}
int64_t inet_recvmsg(struct file *f, struct msghdr_k *m, int flags) {
    struct sock *s = f->priv;
    if (s->type == SOCK_STREAM && s->family == AF_INET) {
        int64_t r = tcp_recvmsg(f, s, m->iov, m->iovlen, flags);
        if (r >= 0 && m->name && m->namelen) {           /* recvfrom on TCP: the peer */
            struct sockaddr_in_k a = { AF_INET, s->rport, s->raddr, { 0 } };
            copy_to_user(m->name, &a, MIN((size_t)m->namelen, sizeof a));
            m->namelen = sizeof a;
        } else m->namelen = 0;
        m->controllen = 0;
        m->flags = 0;
        return r;
    }
    return dgram_recvmsg(f, s, m, flags);
}

static ssize_t inet_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct iovec_k iov = { buf, n };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    return inet_recvmsg(f, &m, 0);
}
static ssize_t inet_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct iovec_k iov = { (void *)buf, n };
    struct msghdr_k m = { .iov = &iov, .iovlen = 1 };
    return inet_sendmsg(f, &m, 0);
}
static unsigned inet_poll(struct file *f) {
    struct sock *s = f->priv;
    return s ? __atomic_load_n(&s->pollmask, __ATOMIC_ACQUIRE) : POLLNVAL;
}

/* ------------------------------------------------------------------ options */
#define SOL_SOCKET 1
#define SOL_IP 0
#define SOL_TCP 6
#define SOL_RAW 255
#define SOL_PACKET 263

int inet_setsockopt(struct file *f, int level, int name, const void *uval, int len) {
    struct sock *s = f->priv;
    int v = 0;
    uint8_t raw[16] = { 0 };
    if (len < 0) return -EINVAL;
    if (uval && len && copy_from_user(raw, uval, MIN((size_t)len, sizeof raw))) return -EFAULT;
    if (len >= 4) memcpy(&v, raw, 4);
    else if (len >= 1) v = raw[0];
    int r = 0;
    if (level == SOL_SOCKET && name == 26) {             /* SO_ATTACH_FILTER: struct sock_fprog */
        struct { uint16_t len; uint16_t pad[3]; void *filter; } fp;
        if (len < (int)sizeof fp || copy_from_user(&fp, uval, sizeof fp)) return -EFAULT;
        if (!fp.len || fp.len > 4096) return -EINVAL;
        struct bpf_prog_k *bp = kmalloc(sizeof *bp);
        struct sock_filter_k *ins = kmalloc(fp.len * sizeof *ins);
        if (!bp || !ins) { kfree(bp); kfree(ins); return -ENOMEM; }
        if (copy_from_user(ins, fp.filter, fp.len * sizeof *ins)) { kfree(bp); kfree(ins); return -EFAULT; }
        bp->len = fp.len; bp->insns = ins;
        mutex_lock(&net_mutex);
        struct bpf_prog_k *old = s->filter;
        s->filter = bp;
        mutex_unlock(&net_mutex);
        if (old) { kfree(old->insns); kfree(old); }
        return 0;
    }
    char ifname[17] = { 0 };
    if (level == SOL_SOCKET && name == 25) {             /* SO_BINDTODEVICE */
        if (!capable(CAP_NET_RAW)) return -EPERM;
        if (len > 16) len = 16;
        if (len && copy_from_user(ifname, uval, (size_t)len)) return -EFAULT;
    }
    mutex_lock(&net_mutex);
    if (level == SOL_SOCKET) {
        switch (name) {
        case 2: s->reuseaddr = v; break;
        case 15: s->reuseport = v; break;
        case 6: s->broadcast = v; break;
        case 9: s->keepalive = v; if (s->type == SOCK_STREAM) tcp_keepalive_changed(s); break;
        case 7: case 32:                                 /* SO_SNDBUF(FORCE): Linux doubles the value */
            v = v < 1024 ? 2048 : v > (1 << 22) ? (1 << 23) : v * 2;
            s->sndbuf = v;
            break;
        case 8: case 33:
            v = v < 1024 ? 2048 : v > (1 << 22) ? (1 << 23) : v * 2;
            s->rcvbuf = v;
            break;
        case 13:                                         /* SO_LINGER {int onoff, linger} */
            if (len < 8) { r = -EINVAL; break; }
            s->linger_on = v != 0;
            memcpy(&s->linger_s, raw + 4, 4);
            break;
        case 20: case 21: {                              /* SO_RCVTIMEO / SO_SNDTIMEO: struct timeval */
            if (len < 16) { r = -EINVAL; break; }
            int64_t sec, usec;
            memcpy(&sec, raw, 8); memcpy(&usec, raw + 8, 8);
            if (usec < 0 || usec >= 1000000) { r = -EDOM; break; }
            uint64_t ns = sec < 0 ? 0 : (uint64_t)sec * NS_S + (uint64_t)usec * 1000;
            if (name == 20) s->rcvtimeo = ns; else s->sndtimeo = ns;
            break;
        }
        case 25:
            if (!ifname[0]) s->bound_dev = 0;
            else { struct netdev *d = netdev_by_name(ifname); if (!d) r = -ENODEV; else s->bound_dev = d->index; }
            break;
        case 27: break;                                  /* SO_DETACH_FILTER */
        case 12: s->priority = v; break;
        case 36: s->mark = v; break;
        case 1: case 5: case 10: case 11: case 16: case 18: case 19: case 29: case 35: break;   /* accepted, no effect */
        default: r = -ENOPROTOOPT;
        }
    } else if (level == SOL_IP && s->family == AF_INET) {
        switch (name) {
        case 1: s->tos = v & 0xff; break;                /* IP_TOS */
        case 2: if (v == -1) v = sysctl_ip_default_ttl; if (v < 1 || v > 255) r = -EINVAL; else s->ttl = v; break;
        case 3: s->hdrincl = v; break;
        case 8: s->pktinfo = v; break;
        case 11: s->recverr = v; break;
        case 10: case 4: case 33: case 34: case 35: case 36: case 32: case 15: break;   /* MTU_DISCOVER, OPTIONS, multicast */
        default: r = -ENOPROTOOPT;
        }
    } else if (level == SOL_TCP && s->type == SOCK_STREAM) {
        switch (name) {
        case 1: s->nodelay = v; if (v) tcp_output(s); break;
        case 2: if (v < 88 || v > 65495) r = -EINVAL; else if (!s->snd_mss || (uint32_t)v < s->snd_mss) s->snd_mss = (uint32_t)v; break;
        case 3: s->cork = v; if (!v) tcp_output(s); break;
        case 4: if (v < 1 || v > 32767) r = -EINVAL; else { s->keepidle = (unsigned)v; tcp_keepalive_changed(s); } break;
        case 5: if (v < 1 || v > 32767) r = -EINVAL; else s->keepintvl = (unsigned)v; break;
        case 6: if (v < 1 || v > 127) r = -EINVAL; else s->keepcnt = (unsigned)v; break;
        case 12: case 18: case 13: case 9: case 10: break;  /* QUICKACK, USER_TIMEOUT, CONGESTION, DEFER_ACCEPT, WINDOW_CLAMP */
        default: r = -ENOPROTOOPT;
        }
    } else if (level == SOL_RAW && s->type == SOCK_RAW && s->protocol == IPPROTO_ICMP && name == 1) {
        s->icmp_filter = (uint32_t)v;
    } else if (level == SOL_PACKET && s->family == AF_PACKET) {
        r = name == 1 || name == 2 ? 0 : -ENOPROTOOPT;   /* (DROP_)MEMBERSHIP accepted; no AUXDATA */
    } else r = -ENOPROTOOPT;
    mutex_unlock(&net_mutex);
    return r;
}

int inet_getsockopt(struct file *f, int level, int name, void *uval, int *ulen) {
    struct sock *s = f->priv;
    int len;
    if (copy_from_user(&len, ulen, sizeof len)) return -EFAULT;
    if (len < 0) return -EINVAL;
    uint8_t out[104] = { 0 };
    int olen = 4, v = 0, r = 0;
    mutex_lock(&net_mutex);
    if (level == SOL_SOCKET) {
        switch (name) {
        case 3: v = s->type; break;
        case 4: v = s->err; s->err = 0; sock_changed(s); break;
        case 2: v = s->reuseaddr; break;
        case 15: v = s->reuseport; break;
        case 6: v = s->broadcast; break;
        case 9: v = s->keepalive; break;
        case 7: v = s->sndbuf; break;
        case 8: v = s->rcvbuf; break;
        case 30: v = s->state == TCP_LISTEN && s->type == SOCK_STREAM; break;
        case 38: v = s->family == AF_PACKET ? htons(s->pproto) : s->protocol; break;
        case 39: v = s->family; break;
        case 12: v = s->priority; break;
        case 36: v = s->mark; break;
        case 10: case 11: case 16: v = 0; break;
        case 18: case 19: v = 1; break;
        case 13: olen = 8; memcpy(out + 4, &s->linger_s, 4); v = s->linger_on; break;
        case 20: case 21: {
            uint64_t ns = name == 20 ? s->rcvtimeo : s->sndtimeo;
            int64_t tv[2] = { (int64_t)(ns / NS_S), (int64_t)(ns % NS_S / 1000) };
            memcpy(out, tv, 16); olen = 16;
            break;
        }
        case 17: {                                     /* SO_PEERCRED: unknown for inet */
            int32_t c[3] = { 0, -1, -1 };
            memcpy(out, c, 12); olen = 12;
            break;
        }
        case 25: {
            struct netdev *d = s->bound_dev ? netdev_by_index(s->bound_dev) : nullptr;
            olen = 0;
            if (d) { olen = (int)strlen(d->name) + 1; memcpy(out, d->name, (size_t)olen); }
            break;
        }
        default: r = -ENOPROTOOPT;
        }
    } else if (level == SOL_IP && s->family == AF_INET) {
        switch (name) {
        case 1: v = s->tos; break;
        case 2: v = s->ttl; break;
        case 3: v = s->hdrincl; break;
        case 8: v = s->pktinfo; break;
        case 11: v = s->recverr; break;
        case 14: {                                     /* IP_MTU */
            struct netdev *d; uint32_t nh;
            if (!s->raddr || ip_route(s->raddr, s->bound_dev, &d, &nh, nullptr)) r = -ENOTCONN;
            else v = d->mtu;
            break;
        }
        case 10: v = 0; break;
        default: r = -ENOPROTOOPT;
        }
    } else if (level == SOL_TCP && s->type == SOCK_STREAM) {
        switch (name) {
        case 1: v = s->nodelay; break;
        case 2: v = (int)(s->snd_mss ? s->snd_mss : 536); break;
        case 3: v = s->cork; break;
        case 4: v = (int)s->keepidle; break;
        case 5: v = (int)s->keepintvl; break;
        case 6: v = (int)s->keepcnt; break;
        case 11: {                                     /* TCP_INFO: the leading fields */
            out[0] = (uint8_t)s->state;
            out[1] = 0;                                /* ca_state */
            out[2] = (uint8_t)s->retries;
            out[5] = (uint8_t)((s->ts_ok ? 1 : 0) | (s->sack_ok ? 2 : 0) | (s->ws_ok ? 4 : 0));   /* TCPI_OPT_* */
            out[6] = (uint8_t)((s->snd_wscale & 15) | (s->rcv_wscale << 4));
            uint32_t w[8] = { (uint32_t)(s->rto_ms * 1000), 0, s->snd_mss, s->rcv_mss, 0, 0, 0, 0 };
            memcpy(out + 8, w, sizeof w);
            uint32_t rtt[2] = { (uint32_t)s->srtt_us, (uint32_t)s->rttvar_us };
            memcpy(out + 68, rtt, 8);
            uint32_t cw[2] = { s->ssthresh, s->snd_mss ? s->cwnd / s->snd_mss : 0 };
            memcpy(out + 76, cw, 8);
            olen = 104;
            break;
        }
        case 13: memcpy(out, "reno", 5); olen = 5; break;
        default: r = -ENOPROTOOPT;
        }
    } else if (level == SOL_RAW && s->type == SOCK_RAW && name == 1) {
        v = (int)s->icmp_filter;
    } else r = -ENOPROTOOPT;
    mutex_unlock(&net_mutex);
    if (r) return r;
    if (olen == 4) memcpy(out, &v, 4);
    else if (olen == 8 && name == 13 && level == SOL_SOCKET) memcpy(out, &v, 4);
    int n = MIN(len, olen);
    if (copy_to_user(uval, out, (size_t)n) || copy_to_user(ulen, &n, sizeof n)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ ioctls */
#define SIOCADDRT 0x890B
#define SIOCDELRT 0x890C
#define SIOCGIFNAME 0x8910
#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFADDR 0x8915
#define SIOCSIFADDR 0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCSIFDSTADDR 0x8918
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMETRIC 0x891d
#define SIOCSIFMETRIC 0x891e
#define SIOCGIFMTU 0x8921
#define SIOCSIFMTU 0x8922
#define SIOCSIFHWADDR 0x8924
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFINDEX 0x8933
#define SIOCGIFTXQLEN 0x8942
#define SIOCSIFTXQLEN 0x8943
#define SIOCGIFMAP 0x8970
#define SIOCSIFMAP 0x8971
#define SIOCDARP 0x8953
#define SIOCGARP 0x8954
#define SIOCSARP 0x8955

struct ifreq_k {
    char name[16];
    union {
        struct sockaddr_in_k addr;
        struct { uint16_t family; uint8_t data[14]; } hw;
        int16_t flags;
        int32_t ival;
        uint8_t map[24];
        char newname[16];
        void *data;
    };
};
_Static_assert(sizeof(struct ifreq_k) == 40, "struct ifreq");

static uint32_t classful_mask(uint32_t a) {
    uint32_t h = ntohl(a);
    return htonl(h >> 31 == 0 ? 0xff000000u : h >> 30 == 2 ? 0xffff0000u : 0xffffff00u);
}

static void dev_addr_changed(struct netdev *d) {
    route_add_connected(d);
    poll_notify();
}

static int ifconf(void *arg) {
    struct { int32_t len; int32_t pad; void *buf; } ic;
    if (copy_from_user(&ic, arg, sizeof ic)) return -EFAULT;
    int n = 0, r = 0;
    mutex_lock(&net_mutex);
    list_for_each(it, &netdevs) {
        struct netdev *d = list_entry(it, struct netdev, node);
        if (!d->addr) continue;
        if (ic.buf) {
            if ((n + 1) * (int)sizeof(struct ifreq_k) > ic.len) break;
            struct ifreq_k q;
            memset(&q, 0, sizeof q);
            strncpy(q.name, d->name, 15);
            q.addr = (struct sockaddr_in_k){ AF_INET, 0, d->addr, { 0 } };
            if (copy_to_user((uint8_t *)ic.buf + n * sizeof q, &q, sizeof q)) { r = -EFAULT; break; }
        }
        n++;
    }
    mutex_unlock(&net_mutex);
    if (r) return r;
    ic.len = n * (int)sizeof(struct ifreq_k);
    return copy_to_user(arg, &ic, sizeof ic) ? -EFAULT : 0;
}

static int dev_ioctl(uint64_t cmd, void *arg) {
    if (cmd == SIOCGIFCONF) return ifconf(arg);
    struct ifreq_k q;
    if (copy_from_user(&q, arg, sizeof q)) return -EFAULT;
    q.name[15] = 0;
    bool set = cmd == SIOCSIFFLAGS || cmd == SIOCSIFADDR || cmd == SIOCSIFDSTADDR || cmd == SIOCSIFBRDADDR ||
               cmd == SIOCSIFNETMASK || cmd == SIOCSIFMETRIC || cmd == SIOCSIFMTU || cmd == SIOCSIFHWADDR ||
               cmd == SIOCSIFTXQLEN || cmd == SIOCSIFMAP;
    if (set && !capable(CAP_NET_ADMIN)) return -EPERM;
    mutex_lock(&net_mutex);
    int r = 0;
    struct netdev *d = cmd == SIOCGIFNAME ? netdev_by_index(q.ival) : netdev_by_name(q.name);
    if (!d) { mutex_unlock(&net_mutex); return -ENODEV; }
    bool out = !set;
    switch (cmd) {
    case SIOCGIFNAME: memset(q.name, 0, 16); strncpy(q.name, d->name, 15); break;
    case SIOCGIFINDEX: q.ival = d->index; break;
    case SIOCGIFFLAGS: q.flags = (int16_t)d->flags; break;
    case SIOCSIFFLAGS: {
        unsigned nf = (unsigned)(uint16_t)q.flags, old = d->flags;
        unsigned keep = IFF_LOOPBACK | IFF_BROADCAST | IFF_RUNNING | IFF_NOARP;
        d->flags = (old & keep) | (nf & ~keep);
        if (d->type == ARPHRD_LOOPBACK) d->flags |= IFF_NOARP;
        if (nf & IFF_UP) d->flags |= IFF_RUNNING; else d->flags &= ~IFF_RUNNING;
        if ((old ^ d->flags) & IFF_UP) {
            if (d->flags & IFF_UP) dev_addr_changed(d);
            else { route_flush_dev(d); arp_flush_dev(d); }
        }
        break;
    }
    case SIOCGIFADDR: case SIOCGIFDSTADDR:
        if (!d->addr) { r = -EADDRNOTAVAIL; break; }
        q.addr = (struct sockaddr_in_k){ AF_INET, 0, d->addr, { 0 } };
        break;
    case SIOCGIFBRDADDR:
        if (!d->addr) { r = -EADDRNOTAVAIL; break; }
        q.addr = (struct sockaddr_in_k){ AF_INET, 0, d->bcast, { 0 } };
        break;
    case SIOCGIFNETMASK:
        if (!d->addr) { r = -EADDRNOTAVAIL; break; }
        q.addr = (struct sockaddr_in_k){ AF_INET, 0, d->netmask, { 0 } };
        break;
    case SIOCSIFADDR:
        if (q.addr.family != AF_INET) { r = -EINVAL; break; }
        if (d->addr != q.addr.addr) arp_flush_dev(d);
        d->addr = q.addr.addr;
        d->netmask = d->type == ARPHRD_LOOPBACK ? htonl(0xff000000u) : q.addr.addr ? classful_mask(q.addr.addr) : 0;
        d->bcast = d->type == ARPHRD_LOOPBACK ? 0 : d->addr | ~d->netmask;
        if (!d->addr) route_flush_dev(d);
        dev_addr_changed(d);
        break;
    case SIOCSIFNETMASK:
        if (!d->addr) { r = -EADDRNOTAVAIL; break; }
        d->netmask = q.addr.addr;
        if (d->type != ARPHRD_LOOPBACK) d->bcast = d->addr | ~d->netmask;
        dev_addr_changed(d);
        break;
    case SIOCSIFBRDADDR: d->bcast = q.addr.addr; break;
    case SIOCSIFDSTADDR: break;
    case SIOCGIFMETRIC: q.ival = 0; break;
    case SIOCSIFMETRIC: break;
    case SIOCGIFMTU: q.ival = d->mtu; break;
    case SIOCSIFMTU:
        if (q.ival < 68 || q.ival > (d->type == ARPHRD_LOOPBACK ? 65536 : 1500)) r = -EINVAL;
        else d->mtu = q.ival;
        break;
    case SIOCGIFHWADDR:
        memset(&q.hw, 0, sizeof q.hw);
        q.hw.family = (uint16_t)d->type;
        memcpy(q.hw.data, d->hwaddr, 6);
        break;
    case SIOCSIFHWADDR: r = -EOPNOTSUPP; break;
    case SIOCGIFTXQLEN: q.ival = d->txqlen; break;
    case SIOCSIFTXQLEN: if (q.ival < 0) r = -EINVAL; else d->txqlen = q.ival; break;
    case SIOCGIFMAP: memset(q.map, 0, sizeof q.map); break;
    case SIOCSIFMAP: break;
    default: r = -ENOTTY;
    }
    mutex_unlock(&net_mutex);
    if (!r && out && copy_to_user(arg, &q, sizeof q)) r = -EFAULT;
    return r;
}

/* struct rtentry (LP64) */
struct rtentry_k {
    uint64_t pad1;
    struct sockaddr_in_k dst, gw, mask;
    uint16_t flags; int16_t pad2; uint64_t pad3; void *pad4;
    int16_t metric; char *dev;
    uint64_t mtu, window; uint16_t irtt;
};
_Static_assert(__builtin_offsetof(struct rtentry_k, dev) == 88, "struct rtentry");

static int rt_ioctl(uint64_t cmd, void *arg) {
    if (!capable(CAP_NET_ADMIN)) return -EPERM;
    struct rtentry_k rt;
    if (copy_from_user(&rt, arg, sizeof rt)) return -EFAULT;
    char devname[16] = { 0 };
    if (rt.dev && strncpy_from_user(devname, rt.dev, sizeof devname - 1) < 0) return -EFAULT;
    uint32_t mask = (rt.flags & RTF_HOST) ? 0xffffffffu : rt.mask.addr;
    uint32_t dst = rt.dst.addr, gw = (rt.flags & RTF_GATEWAY) ? rt.gw.addr : 0;
    int metric = rt.metric > 0 ? rt.metric - 1 : 0;
    if (dst & ~mask) return -EINVAL;
    mutex_lock(&net_mutex);
    struct netdev *d = devname[0] ? netdev_by_name(devname) : nullptr;
    int r = 0;
    if (devname[0] && !d) r = -ENODEV;
    else if (cmd == SIOCADDRT) {
        if (!d) {                                          /* the device whose subnet holds the gateway/dst */
            uint32_t via = gw ? gw : dst;
            list_for_each(it, &netdevs) {
                struct netdev *x = list_entry(it, struct netdev, node);
                if (x->addr && (x->flags & IFF_UP) && ((x->addr ^ via) & x->netmask) == 0) { d = x; break; }
            }
        }
        if (!d) r = -ENETUNREACH;
        else r = route_add(dst, mask, gw, d, metric, rt.flags & (RTF_GATEWAY | RTF_HOST));
    } else r = route_del(dst, mask, gw, d, metric);
    mutex_unlock(&net_mutex);
    return r;
}

/* interface, route and ARP ioctls: on any socket (musl's if_nametoindex uses an AF_UNIX one) */
int net_if_ioctl(uint64_t cmd, void *uarg) {
    switch (cmd) {
    case SIOCADDRT: case SIOCDELRT: return rt_ioctl(cmd, uarg);
    case SIOCDARP: case SIOCGARP: case SIOCSARP: {
        if (cmd != SIOCGARP && !capable(CAP_NET_ADMIN)) return -EPERM;
        uint8_t req[68];
        if (copy_from_user(req, uarg, sizeof req)) return -EFAULT;
        mutex_lock(&net_mutex);
        int r = arp_ioctl(cmd, req);
        mutex_unlock(&net_mutex);
        if (!r && cmd == SIOCGARP && copy_to_user(uarg, req, sizeof req)) r = -EFAULT;
        return r;
    }
    default:
        if (cmd >= 0x8910 && cmd <= 0x8980) return dev_ioctl(cmd, uarg);
        return -ENOTTY;
    }
}

static int inet_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct sock *s = f->priv;
    void *uarg = (void *)arg;
    int v;
    switch (cmd) {
    case 0x541B:                                            /* FIONREAD / SIOCINQ */
        mutex_lock(&net_mutex);
        if (s->type == SOCK_STREAM && s->family == AF_INET) v = s->state == TCP_LISTEN ? -1 : (int)s->rcv.len;
        else if (list_empty(&s->rxq)) v = 0;
        else {
            struct pkt *p = list_first(&s->rxq, struct pkt, node);
            v = (int)p->len - (s->family == AF_PACKET && s->type == SOCK_DGRAM ? ETH_HLEN : 0);
        }
        mutex_unlock(&net_mutex);
        if (v < 0) return -EINVAL;
        return copy_to_user(uarg, &v, sizeof v) ? -EFAULT : 0;
    case 0x5411:                                            /* TIOCOUTQ / SIOCOUTQ */
        mutex_lock(&net_mutex);
        v = s->type == SOCK_STREAM ? (int)s->snd.len : 0;
        mutex_unlock(&net_mutex);
        return copy_to_user(uarg, &v, sizeof v) ? -EFAULT : 0;
    case 0x8905: v = 0; return copy_to_user(uarg, &v, sizeof v) ? -EFAULT : 0;   /* SIOCATMARK */
    default: return net_if_ioctl(cmd, uarg);
    }
}

const struct file_ops inet_fops = { .nobkl = true, .read = inet_read, .write = inet_write, .poll = inet_poll,
                                    .ioctl = inet_ioctl, .release = inet_release };
