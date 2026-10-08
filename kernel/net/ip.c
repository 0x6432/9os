/*
 * IPv4 (RFC 791/1122): input validation, local delivery, the routing table, output with
 * fragmentation, reassembly, and ICMP (echo, destination unreachable, time exceeded; errors
 * are reported to the transport that sent the offending datagram). 9os is a host, not a
 * router: datagrams for other addresses are dropped. Runs under net_mutex.
 */
#include <kernel/net.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>

struct list_node routes = LIST_INIT(routes);
int sysctl_ip_forward, sysctl_ip_default_ttl = 64, sysctl_icmp_echo_ignore_all, sysctl_icmp_echo_ignore_broadcasts = 1;
int sysctl_ipv6_forwarding, sysctl_ipv6_disable;
static uint16_t ip_ident;
uint64_t ip_fwd_stats;
uint64_t ip_stats[8];        /* in, delivered, out, frag_ok, reasm_ok, bad, noroute, icmp_in */

/* ------------------------------------------------------------------ routing */
static int prefix_len(uint32_t mask) { return __builtin_popcount(mask); }

int ip_route(uint32_t dst, int oif, struct netdev **dev, uint32_t *nexthop, uint32_t *src) {
    struct netdev *ld = net_dev_for_local(dst);
    if (ld && (!oif || oif == ld->index || ld == loopback_dev)) {      /* to ourselves: loopback */
        *dev = loopback_dev;
        *nexthop = dst;
        if (src) *src = dst;
        return 0;
    }
    if (dst == INADDR_BROADCAST || ipv4_is_multicast(dst)) {
        struct netdev *d = oif ? netdev_by_index(oif) : nullptr;
        if (!d) list_for_each(it, &netdevs) {
            struct netdev *x = list_entry(it, struct netdev, node);
            if (x != loopback_dev && (x->flags & IFF_UP)) { d = x; break; }
        }
        if (!d || !(d->flags & IFF_UP)) return -ENETUNREACH;
        *dev = d; *nexthop = dst;
        if (src) *src = d->addr;
        return 0;
    }
    struct route *best = nullptr;
    list_for_each(it, &routes) {
        struct route *r = list_entry(it, struct route, node);
        if ((dst & r->mask) != r->dst || !(r->dev->flags & IFF_UP)) continue;
        if (oif && r->dev->index != oif) continue;
        if (!best || prefix_len(r->mask) > prefix_len(best->mask) ||
            (prefix_len(r->mask) == prefix_len(best->mask) && r->metric < best->metric)) best = r;
    }
    if (!best) {
        struct netdev *d = oif ? netdev_by_index(oif) : nullptr;
        if (d && (d->flags & IFF_UP)) {          /* SO_BINDTODEVICE without a route: on-link */
            *dev = d; *nexthop = dst;
            if (src) *src = d->addr;
            return 0;
        }
        ip_stats[6]++;
        return -ENETUNREACH;
    }
    *dev = best->dev;
    *nexthop = best->flags & RTF_GATEWAY ? best->gw : dst;
    if (src) *src = best->dev->addr;
    return 0;
}

void route_flush_dev(struct netdev *d) {
    list_for_each_safe(it, tmp, &routes) {
        struct route *r = list_entry(it, struct route, node);
        if (r->dev == d) { list_del(it); kfree(r); }
    }
}

int route_add(uint32_t dst, uint32_t mask, uint32_t gw, struct netdev *d, int metric, unsigned flags) {
    list_for_each(it, &routes) {
        struct route *r = list_entry(it, struct route, node);
        if (r->dst == dst && r->mask == mask && r->metric == metric && r->dev == d) return -EEXIST;
    }
    struct route *r = kzalloc(sizeof *r);
    if (!r) return -ENOMEM;
    *r = (struct route){ .dst = dst & mask, .mask = mask, .gw = gw, .dev = d, .metric = metric, .flags = flags | RTF_UP };
    list_add_tail(&routes, &r->node);
    return 0;
}
int route_del(uint32_t dst, uint32_t mask, uint32_t gw, struct netdev *d, int metric) {
    list_for_each(it, &routes) {
        struct route *r = list_entry(it, struct route, node);
        if (r->dst == (dst & mask) && r->mask == mask && (!d || r->dev == d) && (!gw || r->gw == gw)) {
            list_del(it); kfree(r); return 0;
        }
    }
    return -ESRCH;
}

