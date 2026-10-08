/*
 * M32b: IPv6 (RFC 8200) for a host: input with extension headers (hop-by-hop and destination
 * options, routing headers with no segments left, fragments + reassembly), output with
 * fragmentation, the IPv6 routing table, address management with Duplicate Address Detection,
 * Neighbour Discovery (RFC 4861: NS/NA resolution with a per-neighbour queue, router
 * solicitation and advertisements), SLAAC (RFC 4862: EUI-64 link-local addresses on every
 * ethernet device brought up, global addresses from RA prefixes with the A flag), MLDv1
 * (RFC 2710) messages for mcast.c, and ICMPv6 (RFC 4443: echo, errors reported to transports).
 * Optional forwarding (net.ipv6.conf.all.forwarding) with hop limit / packet too big errors.
 *
 * One timer (ip6_timer) drives DAD probes, router solicitations, neighbour retries, lifetimes
 * of RA-learnt addresses and routes, and reassembly expiry. Runs under net_mutex.
 */
#include <kernel/net6.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>

void eth_send(struct netdev *d, struct pkt *p, const uint8_t *mac, uint16_t type);

static struct list_node ifaddrs = LIST_INIT(ifaddrs), rt6s = LIST_INIT(rt6s), nds = LIST_INIT(nds);
static struct ntimer ip6_timer;
uint64_t ip6_stats[8];       /* in, delivered, out, bad, noroute, frag_creates, reasm_ok, icmp_in */
uint64_t icmp6_stats[4];     /* in, out, in_errors, echo_replies */
int sysctl_ipv6_hop_limit = 64, sysctl_ipv6_accept_ra = 1, sysctl_ipv6_dad_transmits = 1, sysctl_ipv6_autoconf = 1;
int sysctl_icmpv6_echo_ignore_all;

#define ND_RETRIES 3
#define ND_REACHABLE (30 * NS_S)
#define ND_GC (600 * NS_S)
#define RS_INTERVAL (4 * NS_S)
#define RS_COUNT 3

static const uint8_t all_nodes[16] = { 0xff, 0x02, [15] = 1 };
static const uint8_t all_routers[16] = { 0xff, 0x02, [15] = 2 };
static const uint8_t in6_any[16];

bool ip6_prefix_eq(const uint8_t *a, const uint8_t *b, int plen) {
    if (plen <= 0) return true;
    if (plen > 128) plen = 128;
    int n = plen / 8, r = plen % 8;
    if (memcmp(a, b, (size_t)n)) return false;
    if (!r) return true;
    uint8_t m = (uint8_t)(0xff << (8 - r));
    return ((a[n] ^ b[n]) & m) == 0;
}
static void prefix_mask(uint8_t *out, const uint8_t *a, int plen) {
    for (int i = 0; i < 16; i++) {
        int bits = plen - i * 8;
        uint8_t m = bits >= 8 ? 0xff : bits <= 0 ? 0 : (uint8_t)(0xff << (8 - bits));
        out[i] = a[i] & m;
    }
}
static int common_prefix(const uint8_t *a, const uint8_t *b) {
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t x = a[i] ^ b[i];
        if (!x) { n += 8; continue; }
        return n + __builtin_clz((unsigned)x) - 24;
    }
    return n;
}
static int addr_scope(const uint8_t *a) {
    if (ip6_loopback(a)) return RT_SCOPE_HOST;
    if (ip6_linklocal(a)) return RT_SCOPE_LINK;
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0xc0) return RT_SCOPE_SITE;
    if (ip6_multicast(a)) {
        int s = a[1] & 0xf;
        return s <= 1 ? RT_SCOPE_HOST : s == 2 ? RT_SCOPE_LINK : s <= 5 ? RT_SCOPE_SITE : RT_SCOPE_UNIVERSE;
    }
    return RT_SCOPE_UNIVERSE;
}
uint32_t csum_pseudo6(const uint8_t *s, const uint8_t *d, uint8_t proto, uint32_t len) {
    uint32_t t[2] = { htonl(len), htonl(proto) };
    uint32_t sum = csum_partial(s, 16, 0);
    sum = csum_partial(d, 16, sum);
    return csum_partial(t, 8, sum);
}
static void solicited_node(uint8_t *out, const uint8_t *a) {
    static const uint8_t sn[13] = { 0xff, 0x02, [11] = 1, [12] = 0xff };
    memcpy(out, sn, 13);
    memcpy(out + 13, a + 13, 3);
}
static void eui64(uint8_t *iid, const struct netdev *d) {
    const uint8_t *m = d->hwaddr;
    iid[0] = m[0] ^ 2; iid[1] = m[1]; iid[2] = m[2]; iid[3] = 0xff; iid[4] = 0xfe;
    iid[5] = m[3]; iid[6] = m[4]; iid[7] = m[5];
}
static void ip6_kick(uint64_t delay) {
    if (!ip6_timer.active || ip6_timer.when > time_ns() + delay) ntimer_mod(&ip6_timer, delay);
}

/* ------------------------------------------------------------------ addresses */
static bool ifa_usable(const struct inet6_ifaddr *a) { return !(a->flags & (IFA_F_TENTATIVE | IFA_F_DADFAILED)); }
static struct inet6_ifaddr *ifa_find(struct netdev *d, const uint8_t *a) {
    list_for_each(it, &ifaddrs) {
        struct inet6_ifaddr *x = list_entry(it, struct inet6_ifaddr, node);
        if ((!d || x->dev == d) && ip6_eq(x->addr, a)) return x;
    }
    return nullptr;
}
struct netdev *ip6_dev_for_local(const uint8_t *a) {
    if (ip6_loopback(a)) return loopback_dev;
    list_for_each(it, &ifaddrs) {
        struct inet6_ifaddr *x = list_entry(it, struct inet6_ifaddr, node);
        if (ip6_eq(x->addr, a) && ifa_usable(x) && (x->dev->flags & IFF_UP)) return x->dev;
    }
    return nullptr;
}
bool ip6_is_local(const uint8_t *a) { return ip6_dev_for_local(a) != nullptr; }

/* RFC 6724 in brief: same device, an address whose scope covers dst's, not deprecated,
 * longest matching prefix */
static const struct inet6_ifaddr *src_select(struct netdev *d, const uint8_t *dst) {
    int ds = addr_scope(dst);
    const struct inet6_ifaddr *best = nullptr;
    int bscore = -1;
    list_for_each(it, &ifaddrs) {
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if (!ifa_usable(a) || !(a->dev->flags & IFF_UP)) continue;
        if (ds == RT_SCOPE_LINK && (a->dev != d || a->scope != RT_SCOPE_LINK)) continue;
        if (a->scope == RT_SCOPE_HOST && d != loopback_dev) continue;
        if (a->scope == RT_SCOPE_LINK && a->dev != d) continue;
        int score = (a->dev == d ? 1000 : 0);
        if (a->scope == RT_SCOPE_LINK) score += ds == RT_SCOPE_LINK ? 500 : 0;
        else score += ds != RT_SCOPE_LINK ? 500 : 0;
        if (!(a->flags & IFA_F_DEPRECATED)) score += 200;
        score += common_prefix(a->addr, dst);
        if (score > bscore) { best = a; bscore = score; }
    }
    return best;
}
static const uint8_t *linklocal_of(struct netdev *d) {
    list_for_each(it, &ifaddrs) {
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if (a->dev == d && a->scope == RT_SCOPE_LINK && ifa_usable(a)) return a->addr;
    }
    return nullptr;
}

static void ns_send(struct netdev *d, const uint8_t *src, const uint8_t *dst, const uint8_t *target);
static void rs_send(struct netdev *d);

