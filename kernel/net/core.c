/*
 * M32 network core: packet buffers, the device list, the receive queue and the "net" kernel
 * thread, protocol timers, the loopback device, ethernet demultiplexing, checksums and
 * /proc/net/{dev,route}.
 *
 * Drivers call net_rx() from any context (their IRQ threads): frames go onto rxq under a
 * spinlock and the net thread is woken. The thread takes net_mutex, runs every queued frame
 * through the protocols and fires expired timers. Everything above the drivers therefore runs
 * under net_mutex, in either the net thread or a socket syscall; transmission is synchronous
 * from there into the driver (loopback frames go back onto rxq, which keeps the stack free
 * of recursion).
 */
#include <kernel/net.h>
#include <kernel/net6.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/arch.h>
#include <kernel/time.h>
#include <kernel/errno.h>

static const struct lock_class net_mutex_class = { "net_mutex", LR_MUTEX_NET, false };
struct mutex net_mutex = MUTEX_INIT(net_mutex, &net_mutex_class);
struct list_node netdevs = LIST_INIT(netdevs);
struct netdev *loopback_dev;
static int next_ifindex = 1;

/* ------------------------------------------------------------------ packets */
struct pkt *pkt_alloc(size_t payload) {
    size_t cap = PKT_HEADROOM + payload;
    struct pkt *p = kmalloc(sizeof *p + cap);
    if (!p) return nullptr;
    memset(p, 0, sizeof *p);
    p->cap = cap;
    p->data = p->buf + PKT_HEADROOM;
    list_init(&p->node);
    return p;
}

/* buf is 8-byte aligned; start the frame 2 bytes in so the IP header behind the 14-byte
 * ethernet header is 4-byte aligned (RISC-V traps on misaligned loads) */
struct pkt *pkt_alloc_rx(size_t len) {
    struct pkt *p = kmalloc(sizeof *p + len + 2);
    if (!p) return nullptr;
    memset(p, 0, sizeof *p);
    p->cap = len + 2;
    p->data = p->buf + 2;
    p->len = len;
    list_init(&p->node);
    return p;
}

struct pkt *pkt_clone(const struct pkt *o) {
    struct pkt *p = kmalloc(sizeof *p + o->cap);
    if (!p) return nullptr;
    memcpy(p, o, sizeof *p + o->cap);
    ptrdiff_t d = (uint8_t *)p - (uint8_t *)o;
    p->data += d;
    if (p->nh) p->nh += d;
    if (p->th) p->th += d;
    list_init(&p->node);
    if (p->dev) device_get(&p->dev->kdev);
    return p;
}

/* M34: packets pin their device (a removed TUN/TAP netdev is freed with its last packet) */
void pkt_set_dev(struct pkt *p, struct netdev *d) {
    if (p->dev == d) return;
    if (d) device_get(&d->kdev);
    if (p->dev) device_put(&p->dev->kdev);
    p->dev = d;
}
void pkt_free(struct pkt *p) { if (p->dev) device_put(&p->dev->kdev); kfree(p); }
void pkt_queue_purge(struct list_node *q) {
    list_for_each_safe(it, tmp, q) { list_del(it); pkt_free(list_entry(it, struct pkt, node)); }
}

/* ------------------------------------------------------------------ rings */
bool ring_alloc(struct ring *r, size_t cap) {
    r->buf = kmalloc(cap);
    r->cap = r->buf ? cap : 0;
    r->head = r->len = 0;
    return r->buf != nullptr;
}
void ring_free(struct ring *r) { kfree(r->buf); r->buf = nullptr; r->cap = r->head = r->len = 0; }
size_t ring_space(const struct ring *r) { return r->cap - r->len; }
size_t ring_put(struct ring *r, const uint8_t *src, size_t n) {
    n = MIN(n, ring_space(r));
    size_t tail = (r->head + r->len) % (r->cap ? r->cap : 1);
    size_t a = MIN(n, r->cap - tail);
    memcpy(r->buf + tail, src, a);
    memcpy(r->buf, src + a, n - a);
    r->len += n;
    return n;
}
size_t ring_get(struct ring *r, uint8_t *dst, size_t n, size_t skip, bool consume) {
    if (skip >= r->len) return 0;
    n = MIN(n, r->len - skip);
    size_t start = (r->head + skip) % r->cap;
    size_t a = MIN(n, r->cap - start);
    memcpy(dst, r->buf + start, a);
    memcpy(dst + a, r->buf, n - a);
    if (consume) ring_drop(r, skip + n);
    return n;
}
void ring_drop(struct ring *r, size_t n) {
    n = MIN(n, r->len);
    r->head = (r->head + n) % (r->cap ? r->cap : 1);
    r->len -= n;
    if (!r->len) r->head = 0;
}

