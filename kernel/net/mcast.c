/*
 * M32b: IP multicast group membership. Every device keeps a list of groups (struct mc_group,
 * IPv4 and IPv6) with a user count: sockets join with IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP
 * (struct sock_mc, up to 20 per socket) and the stack itself joins the IPv6 all-nodes and
 * solicited-node groups. eth_input() accepts a multicast frame only if its destination MAC
 * belongs to a joined group; ip_input()/ip6_input() check the group itself.
 *
 * IGMPv2 (RFC 2236): an unsolicited Membership Report (sent twice) on the first join of a
 * group, Leave Group to 224.0.0.2 on the last leave, reports after a random delay in answer
 * to queries (general or group-specific, IGMPv1 queries count as 10 s), and suppression when
 * another member's report is heard. MLD (RFC 2710) is the same with ip6.c's mld_send().
 * 224.0.0.1 / ff02::1 are never reported. Runs under net_mutex.
 */
#include <kernel/net.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/errno.h>

struct igmphdr { uint8_t type, code; uint16_t check; uint32_t group; };
#define IGMP_QUERY 0x11
#define IGMP_V1_REPORT 0x12
#define IGMP_V2_REPORT 0x16
#define IGMP_LEAVE 0x17
#define UNSOL_DELAY_NS (1 * NS_S)       /* the repeated unsolicited report (RFC 2236 says 10 s) */

static struct ntimer mc_timer;

static size_t glen(bool v6) { return v6 ? 16 : 4; }
static bool all_hosts(const uint8_t *g, bool v6) {
    static const uint8_t ff02_1[16] = { 0xff, 0x02, [15] = 1 };
    return v6 ? !memcmp(g, ff02_1, 16) || (g[0] == 0xff && (g[1] & 0xf) <= 1) : (g[0] == 224 && g[1] == 0 && g[2] == 0 && g[3] == 1);
}
static struct mc_group *mc_find(struct netdev *d, const void *grp, bool v6) {
    list_for_each(it, &d->mcgroups) {
        struct mc_group *g = list_entry(it, struct mc_group, node);
        if (g->v6 == v6 && !memcmp(g->addr, grp, glen(v6))) return g;
    }
    return nullptr;
}

static void igmp_send(struct netdev *d, uint32_t group, int type) {
    if (!(d->flags & IFF_UP) || d->type == ARPHRD_LOOPBACK) return;
    struct pkt *p = pkt_alloc(sizeof(struct igmphdr));
    if (!p) return;
    struct igmphdr *h = (struct igmphdr *)p->data;
    p->len = sizeof *h;
    h->type = (uint8_t)type; h->code = 0; h->check = 0; h->group = group;
    h->check = htons(csum_fold(csum_partial(h, sizeof *h, 0)));
    uint32_t dst = type == IGMP_LEAVE ? htonl(0xe0000002u) : group;
    struct ip_opts o = { 1, 0xc0, false, d->index, false, true };
    ip_output(p, d->addr, dst, IPPROTO_IGMP, &o);
}

static void send_report(struct netdev *d, struct mc_group *g) {
    if (g->v6) mld_send(d, g->addr, 131);
    else igmp_send(d, *(uint32_t *)g->addr, IGMP_V2_REPORT);
    g->reporter = true;
}

static void mc_tick(struct ntimer *t) {
    uint64_t now = time_ns(), next = UINT64_MAX;
    list_for_each(it, &netdevs) {
        struct netdev *d = list_entry(it, struct netdev, node);
        list_for_each(gi, &d->mcgroups) {
            struct mc_group *g = list_entry(gi, struct mc_group, node);
            if (!g->report_at) continue;
            if (g->report_at <= now) { g->report_at = 0; send_report(d, g); }
            else if (g->report_at < next) next = g->report_at;
        }
    }
    if (next != UINT64_MAX) ntimer_mod(t, next - now);
}
static void mc_schedule(struct mc_group *g, uint64_t delay) {
    uint64_t at = time_ns() + delay;
    if (g->report_at && g->report_at <= at) return;     /* an earlier report is already due */
    g->report_at = at;
    mc_timer.fn = mc_tick;
    if (!mc_timer.active || mc_timer.when > at) ntimer_mod(&mc_timer, delay);
}