static void dad_start(struct inet6_ifaddr *a) {
    if (a->dev == loopback_dev || (a->flags & IFA_F_NODAD) || (a->dev->flags & IFF_NOARP) || !sysctl_ipv6_dad_transmits) {
        a->flags &= ~IFA_F_TENTATIVE;
        if (a->scope == RT_SCOPE_LINK && (a->dev->flags & IFF_UP) && a->dev->type == ARPHRD_ETHER && sysctl_ipv6_accept_ra) {
            a->dev->rs_left = RS_COUNT; a->dev->rs_at = time_ns(); ip6_kick(NS_MS);
        }
        return;
    }
    a->flags |= IFA_F_TENTATIVE;
    a->flags &= ~IFA_F_DADFAILED;
    a->dad_left = sysctl_ipv6_dad_transmits;
    a->dad_at = time_ns() + random_u64() % (100 * NS_MS);
    if (a->dev->flags & IFF_UP) ip6_kick(NS_MS);
}
static void dad_done(struct inet6_ifaddr *a) {
    a->flags &= ~IFA_F_TENTATIVE;
    struct netdev *d = a->dev;
    if (a->scope == RT_SCOPE_LINK && d->type == ARPHRD_ETHER && sysctl_ipv6_accept_ra && !sysctl_ipv6_forwarding) {
        d->rs_left = RS_COUNT;
        d->rs_at = time_ns();
        ip6_kick(NS_MS);
    }
}
static void dad_failed(struct inet6_ifaddr *a) {
    if (a->flags & IFA_F_DADFAILED) return;
    printk("ipv6: %s: duplicate address detected\n", a->dev->name);
    a->flags |= IFA_F_DADFAILED | IFA_F_TENTATIVE;
    a->dad_left = 0;
}

static void ifa_free(struct inet6_ifaddr *a) {
    uint8_t sn[16];
    solicited_node(sn, a->addr);
    if (a->dev != loopback_dev) mc_dev_leave(a->dev, sn, true);
    list_del(&a->node);
    kfree(a);
}

int ip6_addr_add(struct netdev *d, const uint8_t *addr, int plen, unsigned flags, uint32_t pref, uint32_t valid, bool excl) {
    if (ip6_multicast(addr) || ip6_any(addr) || plen < 1 || plen > 128) return -EINVAL;
    if (pref > valid) return -EINVAL;
    struct inet6_ifaddr *a = ifa_find(d, addr);
    if (a) {
        if (excl) return -EEXIST;
        a->plen = plen; a->pref = pref; a->valid = valid; a->tstamp = time_ns();
        if (flags & IFA_F_PERMANENT) a->flags |= IFA_F_PERMANENT;
        return 0;
    }
    if (!(a = kzalloc(sizeof *a))) return -ENOBUFS;
    a->dev = d;
    memcpy(a->addr, addr, 16);
    a->plen = plen;
    a->scope = addr_scope(addr);
    a->flags = flags & (IFA_F_PERMANENT | IFA_F_NODAD);
    a->pref = pref; a->valid = valid; a->tstamp = time_ns();
    list_add_tail(&ifaddrs, &a->node);
    if (d != loopback_dev) {
        uint8_t sn[16];
        solicited_node(sn, addr);
        mc_dev_join(d, sn, true);
    }
    if (plen < 128 && a->scope != RT_SCOPE_LINK) {
        uint8_t pfx[16];
        prefix_mask(pfx, addr, plen);
        ip6_route_add(pfx, plen, nullptr, d, 256, RTPROT_KERNEL);
    }
    dad_start(a);
    if (pref != FOREVER || valid != FOREVER) ip6_kick(NS_S);
    return 0;
}

int ip6_addr_del(struct netdev *d, const uint8_t *addr, int plen) {
    struct inet6_ifaddr *a = ifa_find(d, addr);
    if (!a || (plen && a->plen != plen)) return -EADDRNOTAVAIL;
    if (a->plen < 128 && a->scope != RT_SCOPE_LINK) {
        uint8_t pfx[16];
        prefix_mask(pfx, addr, a->plen);
        bool other = false;                              /* another address still uses the prefix */
        list_for_each(it, &ifaddrs) {
            struct inet6_ifaddr *x = list_entry(it, struct inet6_ifaddr, node);
            if (x != a && x->dev == d && x->plen == a->plen && ip6_prefix_eq(x->addr, pfx, a->plen)) other = true;
        }
        if (!other) {
            list_for_each_safe(it, tmp, &rt6s) {
                struct rt6_info *r = list_entry(it, struct rt6_info, node);
                if (r->dev == d && r->proto == RTPROT_KERNEL && r->plen == a->plen && ip6_eq(r->dst, pfx)) { list_del(it); kfree(r); }
            }
        }
    }
    ifa_free(a);
    return 0;
}

void ip6_addr_dump(void (*cb)(void *ctx, const struct inet6_ifaddr *a), void *ctx, int ifindex) {
    uint64_t now = time_ns();
    list_for_each(it, &ifaddrs) {
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if (ifindex && a->dev->index != ifindex) continue;
        struct inet6_ifaddr c = *a;
        uint32_t el = (uint32_t)((now - a->tstamp) / NS_S);
        if (c.valid != FOREVER) c.valid = c.valid > el ? c.valid - el : 0;
        if (c.pref != FOREVER) c.pref = c.pref > el ? c.pref - el : 0;
        cb(ctx, &c);
    }
}

/* ------------------------------------------------------------------ routes */
static struct rt6_info *rt6_find(const uint8_t *dst, int plen, const uint8_t *gw, struct netdev *d) {
    list_for_each(it, &rt6s) {
        struct rt6_info *r = list_entry(it, struct rt6_info, node);
        if (r->plen != plen || !ip6_eq(r->dst, dst)) continue;
        if (d && r->dev != d) continue;
        if (gw && (!r->has_gw || !ip6_eq(r->gw, gw))) continue;
        return r;
    }
    return nullptr;
}

int ip6_route_add(const uint8_t *dst0, int plen, const uint8_t *gw, struct netdev *d, int metric, int proto) {
    if (plen < 0 || plen > 128) return -EINVAL;
    uint8_t dst[16];
    prefix_mask(dst, dst0, plen);
    if (!d && gw) {                                         /* the device the gateway is on-link at */
        struct netdev *gd; uint8_t nh[16];
        if (ip6_linklocal(gw) || ip6_route(gw, 0, &gd, nh, nullptr) || gd == loopback_dev) return -ENETUNREACH;
        d = gd;
    }
    if (!d) return -ENODEV;
    list_for_each(it, &rt6s) {
        struct rt6_info *r = list_entry(it, struct rt6_info, node);
        if (r->plen == plen && ip6_eq(r->dst, dst) && r->dev == d && r->metric == metric) return -EEXIST;
    }
    struct rt6_info *r = kzalloc(sizeof *r);
    if (!r) return -ENOMEM;
    memcpy(r->dst, dst, 16);
    r->plen = plen;
    if (gw) { memcpy(r->gw, gw, 16); r->has_gw = true; }
    r->dev = d; r->metric = metric; r->proto = (uint8_t)proto;
    list_add_tail(&rt6s, &r->node);
    return 0;
}

int ip6_route_del(const uint8_t *dst0, int plen, const uint8_t *gw, struct netdev *d) {
    uint8_t dst[16];
    prefix_mask(dst, dst0, plen);
    struct rt6_info *r = rt6_find(dst, plen, gw, d);
    if (!r) return -ESRCH;
    list_del(&r->node);
    kfree(r);
    return 0;
}

void ip6_route_dump(void (*cb)(void *ctx, const struct rt6_info *r), void *ctx) {
    list_for_each(it, &rt6s) cb(ctx, list_entry(it, struct rt6_info, node));
}