/* ------------------------------------------------------------------ checksums */
uint32_t csum_partial(const void *buf, size_t len, uint32_t sum) {
    const uint8_t *p = buf;
    uint64_t s = sum;
    if ((uintptr_t)p & 3) {                      /* unusual: big-endian byte pairs */
        for (; len >= 2; p += 2, len -= 2) s += (uint32_t)(p[0] << 8 | p[1]);
        if (len) s += (uint32_t)p[0] << 8;
    } else {
        /* native little-endian lanes; the one's complement sum commutes with the byte swap */
        uint64_t acc = 0;
        while (len >= 32) {
            const uint32_t *w = (const uint32_t *)p;
            acc += (uint64_t)w[0] + w[1] + w[2] + w[3] + w[4] + w[5] + w[6] + w[7];
            p += 32; len -= 32;
        }
        while (len >= 4) { acc += *(const uint32_t *)p; p += 4; len -= 4; }
        if (len >= 2) { acc += *(const uint16_t *)p; p += 2; len -= 2; }
        if (len) acc += *p;
        while (acc >> 16) acc = (acc & 0xffff) + (acc >> 16);
        s += __builtin_bswap16((uint16_t)acc);
    }
    while (s >> 32) s = (s & 0xffffffff) + (s >> 32);
    return (uint32_t)s;
}
uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;            /* big-endian value; store with htons() */
}
uint32_t csum_pseudo(uint32_t s, uint32_t d, uint8_t proto, uint16_t len) {
    uint32_t h[3] = { s, d, htonl(((uint32_t)proto << 16) | len) };
    return csum_partial(h, sizeof h, 0);
}

/* ------------------------------------------------------------------ timers */
static struct list_node timers = LIST_INIT(timers);
static struct wait_queue net_wq = WAIT_QUEUE_INIT(net_wq);
static volatile int net_pending;
static struct thread *net_thread;

static void net_kick(void) {
    __atomic_store_n(&net_pending, 1, __ATOMIC_SEQ_CST);
    wake_up(&net_wq);
}

void ntimer_mod(struct ntimer *t, uint64_t delay_ns) {
    if (t->active) list_del(&t->node);
    t->when = time_ns() + delay_ns;
    t->active = true;
    list_add_tail(&timers, &t->node);
    if (current != net_thread) net_kick();     /* it may be sleeping past the new deadline */
}
void ntimer_del(struct ntimer *t) {
    if (t->active) { list_del(&t->node); t->active = false; }
}
static uint64_t run_timers(void) {           /* returns ns until the next deadline */
    uint64_t now = time_ns(), next = UINT64_MAX;
    for (;;) {
        struct ntimer *due = nullptr;
        list_for_each(it, &timers) {
            struct ntimer *t = list_entry(it, struct ntimer, node);
            if (t->when <= now) { due = t; break; }
        }
        if (!due) break;
        list_del(&due->node);
        due->active = false;
        due->fn(due);                         /* may re-arm itself or free its owner */
    }
    list_for_each(it, &timers) {
        struct ntimer *t = list_entry(it, struct ntimer, node);
        uint64_t left = t->when > now ? t->when - now : 0;
        if (left < next) next = left;
    }
    return next;
}

/* ------------------------------------------------------------------ receive queue */
static const struct lock_class netq_class = { "netq", LR_NETQ, false };
static spinlock_t rxq_lock = SPINLOCK_INIT_CLASS(&netq_class);
static struct list_node rxq = LIST_INIT(rxq);
static int rxq_len;
#define RXQ_MAX 2000