/* address/netmask change: replace the device's connected route (Linux does the same) */
void route_add_connected(struct netdev *d) {
    list_for_each_safe(it, tmp, &routes) {
        struct route *r = list_entry(it, struct route, node);
        if (r->dev == d && !(r->flags & RTF_GATEWAY) && r->metric == 0) { list_del(it); kfree(r); }
    }
    if (d->addr && (d->flags & IFF_UP) && d != loopback_dev && d->netmask != 0xffffffff)
        route_add(d->addr & d->netmask, d->netmask, 0, d, 0, 0);
}

/* ------------------------------------------------------------------ output */
/* IP_MULTICAST_LOOP: a copy of a datagram sent to a group joined on the device comes back in
 * as if received there (through the receive queue, so no recursion) */
static void mc_loopback(struct netdev *d, struct pkt *p) {
    struct iphdr *h = (struct iphdr *)p->data;
    if (d == loopback_dev || !mc_dev_has(d, &h->daddr, false)) return;
    struct pkt *c = pkt_alloc_rx(ETH_HLEN + p->len);
    if (!c) return;
    uint32_t g = ntohl(h->daddr);
    uint8_t *e = c->data;
    e[0] = 0x01; e[1] = 0x00; e[2] = 0x5e; e[3] = (g >> 16) & 0x7f; e[4] = (g >> 8) & 0xff; e[5] = g & 0xff;
    memcpy(e + 6, d->hwaddr, 6);
    e[12] = 0x08; e[13] = 0x00;
    memcpy(e + ETH_HLEN, p->data, p->len);
    c->csum_ok = true;
    net_rx(d, c);
}

static void ip_finish(struct netdev *d, struct pkt *p, uint32_t nexthop, bool loop) {
    ip_stats[2]++;
    if (loop && ipv4_is_multicast(nexthop)) mc_loopback(d, p);
    eth_output(d, p, nexthop, ETH_P_IP);
}

static void ip_fragment(struct netdev *d, struct pkt *p, uint32_t nexthop, bool loop) {
    struct iphdr *h = (struct iphdr *)p->data;
    unsigned hl = (h->ver_ihl & 15) * 4, total = ntohs(h->tot_len);
    unsigned room = ((d->mtu - hl) & ~7u);
    unsigned base = (ntohs(h->frag_off) & IP_OFFMASK) * 8;
    bool more_after = ntohs(h->frag_off) & IP_MF;
    for (unsigned off = 0; off < total - hl; off += room) {
        unsigned n = MIN(room, total - hl - off);
        struct pkt *f = pkt_alloc(hl + n);
        if (!f) break;
        f->len = hl + n;
        memcpy(f->data, h, hl);
        memcpy(f->data + hl, p->data + hl + off, n);
        struct iphdr *fh = (struct iphdr *)f->data;
        bool mf = off + n < total - hl || more_after;
        fh->tot_len = htons((uint16_t)(hl + n));
        fh->frag_off = htons((uint16_t)(((base + off) / 8) | (mf ? IP_MF : 0)));
        fh->check = 0;
        fh->check = htons(csum_fold(csum_partial(fh, hl, 0)));
        ip_stats[3]++;
        ip_finish(d, f, nexthop, loop);
    }
    pkt_free(p);
}

static int ip_send_built(struct pkt *p, struct netdev *d, uint32_t nexthop, bool loop) {
    struct iphdr *h = (struct iphdr *)p->data;
    if (p->len > (size_t)d->mtu) {
        if (ntohs(h->frag_off) & IP_DF) { pkt_free(p); return -EMSGSIZE; }
        ip_fragment(d, p, nexthop, loop);
        return 0;
    }
    ip_finish(d, p, nexthop, loop);
    return 0;
}

int ip_output(struct pkt *p, uint32_t src, uint32_t dst, uint8_t proto, const struct ip_opts *o) {
    struct netdev *d; uint32_t nh, psrc;
    int r = ip_route(dst, o ? o->oif : 0, &d, &nh, &psrc);
    if (r) { pkt_free(p); return r; }
    if (!src) src = psrc;
    bool ra = o && o->ra;
    if (ra) { uint8_t *opt = pkt_push(p, 4); opt[0] = 0x94; opt[1] = 4; opt[2] = opt[3] = 0; }  /* Router Alert */
    struct iphdr *h = (struct iphdr *)pkt_push(p, sizeof *h);
    h->ver_ihl = ra ? 0x46 : 0x45;
    h->tos = o ? o->tos : 0;
    h->tot_len = htons((uint16_t)p->len);
    h->id = htons(ip_ident++);
    h->frag_off = o && o->df ? htons(IP_DF) : 0;
    h->ttl = o && o->ttl ? o->ttl : (uint8_t)sysctl_ip_default_ttl;
    h->protocol = proto;
    h->saddr = src;
    h->daddr = dst;
    h->check = 0;
    h->check = htons(csum_fold(csum_partial(h, ra ? 24 : sizeof *h, 0)));
    return ip_send_built(p, d, nh, o && o->mcloop);
}