static void rt6_flush_dev(struct netdev *d) {
    list_for_each_safe(it, tmp, &rt6s) {
        struct rt6_info *r = list_entry(it, struct rt6_info, node);
        if (r->dev == d) { list_del(it); kfree(r); }
    }
}

int ip6_route(const uint8_t *dst, int oif, struct netdev **dev, uint8_t *nexthop, uint8_t *src) {
    struct netdev *ld = ip6_dev_for_local(dst);
    if (ld && (!oif || oif == ld->index || ld == loopback_dev || oif == loopback_dev->index)) {
        *dev = loopback_dev;
        memcpy(nexthop, dst, 16);
        if (src) memcpy(src, dst, 16);
        return 0;
    }
    struct netdev *d = nullptr;
    const struct rt6_info *best = nullptr;
    if (ip6_multicast(dst)) {
        d = oif ? netdev_by_index(oif) : nullptr;
        if (!d) list_for_each(it, &netdevs) {
            struct netdev *x = list_entry(it, struct netdev, node);
            if (x != loopback_dev && (x->flags & IFF_UP) && (x->flags & IFF_MULTICAST)) { d = x; break; }
        }
        if (!d && (dst[1] & 0xf) == 1) d = loopback_dev;           /* interface-local */
        if (!d || !(d->flags & IFF_UP)) { ip6_stats[4]++; return -ENETUNREACH; }
        memcpy(nexthop, dst, 16);
    } else {
        list_for_each(it, &rt6s) {
            struct rt6_info *r = list_entry(it, struct rt6_info, node);
            if (!(r->dev->flags & IFF_UP) || !ip6_prefix_eq(dst, r->dst, r->plen)) continue;
            if (oif && r->dev->index != oif) continue;
            if (!best || r->plen > best->plen || (r->plen == best->plen && r->metric < best->metric)) best = r;
        }
        if (best) {
            d = best->dev;
            memcpy(nexthop, best->has_gw ? best->gw : dst, 16);
        } else {
            d = oif ? netdev_by_index(oif) : nullptr;               /* scope id / bound device: on-link */
            if (!d || !(d->flags & IFF_UP)) { ip6_stats[4]++; return -ENETUNREACH; }
            memcpy(nexthop, dst, 16);
        }
    }
    *dev = d;
    if (src) {
        const struct inet6_ifaddr *a = src_select(d, dst);
        memcpy(src, a ? a->addr : in6_any, 16);
    }
    return 0;
}

int ip6_sock_mtu(const uint8_t *dst, int oif) {
    struct netdev *d; uint8_t nh[16];
    if (ip6_route(dst, oif, &d, nh, nullptr)) return -ENOTCONN;
    return d->mtu;
}

/* ------------------------------------------------------------------ neighbour discovery cache */
#define NUD_INCOMPLETE 0x01
#define NUD_REACHABLE 0x02
#define NUD_STALE 0x04
#define NUD_FAILED 0x20
#define NUD_PERMANENT 0x80
#define ND_MAX_QUEUE 8

struct nd_entry {
    struct list_node node;
    struct netdev *dev;
    uint8_t ip[16], mac[6];
    int state, tries;
    bool router;
    uint64_t next_try, expires, used;
    struct list_node queue;
    int nqueue;
};

static struct nd_entry *nd_find(struct netdev *d, const uint8_t *ip) {
    list_for_each(it, &nds) {
        struct nd_entry *n = list_entry(it, struct nd_entry, node);
        if (n->dev == d && ip6_eq(n->ip, ip)) return n;
    }
    return nullptr;
}
static struct nd_entry *nd_new(struct netdev *d, const uint8_t *ip) {
    struct nd_entry *n = kzalloc(sizeof *n);
    if (!n) return nullptr;
    n->dev = d;
    memcpy(n->ip, ip, 16);
    list_init(&n->queue);
    n->used = time_ns();
    list_add(&nds, &n->node);
    return n;
}
static void nd_free(struct nd_entry *n) {
    list_del(&n->node);
    pkt_queue_purge(&n->queue);
    kfree(n);
}
static void nd_flush_dev(struct netdev *d) {
    list_for_each_safe(it, tmp, &nds) {
        struct nd_entry *n = list_entry(it, struct nd_entry, node);
        if (n->dev == d) nd_free(n);
    }
}
/* the link-layer address is known now: state, then the frames that waited for it */
static void nd_resolved(struct nd_entry *n, const uint8_t *mac, int state) {
    memcpy(n->mac, mac, 6);
    n->state = state;
    n->tries = 0;
    n->expires = time_ns() + ND_REACHABLE;
    list_for_each_safe(it, tmp, &n->queue) {
        struct pkt *p = list_entry(it, struct pkt, node);
        list_del(it);
        eth_send(n->dev, p, n->mac, ETH_P_IPV6);
    }
    n->nqueue = 0;
}
/* a link-layer address option from the neighbour itself (NS, RS, RA): RFC 4861 7.2.3 */
static void nd_learn(struct netdev *d, const uint8_t *ip, const uint8_t *mac, bool router) {
    struct nd_entry *n = nd_find(d, ip);
    if (!n && !(n = nd_new(d, ip))) return;
    if (router) n->router = true;
    if (n->state == NUD_PERMANENT) return;
    if (n->state == NUD_INCOMPLETE || n->state == 0 || memcmp(n->mac, mac, 6)) nd_resolved(n, mac, NUD_STALE);
}

void nd_dump(void (*cb)(void *ctx, struct netdev *d, const uint8_t *ip, const uint8_t *mac, int state), void *ctx) {
    list_for_each(it, &nds) {
        struct nd_entry *n = list_entry(it, struct nd_entry, node);
        cb(ctx, n->dev, n->ip, n->state & (NUD_REACHABLE | NUD_STALE | NUD_PERMANENT) ? n->mac : nullptr,
           (n->state ? n->state : NUD_INCOMPLETE) | (n->router ? 0x10000 : 0));   /* 0x10000: router */
    }
}
int nd_set(struct netdev *d, const uint8_t *ip, const uint8_t *mac, bool perm) {
    struct nd_entry *n = nd_find(d, ip);
    if (!n && !(n = nd_new(d, ip))) return -ENOMEM;
    nd_resolved(n, mac, perm ? NUD_PERMANENT : NUD_REACHABLE);
    return 0;
}
int nd_del(struct netdev *d, const uint8_t *ip) {
    struct nd_entry *n = nd_find(d, ip);
    if (!n) return -ENOENT;
    nd_free(n);
    return 0;
}

static void nd_solicit(struct nd_entry *n, struct pkt *p) {
    /* source: the queued packet's own source when it is ours on this device (RFC 4861 7.2.2) */
    const uint8_t *src = nullptr;
    if (p) {
        struct ip6hdr *h = (struct ip6hdr *)p->data;
        struct inet6_ifaddr *a = ifa_find(n->dev, h->src);
        if (a && ifa_usable(a)) src = a->addr;
    }
    if (!src) { const struct inet6_ifaddr *a = src_select(n->dev, n->ip); if (a) src = a->addr; }
    if (!src) return;
    uint8_t sn[16];
    solicited_node(sn, n->ip);
    ns_send(n->dev, src, sn, n->ip);
}

