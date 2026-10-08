/*
 * Datagram transports: UDP (RFC 768), raw IP sockets, ICMP "ping" sockets (SOCK_DGRAM,
 * IPPROTO_ICMP: unprivileged echo, the kernel owns the identifier) and AF_PACKET taps.
 * Received datagrams are queued on the socket as packets; runs under net_mutex.
 */
#include <kernel/net.h>
#include <kernel/net6.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/errno.h>

struct list_node udp_socks = LIST_INIT(udp_socks), raw_socks = LIST_INIT(raw_socks), packet_socks = LIST_INIT(packet_socks);
struct udphdr { uint16_t sport, dport, len, check; };
struct icmphdr { uint8_t type, code; uint16_t check; uint16_t id, seq; };
uint64_t udp_stats[4];       /* in, noport, inerr, out */

static bool is_bcast(struct netdev *d, uint32_t a) {
    return a == INADDR_BROADCAST || ipv4_is_multicast(a) || (d && d->addr && a == (d->addr | ~d->netmask));
}

/* ------------------------------------------------------------------ UDP */
static bool udp_match(struct sock *s, struct iphdr *h, struct udphdr *u, struct netdev *d) {
    if (s->lport != u->dport || !sock_v4ok(s)) return false;
    if (ipv4_is_multicast(h->daddr)) {                   /* bound to ANY or to the group, and a member */
        if (s->laddr && s->laddr != h->daddr) return false;
        if (!sock_mc_allowed(s, &h->daddr, false, d ? d->index : 0)) return false;
    } else if (s->laddr && s->laddr != h->daddr && !is_bcast(d, h->daddr)) return false;
    if (s->connected && (s->raddr != h->saddr || s->rport != u->sport)) return false;
    if (s->bound_dev && d && s->bound_dev != d->index && d != loopback_dev) return false;
    return true;
}

void udp_input(struct pkt *p) {
    struct iphdr *h = (struct iphdr *)p->nh;
    struct udphdr *u = (struct udphdr *)p->data;
    udp_stats[0]++;
    if (p->len < sizeof *u || ntohs(u->len) < sizeof *u || ntohs(u->len) > p->len) goto bad;
    p->len = ntohs(u->len);
    if (u->check && !p->csum_ok &&
        csum_fold(csum_partial(u, p->len, csum_pseudo(h->saddr, h->daddr, IPPROTO_UDP, (uint16_t)p->len)))) goto bad;
    p->th = p->data;
    pkt_pull(p, sizeof *u);
    if (is_bcast(p->dev, h->daddr)) {
        list_for_each(it, &udp_socks) {
            struct sock *s = list_entry(it, struct sock, node);
            if (!udp_match(s, h, u, p->dev)) continue;
            struct pkt *c = pkt_clone(p);
            if (c) sock_queue_rx(s, c);
        }
        pkt_free(p);
        return;
    }
    struct sock *best = nullptr; int score = -1;
    list_for_each(it, &udp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (!udp_match(s, h, u, p->dev)) continue;
        int sc = (s->connected ? 2 : 0) + (s->laddr ? 1 : 0);
        if (sc > score) { best = s; score = sc; }
    }
    if (best) { sock_queue_rx(best, p); return; }
    udp_stats[1]++;
    icmp_send_unreach(p, 3, 3);
    pkt_free(p);
    return;
bad:
    udp_stats[2]++;
    pkt_free(p);
}

/* ICMP error about a datagram we sent from laddr:lport to raddr:rport */
void udp_err(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport, int err) {
    list_for_each(it, &udp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->lport != lport || (s->laddr && s->laddr != laddr) || !sock_v4ok(s)) continue;
        /* like Linux: only connected sockets (or IP_RECVERR ones) hear about errors */
        if (!s->connected && !s->recverr) continue;
        if (s->connected && (s->raddr != raddr || s->rport != rport)) continue;
        s->err = err;
        sock_changed(s);
    }
}

/* output interface for a datagram to a multicast group: IP_MULTICAST_IF, else SO_BINDTODEVICE */
int mc_oif(struct sock *s) {
    if (s->mc_ifindex) return s->mc_ifindex;
    if (s->mc_addr) { struct netdev *d = net_dev_for_local(s->mc_addr); if (d) return d->index; }
    return s->bound_dev;
}