int mc_dev_join(struct netdev *d, const void *grp, bool v6) {
    struct mc_group *g = mc_find(d, grp, v6);
    if (g) { g->users++; return 0; }
    if (!(g = kzalloc(sizeof *g))) return -ENOBUFS;
    memcpy(g->addr, grp, glen(v6));
    g->v6 = v6;
    g->users = 1;
    list_add_tail(&d->mcgroups, &g->node);
    if (!all_hosts(g->addr, v6) && d->type != ARPHRD_LOOPBACK) {
        send_report(d, g);                               /* unsolicited, repeated once */
        mc_schedule(g, UNSOL_DELAY_NS / 2 + random_u64() % (UNSOL_DELAY_NS / 2));
    }
    return 0;
}

int mc_dev_leave(struct netdev *d, const void *grp, bool v6) {
    struct mc_group *g = mc_find(d, grp, v6);
    if (!g) return -EADDRNOTAVAIL;
    if (--g->users) return 0;
    if (!all_hosts(g->addr, v6) && d->type != ARPHRD_LOOPBACK && g->reporter) {
        if (v6) mld_send(d, g->addr, 132);
        else igmp_send(d, *(uint32_t *)g->addr, IGMP_LEAVE);
    }
    list_del(&g->node);
    kfree(g);
    return 0;
}

bool mc_dev_has(struct netdev *d, const void *grp, bool v6) { return mc_find(d, grp, v6) != nullptr; }

/* destination MAC filter: 01:00:5e + low 23 bits (IPv4), 33:33 + low 32 bits (IPv6) */
bool mc_mac_ok(struct netdev *d, const uint8_t *mac) {
    if (d->flags & (IFF_PROMISC | IFF_ALLMULTI)) return true;
    if (mac[0] == 0x01 && mac[1] == 0x00 && mac[2] == 0x5e && !(mac[3] & 0x80)) {
        if (mac[3] == 0 && mac[4] == 0 && mac[5] == 1) return true;          /* 224.0.0.1 */
        list_for_each(it, &d->mcgroups) {
            struct mc_group *g = list_entry(it, struct mc_group, node);
            if (!g->v6 && (g->addr[1] & 0x7f) == mac[3] && g->addr[2] == mac[4] && g->addr[3] == mac[5]) return true;
        }
        return false;
    }
    if (mac[0] == 0x33 && mac[1] == 0x33) {
        list_for_each(it, &d->mcgroups) {
            struct mc_group *g = list_entry(it, struct mc_group, node);
            if (g->v6 && !memcmp(g->addr + 12, mac + 2, 4)) return true;
        }
        return mac[2] == 0 && mac[3] == 0 && mac[4] == 0 && mac[5] == 1;     /* ff02::1 */
    }
    return false;                                         /* other multicast MACs: not ours */
}

void mc_dev_flush(struct netdev *d) {
    list_for_each_safe(it, tmp, &d->mcgroups) {
        struct mc_group *g = list_entry(it, struct mc_group, node);
        list_del(it);
        kfree(g);
    }
}

void mc_query(struct netdev *d, const void *grp, bool v6, uint64_t max_ns) {
    if (max_ns < NS_MS) max_ns = NS_MS;
    list_for_each(it, &d->mcgroups) {
        struct mc_group *g = list_entry(it, struct mc_group, node);
        if (g->v6 != v6 || all_hosts(g->addr, v6)) continue;
        if (grp && memcmp(g->addr, grp, glen(v6))) continue;
        mc_schedule(g, random_u64() % max_ns);
    }
}

void mc_heard_report(struct netdev *d, const void *grp, bool v6) {
    struct mc_group *g = mc_find(d, grp, v6);
    if (g && g->report_at) { g->report_at = 0; g->reporter = false; }
}

/* IGMP (protocol 2): p->data at the IGMP header, p->nh the IP header */
void igmp_input(struct pkt *p) {
    struct netdev *d = p->dev;
    struct igmphdr *h = (struct igmphdr *)p->data;
    if (p->len < sizeof *h || (!p->csum_ok && csum_fold(csum_partial(p->data, p->len, 0)))) goto out;
    if (!d || d->type == ARPHRD_LOOPBACK) goto out;
    switch (h->type) {
    case IGMP_QUERY: {
        uint64_t max = h->code ? (uint64_t)h->code * 100 * NS_MS : 10 * NS_S;   /* code 0: IGMPv1 */
        mc_query(d, h->group ? &h->group : nullptr, false, max);
        break;
    }
    case IGMP_V1_REPORT: case IGMP_V2_REPORT:
        mc_heard_report(d, &h->group, false);
        break;
    }
out:
    pkt_free(p);
}