/* transmit an IPv6 datagram (p->data at its header) to nexthop on d */
static void nd_output(struct netdev *d, struct pkt *p, const uint8_t *nexthop) {
    if ((d->flags & IFF_NOARP) || d->type != ARPHRD_ETHER) { eth_output(d, p, 0, ETH_P_IPV6); return; }
    if (ip6_multicast(nexthop)) {
        uint8_t m[6] = { 0x33, 0x33, nexthop[12], nexthop[13], nexthop[14], nexthop[15] };
        eth_send(d, p, m, ETH_P_IPV6);
        return;
    }
    uint64_t now = time_ns();
    struct nd_entry *n = nd_find(d, nexthop);
    if (n && (n->state & (NUD_REACHABLE | NUD_STALE | NUD_PERMANENT))) {
        n->used = now;
        eth_send(d, p, n->mac, ETH_P_IPV6);
        if (n->state == NUD_STALE && now >= n->next_try) {        /* reconfirm with a unicast probe */
            n->next_try = now + 5 * NS_S;
            const struct inet6_ifaddr *a = src_select(d, n->ip);
            if (a) ns_send(d, a->addr, n->ip, n->ip);
        }
        return;
    }
    if (!n && !(n = nd_new(d, nexthop))) { d->tx_dropped++; pkt_free(p); return; }
    if (n->nqueue >= ND_MAX_QUEUE) {
        struct pkt *o = list_first(&n->queue, struct pkt, node);
        list_del(&o->node);
        pkt_free(o);
        n->nqueue--;
        d->tx_dropped++;
    }
    list_add_tail(&n->queue, &p->node);
    n->nqueue++;
    if (!n->tries) {
        n->state = NUD_INCOMPLETE;
        n->tries = 1;
        n->next_try = now + NS_S;
        nd_solicit(n, p);
        ip6_kick(NS_S);
    }
}

/* ------------------------------------------------------------------ output */
static uint32_t frag_ident;

static void mc6_loopback(struct netdev *d, struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->data;
    if (d == loopback_dev || !mc_dev_has(d, h->dst, true)) return;
    struct pkt *c = pkt_alloc_rx(ETH_HLEN + p->len);
    if (!c) return;
    uint8_t *e = c->data;
    e[0] = 0x33; e[1] = 0x33; memcpy(e + 2, h->dst + 12, 4);
    memcpy(e + 6, d->hwaddr, 6);
    e[12] = 0x86; e[13] = 0xdd;
    memcpy(e + ETH_HLEN, p->data, p->len);
    c->csum_ok = true;
    net_rx(d, c);
}

static void ip6_finish(struct netdev *d, struct pkt *p, const uint8_t *nexthop, bool loop) {
    ip6_stats[2]++;
    if (loop && ip6_multicast(nexthop)) mc6_loopback(d, p);
    nd_output(d, p, nexthop);
}

/* RFC 8200 4.5: the unfragmentable part (header + hop-by-hop options) in every fragment */
static int ip6_fragment(struct netdev *d, struct pkt *p, const uint8_t *nexthop, bool loop) {
    struct ip6hdr *h = (struct ip6hdr *)p->data;
    size_t unfrag = sizeof *h;
    uint8_t *nxtp = &h->nxt;
    if (h->nxt == IPPROTO_HOPOPTS && p->len >= unfrag + 8) {
        nxtp = p->data + unfrag;
        unfrag += (size_t)(p->data[unfrag + 1] + 1) * 8;
    }
    if (unfrag > p->len) { pkt_free(p); return -EINVAL; }
    size_t total = p->len - unfrag;
    size_t room = ((size_t)d->mtu - unfrag - 8) & ~(size_t)7;
    uint8_t nxt = *nxtp;
    uint32_t id = htonl(++frag_ident);
    for (size_t off = 0; off < total; off += room) {
        size_t n = MIN(room, total - off);
        struct pkt *f = pkt_alloc(unfrag + 8 + n);
        if (!f) break;
        f->len = unfrag + 8 + n;
        memcpy(f->data, p->data, unfrag);
        *(f->data + (nxtp - p->data)) = IPPROTO_FRAGMENT;
        uint8_t *fh = f->data + unfrag;
        fh[0] = nxt; fh[1] = 0;
        uint16_t ol = (uint16_t)(off | (off + n < total ? 1 : 0));
        fh[2] = (uint8_t)(ol >> 8); fh[3] = (uint8_t)ol;
        memcpy(fh + 4, &id, 4);
        memcpy(fh + 8, p->data + unfrag + off, n);
        ((struct ip6hdr *)f->data)->plen = htons((uint16_t)(f->len - sizeof *h));
        ip6_stats[5]++;
        ip6_finish(d, f, nexthop, loop);
    }
    pkt_free(p);
    return 0;
}

static int ip6_send_built(struct pkt *p, struct netdev *d, const uint8_t *nexthop, bool loop, bool dontfrag) {
    if (p->len > (size_t)d->mtu) {
        if (dontfrag || d->mtu < 1280) { pkt_free(p); return -EMSGSIZE; }
        return ip6_fragment(d, p, nexthop, loop);
    }
    ip6_finish(d, p, nexthop, loop);
    return 0;
}

int ip6_output(struct pkt *p, const uint8_t *src, const uint8_t *dst, uint8_t proto, const struct ip6_opts *o) {
    struct netdev *d; uint8_t nh[16], psrc[16];
    int r = ip6_route(dst, o ? o->oif : 0, &d, nh, psrc);
    if (r) { pkt_free(p); return r; }
    if (!src) src = psrc;
    uint8_t nxt = proto;
    if (o && o->ra) {                                         /* hop-by-hop: Router Alert (MLD) + PadN */
        uint8_t *hb = pkt_push(p, 8);
        hb[0] = proto; hb[1] = 0; hb[2] = 5; hb[3] = 2; hb[4] = hb[5] = 0; hb[6] = 1; hb[7] = 0;
        nxt = IPPROTO_HOPOPTS;
    }
    struct ip6hdr *h = (struct ip6hdr *)pkt_push(p, sizeof *h);
    h->vtc_flow = htonl(6u << 28 | (uint32_t)(o ? o->tclass : 0) << 20);
    h->plen = htons((uint16_t)(p->len - sizeof *h));
    h->nxt = nxt;
    h->hlim = (uint8_t)(o && o->hlim > 0 ? o->hlim : ip6_multicast(dst) ? 1 : sysctl_ipv6_hop_limit);
    memcpy(h->src, src, 16);
    memcpy(h->dst, dst, 16);
    return ip6_send_built(p, d, nh, o && o->mcloop, o && o->dontfrag);
}

/* ------------------------------------------------------------------ ICMPv6 */
struct icmp6hdr { uint8_t type, code; uint16_t check; uint32_t data; };

static int icmp6_xmit(const uint8_t *src, const uint8_t *dst, int type, int code, uint32_t data,
                      const void *body, size_t blen, const struct ip6_opts *o) {
    struct pkt *p = pkt_alloc(sizeof(struct icmp6hdr) + blen + 8);   /* + room for a hop-by-hop header */
    if (!p) return -ENOBUFS;
    struct icmp6hdr *ic = (struct icmp6hdr *)p->data;
    p->len = sizeof *ic + blen;
    ic->type = (uint8_t)type; ic->code = (uint8_t)code; ic->check = 0; ic->data = data;
    memcpy(ic + 1, body, blen);
    uint8_t s[16];
    if (src) memcpy(s, src, 16);
    else {
        struct netdev *d; uint8_t nh[16];
        int r = ip6_route(dst, o ? o->oif : 0, &d, nh, s);
        if (r) { pkt_free(p); return r; }
    }
    ic->check = htons(csum_fold(csum_partial(ic, p->len, csum_pseudo6(s, dst, IPPROTO_ICMPV6, (uint32_t)p->len))));
    icmp6_stats[1]++;
    return ip6_output(p, s, dst, IPPROTO_ICMPV6, o);
}