int ip_output_hdrincl(struct pkt *p, int oif) {
    if (p->len < sizeof(struct iphdr)) { pkt_free(p); return -EINVAL; }
    struct iphdr *h = (struct iphdr *)p->data;
    struct netdev *d; uint32_t nh, psrc;
    int r = ip_route(h->daddr, oif, &d, &nh, &psrc);
    if (r) { pkt_free(p); return r; }
    unsigned hl = (h->ver_ihl & 15) * 4;
    if (hl < 20 || hl > p->len) { pkt_free(p); return -EINVAL; }
    if (!h->saddr) h->saddr = psrc;
    if (!h->id) h->id = htons(ip_ident++);
    h->tot_len = htons((uint16_t)p->len);
    h->check = 0;
    h->check = htons(csum_fold(csum_partial(h, hl, 0)));
    return ip_send_built(p, d, nh, false);
}

/* ------------------------------------------------------------------ ICMP */
struct icmphdr { uint8_t type, code; uint16_t check; uint16_t id, seq; };

static void icmp_send(uint32_t dst, uint32_t src, uint8_t type, uint8_t code, uint32_t rest, const void *data, size_t len) {
    struct pkt *p = pkt_alloc(sizeof(struct icmphdr) + len);
    if (!p) return;
    struct icmphdr *ic = (struct icmphdr *)p->data;
    p->len = sizeof *ic + len;
    ic->type = type; ic->code = code; ic->check = 0;
    memcpy(&ic->id, &rest, 4);
    memcpy(ic + 1, data, len);
    ic->check = htons(csum_fold(csum_partial(ic, p->len, 0)));
    ip_output(p, src, dst, IPPROTO_ICMP, nullptr);
}

/* reply to orig->nh's sender with an error quoting its header + 8 bytes (never about ICMP
 * errors, fragments other than the first, or broadcasts) */
static void icmp_unreach_rest(struct pkt *orig, int type, int code, uint32_t rest);
void icmp_send_unreach(struct pkt *orig, int type, int code) { icmp_unreach_rest(orig, type, code, 0); }
static void icmp_unreach_rest(struct pkt *orig, int type, int code, uint32_t rest) {
    struct iphdr *h = (struct iphdr *)orig->nh;
    unsigned hl = (h->ver_ihl & 15) * 4;
    if (ntohs(h->frag_off) & IP_OFFMASK) return;
    if (h->daddr == INADDR_BROADCAST || ipv4_is_multicast(h->daddr) || !h->saddr) return;
    if (h->protocol == IPPROTO_ICMP) {
        uint8_t t = ((uint8_t *)h)[hl];
        if (t != 0 && t != 8) return;
    }
    size_t q = MIN((size_t)ntohs(h->tot_len), hl + 8u);
    uint32_t src = net_is_local_addr(h->daddr) ? h->daddr : 0;
    icmp_send(h->saddr, src, (uint8_t)type, (uint8_t)code, rest, h, q);
}

/* RFC 1191 "fragmentation needed and DF set" carrying the next-hop MTU */
void icmp_send_unreach_mtu(struct pkt *orig, uint16_t mtu) { icmp_unreach_rest(orig, 3, 4, htonl(mtu)); }

static int icmp_errno(int type, int code) {
    if (type == 3) {
        static const int map[16] = { ENETUNREACH, EHOSTUNREACH, ENOPROTOOPT, ECONNREFUSED, EMSGSIZE, EOPNOTSUPP,
                                     ENETUNREACH, EHOSTDOWN, ENONET, ENETUNREACH, EHOSTUNREACH, ENETUNREACH,
                                     EHOSTUNREACH, EHOSTUNREACH, EHOSTUNREACH, EHOSTUNREACH };
        return map[code & 15];
    }
    if (type == 11) return EHOSTUNREACH;
    return EPROTO;
}