int udp_send(struct sock *s, const uint8_t *data, size_t len, uint32_t daddr, uint16_t dport) {
    if (len > 65507) return -EMSGSIZE;
    struct netdev *d; uint32_t nh, src;
    bool mc = ipv4_is_multicast(daddr);
    int oif = mc ? mc_oif(s) : s->bound_dev;
    int r = ip_route(daddr, oif, &d, &nh, &src);
    if (r) return r;
    if (!mc && is_bcast(d, daddr) && !s->broadcast) return -EACCES;
    if (s->laddr && !ipv4_is_multicast(s->laddr) && s->laddr != INADDR_BROADCAST) src = s->laddr;
    struct pkt *p = pkt_alloc(sizeof(struct udphdr) + len);
    if (!p) return -ENOBUFS;
    struct udphdr *u = (struct udphdr *)p->data;
    p->len = sizeof *u + len;
    u->sport = s->lport; u->dport = dport;
    u->len = htons((uint16_t)p->len);
    u->check = 0;
    memcpy(u + 1, data, len);
    uint16_t c = csum_fold(csum_partial(u, p->len, csum_pseudo(src, daddr, IPPROTO_UDP, (uint16_t)p->len)));
    u->check = htons(c ? c : 0xffff);
    udp_stats[3]++;
    struct ip_opts o = { mc ? (s->mc_ttl ? s->mc_ttl : 1) : (uint8_t)s->ttl, (uint8_t)s->tos, false, oif, mc && s->mc_loop, false };
    r = ip_output(p, src, daddr, IPPROTO_UDP, &o);
    return r ? r : (int)len;
}

int net_proc_udp(char *buf, size_t max, bool raw) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    int i = 0;
    list_for_each(it, raw ? &raw_socks : &udp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if ((size_t)n >= max) break;
        if (raw && s->type != 3) continue;                 /* ping sockets live here too */
        if (s->family != AF_INET) continue;
        n += snprintf(buf + n, max - n, "%5d: %08X:%04X %08X:%04X %02X %08X:%08X 00:00000000 00000000 %5u        0 %lu 2 0000000000000000 0\n",
                      i++, s->laddr, raw ? s->protocol : ntohs(s->lport), s->raddr, raw ? 0 : ntohs(s->rport),
                      s->connected ? TCP_ESTABLISHED : TCP_CLOSE, 0, (unsigned)s->rxbytes, s->uid, s->ino);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}

/* ------------------------------------------------------------------ raw + ping */
/* raw sockets get the whole datagram, IP header included (ICMP: after icmp_input checked it) */
void raw_input(struct pkt *p) {
    struct iphdr *h = (struct iphdr *)p->nh;
    list_for_each(it, &raw_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->type != 3 || s->protocol != h->protocol || s->family != AF_INET) continue;   /* SOCK_RAW */
        if (s->laddr && s->laddr != h->daddr) continue;
        if (s->connected && s->raddr != h->saddr) continue;
        if (s->bound_dev && p->dev && s->bound_dev != p->dev->index) continue;
        if (h->protocol == IPPROTO_ICMP && p->len >= 1 && p->data[0] < 32 && (s->icmp_filter & (1u << p->data[0]))) continue;
        struct pkt *c = pkt_clone(p);
        if (!c) continue;
        size_t hl = c->data - c->nh;
        pkt_push(c, hl);                     /* back to the IP header */
        sock_queue_rx(s, c);
    }
}

