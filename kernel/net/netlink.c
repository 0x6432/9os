/*
 * M32b: AF_NETLINK, NETLINK_ROUTE (rtnetlink) — enough for BusyBox `ip` and musl's getifaddrs()
 * and if_nameindex(): RTM_GET/NEW/DEL/SETLINK, RTM_GET/NEW/DELADDR, RTM_GET/NEW/DELROUTE and
 * RTM_GET/NEW/DELNEIGH for IPv4 and IPv6, with dumps (NLM_F_DUMP: NLM_F_MULTI messages packed
 * into datagrams of up to 4 KiB, then NLMSG_DONE) and NLMSG_ERROR acknowledgements.
 *
 * A netlink socket is a struct sock (family AF_NETLINK) on the inet file operations; requests
 * are processed synchronously inside sendmsg() and the replies are queued on the socket's
 * datagram queue (so musl can read them with MSG_DONTWAIT right after send()). Port ids are the
 * process id for the first socket, then unique negative numbers, as in Linux. No multicast
 * notifications are sent (bind() accepts the groups). Runs under net_mutex.
 */
#include <kernel/net.h>
#include <kernel/net6.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/process.h>
#include <kernel/cred.h>
#include <kernel/printk.h>
#include <kernel/time.h>

struct list_node netlink_socks = LIST_INIT(netlink_socks);