void icmp6_send_error(struct pkt *orig, int type, int code, uint32_t info) {
    struct ip6hdr *h = (struct ip6hdr *)orig->nh;
    if (ip6_any(h->src) || ip6_multicast(h->src)) return;
    if (ip6_multicast(h->dst) && type != 2 && !(type == 4 && code == 2)) return;
    if (h->nxt == IPPROTO_ICMPV6) {                           /* never about an ICMPv6 error */
        uint8_t *t = orig->nh + sizeof *h;
        if (t < orig->data + orig->len && *t < 128) return;
    }
    size_t have = (size_t)(orig->data + orig->len - orig->nh);
    size_t q = MIN(have, MIN((size_t)ntohs(h->plen) + sizeof *h, (size_t)(1280 - 40 - 8)));
    struct ip6_opts o = { 0 };
    if (ip6_needs_scope(h->src) && orig->dev) o.oif = orig->dev->index;
    const uint8_t *src = ip6_is_local(h->dst) ? h->dst : nullptr;
    uint8_t copy[1232];
    memcpy(copy, h, q);
    icmp6_xmit(src, copy + 8, type, code, htonl(info), copy, q, &o);
}

static int icmp6_errno(int type, int code) {
    if (type == 1) {
        static const int map[8] = { ENETUNREACH, EACCES, EHOSTUNREACH, EHOSTUNREACH, ECONNREFUSED, EACCES, EACCES, EHOSTUNREACH };
        return map[code & 7];
    }
    if (type == 2) return EMSGSIZE;
    if (type == 3) return EHOSTUNREACH;
    return EPROTO;
}

/* the transport header inside an ICMPv6 error's quoted datagram */
static void icmp6_error_input(struct pkt *p, int type, int code) {
    if (p->len < 8 + sizeof(struct ip6hdr) + 4) return;
    struct ip6hdr *in = (struct ip6hdr *)(p->data + 8);
    uint8_t *q = (uint8_t *)(in + 1), *end = p->data + p->len;
    uint8_t nxt = in->nxt;
    while ((nxt == IPPROTO_HOPOPTS || nxt == IPPROTO_DSTOPTS || nxt == IPPROTO_ROUTING || nxt == IPPROTO_FRAGMENT) && q + 8 <= end) {
        uint8_t n2 = q[0];
        if (nxt == IPPROTO_FRAGMENT) { if ((q[2] << 8 | q[3]) & ~7) return; q += 8; }
        else q += (q[1] + 1) * 8;
        nxt = n2;
    }
    if (q + 4 > end) return;
    uint16_t sport, dport;
    memcpy(&sport, q, 2); memcpy(&dport, q + 2, 2);
    int err = icmp6_errno(type, code);
    if (nxt == IPPROTO_UDP) udp6_err(in->src, sport, in->dst, dport, err);
    else if (nxt == IPPROTO_TCP) tcp6_err(in->src, sport, in->dst, dport, err);
}

static void ns_send(struct netdev *d, const uint8_t *src, const uint8_t *dst, const uint8_t *target) {
    uint8_t b[16 + 8];
    memcpy(b, target, 16);
    size_t n = 16;
    if (!ip6_any(src)) { b[16] = 1; b[17] = 1; memcpy(b + 18, d->hwaddr, 6); n += 8; }   /* source link-layer address */
    struct ip6_opts o = { .hlim = 255, .oif = d->index };
    icmp6_xmit(src, dst, 135, 0, 0, b, n, &o);
}
static void na_send(struct netdev *d, const uint8_t *src, const uint8_t *dst, const uint8_t *target, bool solicited) {
    uint8_t b[16 + 8];
    memcpy(b, target, 16);
    b[16] = 2; b[17] = 1; memcpy(b + 18, d->hwaddr, 6);         /* target link-layer address */
    uint32_t fl = (solicited ? 0x40000000u : 0) | 0x20000000u | (sysctl_ipv6_forwarding ? 0x80000000u : 0);
    struct ip6_opts o = { .hlim = 255, .oif = d->index };
    icmp6_xmit(src, dst, 136, 0, htonl(fl), b, sizeof b, &o);
}
static void rs_send(struct netdev *d) {
    const uint8_t *ll = linklocal_of(d);
    uint8_t b[8] = { 1, 1 };
    memcpy(b + 2, d->hwaddr, 6);
    struct ip6_opts o = { .hlim = 255, .oif = d->index };
    icmp6_xmit(ll ? ll : in6_any, all_routers, 133, 0, 0, b, ll ? 8 : 0, &o);
}

void mld_send(struct netdev *d, const uint8_t *grp, int type) {
    if (!(d->flags & IFF_UP) || d->type == ARPHRD_LOOPBACK || sysctl_ipv6_disable) return;
    const uint8_t *ll = linklocal_of(d);
    struct ip6_opts o = { .hlim = 1, .oif = d->index, .ra = true };
    icmp6_xmit(ll ? ll : in6_any, type == 132 ? all_routers : grp, type, 0, 0, grp, 16, &o);
}

/* find option `type` in an ND message's options [p, end) */
static const uint8_t *nd_opt(const uint8_t *p, const uint8_t *end, int type, bool *bad) {
    const uint8_t *found = nullptr;
    while (p + 2 <= end) {
        size_t l = (size_t)p[1] * 8;
        if (!l || p + l > end) { *bad = true; return nullptr; }
        if (p[0] == type && !found) found = p;
        p += l;
    }
    return found;
}

static void ns_input(struct netdev *d, struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    if (h->hlim != 255 || p->len < 24 || p->data[1]) return;
    const uint8_t *target = p->data + 8;
    if (ip6_multicast(target)) return;
    bool bad = false, any = ip6_any(h->src);
    const uint8_t *sl = nd_opt(p->data + 24, p->data + p->len, 1, &bad);
    if (bad || (any && sl)) return;
    if (any) { uint8_t sn[16]; solicited_node(sn, target); if (!ip6_eq(h->dst, sn)) return; }
    struct inet6_ifaddr *a = ifa_find(d, target);
    if (!a) return;
    if (a->flags & IFA_F_TENTATIVE) { if (any) dad_failed(a); return; }
    if (a->flags & IFA_F_DADFAILED) return;
    if (!any && sl && sl[1] == 1) nd_learn(d, h->src, sl + 2, false);
    na_send(d, target, any ? all_nodes : h->src, target, !any);
}

static void na_input(struct netdev *d, struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    if (h->hlim != 255 || p->len < 24 || p->data[1]) return;
    uint8_t fl = p->data[4];
    bool router = fl & 0x80, solicited = fl & 0x40, override = fl & 0x20;
    const uint8_t *target = p->data + 8;
    if (ip6_multicast(target) || (ip6_multicast(h->dst) && solicited)) return;
    bool bad = false;
    const uint8_t *tl = nd_opt(p->data + 24, p->data + p->len, 2, &bad);
    if (bad) return;
    struct inet6_ifaddr *a = ifa_find(d, target);
    if (a) { if (a->flags & IFA_F_TENTATIVE) dad_failed(a); return; }
    struct nd_entry *n = nd_find(d, target);
    if (!n || n->state == NUD_PERMANENT) return;
    const uint8_t *mac = tl && tl[1] == 1 ? tl + 2 : nullptr;
    if (n->state == NUD_INCOMPLETE || n->state == 0) {
        if (!mac) return;
        nd_resolved(n, mac, solicited ? NUD_REACHABLE : NUD_STALE);
    } else {
        bool same = !mac || !memcmp(mac, n->mac, 6);
        if (!override && !same) { if (n->state == NUD_REACHABLE) n->state = NUD_STALE; return; }
        if (mac && !same) memcpy(n->mac, mac, 6);
        if (solicited) { n->state = NUD_REACHABLE; n->expires = time_ns() + ND_REACHABLE; n->next_try = 0; }
        else if (!same) n->state = NUD_STALE;
    }
    n->router = router;
}