/* ------------------------------------------------------------------ socket memberships */
int sock_mc_join(struct sock *s, const void *grp, bool v6, int ifindex) {
    struct netdev *d = netdev_by_index(ifindex);
    if (!d) return -ENODEV;
    for (int i = 0; i < s->nmc; i++)
        if (s->mc[i].v6 == v6 && s->mc[i].ifindex == ifindex && !memcmp(s->mc[i].grp, grp, glen(v6))) return -EADDRINUSE;
    if (s->nmc >= (int)(sizeof s->mc / sizeof s->mc[0])) return -ENOBUFS;
    int r = mc_dev_join(d, grp, v6);
    if (r) return r;
    struct sock_mc *m = &s->mc[s->nmc++];
    memset(m, 0, sizeof *m);
    memcpy(m->grp, grp, glen(v6));
    m->v6 = v6; m->ifindex = ifindex;
    return 0;
}

int sock_mc_leave(struct sock *s, const void *grp, bool v6, int ifindex) {
    for (int i = 0; i < s->nmc; i++) {
        struct sock_mc *m = &s->mc[i];
        if (m->v6 != v6 || memcmp(m->grp, grp, glen(v6)) || (ifindex && m->ifindex != ifindex)) continue;
        struct netdev *d = netdev_by_index(m->ifindex);
        if (d) mc_dev_leave(d, m->grp, v6);
        s->mc[i] = s->mc[--s->nmc];
        return 0;
    }
    return -EADDRNOTAVAIL;
}

void sock_mc_drop_all(struct sock *s) {
    while (s->nmc) {
        struct sock_mc *m = &s->mc[--s->nmc];
        struct netdev *d = netdev_by_index(m->ifindex);
        if (d) mc_dev_leave(d, m->grp, m->v6);
    }
}

/* may s receive a datagram for group grp that arrived on ifindex? (Linux ip_mc_sf_allow) */
bool sock_mc_allowed(struct sock *s, const void *grp, bool v6, int ifindex) {
    for (int i = 0; i < s->nmc; i++) {
        struct sock_mc *m = &s->mc[i];
        if (m->v6 == v6 && !memcmp(m->grp, grp, glen(v6)) && (!ifindex || m->ifindex == ifindex)) return true;
    }
    return s->mc_all;
}

/* /proc/net/igmp and /proc/net/igmp6 in Linux's format */
int net_proc_igmp(char *buf, size_t max, bool v6) {
    mutex_lock(&net_mutex);
    int n = v6 ? 0 : snprintf(buf, max, "Idx\tDevice    : Count Querier\tGroup    Users Timer\tReporter\n");
    list_for_each(it, &netdevs) {
        struct netdev *d = list_entry(it, struct netdev, node);
        int cnt = 0;
        list_for_each(gi, &d->mcgroups) if (list_entry(gi, struct mc_group, node)->v6 == v6) cnt++;
        if (!v6 && (size_t)n < max) n += snprintf(buf + n, max - n, "%d\t%-10s: %5d      V2\n", d->index, d->name, cnt);
        list_for_each(gi, &d->mcgroups) {
            struct mc_group *g = list_entry(gi, struct mc_group, node);
            if (g->v6 != v6 || (size_t)n >= max) continue;
            uint64_t now = time_ns(), left = g->report_at > now ? (g->report_at - now) / 10000000 : 0;
            if (!v6) {
                uint32_t a; memcpy(&a, g->addr, 4);
                n += snprintf(buf + n, max - n, "\t\t\t\t%08X %5d %d:%08X\t\t%d\n", a, g->users, g->report_at ? 1 : 0, (unsigned)left, g->reporter);
            } else {
                n += snprintf(buf + n, max - n, "%-4d %-15s ", d->index, d->name);
                for (int k = 0; k < 16 && (size_t)n < max; k++) n += snprintf(buf + n, max - n, "%02x", g->addr[k]);
                if ((size_t)n < max) n += snprintf(buf + n, max - n, " %5d %08X %8u\n", g->users, g->reporter ? 4 : 0, (unsigned)left);
            }
        }
    }
    mutex_unlock(&net_mutex);
    return MIN(n, (int)max);
}