bool ping_input(struct pkt *p) {
    struct icmphdr *ic = (struct icmphdr *)p->data;
    struct iphdr *h = (struct iphdr *)p->nh;
    list_for_each(it, &raw_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->type != 2 || s->lport != ic->id || s->family != AF_INET) continue;   /* SOCK_DGRAM ping socket */
        if (s->laddr && s->laddr != h->daddr) continue;
        if (s->connected && s->raddr != h->saddr) continue;
        sock_queue_rx(s, p);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ IPv6: UDP, raw, ping */
static bool udp6_match(struct sock *s, struct ip6hdr *h, struct udphdr *u, struct netdev *d) {
    if (s->lport != u->dport || !sock_v6ok(s)) return false;
    bool la = !ip6_any(s->laddr6);
    if (ip6_multicast(h->dst)) {
        if (la && !ip6_eq(s->laddr6, h->dst)) return false;
        if (!sock_mc_allowed(s, h->dst, true, d ? d->index : 0)) return false;
    } else if (la && !ip6_eq(s->laddr6, h->dst)) return false;
    if (s->connected && s->v6 && (!ip6_eq(s->raddr6, h->src) || s->rport != u->sport)) return false;
    if (s->bound_dev && d && s->bound_dev != d->index && d != loopback_dev) return false;
    return true;
}

void udp6_input(struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    struct udphdr *u = (struct udphdr *)p->data;
    udp_stats[0]++;
    if (p->len < sizeof *u || ntohs(u->len) < sizeof *u || ntohs(u->len) > p->len) goto bad;
    p->len = ntohs(u->len);
    if (!u->check) goto bad;                               /* mandatory over IPv6 */
    if (!p->csum_ok && csum_fold(csum_partial(u, p->len, csum_pseudo6(h->src, h->dst, IPPROTO_UDP, (uint32_t)p->len)))) goto bad;
    p->th = p->data;
    pkt_pull(p, sizeof *u);
    if (ip6_multicast(h->dst)) {
        list_for_each(it, &udp_socks) {
            struct sock *s = list_entry(it, struct sock, node);
            if (!udp6_match(s, h, u, p->dev)) continue;
            struct pkt *c = pkt_clone(p);
            if (c) sock_queue_rx(s, c);
        }
        pkt_free(p);
        return;
    }
    struct sock *best = nullptr; int score = -1;
    list_for_each(it, &udp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (!udp6_match(s, h, u, p->dev)) continue;
        int sc = (s->connected ? 2 : 0) + (!ip6_any(s->laddr6) ? 1 : 0);
        if (sc > score) { best = s; score = sc; }
    }
    if (best) { sock_queue_rx(best, p); return; }
    udp_stats[1]++;
    icmp6_send_error(p, 1, 4, 0);                          /* port unreachable */
    pkt_free(p);
    return;
bad:
    udp_stats[2]++;
    pkt_free(p);
}

void udp6_err(const uint8_t *laddr, uint16_t lport, const uint8_t *raddr, uint16_t rport, int err) {
    list_for_each(it, &udp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->lport != lport || !sock_v6ok(s) || (!ip6_any(s->laddr6) && !ip6_eq(s->laddr6, laddr))) continue;
        if (!s->connected && !s->recverr) continue;
        if (s->connected && (!ip6_eq(s->raddr6, raddr) || s->rport != rport)) continue;
        s->err = err;
        sock_changed(s);
    }
}

int udp6_send(struct sock *s, const uint8_t *data, size_t len, const uint8_t *daddr, uint16_t dport,
              const struct ip6_opts *o, const uint8_t *src) {
    if (len > 65527) return -EMSGSIZE;
    struct netdev *d; uint8_t nh[16], psrc[16];
    int r = ip6_route(daddr, o->oif, &d, nh, psrc);
    if (r) return r;
    if (src) memcpy(psrc, src, 16);
    else if (!ip6_any(s->laddr6) && !ip6_multicast(s->laddr6)) memcpy(psrc, s->laddr6, 16);
    struct pkt *p = pkt_alloc(sizeof(struct udphdr) + len);
    if (!p) return -ENOBUFS;
    struct udphdr *u = (struct udphdr *)p->data;
    p->len = sizeof *u + len;
    u->sport = s->lport; u->dport = dport;
    u->len = htons((uint16_t)p->len);
    u->check = 0;
    memcpy(u + 1, data, len);
    uint16_t c = csum_fold(csum_partial(u, p->len, csum_pseudo6(psrc, daddr, IPPROTO_UDP, (uint32_t)p->len)));
    u->check = htons(c ? c : 0xffff);
    udp_stats[3]++;
    r = ip6_output(p, psrc, daddr, IPPROTO_UDP, o);
    return r ? r : (int)len;
}