static void ra_input(struct netdev *d, struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    if (h->hlim != 255 || !ip6_linklocal(h->src) || p->len < 16 || p->data[1]) return;
    if (!sysctl_ipv6_accept_ra || sysctl_ipv6_forwarding || d->type != ARPHRD_ETHER) return;
    const uint8_t *opt = p->data + 16, *end = p->data + p->len;
    bool bad = false;
    nd_opt(opt, end, 0, &bad);
    if (bad) return;
    d->rs_left = 0;                                       /* answered */
    uint64_t now = time_ns();
    uint16_t life = (uint16_t)(p->data[6] << 8 | p->data[7]);
    struct rt6_info *def = rt6_find(in6_any, 0, h->src, d);
    if (life) {
        if (!def && !ip6_route_add(in6_any, 0, h->src, d, 1024, RTPROT_RA)) def = rt6_find(in6_any, 0, h->src, d);
        if (def) def->expires = now + (uint64_t)life * NS_S;
        ip6_kick(NS_S);
    } else if (def) { list_del(&def->node); kfree(def); }
    if (p->data[4]) d->hlim6 = p->data[4];
    for (const uint8_t *o = opt; o + 2 <= end && o[1]; o += o[1] * 8) {
        if (o[0] == 1 && o[1] == 1) nd_learn(d, h->src, o + 2, true);
        else if (o[0] == 5 && o[1] == 1) {                       /* MTU */
            uint32_t mtu = (uint32_t)o[4] << 24 | o[5] << 16 | o[6] << 8 | o[7];
            if (mtu >= 1280 && mtu <= 1500 && (int)mtu < d->mtu) d->mtu = (int)mtu;
        } else if (o[0] == 3 && o[1] == 4) {                     /* prefix information */
            int plen = o[2];
            uint8_t fl = o[3];
            uint32_t valid = (uint32_t)o[4] << 24 | o[5] << 16 | o[6] << 8 | o[7];
            uint32_t pref = (uint32_t)o[8] << 24 | o[9] << 16 | o[10] << 8 | o[11];
            const uint8_t *pfx = o + 16;
            if (ip6_linklocal(pfx) || ip6_multicast(pfx) || plen > 128 || pref > valid) continue;
            if (fl & 0x80) {                                     /* on-link */
                struct rt6_info *r = rt6_find(pfx, plen, nullptr, d);
                if (r && r->has_gw) r = nullptr;
                if (valid) {
                    if (!r && !ip6_route_add(pfx, plen, nullptr, d, 256, RTPROT_RA)) r = rt6_find(pfx, plen, nullptr, d);
                    if (r && r->proto == RTPROT_RA) r->expires = valid == FOREVER ? 0 : now + (uint64_t)valid * NS_S;
                } else if (r && r->proto == RTPROT_RA) { list_del(&r->node); kfree(r); }
            }
            if ((fl & 0x40) && plen == 64 && sysctl_ipv6_autoconf) {   /* SLAAC */
                uint8_t a[16];
                memcpy(a, pfx, 8);
                eui64(a + 8, d);
                struct inet6_ifaddr *ifa = ifa_find(d, a);
                if (ifa) {
                    if (!ifa->autoconf) continue;
                    /* RFC 4862 5.5.3 e): never cut a lifetime below two hours from an RA */
                    uint32_t el = (uint32_t)((now - ifa->tstamp) / NS_S);
                    uint32_t left = ifa->valid == FOREVER ? FOREVER : ifa->valid > el ? ifa->valid - el : 0;
                    if (valid > 7200 || valid > left) ifa->valid = valid;
                    else ifa->valid = left > 7200 ? 7200 : left;
                    ifa->pref = pref > ifa->valid ? ifa->valid : pref;
                    ifa->tstamp = now;
                    if (ifa->pref) ifa->flags &= ~IFA_F_DEPRECATED;
                } else if (valid) {
                    if (!ip6_addr_add(d, a, 64, 0, pref, valid, true) && (ifa = ifa_find(d, a))) ifa->autoconf = true;
                }
            }
        }
    }
}

static void icmp6_input(struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    struct netdev *d = p->dev;
    icmp6_stats[0]++;
    if (p->len < 4 || (!p->csum_ok && csum_fold(csum_partial(p->data, p->len, csum_pseudo6(h->src, h->dst, IPPROTO_ICMPV6, (uint32_t)p->len))))) {
        icmp6_stats[2]++;
        pkt_free(p);
        return;
    }
    p->th = p->data;
    int type = p->data[0], code = p->data[1];
    bool tentative_dst = false;
    if (!ip6_multicast(h->dst) && !ip6_loopback(h->dst)) {
        struct inet6_ifaddr *a = ifa_find(nullptr, h->dst);
        tentative_dst = a && !ifa_usable(a);
    }
    if (tentative_dst && type != 135 && type != 136) { pkt_free(p); return; }
    raw6_input(p, IPPROTO_ICMPV6);
    switch (type) {
    case 128:                                                   /* echo request */
        if (sysctl_icmpv6_echo_ignore_all || p->len < 8) break;
        {
            struct ip6_opts o = { 0 };
            if (ip6_needs_scope(h->src) && d) o.oif = d->index;
            uint32_t idseq; memcpy(&idseq, p->data + 4, 4);
            const uint8_t *src = ip6_multicast(h->dst) ? nullptr : h->dst;
            uint8_t dst[16]; memcpy(dst, h->src, 16);
            icmp6_xmit(src, dst, 129, 0, idseq, p->data + 8, p->len - 8, &o);
        }
        break;
    case 129:
        icmp6_stats[3]++;
        if (ping6_input(p)) return;
        break;
    case 1: case 2: case 3: case 4:
        icmp6_error_input(p, type, code);
        break;
    case 130:                                                   /* MLD query (v1, or v2 read as v1) */
        if (p->len >= 24 && d && d != loopback_dev && ip6_linklocal(h->src)) {
            uint16_t maxd = (uint16_t)(p->data[4] << 8 | p->data[5]);
            const uint8_t *g = p->data + 8;
            mc_query(d, ip6_any(g) ? nullptr : g, true, (uint64_t)(maxd ? maxd : 1) * NS_MS);
        }
        break;
    case 131:
        if (p->len >= 24 && d && d != loopback_dev) mc_heard_report(d, p->data + 8, true);
        break;
    case 134: if (d && code == 0) ra_input(d, p); break;
    case 135: if (d && code == 0 && d != loopback_dev) ns_input(d, p); break;
    case 136: if (d && code == 0 && d != loopback_dev) na_input(d, p); break;
    }
    pkt_free(p);
}

/* ------------------------------------------------------------------ reassembly */
struct frag6_q {
    struct list_node node;
    uint8_t src[16], dst[16];
    uint32_t id;
    uint8_t nxt;
    uint8_t hdr[40];
    bool have_first;
    uint8_t *data;
    uint8_t have[8192 / 8];
    unsigned total;
    uint64_t expires;
    struct netdev *dev;
};
static struct list_node frag6qs = LIST_INIT(frag6qs);
static int nfrag6qs;

static void frag6_free(struct frag6_q *q) { list_del(&q->node); kfree(q->data); kfree(q); nfrag6qs--; }

/* p->data at the fragment header. Returns the reassembled datagram (data after the IPv6 header)
 * and its first next-header, or nullptr */