struct nlmsghdr { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct rtattr { uint16_t len, type; };
struct ifinfomsg { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct ifaddrmsg { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct rtmsg { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; };
struct ndmsg { uint8_t family, pad1; uint16_t pad2; int32_t ifindex; uint16_t state; uint8_t flags, type; };

#define NLM_F_REQUEST 1
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300
#define NLM_F_REPLACE 0x100
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLMSG_NOOP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define RTM_GETLINK 18
#define RTM_SETLINK 19
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26
#define RTM_NEWNEIGH 28
#define RTM_DELNEIGH 29
#define RTM_GETNEIGH 30
#define IFLA_ADDRESS 1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_QDISC 6
#define IFLA_STATS 7
#define IFLA_TXQLEN 13
#define IFLA_OPERSTATE 16
#define IFLA_LINKMODE 17
#define IFLA_STATS64 23
#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define IFA_BROADCAST 4
#define IFA_CACHEINFO 6
#define IFA_FLAGS 8
#define RTA_DST 1
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_PRIORITY 6
#define RTA_PREFSRC 7
#define RTA_CACHEINFO 12
#define RTA_TABLE 15
#define NDA_DST 1
#define NDA_LLADDR 2
#define IFF_LOWER_UP 0x10000
#define RT_TABLE_MAIN 254
#define RTPROT_KERNEL 2
#define RTPROT_BOOT 3
#define RTPROT_RA 9
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK 253
#define RT_SCOPE_HOST 254
#define RTN_UNICAST 1
#define NUD_PERMANENT 0x80
#define NL_CHUNK 4096

#define ALIGN4(x) (((x) + 3u) & ~3u)

/* ------------------------------------------------------------------ reply building */
struct nlout {
    struct sock *s;
    struct pkt *p;                  /* datagram being filled */
    size_t msg;                     /* offset of the open message */
    uint32_t seq;
    bool dump;
    int err;
};

static void out_flush(struct nlout *o) {
    if (o->p && o->p->len) sock_queue_rx(o->s, o->p);
    else if (o->p) pkt_free(o->p);
    o->p = nullptr;
}
static uint8_t *out_room(struct nlout *o, size_t n) {
    if (o->p && o->p->len + n > o->p->cap - PKT_HEADROOM) {
        if (!o->dump) { o->err = -EMSGSIZE; return nullptr; }
        return nullptr;
    }
    uint8_t *r = o->p->data + o->p->len;
    memset(r, 0, ALIGN4(n));
    o->p->len += ALIGN4(n);
    return r;
}
static struct nlmsghdr *msg_begin(struct nlout *o, uint16_t type, size_t body) {
    size_t want = 16 + ALIGN4(body) + 512;          /* attributes follow */
    if (o->p && o->p->len + want > NL_CHUNK && o->p->len) out_flush(o);
    if (!o->p) {
        size_t cap = want > NL_CHUNK ? want : NL_CHUNK;
        if (!(o->p = pkt_alloc(cap))) { o->err = -ENOBUFS; return nullptr; }
    }
    o->msg = o->p->len;
    struct nlmsghdr *h = (struct nlmsghdr *)out_room(o, 16 + body);
    if (!h) return nullptr;
    h->type = type;
    h->flags = o->dump ? NLM_F_MULTI : 0;
    h->seq = o->seq;
    h->pid = o->s->nl_pid;
    return h;
}
static void msg_end(struct nlout *o) {
    struct nlmsghdr *h = (struct nlmsghdr *)(o->p->data + o->msg);
    h->len = (uint32_t)(o->p->len - o->msg);
}
static void *msg_body(struct nlout *o) { return o->p->data + o->msg + 16; }
static void attr(struct nlout *o, uint16_t type, const void *data, size_t len) {
    struct rtattr *a = (struct rtattr *)out_room(o, 4 + len);
    if (!a) return;
    a->len = (uint16_t)(4 + len);
    a->type = type;
    memcpy(a + 1, data, len);
}
static void attr_u32(struct nlout *o, uint16_t type, uint32_t v) { attr(o, type, &v, 4); }
static void attr_u8(struct nlout *o, uint16_t type, uint8_t v) { attr(o, type, &v, 1); }

static void send_error(struct sock *s, const struct nlmsghdr *req, int err) {
    struct nlout o = { .s = s, .seq = req->seq };
    struct nlmsghdr *h = msg_begin(&o, NLMSG_ERROR, 4 + 16);
    if (!h) { out_flush(&o); return; }
    int32_t e = err;
    memcpy(msg_body(&o), &e, 4);
    memcpy((uint8_t *)msg_body(&o) + 4, req, 16);
    msg_end(&o);
    out_flush(&o);
}
static void send_done(struct nlout *o) {
    struct nlmsghdr *h = msg_begin(o, NLMSG_DONE, 4);
    if (h) msg_end(o);
    out_flush(o);
}

/* attributes of a request: tb[type] points at the payload, len in tl[type] */
#define TB_MAX 32
static void parse_attrs(const uint8_t *p, size_t len, const uint8_t **tb, size_t *tl) {
    memset(tb, 0, sizeof(*tb) * TB_MAX);
    while (len >= 4) {
        const struct rtattr *a = (const struct rtattr *)p;
        if (a->len < 4 || a->len > len) break;
        if ((a->type & 0x3fff) < TB_MAX) { tb[a->type & 0x3fff] = p + 4; tl[a->type & 0x3fff] = a->len - 4u; }
        size_t step = ALIGN4(a->len);
        if (step >= len) break;
        p += step; len -= step;
    }
}

/* ------------------------------------------------------------------ links */
static int operstate(struct netdev *d) {
    if (d->type == ARPHRD_LOOPBACK) return 0;              /* IF_OPER_UNKNOWN, as Linux */
    return (d->flags & IFF_UP) ? 6 : 2;                    /* UP / DOWN */
}
static void fill_link(struct nlout *o, struct netdev *d, uint16_t type) {
    if (!msg_begin(o, type, sizeof(struct ifinfomsg))) return;
    struct ifinfomsg *m = msg_body(o);
    m->family = 0;
    m->type = (uint16_t)d->type;
    m->index = d->index;
    m->flags = d->flags | ((d->flags & IFF_RUNNING) ? IFF_LOWER_UP : 0);
    m->change = 0;
    attr(o, IFLA_IFNAME, d->name, strlen(d->name) + 1);
    attr_u32(o, IFLA_TXQLEN, (uint32_t)d->txqlen);
    attr_u8(o, IFLA_OPERSTATE, (uint8_t)operstate(d));
    attr_u8(o, IFLA_LINKMODE, 0);
    attr_u32(o, IFLA_MTU, (uint32_t)d->mtu);
    attr(o, IFLA_QDISC, d->type == ARPHRD_LOOPBACK ? "noqueue" : "pfifo_fast", d->type == ARPHRD_LOOPBACK ? 8 : 11);
    if (d->type == ARPHRD_ETHER || d->type == ARPHRD_LOOPBACK) {
        static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, z[6];
        attr(o, IFLA_ADDRESS, d->hwaddr, 6);
        attr(o, IFLA_BROADCAST, d->type == ARPHRD_LOOPBACK ? z : bc, 6);
    }
    uint32_t st[24] = { (uint32_t)d->rx_packets, (uint32_t)d->tx_packets, (uint32_t)d->rx_bytes, (uint32_t)d->tx_bytes,
                        (uint32_t)d->rx_errors, (uint32_t)d->tx_errors, (uint32_t)d->rx_dropped, (uint32_t)d->tx_dropped,
                        (uint32_t)d->multicast };
    attr(o, IFLA_STATS, st, sizeof st);
    uint64_t st64[24] = { d->rx_packets, d->tx_packets, d->rx_bytes, d->tx_bytes, d->rx_errors, d->tx_errors,
                          d->rx_dropped, d->tx_dropped, d->multicast };
    attr(o, IFLA_STATS64, st64, sizeof st64);
    msg_end(o);
}

static struct netdev *link_target(const struct ifinfomsg *m, const uint8_t **tb, const size_t *tl) {
    if (m->index) return netdev_by_index(m->index);
    if (tb[IFLA_IFNAME]) {
        char n[16] = { 0 };
        memcpy(n, tb[IFLA_IFNAME], MIN(tl[IFLA_IFNAME], (size_t)15));
        return netdev_by_name(n);
    }
    return nullptr;
}

static int do_link(struct nlout *o, const struct nlmsghdr *h, const uint8_t *body, size_t blen) {
    struct ifinfomsg m = { 0 };
    memcpy(&m, body, MIN(blen, sizeof m));
    const uint8_t *tb[TB_MAX]; size_t tl[TB_MAX];
    parse_attrs(body + ALIGN4(sizeof m), blen > ALIGN4(sizeof m) ? blen - ALIGN4(sizeof m) : 0, tb, tl);
    if (h->type == RTM_GETLINK) {
        if ((h->flags & NLM_F_DUMP) == NLM_F_DUMP) {
            list_for_each(it, &netdevs) fill_link(o, list_entry(it, struct netdev, node), RTM_NEWLINK);
            return 1;
        }
        struct netdev *d = link_target(&m, tb, tl);
        if (!d) return -ENODEV;
        fill_link(o, d, RTM_NEWLINK);
        return 0;
    }
    if (h->type == RTM_DELLINK) return -EOPNOTSUPP;
    struct netdev *d = link_target(&m, tb, tl);           /* NEWLINK on an existing device, SETLINK */
    if (!d) return h->flags & NLM_F_CREATE ? -EOPNOTSUPP : -ENODEV;
    if (h->type == RTM_NEWLINK && (h->flags & NLM_F_EXCL)) return -EEXIST;
    if (tb[IFLA_MTU] && tl[IFLA_MTU] >= 4) {
        uint32_t mtu; memcpy(&mtu, tb[IFLA_MTU], 4);
        if (mtu < 68 || mtu > (d->type == ARPHRD_LOOPBACK ? 65536u : 1500u)) return -EINVAL;
        d->mtu = (int)mtu;
    }
    if (tb[IFLA_IFNAME] && m.index) {                       /* rename */
        char n[16] = { 0 };
        memcpy(n, tb[IFLA_IFNAME], MIN(tl[IFLA_IFNAME], (size_t)15));
        struct netdev *x = netdev_by_name(n);
        if (x && x != d) return -EEXIST;
        if (d->flags & IFF_UP) return -EBUSY;
        memcpy(d->name, n, 16);
    }
    if (tb[IFLA_TXQLEN] && tl[IFLA_TXQLEN] >= 4) { uint32_t q; memcpy(&q, tb[IFLA_TXQLEN], 4); d->txqlen = (int)q; }
    if (m.change || m.flags) {
        unsigned chg = m.change ? m.change : 0xffffffffu;
        netdev_set_flags(d, (d->flags & ~chg) | (m.flags & chg));
    }
    return 0;
}

/* ------------------------------------------------------------------ addresses */
static int mask_len(uint32_t mask) { return __builtin_popcount(mask); }
static uint32_t len_mask(int n) { return n <= 0 ? 0 : htonl(n >= 32 ? 0xffffffffu : ~(0xffffffffu >> n)); }

static void fill_addr4(struct nlout *o, struct netdev *d, uint16_t type) {
    if (!msg_begin(o, type, sizeof(struct ifaddrmsg))) return;
    struct ifaddrmsg *m = msg_body(o);
    m->family = AF_INET;
    m->prefixlen = (uint8_t)mask_len(d->netmask);
    m->flags = 0x80;                                        /* IFA_F_PERMANENT */
    m->scope = d->type == ARPHRD_LOOPBACK ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
    m->index = (uint32_t)d->index;
    attr(o, IFA_ADDRESS, &d->addr, 4);
    attr(o, IFA_LOCAL, &d->addr, 4);
    if (d->bcast) attr(o, IFA_BROADCAST, &d->bcast, 4);
    attr(o, IFA_LABEL, d->name, strlen(d->name) + 1);
    attr_u32(o, IFA_FLAGS, 0x80);
    uint32_t ci[4] = { 0xffffffffu, 0xffffffffu, 0, 0 };   /* preferred, valid: forever */
    attr(o, IFA_CACHEINFO, ci, sizeof ci);
    msg_end(o);
}
static void fill_addr6_cb(void *ctx, const struct inet6_ifaddr *a) {
    struct nlout *o = ctx;
    if (!msg_begin(o, RTM_NEWADDR, sizeof(struct ifaddrmsg))) return;
    struct ifaddrmsg *m = msg_body(o);
    m->family = AF_INET6;
    m->prefixlen = (uint8_t)a->plen;
    m->flags = (uint8_t)a->flags;
    m->scope = (uint8_t)a->scope;
    m->index = (uint32_t)a->dev->index;
    attr(o, IFA_ADDRESS, a->addr, 16);
    attr_u32(o, IFA_FLAGS, a->flags);
    uint32_t ci[4] = { a->pref, a->valid, 0, 0 };
    attr(o, IFA_CACHEINFO, ci, sizeof ci);
    msg_end(o);
}

static int do_addr(struct nlout *o, const struct nlmsghdr *h, const uint8_t *body, size_t blen) {
    struct ifaddrmsg m = { 0 };
    memcpy(&m, body, MIN(blen, sizeof m));
    if (h->type == RTM_GETADDR) {
        if ((h->flags & NLM_F_DUMP) != NLM_F_DUMP) return -EOPNOTSUPP;
        if (m.family == 0 || m.family == AF_INET)
            list_for_each(it, &netdevs) {
                struct netdev *d = list_entry(it, struct netdev, node);
                if (d->addr && (!m.index || (int)m.index == d->index)) fill_addr4(o, d, RTM_NEWADDR);
            }
        if (m.family == 0 || m.family == AF_INET6) ip6_addr_dump(fill_addr6_cb, o, (int)m.index);
        return 1;
    }
    const uint8_t *tb[TB_MAX]; size_t tl[TB_MAX];
    parse_attrs(body + ALIGN4(sizeof m), blen > ALIGN4(sizeof m) ? blen - ALIGN4(sizeof m) : 0, tb, tl);
    struct netdev *d = netdev_by_index((int)m.index);
    if (!d) return -ENODEV;
    const uint8_t *a = tb[IFA_LOCAL] ? tb[IFA_LOCAL] : tb[IFA_ADDRESS];
    size_t al = tb[IFA_LOCAL] ? tl[IFA_LOCAL] : tl[IFA_ADDRESS];
    if (m.family == AF_INET6) {
        if (!a || al < 16) return -EINVAL;
        if (h->type == RTM_NEWADDR) return ip6_addr_add(d, a, m.prefixlen ? m.prefixlen : 128, 0x80, 0xffffffffu, 0xffffffffu, (h->flags & NLM_F_EXCL) != 0);
        return ip6_addr_del(d, a, m.prefixlen);
    }
    if (m.family != AF_INET) return -EAFNOSUPPORT;
    if (h->type == RTM_NEWADDR) {
        if (!a || al < 4 || m.prefixlen > 32) return -EINVAL;
        uint32_t addr; memcpy(&addr, a, 4);
        if (d->addr == addr && (h->flags & NLM_F_EXCL)) return -EEXIST;
        /* one IPv4 address per interface: a different one needs "ip addr replace" (or ifconfig) */
        if (d->addr && d->addr != addr && !(h->flags & NLM_F_REPLACE)) return -EEXIST;
        if (d->addr != addr) arp_flush_dev(d);
        d->addr = addr;
        d->netmask = d->type == ARPHRD_LOOPBACK && !m.prefixlen ? htonl(0xff000000u) : len_mask(m.prefixlen);
        if (tb[IFA_BROADCAST] && tl[IFA_BROADCAST] >= 4) memcpy(&d->bcast, tb[IFA_BROADCAST], 4);
        else d->bcast = d->type == ARPHRD_LOOPBACK || m.prefixlen >= 31 ? 0 : d->addr | ~d->netmask;
        net_dev_addr_changed(d);
        return 0;
    }
    /* RTM_DELADDR: the one IPv4 address of the device (when it matches) */
    if (!d->addr) return -EADDRNOTAVAIL;
    if (a && al >= 4 && memcmp(a, &d->addr, 4)) return -EADDRNOTAVAIL;
    d->addr = d->netmask = d->bcast = 0;
    arp_flush_dev(d);
    route_flush_dev(d);
    net_dev_addr_changed(d);
    return 0;
}

/* ------------------------------------------------------------------ routes */
static void fill_route4(struct nlout *o, struct route *r, uint16_t type) {
    if (!msg_begin(o, type, sizeof(struct rtmsg))) return;
    struct rtmsg *m = msg_body(o);
    m->family = AF_INET;
    m->dst_len = (uint8_t)mask_len(r->mask);
    m->table = RT_TABLE_MAIN;
    bool connected = !(r->flags & RTF_GATEWAY) && r->metric == 0 && r->dev->addr && (r->dev->addr & r->mask) == r->dst &&
                     r->mask == r->dev->netmask;
    m->protocol = connected ? RTPROT_KERNEL : RTPROT_BOOT;
    m->scope = (r->flags & RTF_GATEWAY) ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    m->type = RTN_UNICAST;
    attr_u32(o, RTA_TABLE, RT_TABLE_MAIN);
    if (m->dst_len) attr(o, RTA_DST, &r->dst, 4);
    if (r->metric) attr_u32(o, RTA_PRIORITY, (uint32_t)r->metric);
    if (connected) attr(o, RTA_PREFSRC, &r->dev->addr, 4);
    if (r->flags & RTF_GATEWAY) attr(o, RTA_GATEWAY, &r->gw, 4);
    attr_u32(o, RTA_OIF, (uint32_t)r->dev->index);
    msg_end(o);
}
static void fill_route6_cb(void *ctx, const struct rt6_info *r) {
    struct nlout *o = ctx;
    if (!msg_begin(o, RTM_NEWROUTE, sizeof(struct rtmsg))) return;
    struct rtmsg *m = msg_body(o);
    m->family = AF_INET6;
    m->dst_len = (uint8_t)r->plen;
    m->table = RT_TABLE_MAIN;
    m->protocol = r->proto;
    m->scope = RT_SCOPE_UNIVERSE;
    m->type = RTN_UNICAST;
    attr_u32(o, RTA_TABLE, RT_TABLE_MAIN);
    if (r->plen) attr(o, RTA_DST, r->dst, 16);
    attr_u32(o, RTA_PRIORITY, (uint32_t)r->metric);
    if (r->has_gw) attr(o, RTA_GATEWAY, r->gw, 16);
    attr_u32(o, RTA_OIF, (uint32_t)r->dev->index);
    if (r->expires) {                                       /* rta_expires in clock ticks (USER_HZ 100) */
        uint64_t now = time_ns();
        uint32_t ci[8] = { 0 };
        ci[2] = r->expires > now ? (uint32_t)((r->expires - now) / (NS_S / 100)) : 0;
        attr(o, RTA_CACHEINFO, ci, sizeof ci);
    }
    msg_end(o);
}

static int do_route(struct nlout *o, const struct nlmsghdr *h, const uint8_t *body, size_t blen) {
    struct rtmsg m = { 0 };
    memcpy(&m, body, MIN(blen, sizeof m));
    const uint8_t *tb[TB_MAX]; size_t tl[TB_MAX];
    parse_attrs(body + ALIGN4(sizeof m), blen > ALIGN4(sizeof m) ? blen - ALIGN4(sizeof m) : 0, tb, tl);
    int oif = 0;
    if (tb[RTA_OIF] && tl[RTA_OIF] >= 4) memcpy(&oif, tb[RTA_OIF], 4);
    uint32_t metric = 0;
    if (tb[RTA_PRIORITY] && tl[RTA_PRIORITY] >= 4) memcpy(&metric, tb[RTA_PRIORITY], 4);
    if (h->type == RTM_GETROUTE) {
        if ((h->flags & NLM_F_DUMP) == NLM_F_DUMP) {
            if (m.family == 0 || m.family == AF_INET)
                list_for_each(it, &routes) {
                    struct route *r = list_entry(it, struct route, node);
                    if (r->dev != loopback_dev) fill_route4(o, r, RTM_NEWROUTE);
                }
            if (m.family == 0 || m.family == AF_INET6) ip6_route_dump(fill_route6_cb, o);
            return 1;
        }
        if (m.family == AF_INET6 && tb[RTA_DST] && tl[RTA_DST] >= 16) {             /* ip -6 route get */
            struct netdev *d; uint8_t nh[16], src[16];
            int r = ip6_route(tb[RTA_DST], oif, &d, nh, src);
            if (r) return r;
            if (!msg_begin(o, RTM_NEWROUTE, sizeof(struct rtmsg))) return -ENOBUFS;
            struct rtmsg *rm = msg_body(o);
            rm->family = AF_INET6; rm->dst_len = 128; rm->table = RT_TABLE_MAIN; rm->type = RTN_UNICAST;
            rm->flags = 0x200;
            attr_u32(o, RTA_TABLE, RT_TABLE_MAIN);
            attr(o, RTA_DST, tb[RTA_DST], 16);
            attr_u32(o, RTA_OIF, (uint32_t)d->index);
            if (!ip6_eq(nh, tb[RTA_DST])) attr(o, RTA_GATEWAY, nh, 16);
            if (!ip6_any(src)) attr(o, RTA_PREFSRC, src, 16);
            uint32_t ci[8] = { 0 };                         /* struct rta_cacheinfo (BusyBox reads it for cloned v6 routes) */
            attr(o, RTA_CACHEINFO, ci, sizeof ci);
            msg_end(o);
            return 0;
        }
        if (m.family != AF_INET || !tb[RTA_DST] || tl[RTA_DST] < 4) return -EINVAL;   /* ip route get */
        uint32_t dst; memcpy(&dst, tb[RTA_DST], 4);
        struct netdev *d; uint32_t nh, src;
        int r = ip_route(dst, oif, &d, &nh, &src);
        if (r) return r;
        if (!msg_begin(o, RTM_NEWROUTE, sizeof(struct rtmsg))) return -ENOBUFS;
        struct rtmsg *rm = msg_body(o);
        rm->family = AF_INET; rm->dst_len = 32; rm->table = RT_TABLE_MAIN; rm->type = RTN_UNICAST;
        rm->flags = 0x200;                                  /* RTM_F_CLONED */
        attr_u32(o, RTA_TABLE, RT_TABLE_MAIN);
        attr(o, RTA_DST, &dst, 4);
        attr_u32(o, RTA_OIF, (uint32_t)d->index);
        if (nh != dst) attr(o, RTA_GATEWAY, &nh, 4);
        if (src) attr(o, RTA_PREFSRC, &src, 4);
        msg_end(o);
        return 0;
    }
    if (m.type && m.type != RTN_UNICAST) return -EOPNOTSUPP;
    if (m.family == AF_INET6) {
        uint8_t dst[16] = { 0 }, gw[16] = { 0 };
        if (m.dst_len > 128) return -EINVAL;
        if (tb[RTA_DST] && tl[RTA_DST] >= 16) memcpy(dst, tb[RTA_DST], 16);
        bool has_gw = tb[RTA_GATEWAY] && tl[RTA_GATEWAY] >= 16;
        if (has_gw) memcpy(gw, tb[RTA_GATEWAY], 16);
        struct netdev *d = oif ? netdev_by_index(oif) : nullptr;
        if (oif && !d) return -ENODEV;
        if (h->type == RTM_NEWROUTE) {
            if (h->flags & NLM_F_REPLACE) ip6_route_del(dst, m.dst_len, nullptr, d);
            return ip6_route_add(dst, m.dst_len, has_gw ? gw : nullptr, d, metric ? (int)metric : 1024, RTPROT_BOOT);
        }
        return ip6_route_del(dst, m.dst_len, has_gw ? gw : nullptr, d);
    }
    if (m.family != AF_INET || m.dst_len > 32) return -EINVAL;
    uint32_t dst = 0, gw = 0, mask = len_mask(m.dst_len);
    if (tb[RTA_DST] && tl[RTA_DST] >= 4) memcpy(&dst, tb[RTA_DST], 4);
    if (tb[RTA_GATEWAY] && tl[RTA_GATEWAY] >= 4) memcpy(&gw, tb[RTA_GATEWAY], 4);
    if (dst & ~mask) return -EINVAL;
    struct netdev *d = oif ? netdev_by_index(oif) : nullptr;
    if (oif && !d) return -ENODEV;
    if (h->type == RTM_DELROUTE) return route_del(dst, mask, gw, d, (int)metric);
    if (!d) {
        uint32_t via = gw ? gw : dst;
        list_for_each(it, &netdevs) {
            struct netdev *x = list_entry(it, struct netdev, node);
            if (x->addr && (x->flags & IFF_UP) && ((x->addr ^ via) & x->netmask) == 0) { d = x; break; }
        }
        if (!d) return -ENETUNREACH;
    }
    if (h->flags & NLM_F_REPLACE) route_del(dst, mask, 0, d, (int)metric);
    return route_add(dst, mask, gw, d, (int)metric, (gw ? RTF_GATEWAY : 0) | (m.dst_len == 32 ? RTF_HOST : 0));
}

/* ------------------------------------------------------------------ neighbours */
struct neigh_ctx { struct nlout *o; int family, ifindex; };
static void fill_neigh(struct nlout *o, int family, struct netdev *d, const void *ip, const uint8_t *mac, int state) {
    if (!msg_begin(o, RTM_NEWNEIGH, sizeof(struct ndmsg))) return;
    struct ndmsg *m = msg_body(o);
    m->family = (uint8_t)family;
    m->ifindex = d->index;
    m->state = (uint16_t)state;
    m->flags = state & 0x10000 ? 0x80 : 0;                 /* NTF_ROUTER */
    m->type = RTN_UNICAST;
    attr(o, NDA_DST, ip, family == AF_INET6 ? 16 : 4);
    if (mac) attr(o, NDA_LLADDR, mac, 6);
    msg_end(o);
}
static void neigh4_cb(void *ctx, struct netdev *d, uint32_t ip, const uint8_t *mac, int state) {
    struct neigh_ctx *c = ctx;
    if (!c->ifindex || c->ifindex == d->index) fill_neigh(c->o, AF_INET, d, &ip, mac, state);
}
static void neigh6_cb(void *ctx, struct netdev *d, const uint8_t *ip, const uint8_t *mac, int state) {
    struct neigh_ctx *c = ctx;
    if (!c->ifindex || c->ifindex == d->index) fill_neigh(c->o, AF_INET6, d, ip, mac, state);
}

static int do_neigh(struct nlout *o, const struct nlmsghdr *h, const uint8_t *body, size_t blen) {
    struct ndmsg m = { 0 };
    memcpy(&m, body, MIN(blen, sizeof m));
    if (h->type == RTM_GETNEIGH) {
        if ((h->flags & NLM_F_DUMP) != NLM_F_DUMP) return -EOPNOTSUPP;
        struct neigh_ctx c = { o, m.family, m.ifindex };
        if (m.family == 0 || m.family == AF_INET) arp_dump(neigh4_cb, &c);
        if (m.family == 0 || m.family == AF_INET6) nd_dump(neigh6_cb, &c);
        return 1;
    }
    const uint8_t *tb[TB_MAX]; size_t tl[TB_MAX];
    parse_attrs(body + ALIGN4(sizeof m), blen > ALIGN4(sizeof m) ? blen - ALIGN4(sizeof m) : 0, tb, tl);
    struct netdev *d = netdev_by_index(m.ifindex);
    if (!d) return -ENODEV;
    size_t al = m.family == AF_INET6 ? 16 : 4;
    if (!tb[NDA_DST] || tl[NDA_DST] < al) return -EINVAL;
    if (h->type == RTM_NEWNEIGH) {
        if (!tb[NDA_LLADDR] || tl[NDA_LLADDR] < 6) return -EINVAL;
        bool perm = m.state & NUD_PERMANENT;
        if (m.family == AF_INET6) return nd_set(d, tb[NDA_DST], tb[NDA_LLADDR], perm);
        uint32_t ip; memcpy(&ip, tb[NDA_DST], 4);
        return arp_set(d, ip, tb[NDA_LLADDR], perm);
    }
    if (m.family == AF_INET6) return nd_del(d, tb[NDA_DST]);
    uint32_t ip; memcpy(&ip, tb[NDA_DST], 4);
    return arp_del(d, ip);
}

/* ------------------------------------------------------------------ dispatch */
static void one_request(struct sock *s, const struct nlmsghdr *h) {
    if (!(h->flags & NLM_F_REQUEST) || h->type < 16) {      /* control messages: ignored */
        if (h->flags & NLM_F_ACK) send_error(s, h, 0);
        return;
    }
    const uint8_t *body = (const uint8_t *)h + 16;
    size_t blen = h->len - 16;
    bool get = (h->type & 3) == 2;                          /* RTM_GET* */
    bool dump = get && (h->flags & NLM_F_DUMP) == NLM_F_DUMP;
    if (!get && !capable(CAP_NET_ADMIN)) { send_error(s, h, -EPERM); return; }
    struct nlout o = { .s = s, .seq = h->seq, .dump = dump };
    int r;
    switch (h->type) {
    case RTM_NEWLINK: case RTM_DELLINK: case RTM_GETLINK: case RTM_SETLINK: r = do_link(&o, h, body, blen); break;
    case RTM_NEWADDR: case RTM_DELADDR: case RTM_GETADDR: r = do_addr(&o, h, body, blen); break;
    case RTM_NEWROUTE: case RTM_DELROUTE: case RTM_GETROUTE: r = do_route(&o, h, body, blen); break;
    case RTM_NEWNEIGH: case RTM_DELNEIGH: case RTM_GETNEIGH: r = do_neigh(&o, h, body, blen); break;
    default: r = dump ? 1 : -EOPNOTSUPP;                    /* rules, qdiscs, ...: empty dumps */
    }
    if (o.err && r >= 0) r = o.err;
    if (r == 1) { send_done(&o); return; }                  /* dump: messages, then NLMSG_DONE */
    out_flush(&o);
    if (r < 0 || (h->flags & NLM_F_ACK)) send_error(s, h, r < 0 ? r : 0);
}

int netlink_rcv(struct sock *s, const uint8_t *buf, size_t len) {
    netlink_autobind(s);
    size_t off = 0;
    while (len - off >= 16) {
        struct nlmsghdr h;
        memcpy(&h, buf + off, 16);
        if (h.len < 16 || h.len > len - off) break;
        uint8_t *copy = kmalloc(h.len);                     /* aligned for the casts below */
        if (!copy) return -ENOBUFS;
        memcpy(copy, buf + off, h.len);
        one_request(s, (struct nlmsghdr *)copy);
        kfree(copy);
        off += ALIGN4(h.len);
    }
    return 0;
}

/* ------------------------------------------------------------------ port ids */
static bool pid_used(uint32_t pid, struct sock *self) {
    list_for_each(it, &netlink_socks) {
        struct sock *o = list_entry(it, struct sock, node);
        if (o != self && o->nl_pid == pid) return true;
    }
    return false;
}
int netlink_bind(struct sock *s, uint32_t pid, uint32_t groups) {
    s->nl_groups = groups;
    if (s->nl_pid) return pid && pid != s->nl_pid ? -EINVAL : 0;
    if (pid) {
        if (pid_used(pid, s)) return -EADDRINUSE;
        s->nl_pid = pid;
        return 0;
    }
    netlink_autobind(s);
    return 0;
}
void netlink_autobind(struct sock *s) {
    static uint32_t next = 0;
    if (s->nl_pid) return;
    uint32_t pid = (uint32_t)curproc->pid;
    while (!pid || pid_used(pid, s)) pid = (uint32_t)(-4096 - (int32_t)next++);
    s->nl_pid = pid;
}

int net_proc_netlink(char *buf, size_t max) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, "sk               Eth Pid        Groups   Rmem     Wmem     Dump  Locks    Drops    Inode\n");
    list_for_each(it, &netlink_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if ((size_t)n >= max) break;
        n += snprintf(buf + n, max - n, "%016lx %-3d %-10u %08x %-8u %-8d %-5d %-8d %-8d %lu\n",
                      (unsigned long)(uintptr_t)s, s->protocol, s->nl_pid, s->nl_groups, (unsigned)s->rxbytes, 0, 0, 2, 0, s->ino);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}