/* raw IPv6 sockets get the payload after the IPv6 header (and extension headers) */
void raw6_input(struct pkt *p, int proto) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    list_for_each(it, &raw_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->type != 3 || s->family != AF_INET6 || s->protocol != proto) continue;
        if (!ip6_any(s->laddr6) && !ip6_eq(s->laddr6, h->dst)) continue;
        if (s->connected && !ip6_eq(s->raddr6, h->src)) continue;
        if (s->bound_dev && p->dev && s->bound_dev != p->dev->index && p->dev != loopback_dev) continue;
        if (proto == IPPROTO_ICMPV6 && p->len >= 1 && (s->icmp6_filter[p->data[0] >> 5] & (1u << (p->data[0] & 31)))) continue;
        if (ip6_multicast(h->dst) && proto != IPPROTO_ICMPV6 && !sock_mc_allowed(s, h->dst, true, p->dev ? p->dev->index : 0)) continue;
        struct pkt *c = pkt_clone(p);
        if (c) sock_queue_rx(s, c);
    }
}

bool ping6_input(struct pkt *p) {
    struct ip6hdr *h = (struct ip6hdr *)p->nh;
    uint16_t id; memcpy(&id, p->data + 4, 2);
    list_for_each(it, &raw_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (s->type != 2 || s->family != AF_INET6 || s->lport != id) continue;
        if (!ip6_any(s->laddr6) && !ip6_eq(s->laddr6, h->dst)) continue;
        if (s->connected && !ip6_eq(s->raddr6, h->src)) continue;
        sock_queue_rx(s, p);
        return true;
    }
    return false;
}

int net_fmt_addr6(char *b, size_t max, const uint8_t *a) {
    uint32_t w[4];
    memcpy(w, a, 16);
    return snprintf(b, max, "%08X%08X%08X%08X", w[0], w[1], w[2], w[3]);
}
void sock_addr6(const struct sock *s, bool remote, uint8_t *out) {
    uint32_t a4 = remote ? s->raddr : s->laddr;
    if (s->v6 || !a4) memcpy(out, remote ? s->raddr6 : s->laddr6, 16);
    else ip6_mapped(out, a4);
}

int net_proc_udp6(char *buf, size_t max, bool raw) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n");
    int i = 0;
    list_for_each(it, raw ? &raw_socks : &udp_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if ((size_t)n + 200 >= max) break;
        if ((raw && s->type != 3) || s->family != AF_INET6) continue;
        uint8_t la[16], ra[16];
        sock_addr6(s, false, la); sock_addr6(s, true, ra);
        n += snprintf(buf + n, max - n, "%5d: ", i++);
        n += net_fmt_addr6(buf + n, max - n, la);
        n += snprintf(buf + n, max - n, ":%04X ", raw ? s->protocol : ntohs(s->lport));
        n += net_fmt_addr6(buf + n, max - n, ra);
        n += snprintf(buf + n, max - n, ":%04X %02X %08X:%08X 00:00000000 00000000 %5u        0 %lu 2 0000000000000000 0\n",
                      raw ? 0 : ntohs(s->rport), s->connected ? TCP_ESTABLISHED : TCP_CLOSE, 0, (unsigned)s->rxbytes, s->uid, s->ino);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}

/* ------------------------------------------------------------------ AF_PACKET */
void packet_input(struct netdev *d, struct pkt *p, bool outgoing) {
    if (list_empty(&packet_socks) || p->len < ETH_HLEN) return;
    uint16_t proto = (uint16_t)(p->data[12] << 8 | p->data[13]);
    list_for_each(it, &packet_socks) {
        struct sock *s = list_entry(it, struct sock, node);
        if (!s->pproto) continue;
        if (s->pproto != ETH_P_ALL && (outgoing || s->pproto != proto)) continue;
        if (s->ifindex && s->ifindex != d->index) continue;
        struct pkt *c = pkt_clone(p);
        if (!c) continue;
        c->dev = d;
        c->proto = proto;
        c->pkttype = outgoing ? PACKET_OUTGOING : p->pkttype;
        c->nh = c->data;                     /* frame start */
        sock_queue_rx(s, c);
    }
}
