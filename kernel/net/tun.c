/*
 * M32b: TUN/TAP (/dev/net/tun). TUNSETIFF attaches the file to a new interface: IFF_TUN carries
 * IP datagrams (ARPHRD_NONE, point-to-point, no ARP), IFF_TAP ethernet frames. What the stack
 * transmits on the interface is read() from the file; what is write()n is received by the stack.
 * Without IFF_NO_PI every packet has a 4-byte {flags, ethertype} prefix. The interface goes away
 * with the file (no TUNSETPERSIST). The stack itself always works on ethernet frames, so a TUN
 * device gets (and drops) a dummy 14-byte header.
 */
#include <kernel/net.h>
#include <kernel/vfs.h>
#include <kernel/cred.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/printk.h>
#ifndef EBADFD
#define EBADFD 77
#endif

#define IFF_TUN   0x0001
#define IFF_TAP   0x0002
#define IFF_NO_PI 0x1000
#define TUNSETIFF     0x400454caul
#define TUNSETPERSIST 0x400454cbul
#define TUNGETIFF     0x800454d2ul
#define TUN_QMAX 500

struct tun {
    struct netdev *dev;
    unsigned flags;
    struct list_node q;          /* frames transmitted by the stack, waiting for read() */
    int qlen;
};

static void tun_xmit(struct netdev *d, struct pkt *p) {
    struct tun *t = d->priv;
    if (!t || t->qlen >= TUN_QMAX) { d->tx_dropped++; pkt_free(p); return; }
    if (t->flags & IFF_TUN) {                    /* strip the stack's dummy ethernet header */
        p->proto = (uint16_t)(p->data[12] << 8 | p->data[13]);
        pkt_pull(p, ETH_HLEN);
    }
    d->tx_packets++;
    d->tx_bytes += p->len;
    list_add_tail(&t->q, &p->node);
    t->qlen++;
    poll_notify();
}

static int tun_open(struct inode *i, struct file *f) { f->priv = nullptr; return 0; }

static int tun_attach(struct file *f, const char *want, unsigned flags) {
    if (!capable(CAP_NET_ADMIN)) return -EPERM;
    if (!(flags & (IFF_TUN | IFF_TAP)) || (flags & IFF_TUN && flags & IFF_TAP)) return -EINVAL;
    struct tun *t = kzalloc(sizeof *t);
    if (!t) return -ENOMEM;
    t->flags = flags;
    list_init(&t->q);
    const char *pat = flags & IFF_TUN ? "tun%d" : "tap%d";
    char name[16];
    mutex_lock(&net_mutex);
    if (want[0] && !strchr(want, '%')) {
        if (netdev_by_name(want)) { mutex_unlock(&net_mutex); kfree(t); return -EBUSY; }
        strncpy(name, want, 15); name[15] = 0;
    } else {
        if (want[0]) {                           /* a user pattern: exactly one %d */
            const char *pc = strchr(want, '%');
            if (pc[1] != 'd' || strchr(pc + 1, '%')) { mutex_unlock(&net_mutex); kfree(t); return -EINVAL; }
            pat = want;
        }
        for (int n = 0; ; n++) { snprintf(name, sizeof name, pat, n); if (!netdev_by_name(name)) break; }
    }
    mutex_unlock(&net_mutex);
    uint8_t mac[6] = { 0x02, 0x00, 0x00, 0, 0, 0 };
    uint64_t r = random_u64();
    memcpy(mac + 3, &r, 3);
    struct netdev *d = netdev_register(name, flags & IFF_TUN ? ARPHRD_NONE : ARPHRD_ETHER, flags & IFF_TUN ? nullptr : mac,
                                       1500, tun_xmit, t);
    if (!d) { kfree(t); return -ENOMEM; }
    mutex_lock(&net_mutex);
    if (flags & IFF_TUN) d->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
    t->dev = d;
    f->priv = t;
    mutex_unlock(&net_mutex);
    return 0;
}