void net_rx(struct netdev *d, struct pkt *p) {
    pkt_set_dev(p, d);
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&rxq_lock);
    bool drop = rxq_len >= RXQ_MAX;
    if (!drop) { list_add_tail(&rxq, &p->node); rxq_len++; }
    spin_unlock(&rxq_lock);
    arch_irq_restore(f);
    if (drop) { d->rx_dropped++; pkt_free(p); return; }
    net_kick();
}

static void eth_input(struct netdev *d, struct pkt *p) {
    if (p->len < ETH_HLEN || !(d->flags & IFF_UP)) { d->rx_dropped++; pkt_free(p); return; }
    d->rx_packets++;
    d->rx_bytes += p->len;
    uint8_t *e = p->data;
    p->proto = (uint16_t)(e[12] << 8 | e[13]);
    static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (!memcmp(e, bc, 6)) p->pkttype = PACKET_BROADCAST;
    else if (e[0] & 1) {
        /* multicast: only groups joined on the device (IFF_PROMISC/IFF_ALLMULTI: all) */
        p->pkttype = d->type == ARPHRD_ETHER && !mc_mac_ok(d, e) ? PACKET_OTHERHOST : PACKET_MULTICAST;
        d->multicast++;
    }
    else if (d->type == ARPHRD_ETHER && memcmp(e, d->hwaddr, 6)) p->pkttype = PACKET_OTHERHOST;
    else p->pkttype = PACKET_HOST;
    packet_input(d, p, false);                /* AF_PACKET taps see the whole frame */
    if (p->pkttype == PACKET_OTHERHOST) { pkt_free(p); return; }
    pkt_pull(p, ETH_HLEN);
    if (p->proto == ETH_P_IP) ip_input(d, p);
    else if (p->proto == ETH_P_IPV6) ip6_input(d, p);
    else if (p->proto == ETH_P_ARP && !(d->flags & IFF_NOARP)) arp_input(d, p);
    else pkt_free(p);
}

static void net_thread_fn(void *arg) {
    uint64_t next = UINT64_MAX;
    for (;;) {
        uint64_t f = sched_wait_lock();
        if (!__atomic_load_n(&net_pending, __ATOMIC_SEQ_CST)) wait_event_timeout_locked(&net_wq, next, f);
        else sched_wait_unlock(f);
        __atomic_store_n(&net_pending, 0, __ATOMIC_SEQ_CST);
        mutex_lock(&net_mutex);
        for (int budget = 0; budget < 256; budget++) {
            uint64_t fl = arch_irq_save();
            spin_lock_ipi(&rxq_lock);
            struct pkt *p = nullptr;
            if (!list_empty(&rxq)) { p = list_first(&rxq, struct pkt, node); list_del(&p->node); rxq_len--; }
            bool more = !list_empty(&rxq);
            spin_unlock(&rxq_lock);
            arch_irq_restore(fl);
            if (!p) break;
            eth_input(p->dev, p);
            if (budget == 255 && more) __atomic_store_n(&net_pending, 1, __ATOMIC_SEQ_CST);
        }
        next = run_timers();
        mutex_unlock(&net_mutex);
    }
}

