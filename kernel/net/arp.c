/*
 * Ethernet output and ARP (RFC 826). The neighbour cache maps IPv4 next hops to MAC
 * addresses; frames for an unresolved next hop wait on the entry (a few per entry) while a
 * request is retried once a second, and are dropped with the entry after three attempts.
 * Complete entries expire after a minute and are refreshed by any ARP packet from the host.
 * Everything here runs under net_mutex.
 */
#include <kernel/net.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>

#define ARP_MAX_QUEUE 8
#define ARP_RETRIES 3
#define ARP_TIMEOUT (60 * NS_S)

struct neigh {
    struct list_node node;
    struct netdev *dev;
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    bool complete, permanent;
    int tries;
    uint64_t expires, next_try;
    struct list_node queue;
    int nqueue;
};
static struct list_node neighs = LIST_INIT(neighs);
static struct ntimer arp_timer;
static const uint8_t bcast_mac[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6], spa[4], tha[6], tpa[4];
} __attribute__((packed));

static void frame_send(struct netdev *d, struct pkt *p, const uint8_t *dst, uint16_t type) {
    uint8_t *e = pkt_push(p, ETH_HLEN);
    memcpy(e, dst, 6);
    memcpy(e + 6, d->hwaddr, 6);
    e[12] = type >> 8; e[13] = type & 0xff;
    if (!(d->flags & IFF_UP)) { d->tx_dropped++; pkt_free(p); return; }
    p->dev = d;
    packet_input(d, p, true);                 /* ETH_P_ALL taps see outgoing frames */
    d->xmit(d, p);
}

static void arp_send(struct netdev *d, int op, const uint8_t *tha, uint32_t tip, const uint8_t *dst) {
    struct pkt *p = pkt_alloc(sizeof(struct arp_pkt));
    if (!p) return;
    struct arp_pkt *a = (void *)p->data;
    p->len = sizeof *a;
    a->htype = htons(1); a->ptype = htons(ETH_P_IP); a->hlen = 6; a->plen = 4; a->op = htons(op);
    memcpy(a->sha, d->hwaddr, 6);
    memcpy(a->spa, &d->addr, 4);
    memcpy(a->tha, tha, 6);
    memcpy(a->tpa, &tip, 4);
    frame_send(d, p, dst, ETH_P_ARP);
}

static struct neigh *neigh_find(struct netdev *d, uint32_t ip) {
    list_for_each(it, &neighs) {
        struct neigh *n = list_entry(it, struct neigh, node);
        if (n->dev == d && n->ip == ip) return n;
    }
    return nullptr;
}
static void neigh_free(struct neigh *n) {
    list_del(&n->node);
    pkt_queue_purge(&n->queue);
    kfree(n);
}
static struct neigh *neigh_new(struct netdev *d, uint32_t ip) {
    struct neigh *n = kzalloc(sizeof *n);
    if (!n) return nullptr;
    n->dev = d; n->ip = ip;
    list_init(&n->queue);
    list_add(&neighs, &n->node);
    return n;
}

static void neigh_complete(struct neigh *n, const uint8_t *mac) {
    memcpy(n->mac, mac, 6);
    n->complete = true;
    n->expires = time_ns() + ARP_TIMEOUT;
    list_for_each_safe(it, tmp, &n->queue) {
        struct pkt *p = list_entry(it, struct pkt, node);
        list_del(it);
        frame_send(n->dev, p, n->mac, ETH_P_IP);
    }
    n->nqueue = 0;
}