static int tun_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct { char name[16]; uint16_t flags; uint8_t pad[22]; } ifr;
    switch (cmd) {
    case TUNSETIFF: {
        if (f->priv) return -EINVAL;
        if (copy_from_user(&ifr, (void *)arg, sizeof ifr)) return -EFAULT;
        ifr.name[15] = 0;
        int r = tun_attach(f, ifr.name, ifr.flags);
        if (r) return r;
        struct tun *t = f->priv;
        memcpy(ifr.name, t->dev->name, 16);
        return copy_to_user((void *)arg, &ifr, sizeof ifr) ? -EFAULT : 0;
    }
    case TUNGETIFF: {
        struct tun *t = f->priv;
        if (!t) return -EBADFD;
        memset(&ifr, 0, sizeof ifr);
        memcpy(ifr.name, t->dev->name, 16);
        ifr.flags = (uint16_t)t->flags;
        return copy_to_user((void *)arg, &ifr, sizeof ifr) ? -EFAULT : 0;
    }
    case TUNSETPERSIST: return arg ? -EOPNOTSUPP : 0;
    default: return -ENOTTY;
    }
}

static ssize_t tun_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct tun *t = f->priv;
    if (!t) return -EBADFD;
    mutex_lock(&net_mutex);
    while (list_empty(&t->q)) {
        if (f->flags & O_NONBLOCK) { mutex_unlock(&net_mutex); return -EAGAIN; }
        uint64_t seq = poll_seq_read();
        mutex_unlock(&net_mutex);
        if (poll_wait_seq(seq, UINT64_MAX) == -EINTR) return -EINTR;
        mutex_lock(&net_mutex);
    }
    struct pkt *p = list_first(&t->q, struct pkt, node);
    list_del(&p->node);
    t->qlen--;
    mutex_unlock(&net_mutex);
    size_t pi = t->flags & IFF_NO_PI ? 0 : 4, done = 0;
    int r = 0;
    if (pi) {
        uint8_t h[4] = { 0, 0, 0, 0 };
        uint16_t proto = t->flags & IFF_TUN ? p->proto : ETH_P_ALL;
        if (t->flags & IFF_TUN) { h[2] = (uint8_t)(proto >> 8); h[3] = (uint8_t)proto; }
        if (p->len + 4 > n) h[0] = 1;                                  /* TUN_PKT_STRIP */
        r = copy_to_user(buf, h, MIN(n, (size_t)4));
        done = MIN(n, (size_t)4);
    }
    size_t c = MIN(p->len, n - done);
    if (!r && c) r = copy_to_user((uint8_t *)buf + done, p->data, c);
    pkt_free(p);
    return r ? -EFAULT : (ssize_t)(done + c);
}

static ssize_t tun_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct tun *t = f->priv;
    if (!t) return -EBADFD;
    size_t pi = t->flags & IFF_NO_PI ? 0 : 4;
    if (n < pi + 1 || n > 65535 + pi + ETH_HLEN) return -EINVAL;
    size_t len = n - pi, hl = t->flags & IFF_TUN ? ETH_HLEN : 0;
    struct pkt *p = pkt_alloc_rx(len + hl);
    if (!p) return -ENOMEM;
    p->len = len + hl;
    if (copy_from_user(p->data + hl, (const uint8_t *)buf + pi, len)) { pkt_free(p); return -EFAULT; }
    if (hl) {                                    /* dummy header for the stack */
        memset(p->data, 0, ETH_HLEN);
        uint8_t v = p->data[hl] >> 4;
        uint16_t proto = v == 6 ? 0x86dd : ETH_P_IP;
        p->data[12] = (uint8_t)(proto >> 8); p->data[13] = (uint8_t)proto;
    }
    struct netdev *d = t->dev;
    if (!(d->flags & IFF_UP)) { d->rx_dropped++; pkt_free(p); return -EIO; }
    net_rx(d, p);
    return (ssize_t)n;
}

static unsigned tun_poll(struct file *f) {
    struct tun *t = f->priv;
    unsigned m = POLLOUT | POLLWRNORM;
    if (t && __atomic_load_n(&t->qlen, __ATOMIC_RELAXED)) m |= POLLIN | POLLRDNORM;
    return m;
}

static void tun_release(struct file *f) {
    struct tun *t = f->priv;
    if (!t) return;
    mutex_lock(&net_mutex);
    t->dev->priv = nullptr;
    netdev_unregister(t->dev);
    pkt_queue_purge(&t->q);
    mutex_unlock(&net_mutex);
    kfree(t);
}

static const struct file_ops tun_fops = {
    .nobkl = true, .open = tun_open, .read = tun_read, .write = tun_write, .ioctl = tun_ioctl,
    .poll = tun_poll, .release = tun_release,
};

void chrdev_register(unsigned major, unsigned minor, const struct file_ops *ops);
void tun_init(void) { chrdev_register(10, 200, &tun_fops); }