/* ------------------------------------------------------------------ devices */
/* ---- M34: sysfs */
static struct netdev *nd(struct device *k) { return k->priv; }
#define NLOCK(expr) ({ bool __o = !mutex_owned(&net_mutex); if (__o) mutex_lock(&net_mutex); int __r = (expr); if (__o) mutex_unlock(&net_mutex); __r; })
static int a_ifindex(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%d\n", nd(k)->index < 0 ? -nd(k)->index : nd(k)->index); }
static int a_type(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%d\n", nd(k)->type); }
static int a_mtu(struct device *k, char *b, size_t n) { return NLOCK(dev_printf(b, n, "%d\n", nd(k)->mtu)); }
static int a_flags(struct device *k, char *b, size_t n) { return NLOCK(dev_printf(b, n, "0x%x\n", nd(k)->flags & 0xffff)); }
static int a_txq(struct device *k, char *b, size_t n) { return NLOCK(dev_printf(b, n, "%d\n", nd(k)->txqlen)); }
static int a_addrlen(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%d\n", nd(k)->type == ARPHRD_NONE ? 0 : ETH_ALEN); }
static int mac_str(char *b, size_t n, const uint8_t *m) {
    return dev_printf(b, n, "%02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static int a_address(struct device *k, char *b, size_t n) { return nd(k)->type == ARPHRD_NONE ? dev_printf(b, n, "\n") : NLOCK(mac_str(b, n, nd(k)->hwaddr)); }
static int a_broadcast(struct device *k, char *b, size_t n) {
    static const uint8_t ff[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, zero[6];
    return nd(k)->type == ARPHRD_NONE ? dev_printf(b, n, "\n") : mac_str(b, n, nd(k)->type == ARPHRD_LOOPBACK ? zero : ff);
}
static int a_operstate(struct device *k, char *b, size_t n) {
    struct netdev *d = nd(k);
    unsigned f = __atomic_load_n(&d->flags, __ATOMIC_RELAXED);
    return dev_printf(b, n, "%s\n", d->type == ARPHRD_LOOPBACK ? "unknown" : (f & IFF_UP) && (f & IFF_RUNNING) ? "up" : "down");
}
static int a_carrier(struct device *k, char *b, size_t n) {
    unsigned f = __atomic_load_n(&nd(k)->flags, __ATOMIC_RELAXED);
    if (!(f & IFF_UP)) return -EINVAL;
    return dev_printf(b, n, "%d\n", (f & IFF_RUNNING) != 0);
}
static int a_zero(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x0\n"); }
#define STAT(f) static int a_##f(struct device *k, char *b, size_t n) { return NLOCK(dev_printf(b, n, "%lu\n", nd(k)->f)); }
STAT(rx_packets) STAT(rx_bytes) STAT(rx_errors) STAT(rx_dropped) STAT(tx_packets) STAT(tx_bytes) STAT(tx_errors) STAT(tx_dropped) STAT(multicast)
static const struct dev_attr net_attrs[] = {
    { "ifindex", 0444, a_ifindex, nullptr }, { "iflink", 0444, a_ifindex, nullptr }, { "type", 0444, a_type, nullptr },
    { "address", 0444, a_address, nullptr }, { "broadcast", 0444, a_broadcast, nullptr }, { "addr_len", 0444, a_addrlen, nullptr },
    { "mtu", 0444, a_mtu, nullptr }, { "flags", 0444, a_flags, nullptr }, { "operstate", 0444, a_operstate, nullptr },
    { "carrier", 0444, a_carrier, nullptr }, { "tx_queue_len", 0444, a_txq, nullptr }, { "dev_id", 0444, a_zero, nullptr },
    { "statistics/rx_packets", 0444, a_rx_packets, nullptr }, { "statistics/rx_bytes", 0444, a_rx_bytes, nullptr },
    { "statistics/rx_errors", 0444, a_rx_errors, nullptr }, { "statistics/rx_dropped", 0444, a_rx_dropped, nullptr },
    { "statistics/tx_packets", 0444, a_tx_packets, nullptr }, { "statistics/tx_bytes", 0444, a_tx_bytes, nullptr },
    { "statistics/tx_errors", 0444, a_tx_errors, nullptr }, { "statistics/tx_dropped", 0444, a_tx_dropped, nullptr },
    { "statistics/multicast", 0444, a_multicast, nullptr }, { nullptr, 0, nullptr, nullptr },
};
static void net_uevent(struct device *k, struct uenv *e) {
    uenv_add(e, "INTERFACE=%s", nd(k)->name);
    uenv_add(e, "IFINDEX=%d", nd(k)->index < 0 ? -nd(k)->index : nd(k)->index);
}

static void netdev_release(struct device *k) { kfree(nd(k)); }

struct netdev *netdev_register(const char *name, int type, const uint8_t *hw, int mtu,
                               void (*xmit)(struct netdev *, struct pkt *), void *priv, struct device *parent) {
    struct netdev *d = kzalloc(sizeof *d);
    if (!d) return nullptr;
    strncpy(d->name, name, sizeof d->name - 1);
    d->type = type;
    if (hw) memcpy(d->hwaddr, hw, ETH_ALEN);
    d->mtu = mtu;
    d->txqlen = 1000;
    d->xmit = xmit;
    d->priv = priv;
    list_init(&d->mcgroups);
    d->flags = type == ARPHRD_LOOPBACK ? IFF_LOOPBACK | IFF_NOARP : IFF_BROADCAST | IFF_MULTICAST;
    mutex_lock(&net_mutex);
    d->index = next_ifindex++;
    list_add_tail(&netdevs, &d->node);
    mutex_unlock(&net_mutex);
    struct device *k = &d->kdev;
    strncpy(k->name, d->name, sizeof k->name - 1);
    k->release = netdev_release;
    k->parent = parent; k->subsys = "net"; k->attrs = net_attrs; k->uevent = net_uevent; k->priv = d;
    device_add(k);
    return d;
}
/* a device going away (TUN/TAP close): forget its addresses, routes and neighbours. Frames for it
 * may still sit in queues: packets hold a reference, the struct is freed with the last one */
void netdev_unregister(struct netdev *d) {
    d->flags &= ~(IFF_UP | IFF_RUNNING);
    route_flush_dev(d);
    arp_flush_dev(d);
    netdev_down_hook(d);
    mc_dev_flush(d);
    d->addr = d->netmask = d->bcast = 0;
    list_del(&d->node);
    list_init(&d->node);
    device_del(&d->kdev);
    d->index = -d->index;
    device_put(&d->kdev);       /* freed once queued packets and sysfs files let go */
}
/* SIOCSIFNAME / RTM_SETLINK IFLA_IFNAME (net_mutex held): only while down, names unique */
int netdev_rename(struct netdev *d, const char *name) {
    char n[16] = { 0 };
    strncpy(n, name, 15);
    if (!n[0] || strchr(n, '/') || strchr(n, ':') || strchr(n, ' ') || !strcmp(n, ".") || !strcmp(n, "..")) return -EINVAL;
    struct netdev *x = netdev_by_name(n);
    if (x == d) return 0;
    if (x) return -EEXIST;
    if (d->flags & IFF_UP) return -EBUSY;
    memcpy(d->name, n, 16);
    device_rename(&d->kdev, n);
    return 0;
}
struct netdev *netdev_by_index(int idx) {
    list_for_each(it, &netdevs) { struct netdev *d = list_entry(it, struct netdev, node); if (d->index == idx) return d; }
    return nullptr;
}
struct netdev *netdev_by_name(const char *name) {
    list_for_each(it, &netdevs) { struct netdev *d = list_entry(it, struct netdev, node); if (!strncmp(d->name, name, 16)) return d; }
    return nullptr;
}
struct netdev *net_dev_for_local(uint32_t a) {
    if (ipv4_is_loopback(a)) return loopback_dev;
    list_for_each(it, &netdevs) {
        struct netdev *d = list_entry(it, struct netdev, node);
        if (d->addr && d->addr == a && (d->flags & IFF_UP)) return d;
    }
    return nullptr;
}
bool net_is_local_addr(uint32_t a) { return net_dev_for_local(a) != nullptr; }

/* the ethernet header is already in place: loop the frame back as a receive */
int sysctl_lo_drop_every;              /* /proc/sys/net/core/9os_lo_drop_every: loss for tests */
static void lo_xmit(struct netdev *d, struct pkt *p) {
    if (sysctl_lo_drop_every && random_u64() % (unsigned)sysctl_lo_drop_every == 0) { d->tx_dropped++; pkt_free(p); return; }
    d->tx_packets++;
    d->tx_bytes += p->len;
    p->csum_ok = true;
    p->nh = p->th = nullptr;
    net_rx(d, p);
}

/* ------------------------------------------------------------------ /proc/net/dev, route */
int net_proc_dev(char *buf, size_t max) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, "Inter-|   Receive                                                |  Transmit\n"
                               " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    list_for_each(it, &netdevs) {
        struct netdev *d = list_entry(it, struct netdev, node);
        if ((size_t)n >= max) break;
        n += snprintf(buf + n, max - n, "%6s: %7lu %7lu %4lu %4lu    0     0          0 %9lu %8lu %7lu %4lu %4lu    0     0       0          0\n",
                      d->name, d->rx_bytes, d->rx_packets, d->rx_errors, d->rx_dropped, d->multicast,
                      d->tx_bytes, d->tx_packets, d->tx_errors, d->tx_dropped);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}

int net_proc_route(char *buf, size_t max) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    list_for_each(it, &routes) {
        struct route *r = list_entry(it, struct route, node);
        if ((size_t)n >= max) break;
        if (r->dev == loopback_dev) continue;              /* Linux keeps these in the local table */
        n += snprintf(buf + n, max - n, "%s\t%08X\t%08X\t%04X\t0\t0\t%d\t%08X\t0\t0\t0\n",
                      r->dev->name, r->dst, r->gw, r->flags, r->metric, r->mask);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}

extern uint64_t ip_stats[8], udp_stats[4], tcp_stats[8], ip_fwd_stats;
int net_proc_file(int which, char *buf, size_t max) {
    switch (which) {
    case 0: return net_proc_dev(buf, max);
    case 1: return net_proc_route(buf, max);
    case 2: return net_proc_arp(buf, max);
    case 3: return net_proc_tcp(buf, max);
    case 4: return net_proc_udp(buf, max, false);
    case 5: return net_proc_udp(buf, max, true);
    case 6: return snprintf(buf, max, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    case 7: {
        int n = snprintf(buf, max, "Ip: Forwarding DefaultTTL InReceives InHdrErrors ForwDatagrams InDelivers OutRequests FragCreates ReasmOKs OutNoRoutes\n"
                                   "Ip: %d %d %lu %lu %lu %lu %lu %lu %lu %lu\n", sysctl_ip_forward ? 1 : 2, sysctl_ip_default_ttl, ip_stats[0], ip_stats[5], ip_fwd_stats, ip_stats[1], ip_stats[2], ip_stats[3], ip_stats[4], ip_stats[6]);
        n += snprintf(buf + n, max - n, "Icmp: InMsgs\nIcmp: %lu\n", ip_stats[7]);
        n += snprintf(buf + n, max - n, "Tcp: ActiveOpens PassiveOpens InSegs OutSegs RetransSegs InErrs OutRsts InRsts\n"
                                        "Tcp: %lu %lu %lu %lu %lu %lu %lu %lu\n", tcp_stats[6], tcp_stats[7], tcp_stats[0], tcp_stats[1], tcp_stats[2],
                      tcp_stats[3], tcp_stats[5], tcp_stats[4]);
        n += snprintf(buf + n, max - n, "Udp: InDatagrams NoPorts InErrors OutDatagrams\nUdp: %lu %lu %lu %lu\n",
                      udp_stats[0], udp_stats[1], udp_stats[2], udp_stats[3]);
        return MIN(n, (int)max);
    }
    case 8: return net_proc_igmp(buf, max, false);
    case 9: return net_proc_igmp(buf, max, true);
    case 10: return net_proc_if_inet6(buf, max);
    case 11: return net_proc_ipv6_route(buf, max);
    case 12: return net_proc_tcp6(buf, max);
    case 13: return net_proc_udp6(buf, max, false);
    case 14: return net_proc_udp6(buf, max, true);
    case 15: return net_proc_snmp6(buf, max);
    case 16: return net_proc_netlink(buf, max);
    case 17: {
        extern uint64_t tcp_ext_stats[6];
        return snprintf(buf, max, "TcpExt: TCPTimeouts TCPLossProbes TCPLostRetransmit TCPSackRecovery TCPRenoRecovery TCPEarlyRetrans\n"
                                  "TcpExt: %lu %lu %lu %lu %lu %lu\n", tcp_ext_stats[0], tcp_ext_stats[1], tcp_ext_stats[2],
                        tcp_ext_stats[3], tcp_ext_stats[4], tcp_ext_stats[5]);
    }
    }
    return 0;
}

void net_init(void) {
    static const uint8_t zero[6];
    loopback_dev = netdev_register("lo", ARPHRD_LOOPBACK, zero, 65536, lo_xmit, nullptr, nullptr);
    loopback_dev->addr = INADDR_LOOPBACK;           /* up from the start, like Linux after "ip link set lo up" */
    loopback_dev->netmask = htonl(0xff000000u);
    loopback_dev->flags |= IFF_UP | IFF_RUNNING;
    arp_init();
    ip6_init();
    tcp_init();
    tun_init();
    net_thread = thread_create("net", net_thread_fn, nullptr);
}