static void icmp_input(struct pkt *p) {
    ip_stats[7]++;
    struct iphdr *h = (struct iphdr *)p->nh;
    struct icmphdr *ic = (struct icmphdr *)p->data;
    if (p->len < sizeof *ic || (!p->csum_ok && csum_fold(csum_partial(p->data, p->len, 0)))) { ip_stats[5]++; pkt_free(p); return; }
    p->th = p->data;
    raw_input(p);
    if (ic->type == 8) {                                      /* echo request */
        bool bc = h->daddr == INADDR_BROADCAST || ipv4_is_multicast(h->daddr) || !net_is_local_addr(h->daddr);
        if (sysctl_icmp_echo_ignore_all || (bc && sysctl_icmp_echo_ignore_broadcasts)) { pkt_free(p); return; }
        icmp_send(h->saddr, bc ? 0 : h->daddr, 0, 0, *(uint32_t *)&ic->id, ic + 1, p->len - sizeof *ic);
    } else if (ic->type == 0) {
        if (ping_input(p)) return;
    } else if ((ic->type == 3 || ic->type == 11) && p->len >= sizeof *ic + sizeof(struct iphdr) + 8) {
        struct iphdr *in = (struct iphdr *)(ic + 1);
        unsigned ihl = (in->ver_ihl & 15) * 4;
        if (p->len >= sizeof *ic + ihl + 8) {
            uint16_t *ports = (uint16_t *)((uint8_t *)in + ihl);
            int err = icmp_errno(ic->type, ic->code);
            if (in->protocol == IPPROTO_UDP) {
                udp_err(in->saddr, ports[0], in->daddr, ports[1], err);
            } else if (in->protocol == IPPROTO_TCP) {
                tcp_err(in->saddr, ports[0], in->daddr, ports[1], err);
            }
        }
    }
    pkt_free(p);
}

/* ------------------------------------------------------------------ reassembly */
struct frag_q {
    struct list_node node;
    uint32_t src, dst;
    uint16_t id;
    uint8_t proto;
    uint8_t *data;               /* 64 KiB payload buffer */
    uint8_t hdr[60]; unsigned hl;
    uint8_t have[8192 / 8];      /* 8-byte units received */
    unsigned total;              /* payload length once the last fragment arrived, else 0 */
    uint64_t expires;
    struct netdev *dev;
};
static struct list_node fragqs = LIST_INIT(fragqs);
static int nfragqs;
static struct ntimer frag_timer;

static void fragq_free(struct frag_q *q) { list_del(&q->node); kfree(q->data); kfree(q); nfragqs--; }

static void frag_tick(struct ntimer *t) {
    uint64_t now = time_ns();
    list_for_each_safe(it, tmp, &fragqs) {
        struct frag_q *q = list_entry(it, struct frag_q, node);
        if (now >= q->expires) fragq_free(q);
    }
    if (!list_empty(&fragqs)) ntimer_mod(t, NS_S);
}

static bool frag_complete(struct frag_q *q) {
    if (!q->total) return false;
    for (unsigned u = 0; u < (q->total + 7) / 8; u++) if (!(q->have[u / 8] & (1 << (u % 8)))) return false;
    return true;
}

static struct pkt *ip_reassemble(struct pkt *p) {
    struct iphdr *h = (struct iphdr *)p->nh;
    unsigned hl = (h->ver_ihl & 15) * 4;
    unsigned off = (ntohs(h->frag_off) & IP_OFFMASK) * 8, len = p->len;
    bool mf = ntohs(h->frag_off) & IP_MF;
    struct frag_q *q = nullptr;
    list_for_each(it, &fragqs) {
        struct frag_q *x = list_entry(it, struct frag_q, node);
        if (x->src == h->saddr && x->dst == h->daddr && x->id == h->id && x->proto == h->protocol) { q = x; break; }
    }
    if (off + len > 65535 - 20 || (mf && (len & 7))) { pkt_free(p); return nullptr; }
    if (!q) {
        if (nfragqs >= 32) { pkt_free(p); return nullptr; }
        q = kzalloc(sizeof *q);
        if (q) q->data = kmalloc(65536);
        if (!q || !q->data) { kfree(q); pkt_free(p); return nullptr; }
        q->src = h->saddr; q->dst = h->daddr; q->id = h->id; q->proto = h->protocol;
        q->expires = time_ns() + 30 * NS_S;
        q->dev = p->dev;
        list_add(&fragqs, &q->node);
        nfragqs++;
        if (!frag_timer.active) { frag_timer.fn = frag_tick; ntimer_mod(&frag_timer, NS_S); }
    }
    if (off == 0) { memcpy(q->hdr, h, hl); q->hl = hl; }
    memcpy(q->data + off, p->data, len);
    for (unsigned u = off / 8; u < (off + len + 7) / 8; u++) q->have[u / 8] |= 1 << (u % 8);
    if (!mf) q->total = off + len;
    pkt_free(p);
    if (!q->hl || !frag_complete(q)) return nullptr;
    struct pkt *r = pkt_alloc(q->hl + q->total);
    if (!r) { fragq_free(q); return nullptr; }
    r->dev = q->dev;
    r->len = q->hl + q->total;
    memcpy(r->data, q->hdr, q->hl);
    memcpy(r->data + q->hl, q->data, q->total);
    struct iphdr *rh = (struct iphdr *)r->data;
    rh->tot_len = htons((uint16_t)r->len);
    rh->frag_off = 0;
    r->nh = r->data;
    pkt_pull(r, q->hl);
    ip_stats[4]++;
    fragq_free(q);
    return r;
}