void eth_output(struct netdev *d, struct pkt *p, uint32_t nexthop, uint16_t type) {
    static const uint8_t zero[6];
    if (d->flags & IFF_NOARP) { frame_send(d, p, d->type == ARPHRD_LOOPBACK ? zero : bcast_mac, type); return; }
    if (nexthop == INADDR_BROADCAST || (d->bcast && nexthop == d->bcast) || nexthop == (d->addr | ~d->netmask)) {
        frame_send(d, p, bcast_mac, type); return;
    }
    if (ipv4_is_multicast(nexthop)) {
        uint32_t h = ntohl(nexthop);
        uint8_t m[6] = { 0x01, 0x00, 0x5e, (h >> 16) & 0x7f, (h >> 8) & 0xff, h & 0xff };
        frame_send(d, p, m, type); return;
    }
    struct neigh *n = neigh_find(d, nexthop);
    uint64_t now = time_ns();
    if (n && n->complete && (n->permanent || now < n->expires)) { frame_send(d, p, n->mac, type); return; }
    if (!n && !(n = neigh_new(d, nexthop))) { d->tx_dropped++; pkt_free(p); return; }
    if (n->complete) {                  /* stale: keep using it while re-resolving */
        frame_send(d, p, n->mac, type);
        if (now >= n->next_try) { n->next_try = now + NS_S; arp_send(d, 1, n->mac, nexthop, n->mac); }
        return;
    }
    if (n->nqueue >= ARP_MAX_QUEUE) {   /* drop the oldest */
        struct pkt *o = list_first(&n->queue, struct pkt, node);
        list_del(&o->node);
        pkt_free(o);
        n->nqueue--;
        d->tx_dropped++;
    }
    list_add_tail(&n->queue, &p->node);
    n->nqueue++;
    if (!n->tries) {
        n->tries = 1;
        n->next_try = now + NS_S;
        arp_send(d, 1, (const uint8_t[6]){ 0 }, nexthop, bcast_mac);
        if (!arp_timer.active) ntimer_mod(&arp_timer, NS_S);
    }
}

void arp_input(struct netdev *d, struct pkt *p) {
    if (p->len < sizeof(struct arp_pkt)) goto out;
    struct arp_pkt *a = (void *)p->data;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_P_IP || a->hlen != 6 || a->plen != 4) goto out;
    uint32_t sip, tip;
    memcpy(&sip, a->spa, 4);
    memcpy(&tip, a->tpa, 4);
    int op = ntohs(a->op);
    bool for_us = d->addr && tip == d->addr;
    struct neigh *n = sip ? neigh_find(d, sip) : nullptr;
    if (n && !n->permanent) { neigh_complete(n, a->sha); n->tries = 0; }
    else if (!n && for_us && sip) { n = neigh_new(d, sip); if (n) neigh_complete(n, a->sha); }
    if (op == 1 && for_us) arp_send(d, 2, a->sha, sip, a->sha);
out:
    pkt_free(p);
}

static void arp_tick(struct ntimer *t) {
    uint64_t now = time_ns();
    bool again = false;
    list_for_each_safe(it, tmp, &neighs) {
        struct neigh *n = list_entry(it, struct neigh, node);
        if (n->permanent) continue;
        if (n->complete) {
            if (now >= n->expires + ARP_TIMEOUT) neigh_free(n);   /* long unused */
            else again = true;
            continue;
        }
        if (now < n->next_try) { again = true; continue; }
        if (n->tries >= ARP_RETRIES) {
            n->dev->tx_errors += n->nqueue;
            /* report host unreachable for the first queued datagram (ICMP-style) */
            if (!list_empty(&n->queue)) {
                struct pkt *q = list_first(&n->queue, struct pkt, node);
                q->nh = q->data;
                icmp_send_unreach(q, 3, 1);
            }
            neigh_free(n);
            continue;
        }
        n->tries++;
        n->next_try = now + NS_S;
        arp_send(n->dev, 1, (const uint8_t[6]){ 0 }, n->ip, bcast_mac);
        again = true;
    }
    if (again) ntimer_mod(t, NS_S);
}

void arp_init(void) { arp_timer.fn = arp_tick; }

void arp_flush_dev(struct netdev *d) {
    list_for_each_safe(it, tmp, &neighs) {
        struct neigh *n = list_entry(it, struct neigh, node);
        if (n->dev == d) neigh_free(n);
    }
}