static struct pkt *ip6_reassemble(struct pkt *p, uint8_t *nxt) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    if (p->len < 8) { pkt_free(p); return nullptr; }
    uint8_t *fh = p->data;
    unsigned off = (unsigned)((fh[2] << 8 | fh[3]) & ~7);
    bool mf = fh[3] & 1;
    uint32_t id; memcpy(&id, fh + 4, 4);
    uint8_t fnxt = fh[0];
    pkt_pull(p, 8);
    unsigned len = (unsigned)p->len;
    if (!off && !mf) { *nxt = fnxt; return p; }                    /* atomic fragment */
    if (mf && (len & 7)) { icmp6_send_error(p, 4, 0, 4); pkt_free(p); return nullptr; }
    if (off + len > 65535) { pkt_free(p); return nullptr; }
    struct frag6_q *q = nullptr;
    list_for_each(it, &frag6qs) {
        struct frag6_q *x = list_entry(it, struct frag6_q, node);
        if (x->id == id && ip6_eq(x->src, h->src) && ip6_eq(x->dst, h->dst)) { q = x; break; }
    }
    if (!q) {
        if (nfrag6qs >= 32) { pkt_free(p); return nullptr; }
        q = kzalloc(sizeof *q);
        if (q) q->data = kmalloc(65536);
        if (!q || !q->data) { kfree(q); pkt_free(p); return nullptr; }
        memcpy(q->src, h->src, 16); memcpy(q->dst, h->dst, 16);
        q->id = id;
        q->expires = time_ns() + 60 * NS_S;
        q->dev = p->dev;
        list_add(&frag6qs, &q->node);
        nfrag6qs++;
        ip6_kick(NS_S);
    }
    if (off == 0) { memcpy(q->hdr, h, 40); q->nxt = fnxt; q->have_first = true; }
    memcpy(q->data + off, p->data, len);
    for (unsigned u = off / 8; u < (off + len + 7) / 8; u++) q->have[u / 8] |= (uint8_t)(1 << (u % 8));
    if (!mf) q->total = off + len;
    pkt_free(p);
    if (!q->have_first || !q->total) return nullptr;
    for (unsigned u = 0; u < (q->total + 7) / 8; u++) if (!(q->have[u / 8] & (1 << (u % 8)))) return nullptr;
    struct pkt *r = pkt_alloc(40 + q->total);
    if (!r) { frag6_free(q); return nullptr; }
    r->dev = q->dev;
    r->len = 40 + q->total;
    memcpy(r->data, q->hdr, 40);
    memcpy(r->data + 40, q->data, q->total);
    struct ip6hdr *rh = (struct ip6hdr *)r->data;
    rh->plen = htons((uint16_t)q->total);
    rh->nxt = q->nxt;
    r->nh = r->data;
    pkt_pull(r, 40);
    *nxt = q->nxt;
    ip6_stats[6]++;
    frag6_free(q);
    return r;
}

/* ------------------------------------------------------------------ input */
/* TLV options of a hop-by-hop / destination options header; false: dropped (maybe with an error) */
static bool ip6_options(struct pkt *p, const uint8_t *o, size_t len) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    for (size_t i = 2; i < len;) {
        uint8_t t = o[i];
        if (t == 0) { i++; continue; }                           /* Pad1 */
        if (i + 2 > len || i + 2 + o[i + 1] > len) return false;
        if (t != 1 && t != 5) {                                  /* not PadN / Router Alert */
            switch (t >> 6) {
            case 0: break;
            case 1: return false;
            case 3: if (ip6_multicast(h->dst)) return false;
                /* fallthrough */
            case 2: icmp6_send_error(p, 4, 2, (uint32_t)(o + i - p->nh)); return false;
            }
        }
        i += 2 + o[i + 1];
    }
    return true;
}

static void ip6_forward(struct netdev *in, struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->data;
    p->nh = p->data;
    if (p->pkttype != PACKET_HOST || in == loopback_dev || ip6_multicast(h->dst) || ip6_linklocal(h->src) ||
        ip6_linklocal(h->dst) || ip6_loopback(h->dst) || ip6_any(h->src)) { pkt_free(p); return; }
    if (h->hlim <= 1) { icmp6_send_error(p, 3, 0, 0); pkt_free(p); return; }
    struct netdev *out; uint8_t nh[16];
    if (ip6_route(h->dst, 0, &out, nh, nullptr) || out == loopback_dev) { icmp6_send_error(p, 1, 0, 0); pkt_free(p); return; }
    if (p->len > (size_t)out->mtu) { icmp6_send_error(p, 2, 0, (uint32_t)out->mtu); pkt_free(p); return; }
    h->hlim--;
    ip6_finish(out, p, nh, false);
}

void ip6_input(struct netdev *d, struct pkt *p) {
    ip6_stats[0]++;
    if (sysctl_ipv6_disable || p->len < sizeof(struct ip6hdr)) goto bad;
    struct ip6hdr *h = (struct ip6hdr *)p->data;
    if ((ntohl(h->vtc_flow) >> 28) != 6 || sizeof *h + ntohs(h->plen) > p->len || ip6_multicast(h->src)) goto bad;
    p->len = sizeof *h + ntohs(h->plen);
    if (ip6_multicast(h->dst)) {
        if (d != loopback_dev && !ip6_eq(h->dst, all_nodes) && !mc_dev_has(d, h->dst, true) && !(d->flags & IFF_PROMISC)) {
            pkt_free(p); return;
        }
    } else if (!ifa_find(nullptr, h->dst) && !(ip6_loopback(h->dst) && d == loopback_dev)) {
        if (sysctl_ipv6_forwarding) ip6_forward(d, p); else pkt_free(p);
        return;
    }
    if ((ip6_loopback(h->dst) || ip6_loopback(h->src)) && d != loopback_dev) goto bad;
    p->nh = p->data;
    pkt_pull(p, sizeof *h);
    uint8_t nxt = h->nxt;
    size_t nxt_off = 6;                                           /* where the current next-header value sits */
    for (bool first = true;; first = false) {
        if (nxt != IPPROTO_HOPOPTS && nxt != IPPROTO_DSTOPTS && nxt != IPPROTO_ROUTING && nxt != IPPROTO_FRAGMENT) break;
        if (nxt == IPPROTO_HOPOPTS && !first) { icmp6_send_error(p, 4, 1, (uint32_t)nxt_off); goto drop; }
        if (p->len < 8) goto bad;
        if (nxt == IPPROTO_FRAGMENT) {
            p = ip6_reassemble(p, &nxt);
            if (!p) return;
            h = (struct ip6hdr *)p->nh;
            nxt_off = (size_t)(p->data - p->nh) - 8;
            continue;
        }
        size_t hl = (size_t)(p->data[1] + 1) * 8;
        if (hl > p->len) goto bad;
        if (nxt == IPPROTO_ROUTING) {
            if (p->data[3]) { icmp6_send_error(p, 4, 0, (uint32_t)(p->data + 2 - p->nh)); goto drop; }   /* segments left */
        } else if (!ip6_options(p, p->data, hl)) goto drop;
        nxt_off = (size_t)(p->data - p->nh);
        nxt = p->data[0];
        pkt_pull(p, hl);
    }
    ip6_stats[1]++;
    switch (nxt) {
    case IPPROTO_ICMPV6: icmp6_input(p); return;
    case IPPROTO_UDP: raw6_input(p, nxt); udp6_input(p); return;
    case IPPROTO_TCP: raw6_input(p, nxt); tcp_input(p); return;
    case IPPROTO_NONE: goto drop;
    default:
        raw6_input(p, nxt);
        if (!ip6_multicast(h->dst)) icmp6_send_error(p, 4, 1, (uint32_t)nxt_off);
        goto drop;
    }
bad:
    ip6_stats[3]++;
    d->rx_errors++;
drop:
    pkt_free(p);
}