/* ------------------------------------------------------------------ input */
/* RFC 1812 forwarding of a datagram that is not for us (net.ipv4.ip_forward=1); p->data: IP header */
static void ip_forward(struct netdev *in, struct pkt *p) {
    struct iphdr *h = (struct iphdr *)p->data;
    p->nh = p->data;
    if (p->pkttype != PACKET_HOST || in->type == ARPHRD_LOOPBACK || !h->saddr || ipv4_is_loopback(h->daddr) ||
        ipv4_is_loopback(h->saddr) || ipv4_is_multicast(h->daddr) || ipv4_is_multicast(h->saddr) ||
        (in->addr && h->daddr == (in->addr | ~in->netmask))) { pkt_free(p); return; }
    if (h->ttl <= 1) { icmp_send_unreach(p, 11, 0); pkt_free(p); return; }   /* time exceeded in transit */
    struct netdev *out; uint32_t nh, src;
    if (ip_route(h->daddr, 0, &out, &nh, &src) || out->type == ARPHRD_LOOPBACK) {
        icmp_send_unreach(p, 3, 0); pkt_free(p); return;                      /* network unreachable */
    }
    if (p->len > (size_t)out->mtu && (ntohs(h->frag_off) & IP_DF)) {          /* RFC 1191 */
        icmp_send_unreach_mtu(p, (uint16_t)out->mtu);
        pkt_free(p); return;
    }
    h->ttl--;
    uint32_t c = ntohs(h->check) + 0x0100;                                  /* RFC 1624 incremental update */
    h->check = htons((uint16_t)(c + (c >> 16)));
    ip_fwd_stats++;
    ip_send_built(p, out, nh, false);
}

void ip_input(struct netdev *d, struct pkt *p) {
    ip_stats[0]++;
    if (p->len < sizeof(struct iphdr)) goto bad;
    struct iphdr *h = (struct iphdr *)p->data;
    unsigned hl = (h->ver_ihl & 15) * 4, tot = ntohs(h->tot_len);
    if ((h->ver_ihl >> 4) != 4 || hl < 20 || hl > p->len || tot < hl || tot > p->len) goto bad;
    if (!p->csum_ok && csum_fold(csum_partial(h, hl, 0))) goto bad;
    p->len = tot;                                  /* strip ethernet padding */
    bool mc = ipv4_is_multicast(h->daddr);
    if (mc && d != loopback_dev && ntohl(h->daddr) != 0xe0000001u && !mc_dev_has(d, &h->daddr, false) &&
        !(d->flags & IFF_PROMISC)) { pkt_free(p); return; }      /* a group nobody joined here */
    bool local = net_is_local_addr(h->daddr) || h->daddr == INADDR_BROADCAST || (d->bcast && h->daddr == d->bcast) ||
                 (d->addr && h->daddr == (d->addr | ~d->netmask)) || mc ||
                 (!d->addr && p->pkttype == PACKET_HOST);         /* DHCP before configuration */
    if (!local) {
        if (sysctl_ip_forward) ip_forward(d, p); else pkt_free(p);
        return;
    }
    p->nh = p->data;
    pkt_pull(p, hl);
    if (ntohs(h->frag_off) & (IP_MF | IP_OFFMASK)) {
        p = ip_reassemble(p);
        if (!p) return;
        h = (struct iphdr *)p->nh;
    }
    ip_stats[1]++;
    switch (h->protocol) {
    case IPPROTO_ICMP: icmp_input(p); return;
    case IPPROTO_IGMP: raw_input(p); igmp_input(p); return;
    case IPPROTO_UDP: raw_input(p); udp_input(p); return;
    case IPPROTO_TCP: raw_input(p); tcp_input(p); return;
    default:
        raw_input(p);
        if (!(h->daddr == INADDR_BROADCAST || ipv4_is_multicast(h->daddr))) icmp_send_unreach(p, 3, 2);
        pkt_free(p);
        return;
    }
bad:
    ip_stats[5]++;
    d->rx_errors++;
    pkt_free(p);
}