/* SIOCSARP / SIOCDARP / SIOCGARP: struct arpreq { sockaddr pa, ha; int flags; sockaddr netmask; char dev[16]; } */
struct arpreq_k { struct sockaddr_in_k pa; struct { uint16_t family; uint8_t data[14]; } ha; int32_t flags; struct sockaddr_in_k mask; char dev[16]; };
#define ATF_COM 0x2
#define ATF_PERM 0x4
int arp_ioctl(uint64_t cmd, struct arpreq_k *r) {
    struct netdev *d = r->dev[0] ? netdev_by_name(r->dev) : nullptr;
    if (!d) {
        list_for_each(it, &netdevs) {
            struct netdev *x = list_entry(it, struct netdev, node);
            if (x->addr && ((x->addr ^ r->pa.addr) & x->netmask) == 0) { d = x; break; }
        }
    }
    if (!d) return -ENXIO;
    struct neigh *n = neigh_find(d, r->pa.addr);
    if (cmd == 0x8954) {                     /* SIOCGARP */
        if (!n || !n->complete) return -ENXIO;
        memcpy(r->ha.data, n->mac, 6);
        r->ha.family = ARPHRD_ETHER;
        r->flags = ATF_COM | (n->permanent ? ATF_PERM : 0);
        return 0;
    }
    if (cmd == 0x8953) { if (!n) return -ENXIO; neigh_free(n); return 0; }   /* SIOCDARP */
    if (!n && !(n = neigh_new(d, r->pa.addr))) return -ENOMEM;              /* SIOCSARP */
    n->permanent = r->flags & ATF_PERM;
    neigh_complete(n, r->ha.data);
    return 0;
}

int net_proc_arp(char *buf, size_t max) {
    mutex_lock(&net_mutex);
    int n = snprintf(buf, max, "IP address       HW type     Flags       HW address            Mask     Device\n");
    list_for_each(it, &neighs) {
        struct neigh *e = list_entry(it, struct neigh, node);
        if ((size_t)n >= max) break;
        uint8_t *ip = (uint8_t *)&e->ip, *m = e->mac;
        char ips[16];
        snprintf(ips, sizeof ips, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        n += snprintf(buf + n, max - n, "%-16s 0x%-10x0x%-10x%02x:%02x:%02x:%02x:%02x:%02x     *        %s\n",
                      ips, 1, e->complete ? ATF_COM | (e->permanent ? ATF_PERM : 0) : 0,
                      m[0], m[1], m[2], m[3], m[4], m[5], e->dev->name);
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}

/* netlink (RTM_*NEIGH) access to the cache; state uses the NUD_* values */
void arp_dump(void (*cb)(void *ctx, struct netdev *d, uint32_t ip, const uint8_t *mac, int state), void *ctx) {
    uint64_t now = time_ns();
    list_for_each(it, &neighs) {
        struct neigh *n = list_entry(it, struct neigh, node);
        int st = n->permanent ? 0x80 : !n->complete ? (n->tries ? 0x01 : 0x20) : now < n->expires ? 0x02 : 0x04;
        cb(ctx, n->dev, n->ip, n->complete ? n->mac : nullptr, st);
    }
}
int arp_set(struct netdev *d, uint32_t ip, const uint8_t *mac, bool perm) {
    struct neigh *n = neigh_find(d, ip);
    if (!n && !(n = neigh_new(d, ip))) return -ENOMEM;
    n->permanent = perm;
    neigh_complete(n, mac);
    return 0;
}
int arp_del(struct netdev *d, uint32_t ip) {
    struct neigh *n = neigh_find(d, ip);
    if (!n) return -ENOENT;
    neigh_free(n);
    return 0;
}

void eth_send(struct netdev *d, struct pkt *p, const uint8_t *mac, uint16_t type) { frame_send(d, p, mac, type); }

/* AF_PACKET transmit: a complete frame */
void eth_xmit_raw(struct netdev *d, struct pkt *p) {
    p->dev = d;
    packet_input(d, p, true);
    d->xmit(d, p);
}