/* ------------------------------------------------------------------ timer */
static void ip6_tick(struct ntimer *t) {
    uint64_t now = time_ns();
    bool busy = false, life = false;
    list_for_each_safe(it, tmp, &ifaddrs) {
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if (a->valid != FOREVER || a->pref != FOREVER) {
            uint64_t el = (now - a->tstamp) / NS_S;
            if (a->valid != FOREVER && el >= a->valid) { ip6_addr_del(a->dev, a->addr, 0); continue; }
            if (a->pref != FOREVER && el >= a->pref) a->flags |= IFA_F_DEPRECATED;
            life = true;
        }
        if ((a->flags & IFA_F_TENTATIVE) && !(a->flags & IFA_F_DADFAILED) && (a->dev->flags & IFF_UP)) {
            busy = true;
            if (now < a->dad_at) continue;
            if (a->dad_left > 0) {
                uint8_t sn[16];
                solicited_node(sn, a->addr);
                ns_send(a->dev, in6_any, sn, a->addr);
                a->dad_left--;
                a->dad_at = now + NS_S;
            } else dad_done(a);
        }
    }
    list_for_each(it, &netdevs) {
        struct netdev *d = list_entry(it, struct netdev, node);
        if (d->rs_left <= 0 || !(d->flags & IFF_UP)) continue;
        busy = true;
        if (now < d->rs_at) continue;
        rs_send(d);
        d->rs_left--;
        d->rs_at = now + RS_INTERVAL;
    }
    list_for_each_safe(it, tmp, &rt6s) {
        struct rt6_info *r = list_entry(it, struct rt6_info, node);
        if (!r->expires) continue;
        if (now >= r->expires) { list_del(it); kfree(r); continue; }
        life = true;
    }
    list_for_each_safe(it, tmp, &nds) {
        struct nd_entry *n = list_entry(it, struct nd_entry, node);
        if (n->state == NUD_PERMANENT) continue;
        if (n->state == NUD_REACHABLE) { if (now >= n->expires) n->state = NUD_STALE; life = true; continue; }
        if (n->state == NUD_STALE) { if (now >= n->used + ND_GC) nd_free(n); else life = true; continue; }
        busy = true;
        if (now < n->next_try) continue;
        if (n->tries >= ND_RETRIES) {
            n->dev->tx_errors += (uint64_t)n->nqueue;
            if (!list_empty(&n->queue)) {                     /* address unreachable */
                struct pkt *q = list_first(&n->queue, struct pkt, node);
                q->nh = q->data;
                icmp6_send_error(q, 1, 3, 0);
            }
            nd_free(n);
            continue;
        }
        n->tries++;
        n->next_try = now + NS_S;
        nd_solicit(n, list_empty(&n->queue) ? nullptr : list_first(&n->queue, struct pkt, node));
    }
    list_for_each_safe(it, tmp, &frag6qs) {
        struct frag6_q *q = list_entry(it, struct frag6_q, node);
        if (now >= q->expires) frag6_free(q); else life = true;
    }
    if (busy) ntimer_mod(t, 100 * NS_MS);
    else if (life) ntimer_mod(t, NS_S);
}

/* ------------------------------------------------------------------ device up / down */
void netdev_up_hook(struct netdev *d) {
    if (d == loopback_dev || sysctl_ipv6_disable) return;
    if (d->flags & IFF_MULTICAST) mc_dev_join(d, all_nodes, true);
    list_for_each(it, &ifaddrs) {                       /* addresses configured while down: DAD now */
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if (a->dev == d) dad_start(a);
    }
    if (d->type != ARPHRD_ETHER) return;
    uint8_t ll[16] = { 0xfe, 0x80 };
    eui64(ll + 8, d);
    static const uint8_t fe80[16] = { 0xfe, 0x80 };
    ip6_route_add(fe80, 64, nullptr, d, 256, RTPROT_KERNEL);
    ip6_addr_add(d, ll, 64, IFA_F_PERMANENT, FOREVER, FOREVER, false);
}

/* like Linux with keep_addr_on_down=0: the device loses its IPv6 addresses, routes and neighbours */
void netdev_down_hook(struct netdev *d) {
    if (d == loopback_dev) return;
    list_for_each_safe(it, tmp, &ifaddrs) {
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if (a->dev == d) ifa_free(a);
    }
    rt6_flush_dev(d);
    nd_flush_dev(d);
    if (mc_dev_has(d, all_nodes, true)) mc_dev_leave(d, all_nodes, true);
    d->rs_left = 0;
}

void ip6_init(void) {
    ip6_timer.fn = ip6_tick;
    static const uint8_t lo6[16] = { [15] = 1 };
    ip6_addr_add(loopback_dev, lo6, 128, IFA_F_PERMANENT, FOREVER, FOREVER, false);
    ip6_route_add(lo6, 128, nullptr, loopback_dev, 256, RTPROT_KERNEL);
}

/* ------------------------------------------------------------------ /proc/net */
static int hex16(char *b, size_t max, const uint8_t *a) {
    int n = 0;
    for (int i = 0; i < 16 && (size_t)n < max; i++) n += snprintf(b + n, max - n, "%02x", a[i]);
    return n;
}
int net_proc_if_inet6(char *buf, size_t max) {
    mutex_lock(&net_mutex);
    int n = 0;
    list_for_each(it, &ifaddrs) {
        struct inet6_ifaddr *a = list_entry(it, struct inet6_ifaddr, node);
        if ((size_t)n + 80 >= max) break;
        int sc = a->scope == RT_SCOPE_HOST ? 0x10 : a->scope == RT_SCOPE_LINK ? 0x20 : a->scope == RT_SCOPE_SITE ? 0x40 : 0;
        n += hex16(buf + n, max - n, a->addr);
        n += snprintf(buf + n, max - n, " %02x %02x %02x %02x %8s\n", a->dev->index, a->plen, sc, a->flags & 0xff, a->dev->name);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}
int net_proc_ipv6_route(char *buf, size_t max) {
    mutex_lock(&net_mutex);
    int n = 0;
    list_for_each(it, &rt6s) {
        struct rt6_info *r = list_entry(it, struct rt6_info, node);
        if ((size_t)n + 160 >= max) break;
        unsigned fl = 1 | (r->has_gw ? 2 : 0) | (r->plen == 128 ? 4 : 0) | (r->proto == RTPROT_RA ? 0x40000 : 0) |
                      (r->expires ? 0x400000 : 0) | (r->proto == RTPROT_RA && !r->plen ? 0x10000 : 0);
        n += hex16(buf + n, max - n, r->dst);
        n += snprintf(buf + n, max - n, " %02x ", r->plen);
        n += hex16(buf + n, max - n, in6_any);
        n += snprintf(buf + n, max - n, " 00 ");
        n += hex16(buf + n, max - n, r->has_gw ? r->gw : in6_any);
        n += snprintf(buf + n, max - n, " %08x 00000001 00000000 %08x %8s\n", r->metric, fl, r->dev->name);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}
int net_proc_snmp6(char *buf, size_t max) {
    static const char *const names[] = { "Ip6InReceives", "Ip6InDelivers", "Ip6OutRequests", "Ip6InHdrErrors",
                                         "Ip6OutNoRoutes", "Ip6FragCreates", "Ip6ReasmOKs" };
    int n = 0;
    for (int i = 0; i < 7; i++) n += snprintf(buf + n, max - n, "%-32s%lu\n", names[i], ip6_stats[i]);
    n += snprintf(buf + n, max - n, "%-32s%lu\n%-32s%lu\n%-32s%lu\n%-32s%lu\n", "Icmp6InMsgs", icmp6_stats[0], "Icmp6InErrors",
                  icmp6_stats[2], "Icmp6OutMsgs", icmp6_stats[1], "Icmp6InEchoReplies", icmp6_stats[3]);
    return MIN(n, (int)max);
}
