/* M32b networking: /proc/sys knobs, TCP under loss (SACK, timestamps), TUN/TAP devices,
 * IPv4 forwarding (TTL, ICMP time exceeded / fragmentation needed, re-fragmentation), IPv4
 * multicast with IGMPv2, AF_NETLINK (rtnetlink, BusyBox ip, getifaddrs), IPv6 sockets over
 * loopback (TCP/UDP/ICMPv6, dual stack, ancillary data) and IPv6 on a TAP link (DAD, ND, SLAAC
 * from an injected RA, MLD, ICMPv6 errors).
 * usage: net2test [test]   (as root)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdarg.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static int fails;
static int verbose;
#define CHECK(c, ...) do { if (verbose) { printf("  .. %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } if (!(c)) { printf("net2test: FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf(" (errno %d %s)\n", errno, strerror(errno)); fails++; } } while (0)
#define OK(name) printf("net2test: %s ok\n", name)

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static uint8_t pat(uint64_t i, unsigned seed) { uint64_t x = (i + 1) * 0x9e3779b97f4a7c15ull ^ seed; return (uint8_t)(x >> 31); }
static struct sockaddr_in sin_of(const char *ip, int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}
static uint32_t ip4(const char *s) { struct in_addr a; inet_pton(AF_INET, s, &a); return a.s_addr; }
static uint16_t cksum(const void *d, int n, uint32_t s) {
    const uint8_t *p = d;
    for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]);
    if (n) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return htons((uint16_t)~s);
}

/* ---------------------------------------------------------------- /proc/sys */
static int sysctl_get(const char *path) {
    char p[128], b[32] = { 0 };
    snprintf(p, sizeof p, "/proc/sys/%s", path);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    read(fd, b, sizeof b - 1);
    close(fd);
    return atoi(b);
}
static int sysctl_set(const char *path, int v) {
    char p[128], b[32];
    snprintf(p, sizeof p, "/proc/sys/%s", path);
    int fd = open(p, O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    int n = snprintf(b, sizeof b, "%d\n", v);
    int r = write(fd, b, n) == n ? 0 : -1;
    close(fd);
    return r;
}

static void sysctl_tests(void) {
    CHECK(sysctl_get("net/ipv4/ip_forward") == 0, "ip_forward defaults to 0");
    CHECK(sysctl_get("net/ipv4/ip_default_ttl") == 64, "ip_default_ttl");
    CHECK(sysctl_get("net/ipv4/tcp_sack") == 1 && sysctl_get("net/ipv4/tcp_timestamps") == 1 &&
          sysctl_get("net/ipv4/tcp_window_scaling") == 1, "TCP option knobs on");
    CHECK(sysctl_set("net/ipv4/ip_forward", 2) < 0 && errno == EINVAL, "out of range rejected");
    CHECK(sysctl_set("net/ipv4/ip_default_ttl", 33) == 0 && sysctl_get("net/ipv4/ip_default_ttl") == 33, "write ttl");
    int s = socket(AF_INET, SOCK_DGRAM, 0), v = 0; socklen_t vl = sizeof v;
    CHECK(getsockopt(s, IPPROTO_IP, IP_TTL, &v, &vl) == 0 && v == 33, "new sockets use ip_default_ttl (%d)", v);
    close(s);
    sysctl_set("net/ipv4/ip_default_ttl", 64);
    FILE *f = fopen("/proc/sys/kernel/ostype", "r");
    char line[64] = { 0 };
    CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "Linux\n"), "kernel/ostype");
    if (f) fclose(f);
    CHECK(system("ls /proc/sys/net/ipv4 | grep -qx tcp_sack && ls /proc/sys | grep -qx kernel") == 0, "directory listing");
    CHECK(system("echo 9os-sysctl > /proc/sys/kernel/hostname && [ $(hostname) = 9os-sysctl ] && hostname 9os") == 0, "hostname");
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(1000) || setuid(1000)) _exit(10);
        int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
        _exit(fd < 0 && errno == EACCES ? 0 : 1);
    }
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "unprivileged write denied");
    /* tcp_timestamps=0: new connections negotiate without timestamps */
    sysctl_set("net/ipv4/tcp_timestamps", 0);
    sysctl_set("net/ipv4/tcp_sack", 0);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin_of("127.0.0.1", 0);
    bind(l, (void *)&a, sizeof a); listen(l, 1);
    socklen_t al = sizeof a; getsockname(l, (void *)&a, &al);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    connect(c, (void *)&a, sizeof a);
    struct tcp_info ti; socklen_t tl = sizeof ti;
    memset(&ti, 0, sizeof ti);
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_INFO, &ti, &tl) == 0 && ti.tcpi_options == TCPI_OPT_WSCALE, "tcpi_options=%d", ti.tcpi_options);
    close(c); close(l);
    sysctl_set("net/ipv4/tcp_timestamps", 1);
    sysctl_set("net/ipv4/tcp_sack", 1);
    OK("sysctl");
}

/* ---------------------------------------------------------------- TCP under loss */
static uint64_t retrans(void) {
    FILE *f = fopen("/proc/net/snmp", "r");
    char line[512]; int n = 0; unsigned long long v[8] = { 0 };
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "Tcp: ", 5) && line[5] >= '0' && line[5] <= '9' && ++n)
            sscanf(line + 5, "%llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4]);
    if (f) fclose(f);
    return v[4];
}
static uint64_t lossy_transfer(size_t total) {
    int l = socket(AF_INET, SOCK_STREAM, 0), big = 1 << 20;
    setsockopt(l, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    struct sockaddr_in a = sin_of("127.0.0.1", 0);
    bind(l, (void *)&a, sizeof a); listen(l, 1);
    socklen_t al = sizeof a; getsockname(l, (void *)&a, &al);
    uint64_t t0 = now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        if (connect(c, (void *)&a, sizeof a)) _exit(1);
        static uint8_t b[65536];
        for (size_t off = 0; off < total; ) {
            size_t n = total - off < sizeof b ? total - off : sizeof b;
            for (size_t i = 0; i < n; i++) b[i] = pat(off + i, 9);
            ssize_t w = write(c, b, n);
            if (w <= 0) _exit(2);
            off += (size_t)w;
        }
        shutdown(c, SHUT_WR);
        char x; read(c, &x, 1);
        _exit(0);
    }
    int s = accept(l, NULL, NULL);
    static uint8_t b[65536];
    size_t got = 0, bad = 0;
    for (;;) {
        ssize_t r = read(s, b, sizeof b);
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) if (b[i] != pat(got + i, 9)) bad++;
        got += (size_t)r;
    }
    close(s); close(l);
    int st; waitpid(pid, &st, 0);
    CHECK(got == total && !bad && WIFEXITED(st) && WEXITSTATUS(st) == 0, "lossy transfer: %zu of %zu bytes, %zu corrupt, status %d", got, total, bad, st);
    return now_ms() - t0;
}
static int lo_mtu(int mtu) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q = { 0 };
    strcpy(q.ifr_name, "lo");
    q.ifr_mtu = mtu;
    int r = ioctl(s, SIOCSIFMTU, &q);
    close(s);
    return r;
}
static void tcp_loss(void) {
    const size_t total = 8 << 20;
    CHECK(lo_mtu(1500) == 0, "lo mtu 1500 (many segments in flight)");
    uint64_t r0 = retrans();
    int every = getenv("NET2_LOSS") ? atoi(getenv("NET2_LOSS")) : 53;
    CHECK(sysctl_set("net/core/9os_lo_drop_every", every) == 0, "enable loopback loss");
    uint64_t t_sack = lossy_transfer(total);
    uint64_t r1 = retrans();
    sysctl_set("net/ipv4/tcp_sack", 0);
    uint64_t t_nosack = lossy_transfer(total);
    sysctl_set("net/ipv4/tcp_sack", 1);
    sysctl_set("net/core/9os_lo_drop_every", 0);
    lo_mtu(65536);
    uint64_t r2 = retrans();
    CHECK(r1 > r0 && r2 > r1, "retransmissions happened (%llu, %llu)", (unsigned long long)(r1 - r0), (unsigned long long)(r2 - r1));
    printf("net2test: tcp loss ok (8 MiB with 1/%d loss: SACK %llu ms, %llu rexmits; NewReno %llu ms, %llu rexmits)\n", every,
           (unsigned long long)t_sack, (unsigned long long)(r1 - r0), (unsigned long long)t_nosack, (unsigned long long)(r2 - r1));
}

/* ---------------------------------------------------------------- TUN/TAP */
static int tun_new(const char *pat, int flags, char *name) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) return -1;
    struct ifreq q = { 0 };
    strcpy(q.ifr_name, pat);
    q.ifr_flags = (short)flags;
    if (ioctl(fd, TUNSETIFF, &q) < 0) { close(fd); return -1; }
    strcpy(name, q.ifr_name);
    return fd;
}
static int if_up(const char *name, const char *ip, const char *mask, int mtu) {
    int s = socket(AF_INET, SOCK_DGRAM, 0), r = 0;
    struct ifreq q = { 0 };
    strcpy(q.ifr_name, name);
    struct sockaddr_in a = sin_of(ip, 0);
    memcpy(&q.ifr_addr, &a, sizeof a);
    r |= ioctl(s, SIOCSIFADDR, &q);
    a = sin_of(mask, 0);
    memcpy(&q.ifr_netmask, &a, sizeof a);
    r |= ioctl(s, SIOCSIFNETMASK, &q);
    if (mtu) { q.ifr_mtu = mtu; r |= ioctl(s, SIOCSIFMTU, &q); }
    r |= ioctl(s, SIOCGIFFLAGS, &q);
    q.ifr_flags |= IFF_UP;
    r |= ioctl(s, SIOCSIFFLAGS, &q);
    close(s);
    return r;
}
/* next IPv4 datagram of protocol proto (any if proto < 0) after an off-byte header (14: ethernet,
 * then proto -1 also takes ARP) */
static ssize_t rd_ip(int fd, uint8_t *b, size_t n, int proto, int ms, int off) {
    int eth = off == 14;
    uint64_t end = now_ms() + ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0) return -1;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, left) <= 0) return -1;
        ssize_t r = read(fd, b, n);
        if (r <= 0) return -1;
        const uint8_t *ip = b + off;
        if (eth) {
            uint16_t et = (uint16_t)(b[12] << 8 | b[13]);
            if (proto == -1 && et == 0x0806) return r;
            if (et != 0x0800) continue;
        }
        if ((ip[0] >> 4) == 4 && (proto < 0 || ip[9] == proto)) return r;
    }
}
static size_t mk_ip(uint8_t *b, const char *src, const char *dst, int proto, int ttl, int df, const void *pl, size_t plen) {
    memset(b, 0, 20);
    b[0] = 0x45; b[2] = (uint8_t)((20 + plen) >> 8); b[3] = (uint8_t)(20 + plen);
    b[4] = 0x12; b[5] = 0x34; if (df) b[6] = 0x40;
    b[8] = (uint8_t)ttl; b[9] = (uint8_t)proto;
    uint32_t s = ip4(src), d = ip4(dst);
    memcpy(b + 12, &s, 4); memcpy(b + 16, &d, 4);
    uint16_t c = cksum(b, 20, 0); memcpy(b + 10, &c, 2);
    memcpy(b + 20, pl, plen);
    return 20 + plen;
}
static size_t mk_echo(uint8_t *b, const char *src, const char *dst, int ttl, int df, size_t datalen) {
    uint8_t ic[2048] = { 8, 0, 0, 0, 0x42, 0x42, 0, 1 };
    for (size_t i = 0; i < datalen; i++) ic[8 + i] = (uint8_t)i;
    uint16_t c = cksum(ic, (int)(8 + datalen), 0); memcpy(ic + 2, &c, 2);
    return mk_ip(b, src, dst, 1, ttl, df, ic, 8 + datalen);
}

static void tun_tests(void) {
    char n0[IFNAMSIZ], n1[IFNAMSIZ], nt[IFNAMSIZ];
    int t0 = tun_new("t9a%d", IFF_TUN | IFF_NO_PI, n0), t1 = tun_new("t9b%d", IFF_TUN | IFF_NO_PI, n1);
    CHECK(t0 >= 0 && t1 >= 0 && !strcmp(n0, "t9a0") && !strcmp(n1, "t9b0"), "TUNSETIFF (%s, %s)", n0, n1);
    if (t0 < 0 || t1 < 0) return;
    CHECK(if_nametoindex(n0) > 0, "interface exists");
    CHECK(if_up(n0, "10.99.0.1", "255.255.255.0", 0) == 0 && if_up(n1, "10.98.0.1", "255.255.255.0", 1000) == 0, "configure");
    uint8_t b[4096], r[4096];
    /* local delivery: echo request in, reply out */
    size_t n = mk_echo(b, "10.99.0.2", "10.99.0.1", 64, 0, 32);
    CHECK(write(t0, b, n) == (ssize_t)n, "inject echo request");
    ssize_t k = rd_ip(t0, r, sizeof r, 1, 2000, 0);
    CHECK(k == (ssize_t)n && r[20] == 0 && !memcmp(r + 12, b + 16, 4) && !memcmp(r + 16, b + 12, 4), "echo reply read from tun (%zd)", k);
    /* UDP to a socket bound to the tun address, and its reply */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = sin_of("10.99.0.1", 4444);
    CHECK(bind(u, (void *)&ua, sizeof ua) == 0, "bind UDP to the tun address");
    uint8_t ud[8 + 5] = { 0x1f, 0x90, 0x11, 0x5c, 0, 13, 0, 0, 'h', 'e', 'l', 'l', 'o' };
    n = mk_ip(b, "10.99.0.2", "10.99.0.1", 17, 64, 0, ud, sizeof ud);
    write(t0, b, n);
    struct timeval tv = { 2, 0 };
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char m[16] = { 0 }; struct sockaddr_in from; socklen_t fl = sizeof from;
    CHECK(recvfrom(u, m, sizeof m, 0, (void *)&from, &fl) == 5 && !strcmp(m, "hello") && from.sin_addr.s_addr == ip4("10.99.0.2") &&
          ntohs(from.sin_port) == 8080, "UDP received through tun");
    CHECK(sendto(u, "back", 4, 0, (void *)&from, fl) == 4, "UDP reply");
    k = rd_ip(t0, r, sizeof r, 17, 2000, 0);
    CHECK(k == 32 && !memcmp(r + 28, "back", 4) && r[22] == 0x1f && r[23] == 0x90, "UDP reply read from tun (%zd)", k);
    close(u);
    /* forwarding off: nothing crosses */
    n = mk_echo(b, "10.99.0.2", "10.98.0.2", 5, 0, 32);
    write(t0, b, n);
    CHECK(rd_ip(t1, r, sizeof r, 1, 300, 0) < 0, "no forwarding by default");
    sysctl_set("net/ipv4/ip_forward", 1);
    write(t0, b, n);
    k = rd_ip(t1, r, sizeof r, 1, 2000, 0);
    CHECK(k == (ssize_t)n && r[8] == 4 && cksum(r, 20, 0) == 0 && !memcmp(r + 20, b + 20, n - 20), "forwarded with TTL-1 (%zd, ttl %d)", k, k > 8 ? r[8] : -1);
    /* TTL expiry */
    n = mk_echo(b, "10.99.0.2", "10.98.0.2", 1, 0, 32);
    write(t0, b, n);
    k = rd_ip(t0, r, sizeof r, 1, 2000, 0);
    CHECK(k >= 56 && r[20] == 11 && r[21] == 0 && !memcmp(r + 12, "\x0a\x63\x00\x01", 4), "ICMP time exceeded (%zd, type %d)", k, k > 20 ? r[20] : -1);
    /* DF over a smaller MTU: fragmentation needed with the next-hop MTU */
    n = mk_echo(b, "10.99.0.2", "10.98.0.2", 64, 1, 1200);
    write(t0, b, n);
    k = rd_ip(t0, r, sizeof r, 1, 2000, 0);
    CHECK(k >= 56 && r[20] == 3 && r[21] == 4 && (r[26] << 8 | r[27]) == 1000, "fragmentation needed, mtu %d", k > 27 ? r[26] << 8 | r[27] : -1);
    /* without DF: re-fragmented */
    n = mk_echo(b, "10.99.0.2", "10.98.0.2", 64, 0, 1200);
    write(t0, b, n);
    ssize_t f1 = rd_ip(t1, r, sizeof r, 1, 2000, 0);
    int mf = f1 > 0 && (r[6] & 0x20);
    ssize_t f2 = rd_ip(t1, r, sizeof r, 1, 2000, 0);
    CHECK(f1 > 0 && f1 <= 1000 && mf && f2 > 0 && f1 + f2 - 20 == (ssize_t)n, "fragments %zd + %zd", f1, f2);
    sysctl_set("net/ipv4/ip_forward", 0);
    /* PI header mode */
    int tp = tun_new("", IFF_TUN, nt);
    CHECK(tp >= 0 && !strncmp(nt, "tun", 3), "default name %s", nt);
    if_up(nt, "10.96.0.1", "255.255.255.0", 0);
    uint8_t pb[4096] = { 0, 0, 0x08, 0x00 };
    n = mk_echo(pb + 4, "10.96.0.2", "10.96.0.1", 64, 0, 16);
    write(tp, pb, n + 4);
    k = rd_ip(tp, r, sizeof r, 1, 2000, 4);
    CHECK(k == (ssize_t)n + 4 && r[2] == 0x08 && r[3] == 0x00 && r[24] == 0, "PI header + echo reply (%zd)", k);
    close(tp);
    /* TAP: ARP and ICMP inside ethernet frames */
    char nta[IFNAMSIZ];
    int ta = tun_new("tap%d", IFF_TAP | IFF_NO_PI, nta);
    CHECK(ta >= 0, "TAP device");
    if_up(nta, "10.97.0.1", "255.255.255.0", 0);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq q = { 0 }; strcpy(q.ifr_name, nta);
    ioctl(s, SIOCGIFHWADDR, &q);
    close(s);
    uint8_t mac[6], peer[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(mac, q.ifr_hwaddr.sa_data, 6);
    uint8_t arp[42] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    memcpy(arp + 6, peer, 6); arp[12] = 0x08; arp[13] = 0x06;
    uint8_t ah[8] = { 0, 1, 8, 0, 6, 4, 0, 1 }; memcpy(arp + 14, ah, 8);
    memcpy(arp + 22, peer, 6);
    uint32_t sip = ip4("10.97.0.2"), tip = ip4("10.97.0.1");
    memcpy(arp + 28, &sip, 4); memcpy(arp + 38, &tip, 4);
    write(ta, arp, sizeof arp);
    k = rd_ip(ta, r, sizeof r, -1, 2000, 14);
    CHECK(k >= 42 && r[12] == 8 && r[13] == 6 && r[21] == 2 && !memcmp(r, peer, 6) && !memcmp(r + 22, mac, 6), "ARP reply on tap (%zd)", k);
    uint8_t fr[2048];
    memcpy(fr, mac, 6); memcpy(fr + 6, peer, 6); fr[12] = 8; fr[13] = 0;
    n = mk_echo(fr + 14, "10.97.0.2", "10.97.0.1", 64, 0, 40);
    write(ta, fr, n + 14);
    k = rd_ip(ta, r, sizeof r, 1, 2000, 14);
    CHECK(k == (ssize_t)n + 14 && !memcmp(r, peer, 6) && r[34] == 0, "echo reply frame on tap (%zd)", k);
    close(ta);
    /* the interface goes away with the file */
    close(t0); close(t1);
    CHECK(if_nametoindex(n0) == 0 && if_nametoindex(nta) == 0, "interfaces removed on close");
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(1000) || setuid(1000)) _exit(10);
        char nn[IFNAMSIZ];
        _exit(tun_new("", IFF_TUN, nn) < 0 && errno == EPERM ? 0 : 1);
    }
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "TUNSETIFF needs CAP_NET_ADMIN");
    OK("tun/tap and forwarding");
}


/* ---------------------------------------------------------------- IPv4 multicast + IGMPv2 (over TAP) */
static ssize_t rd_igmp(int fd, uint8_t *b, size_t n, int type, int ms) {
    uint64_t end = now_ms() + ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0) return -1;
        ssize_t k = rd_ip(fd, b, n, 2, left, 14);
        if (k < 0) return -1;
        int ihl = (b[14] & 15) * 4;
        if (k >= 14 + ihl + 8 && b[14 + ihl] == type) return k;
    }
}
static size_t mk_mcframe(uint8_t *f, const uint8_t *src_mac, const uint8_t *dst_mac, const char *src, const char *grp, int port, const char *msg) {
    uint8_t ud[64] = { 0x30, 0x39, (uint8_t)(port >> 8), (uint8_t)port, 0, 0, 0, 0 };
    size_t ml = strlen(msg);
    ud[5] = (uint8_t)(8 + ml);
    memcpy(ud + 8, msg, ml);
    memcpy(f, dst_mac, 6); memcpy(f + 6, src_mac, 6); f[12] = 8; f[13] = 0;
    return 14 + mk_ip(f + 14, src, grp, 17, 1, 0, ud, 8 + ml);
}
static int recv_str(int s, char *m, size_t n, int ms) {
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) return -1;
    memset(m, 0, n);
    return (int)recv(s, m, n - 1, MSG_DONTWAIT);
}
static void mcast_tests(void) {
    char nm[IFNAMSIZ];
    int ta = tun_new("tapm%d", IFF_TAP | IFF_NO_PI, nm);
    CHECK(ta >= 0, "TAP device");
    if (ta < 0) return;
    CHECK(if_up(nm, "10.95.0.1", "255.255.255.0", 0) == 0, "configure %s", nm);
    int ifi = (int)if_nametoindex(nm), one = 1, v = 0;
    struct ifreq q = { 0 }; strcpy(q.ifr_name, nm);
    int t = socket(AF_INET, SOCK_DGRAM, 0);
    ioctl(t, SIOCGIFHWADDR, &q);
    close(t);
    uint8_t mac[6], peer[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x66 }, gmac[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 };
    memcpy(mac, q.ifr_hwaddr.sa_data, 6);
    uint8_t f[2048], r[2048];
    char m[64];
    int a = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in any = sin_of("0.0.0.0", 5000);
    CHECK(bind(a, (void *)&any, sizeof any) == 0, "bind :5000");
    socklen_t vl = sizeof v;
    CHECK(getsockopt(a, IPPROTO_IP, IP_MULTICAST_TTL, &v, &vl) == 0 && v == 1, "default multicast ttl %d", v);
    vl = sizeof v;
    CHECK(getsockopt(a, IPPROTO_IP, IP_MULTICAST_LOOP, &v, &vl) == 0 && v == 1, "default multicast loop %d", v);
    struct ip_mreqn mr = { .imr_ifindex = ifi };
    mr.imr_multiaddr.s_addr = ip4("239.1.2.3");
    CHECK(setsockopt(a, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0, "IP_ADD_MEMBERSHIP (ip_mreqn)");
    CHECK(setsockopt(a, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0 && errno == EADDRINUSE, "joining twice");
    ssize_t k = rd_igmp(ta, r, sizeof r, 0x16, 2000);
    CHECK(k >= 14 + 24 + 8 && !memcmp(r, gmac, 6) && (r[14] & 15) == 6 && r[14 + 20] == 0x94 && r[14 + 8] == 1 &&
          !memcmp(r + 14 + 16, &mr.imr_multiaddr, 4) && !memcmp(r + 14 + 24 + 4, &mr.imr_multiaddr, 4) && cksum(r + 38, 8, 0) == 0,
          "IGMPv2 membership report (%zd)", k);
    CHECK(system("grep -q 030201EF /proc/net/igmp") == 0, "/proc/net/igmp lists the group");
    size_t n = mk_mcframe(f, peer, gmac, "10.95.0.2", "239.1.2.3", 5000, "mc1");
    write(ta, f, n);
    CHECK(recv_str(a, m, sizeof m, 2000) == 3 && !strcmp(m, "mc1"), "datagram to the group received");
    uint8_t mac4[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x04 }, macx[6] = { 0x01, 0x00, 0x5e, 0x7f, 0x7f, 0x7f };
    n = mk_mcframe(f, peer, mac4, "10.95.0.2", "239.1.2.4", 5000, "no1"); write(ta, f, n);      /* group not joined */
    n = mk_mcframe(f, peer, macx, "10.95.0.2", "239.1.2.3", 5000, "no2"); write(ta, f, n);      /* MAC filter */
    n = mk_mcframe(f, peer, gmac, "10.95.0.2", "239.1.2.3", 5000, "mc2"); write(ta, f, n);
    CHECK(recv_str(a, m, sizeof m, 2000) == 3 && !strcmp(m, "mc2"), "other groups and foreign MACs filtered (got %s)", m);
    /* IP_MULTICAST_ALL: by default every socket on the port hears joined groups; 0 restricts to own memberships */
    int b = socket(AF_INET, SOCK_DGRAM, 0), c = socket(AF_INET, SOCK_DGRAM, 0), zero = 0;
    setsockopt(b, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(c, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    CHECK(setsockopt(c, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero) == 0, "IP_MULTICAST_ALL");
    bind(b, (void *)&any, sizeof any); bind(c, (void *)&any, sizeof any);
    n = mk_mcframe(f, peer, gmac, "10.95.0.2", "239.1.2.3", 5000, "mc3"); write(ta, f, n);
    CHECK(recv_str(a, m, sizeof m, 2000) == 3 && recv_str(b, m, sizeof m, 500) == 3 && recv_str(c, m, sizeof m, 200) < 0,
          "delivery to members / mc_all sockets only");
    /* sending: IP_MULTICAST_IF by address, IP_MULTICAST_TTL, IP_MULTICAST_LOOP */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct in_addr ifa = { ip4("10.95.0.1") };
    int ttl = 3;
    CHECK(setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa) == 0 && setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) == 0,
          "IP_MULTICAST_IF / TTL");
    struct in_addr got = { 0 }; vl = sizeof got;
    CHECK(getsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &got, &vl) == 0 && got.s_addr == ifa.s_addr, "IP_MULTICAST_IF read back");
    struct sockaddr_in g = sin_of("239.1.2.3", 5000);
    CHECK(sendto(s, "out", 3, 0, (void *)&g, sizeof g) == 3, "send to the group");
    k = rd_ip(ta, r, sizeof r, 17, 2000, 14);
    CHECK(k == 14 + 20 + 8 + 3 && !memcmp(r, gmac, 6) && !memcmp(r + 6, mac, 6) && r[14 + 8] == 3 && !memcmp(r + 14 + 12, &ifa, 4),
          "frame on the wire: group MAC, ttl %d", k > 22 ? r[22] : -1);
    CHECK(recv_str(a, m, sizeof m, 2000) == 3 && !strcmp(m, "out"), "looped back to local members");
    recv_str(b, m, sizeof m, 500);
    unsigned char loop = 0;
    CHECK(setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) == 0, "IP_MULTICAST_LOOP 0");
    sendto(s, "ou2", 3, 0, (void *)&g, sizeof g);
    k = rd_ip(ta, r, sizeof r, 17, 2000, 14);
    CHECK(k > 0 && recv_str(a, m, sizeof m, 300) < 0, "no loopback copy");
    close(s);
    /* queries: a report for the group follows a general query within the max response time */
    uint8_t qf[64] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    memcpy(qf + 6, peer, 6); qf[12] = 8; qf[13] = 0;
    uint8_t ig[8] = { 0x11, 5, 0, 0, 0, 0, 0, 0 };
    uint16_t cs = cksum(ig, 8, 0); memcpy(ig + 2, &cs, 2);
    n = 14 + mk_ip(qf + 14, "10.95.0.254", "224.0.0.1", 2, 1, 0, ig, 8);
    usleep(1200 * 1000);                                    /* let the repeated unsolicited report go by */
    while (rd_igmp(ta, r, sizeof r, 0x16, 100) > 0) {}
    write(ta, qf, n);
    uint64_t t0 = now_ms();
    k = rd_igmp(ta, r, sizeof r, 0x16, 2000);
    CHECK(k > 0 && !memcmp(r + 14 + 24 + 4, &mr.imr_multiaddr, 4) && now_ms() - t0 < 1000, "report answers a query (%llu ms)", (unsigned long long)(now_ms() - t0));
    /* ip_mreq by interface address on a second socket; the group stays until the last member leaves */
    struct ip_mreq mq = { mr.imr_multiaddr, ifa };
    CHECK(setsockopt(b, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mq, sizeof mq) == 0, "IP_ADD_MEMBERSHIP (ip_mreq)");
    CHECK(setsockopt(a, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof mr) == 0, "IP_DROP_MEMBERSHIP");
    CHECK(rd_igmp(ta, r, sizeof r, 0x17, 300) < 0, "no leave while another socket is a member");
    CHECK(setsockopt(a, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof mr) < 0 && errno == EADDRNOTAVAIL, "dropping twice");
    close(b);
    k = rd_igmp(ta, r, sizeof r, 0x17, 2000);
    CHECK(k > 0 && r[14 + 16] == 224 && r[14 + 19] == 2 && !memcmp(r + 14 + 24 + 4, &mr.imr_multiaddr, 4), "leave group to 224.0.0.2 on close (%zd)", k);
    n = mk_mcframe(f, peer, gmac, "10.95.0.2", "239.1.2.3", 5000, "mc4"); write(ta, f, n);
    CHECK(recv_str(a, m, sizeof m, 300) < 0, "nothing after leaving");
    CHECK(system("grep -q 030201EF /proc/net/igmp") != 0, "group gone from /proc/net/igmp");
    mr.imr_multiaddr.s_addr = ip4("10.1.2.3");
    CHECK(setsockopt(a, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0 && errno == EINVAL, "non-multicast group rejected");
    int ts = socket(AF_INET, SOCK_STREAM, 0);
    mr.imr_multiaddr.s_addr = ip4("239.1.2.3");
    CHECK(setsockopt(ts, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0 && errno == EPROTO, "not on TCP sockets");
    close(ts); close(a); close(c); close(ta);
    OK("multicast");
}

/* ---------------------------------------------------------------- AF_NETLINK (rtnetlink) */
static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int sh(const char *fmt, ...) {
    char c[512];
    va_list ap; va_start(ap, fmt); vsnprintf(c, sizeof c, fmt, ap); va_end(ap);
    if (verbose) printf("  $ %s\n", c);
    int r = system(c);
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}
static void netlink_tests(void) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    CHECK(s >= 0, "AF_NETLINK socket");
    if (s < 0) return;
    CHECK(socket(AF_NETLINK, SOCK_RAW, 31) < 0 && errno == EPROTONOSUPPORT, "only NETLINK_ROUTE");
    struct sockaddr_nl me = { .nl_family = AF_NETLINK };
    CHECK(bind(s, (void *)&me, sizeof me) == 0, "bind");
    socklen_t ml = sizeof me;
    CHECK(getsockname(s, (void *)&me, &ml) == 0 && ml == sizeof me && me.nl_pid == (unsigned)getpid(), "port id = pid (%u)", me.nl_pid);
    int s2 = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    struct sockaddr_nl dup = { .nl_family = AF_NETLINK, .nl_pid = me.nl_pid };
    CHECK(bind(s2, (void *)&dup, sizeof dup) < 0 && errno == EADDRINUSE, "port ids are unique");
    close(s2);
    /* RTM_GETLINK dump: lo and eth0, NLMSG_DONE at the end */
    struct { struct nlmsghdr h; struct ifinfomsg i; } rq = { { sizeof rq, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 7, 0 }, { .ifi_family = AF_UNSPEC } };
    CHECK(send(s, &rq, sizeof rq, 0) == sizeof rq, "send RTM_GETLINK");
    static char b[16384];
    int links = 0, done = 0, lo = 0;
    for (int it = 0; it < 16 && !done; it++) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0) break;
        ssize_t n = recv(s, b, sizeof b, 0);
        for (struct nlmsghdr *h = (void *)b; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type != RTM_NEWLINK || h->nlmsg_seq != 7 || !(h->nlmsg_flags & NLM_F_MULTI)) continue;
            struct ifinfomsg *ii = NLMSG_DATA(h);
            links++;
            int al = IFLA_PAYLOAD(h);
            for (struct rtattr *a = IFLA_RTA(ii); RTA_OK(a, al); a = RTA_NEXT(a, al))
                if (a->rta_type == IFLA_IFNAME && !strcmp(RTA_DATA(a), "lo") && ii->ifi_index == 1 && (ii->ifi_flags & IFF_LOOPBACK)) lo = 1;
        }
    }
    CHECK(done && links >= 2 && lo, "RTM_GETLINK dump (%d links, done %d)", links, done);
    /* errors come back as NLMSG_ERROR acks */
    struct { struct nlmsghdr h; struct ifaddrmsg a; struct rtattr r; uint32_t ip; } ad = {
        { sizeof ad, RTM_NEWADDR, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE, 8, 0 }, { AF_INET, 24, 0, 0, 999 }, { 8, IFA_LOCAL }, 0 };
    send(s, &ad, sizeof ad, 0);
    ssize_t n = recv(s, b, sizeof b, 0);
    struct nlmsghdr *h = (void *)b;
    struct nlmsgerr *e = NLMSG_DATA(h);
    CHECK(n >= (ssize_t)(16 + sizeof *e) && h->nlmsg_type == NLMSG_ERROR && h->nlmsg_seq == 8 && e->error == -ENODEV, "NLMSG_ERROR ack (%d)", n > 16 ? e->error : 0);
    close(s);
    /* musl: getifaddrs and if_nameindex go through netlink */
    struct ifaddrs *ifa, *i;
    int v4lo = 0, v6lo = 0, pk = 0;
    CHECK(getifaddrs(&ifa) == 0, "getifaddrs");
    for (i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr) continue;
        if (i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "lo") && ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) v4lo = 1;
        if (i->ifa_addr->sa_family == AF_INET6 && !strcmp(i->ifa_name, "lo") && IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr)) v6lo = 1;
        if (i->ifa_addr->sa_family == AF_PACKET) pk++;
    }
    freeifaddrs(ifa);
    CHECK(v4lo && v6lo && pk >= 2, "getifaddrs: 127.0.0.1 %d, ::1 %d, links %d", v4lo, v6lo, pk);
    struct if_nameindex *ni = if_nameindex();
    CHECK(ni && ni[0].if_index == 1 && !strcmp(ni[0].if_name, "lo") && ni[1].if_name, "if_nameindex");
    if (ni) if_freenameindex(ni);
    /* BusyBox ip on a TAP device */
    char nm[IFNAMSIZ];
    int ta = tun_new("tapn%d", IFF_TAP | IFF_NO_PI, nm);
    CHECK(ta >= 0, "TAP");
    if (ta < 0) return;
    CHECK(sh("ip link set %s up mtu 1400", nm) == 0 && sh("ip link show %s | grep -q 'UP.*mtu 1400'", nm) == 0, "ip link set up mtu");
    CHECK(sh("ip addr add 10.94.0.1/24 dev %s", nm) == 0 && sh("ip -4 addr show %s | grep -q 'inet 10.94.0.1/24 brd 10.94.0.255'", nm) == 0, "ip addr add");
    CHECK(sh("ip addr add 10.94.0.7/24 dev %s 2>/dev/null", nm) != 0, "a second IPv4 address needs replace");
    CHECK(sh("ifconfig %s | grep -q 'inet addr:10.94.0.1'", nm) == 0 && sh("ip route | grep -q '10.94.0.0/24 dev %s'", nm) == 0, "seen by ioctls, prefix route");
    CHECK(sh("ip route add 10.93.0.0/16 via 10.94.0.2") == 0 && sh("route -n | grep -q '^10.93.0.0 *10.94.0.2 *255.255.0.0'") == 0, "ip route add");
    CHECK(sh("ip route get 10.93.1.1 | grep -q 'via 10.94.0.2 dev %s'", nm) == 0, "ip route get");
    CHECK(sh("ip route del 10.93.0.0/16") == 0 && sh("route -n | grep -q '^10.93.0.0'") != 0, "ip route del");
    /* BusyBox ip neigh only shows/flushes: add and delete with raw RTM_NEWNEIGH/RTM_DELNEIGH */
    int ns = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    struct { struct nlmsghdr h; struct ndmsg n; struct rtattr r1; uint32_t ip; struct rtattr r2; uint8_t mac[6], pad[2]; } nq = {
        { sizeof nq, RTM_NEWNEIGH, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE, 9, 0 }, { .ndm_family = AF_INET, .ndm_ifindex = (int)if_nametoindex(nm), .ndm_state = NUD_PERMANENT },
        { 8, NDA_DST }, ip4("10.94.0.9"), { 10, NDA_LLADDR }, { 2, 0, 0, 0, 0, 9 }, { 0 } };
    send(ns, &nq, sizeof nq, 0);
    n = recv(ns, b, sizeof b, 0);
    CHECK(n >= 36 && h->nlmsg_type == NLMSG_ERROR && e->error == 0 && sh("grep -q '10.94.0.9 .*02:00:00:00:00:09' /proc/net/arp") == 0 &&
          sh("ip neigh show dev %s | grep -q '10.94.0.9 lladdr 02:00:00:00:00:09 PERMANENT'", nm) == 0, "RTM_NEWNEIGH (%d)", n >= 36 ? e->error : 0);
    nq.h.nlmsg_type = RTM_DELNEIGH; nq.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    send(ns, &nq, sizeof nq, 0);
    n = recv(ns, b, sizeof b, 0);
    CHECK(n >= 36 && e->error == 0 && sh("grep -q '10.94.0.9 ' /proc/net/arp") != 0, "RTM_DELNEIGH");
    close(ns);
    CHECK(sh("ip -6 addr add 2001:db8:94::1/64 dev %s", nm) == 0 && sh("ip -6 route | grep -q '2001:db8:94::/64 dev %s'", nm) == 0, "ip -6 addr add");
    CHECK(sh("ip -6 route add 2001:db8:95::/48 via 2001:db8:94::2") == 0 && sh("grep -q '^20010db8009500000000000000000000 30 .* 20010db8009400000000000000000002 ' /proc/net/ipv6_route") == 0,
          "ip -6 route add");
    CHECK(sh("ip -6 route del 2001:db8:95::/48") == 0 && sh("ip -6 addr del 2001:db8:94::1/64 dev %s", nm) == 0 && sh("grep -q 20010db8009400000000000000000001 /proc/net/if_inet6") != 0,
          "ip -6 route/addr del");
    CHECK(sh("ip addr del 10.94.0.1/24 dev %s", nm) == 0 && sh("ip -4 addr show %s | grep -q inet", nm) != 0, "ip addr del");
    CHECK(sh("ip link set %s down", nm) == 0 && sh("ip link show %s | grep -q ',UP'", nm) != 0 && sh("grep -q %s /proc/net/if_inet6", nm) != 0, "ip link set down drops IPv6 addresses");
    CHECK(sh("grep -q '^sk ' /proc/net/netlink") == 0, "/proc/net/netlink");
    close(ta);
    OK("netlink");
}

/* ---------------------------------------------------------------- IPv6 sockets over loopback */
static struct sockaddr_in6 sin6_of(const char *ip, int port) {
    struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return a;
}
static void ipv6_lo_tests(void) {
    int one = 1, v = 0;
    socklen_t vl;
    /* TCP over ::1 */
    int l = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a = sin6_of("::1", 0);
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    CHECK(bind(l, (void *)&a, sizeof a) == 0 && listen(l, 4) == 0, "bind/listen [::1]");
    socklen_t al = sizeof a;
    getsockname(l, (void *)&a, &al);
    CHECK(al == sizeof a && a.sin6_family == AF_INET6 && ntohs(a.sin6_port) >= 1024, "getsockname sockaddr_in6 (port %d)", ntohs(a.sin6_port));
    int port = ntohs(a.sin6_port);
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(connect(c, (void *)&a, sizeof a) == 0, "connect [::1]:%d", port);
    struct sockaddr_in6 pa; al = sizeof pa;
    int k = accept(l, (void *)&pa, &al);
    CHECK(k >= 0 && pa.sin6_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&pa.sin6_addr), "accept, peer ::1");
    char buf[8192];
    size_t total = 4 << 20, sent = 0, got = 0;
    pid_t ch = fork();
    if (!ch) {
        for (size_t i = 0; i < total; i += sizeof buf) { for (size_t j = 0; j < sizeof buf; j++) buf[j] = (char)pat(i + j, 6); if (write(c, buf, sizeof buf) != sizeof buf) _exit(1); }
        _exit(0);
    }
    close(c);
    int bad = 0;
    ssize_t r;
    while ((r = read(k, buf, sizeof buf)) > 0) { for (ssize_t j = 0; j < r; j++) if (buf[j] != (char)pat(got + j, 6)) bad = 1; got += r; }
    int st; waitpid(ch, &st, 0); (void)sent;
    CHECK(got == total && !bad && WIFEXITED(st) && !WEXITSTATUS(st), "4 MiB over TCP/IPv6 (%zu, bad %d)", got, bad);
    CHECK(sh("grep -q '^ *[0-9]*: 00000000000000000000000001000000:%04X 00000000000000000000000000000000:0000 0A' /proc/net/tcp6", port) == 0, "/proc/net/tcp6 listener");
    close(k);
    /* dual stack: a :: listener takes IPv4 connections (peer ::ffff:127.0.0.1) unless IPV6_V6ONLY */
    int d = socket(AF_INET6, SOCK_STREAM, 0);
    vl = sizeof v;
    CHECK(getsockopt(d, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) == 0 && v == 0, "IPV6_V6ONLY default 0");
    struct sockaddr_in6 any = sin6_of("::", 0);
    bind(d, (void *)&any, sizeof any); listen(d, 4);
    al = sizeof any; getsockname(d, (void *)&any, &al);
    int c4 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a4 = sin_of("127.0.0.1", ntohs(any.sin6_port));
    CHECK(connect(c4, (void *)&a4, sizeof a4) == 0, "IPv4 connect to a :: listener");
    al = sizeof pa;
    k = accept(d, (void *)&pa, &al);
    char ps[64] = "";
    inet_ntop(AF_INET6, &pa.sin6_addr, ps, sizeof ps);
    CHECK(k >= 0 && IN6_IS_ADDR_V4MAPPED(&pa.sin6_addr) && !strcmp(ps, "::ffff:127.0.0.1"), "v4-mapped peer %s", ps);
    struct sockaddr_in6 la; al = sizeof la;
    getsockname(k, (void *)&la, &al);
    inet_ntop(AF_INET6, &la.sin6_addr, ps, sizeof ps);
    CHECK(write(c4, "x", 1) == 1 && read(k, buf, 1) == 1 && !strcmp(ps, "::ffff:127.0.0.1"), "data, local %s", ps);
    close(k); close(c4); close(d);
    d = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(setsockopt(d, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one) == 0, "IPV6_V6ONLY");
    any = sin6_of("::", 0);
    bind(d, (void *)&any, sizeof any); listen(d, 4);
    al = sizeof any; getsockname(d, (void *)&any, &al);
    c4 = socket(AF_INET, SOCK_STREAM, 0);
    a4 = sin_of("127.0.0.1", ntohs(any.sin6_port));
    CHECK(connect(c4, (void *)&a4, sizeof a4) < 0 && errno == ECONNREFUSED, "v6only listener refuses IPv4");
    int x4 = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(x4, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    a4 = sin_of("0.0.0.0", ntohs(any.sin6_port));
    CHECK(bind(x4, (void *)&a4, sizeof a4) == 0, "IPv4 bind on the port of a v6only socket");
    close(x4); close(c4); close(d);
    d = socket(AF_INET6, SOCK_STREAM, 0);
    any = sin6_of("::", 0);
    bind(d, (void *)&any, sizeof any);
    al = sizeof any; getsockname(d, (void *)&any, &al);
    x4 = socket(AF_INET, SOCK_STREAM, 0);
    a4 = sin_of("0.0.0.0", ntohs(any.sin6_port));
    CHECK(bind(x4, (void *)&a4, sizeof a4) < 0 && errno == EADDRINUSE, "dual-stack bind owns the IPv4 port");
    close(x4); close(d);
    /* bind errors */
    d = socket(AF_INET6, SOCK_DGRAM, 0);
    a = sin6_of("2001:db8::77", 0);
    CHECK(bind(d, (void *)&a, sizeof a) < 0 && errno == EADDRNOTAVAIL, "bind to a foreign address");
    a = sin6_of("fe80::1", 0);
    CHECK(bind(d, (void *)&a, sizeof a) < 0 && errno == EINVAL, "link-local bind needs a scope id");
    CHECK(bind(d, (void *)&a, 20) < 0 && errno == EINVAL, "short sockaddr_in6");
    close(d);
    /* UDP over ::1 with ancillary data */
    int u = socket(AF_INET6, SOCK_DGRAM, 0), w = socket(AF_INET6, SOCK_DGRAM, 0);
    a = sin6_of("::1", 0);
    bind(u, (void *)&a, sizeof a);
    al = sizeof a; getsockname(u, (void *)&a, &al);
    CHECK(setsockopt(u, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof one) == 0 && setsockopt(u, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof one) == 0 &&
          setsockopt(u, IPPROTO_IPV6, IPV6_RECVTCLASS, &one, sizeof one) == 0, "IPV6_RECVPKTINFO/HOPLIMIT/TCLASS");
    int hops = 9, tc = 0x28;
    CHECK(setsockopt(w, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof hops) == 0 && setsockopt(w, IPPROTO_IPV6, IPV6_TCLASS, &tc, sizeof tc) == 0, "IPV6_UNICAST_HOPS/TCLASS");
    vl = sizeof v;
    CHECK(getsockopt(w, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &v, &vl) == 0 && v == 9, "read back hops %d", v);
    CHECK(sendto(w, "six", 3, 0, (void *)&a, sizeof a) == 3, "sendto [::1]");
    char cb[256], m[16] = "";
    struct iovec iov = { m, sizeof m };
    struct sockaddr_in6 from;
    struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    struct pollfd p = { u, POLLIN, 0 };
    poll(&p, 1, 1000);
    r = recvmsg(u, &mh, MSG_DONTWAIT);
    int gh = -1, gt = -1, gi = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level != IPPROTO_IPV6) continue;
        if (cm->cmsg_type == IPV6_HOPLIMIT) memcpy(&gh, CMSG_DATA(cm), 4);
        if (cm->cmsg_type == IPV6_TCLASS) memcpy(&gt, CMSG_DATA(cm), 4);
        if (cm->cmsg_type == IPV6_PKTINFO) { struct in6_pktinfo pi; memcpy(&pi, CMSG_DATA(cm), sizeof pi); gi = IN6_IS_ADDR_LOOPBACK(&pi.ipi6_addr) ? (int)pi.ipi6_ifindex : -2; }
    }
    CHECK(r == 3 && !memcmp(m, "six", 3) && mh.msg_namelen == sizeof from && IN6_IS_ADDR_LOOPBACK(&from.sin6_addr), "recvmsg (%zd)", r);
    CHECK(gh == 9 && gt == 0x28 && gi == 1, "cmsgs: hoplimit %d tclass %#x ifindex %d", gh, gt, gi);
    /* per-packet IPV6_HOPLIMIT on send */
    char sb[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr sm = { .msg_name = &a, .msg_namelen = sizeof a, .msg_iov = &iov, .msg_iovlen = 1, .msg_control = sb, .msg_controllen = sizeof sb };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&sm);
    cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_HOPLIMIT; cm->cmsg_len = CMSG_LEN(sizeof(int));
    int h2 = 3; memcpy(CMSG_DATA(cm), &h2, sizeof h2);
    iov.iov_len = 3;
    CHECK(sendmsg(w, &sm, 0) == 3, "sendmsg with IPV6_HOPLIMIT");
    iov.iov_len = sizeof m;
    mh.msg_controllen = sizeof cb; mh.msg_namelen = sizeof from;
    poll(&p, 1, 1000);
    recvmsg(u, &mh, MSG_DONTWAIT);
    gh = -1;
    for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) if (cm->cmsg_type == IPV6_HOPLIMIT) memcpy(&gh, CMSG_DATA(cm), 4);
    CHECK(gh == 3, "per-packet hop limit %d", gh);
    /* a v4 socket's datagram reaches a dual-stack v6 socket as ::ffff:127.0.0.1 */
    int u6 = socket(AF_INET6, SOCK_DGRAM, 0);
    any = sin6_of("::", 0);
    bind(u6, (void *)&any, sizeof any);
    al = sizeof any; getsockname(u6, (void *)&any, &al);
    int u4 = socket(AF_INET, SOCK_DGRAM, 0);
    a4 = sin_of("127.0.0.1", ntohs(any.sin6_port));
    sendto(u4, "v4", 2, 0, (void *)&a4, sizeof a4);
    al = sizeof from;
    p.fd = u6; poll(&p, 1, 1000);
    r = recvfrom(u6, m, sizeof m, MSG_DONTWAIT, (void *)&from, &al);
    CHECK(r == 2 && IN6_IS_ADDR_V4MAPPED(&from.sin6_addr), "IPv4 datagram on a dual-stack socket");
    struct sockaddr_in a4b; al = sizeof a4b;
    getsockname(u4, (void *)&a4b, &al);
    CHECK(sendto(u6, "rp", 2, 0, (void *)&from, sizeof from) == 2 && recv(u4, m, sizeof m, 0) == 2, "reply to a v4-mapped address");
    CHECK(sh("grep -q ':%04X 00000000000000000000000000000000:0000 07' /proc/net/udp6", ntohs(any.sin6_port)) == 0, "/proc/net/udp6");
    close(u4); close(u6); close(u); close(w);
    /* ICMPv6: raw socket echo with a filter, and an unprivileged-style ping socket */
    int rs = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    CHECK(rs >= 0, "raw ICMPv6 socket");
    struct icmp6_filter f;
    ICMP6_FILTER_SETBLOCKALL(&f);
    ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &f);
    CHECK(setsockopt(rs, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof f) == 0, "ICMP6_FILTER");
    uint8_t er[16] = { 128, 0, 0, 0, 0x12, 0x34, 0, 1, 'p', 'i', 'n', 'g', '6' };
    a = sin6_of("::1", 0);
    CHECK(sendto(rs, er, 13, 0, (void *)&a, sizeof a) == 13, "send echo request (kernel checksums)");
    uint8_t rb[256];
    p.fd = rs; poll(&p, 1, 1000);
    r = recv(rs, rb, sizeof rb, MSG_DONTWAIT);
    CHECK(r == 13 && rb[0] == 129 && !memcmp(rb + 4, er + 4, 9), "echo reply, request filtered out (%zd, type %d)", r, r > 0 ? rb[0] : -1);
    close(rs);
    int ps6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    CHECK(ps6 >= 0, "ICMPv6 ping socket");
    CHECK(sendto(ps6, er, 13, 0, (void *)&a, sizeof a) == 13, "ping socket send");
    p.fd = ps6; poll(&p, 1, 1000);
    r = recv(ps6, rb, sizeof rb, MSG_DONTWAIT);
    CHECK(r == 13 && rb[0] == 129 && !memcmp(rb + 8, "ping6", 5), "ping socket reply (%zd)", r);
    close(ps6);
    CHECK(sh("ping6 -c 1 -W 2 ::1 >/dev/null") == 0, "busybox ping6 ::1");
    close(l);
    OK("ipv6 loopback");
}

/* ---------------------------------------------------------------- IPv6 on the wire: ND, DAD, SLAAC, MLD (over TAP) */
static uint32_t sum16(const uint8_t *p, int n) { uint32_t s = 0; for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]); if (n) s += (uint32_t)p[0] << 8; return s; }
/* ethernet + IPv6 frame; ICMPv6/UDP checksums filled in */
static size_t mk6(uint8_t *f, const uint8_t *smac, const uint8_t *dmac, const uint8_t *src, const uint8_t *dst, int nxt, int hlim, const void *pl, size_t plen) {
    memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 0x86; f[13] = 0xdd;
    uint8_t *h = f + 14;
    memset(h, 0, 40);
    h[0] = 0x60; h[4] = (uint8_t)(plen >> 8); h[5] = (uint8_t)plen; h[6] = (uint8_t)nxt; h[7] = (uint8_t)hlim;
    memcpy(h + 8, src, 16); memcpy(h + 24, dst, 16);
    memcpy(h + 40, pl, plen);
    int co = nxt == 58 ? 2 : nxt == 17 ? 6 : nxt == 6 ? 16 : -1;
    if (co >= 0) {
        h[40 + co] = h[41 + co] = 0;
        uint16_t c = cksum(h + 40, (int)plen, sum16(h + 8, 32) + (uint32_t)plen + (uint32_t)nxt);
        memcpy(h + 40 + co, &c, 2);
    }
    return 54 + plen;
}
/* next IPv6 frame carrying ICMPv6 type (after hop-by-hop options), UDP (-17) or TCP (-6); returns the
 * upper-layer offset in *off */
static ssize_t rd6(int fd, uint8_t *b, size_t n, int type, int ms, int *off) {
    uint64_t end = now_ms() + ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left <= 0) return -1;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, left) <= 0) return -1;
        ssize_t r = read(fd, b, n);
        if (r < 54 || b[12] != 0x86 || b[13] != 0xdd) continue;
        int nxt = b[14 + 6], o = 54;
        if (nxt == 0) { nxt = b[o]; o += (b[o + 1] + 1) * 8; }
        if (o >= r) continue;
        if (verbose) printf("  rd6: %zd bytes to %02x:%02x:%02x:%02x:%02x:%02x nxt %d type %d\n", r, b[0], b[1], b[2], b[3], b[4], b[5], nxt, b[o]);
        if ((type < 0 && nxt == -type) || (type >= 0 && nxt == 58 && b[o] == type)) { *off = o; return r; }
    }
}
static void a6(uint8_t *d, const char *s) { inet_pton(AF_INET6, s, d); }
static void ipv6_tap_tests(void) {
    char nm[IFNAMSIZ];
    int ta = tun_new("tap6%d", IFF_TAP | IFF_NO_PI, nm);
    CHECK(ta >= 0, "TAP");
    if (ta < 0) return;
    int ifi = (int)if_nametoindex(nm), off = 0;
    struct ifreq q = { 0 }; strcpy(q.ifr_name, nm);
    int t = socket(AF_INET, SOCK_DGRAM, 0);
    ioctl(t, SIOCGIFHWADDR, &q);
    close(t);
    uint8_t mac[6], peer[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x77 }, ll[16], pll[16], sn[16], any[16] = { 0 }, allr[16], alln[16], g[16];
    memcpy(mac, q.ifr_hwaddr.sa_data, 6);
    a6(ll, "fe80::"); ll[8] = mac[0] ^ 2; ll[9] = mac[1]; ll[10] = mac[2]; ll[11] = 0xff; ll[12] = 0xfe; ll[13] = mac[3]; ll[14] = mac[4]; ll[15] = mac[5];
    a6(pll, "fe80::11:22ff:fe33:4477"); a6(allr, "ff02::2"); a6(alln, "ff02::1");
    a6(sn, "ff02::1:ff00:0"); memcpy(sn + 13, ll + 13, 3);
    uint8_t snmac[6] = { 0x33, 0x33, 0xff, ll[13], ll[14], ll[15] };
    uint8_t f[2048], r[2048];
    CHECK(sh("ip link set %s up", nm) == 0, "link up");
    /* DAD for the EUI-64 link-local address: NS from :: to the solicited-node group */
    uint64_t t0 = now_ms();
    ssize_t k = rd6(ta, r, sizeof r, 135, 2000, &off);
    CHECK(k > 0 && !memcmp(r, snmac, 6) && !memcmp(r + 22, any, 16) && !memcmp(r + 38, sn, 16) && r[21] == 255 && !memcmp(r + off + 8, ll, 16),
          "DAD neighbour solicitation (%zd)", k);
    /* MLD report for the solicited-node group (hop limit 1, router alert) */
    CHECK(sh("grep -q '%s.*ff0200000000000000000001ff' /proc/net/igmp6", nm) == 0, "/proc/net/igmp6 has the solicited-node group");
    k = rd6(ta, r, sizeof r, 133, 3000, &off);
    CHECK(k > 0 && !memcmp(r + 22, ll, 16) && !memcmp(r + 38, allr, 16) && r[off + 8] == 1 && !memcmp(r + off + 10, mac, 6), "router solicitation after DAD (%llu ms)",
          (unsigned long long)(now_ms() - t0));
    CHECK(sh("grep -q '%s' /proc/net/if_inet6", nm) == 0 && sh("ip -6 addr show %s | grep -q 'scope link' && ! ip -6 addr show %s | grep -q tentative", nm, nm) == 0, "link-local usable");
    /* router advertisement: prefix 2001:db8:6::/64 on-link + autonomous, default route, MTU 1480 */
    uint8_t ra[64] = { 134, 0, 0, 0, 64, 0, 0x07, 0x08, 0, 0, 0, 0, 0, 0, 0, 0,
                       1, 1, 0x02, 0x11, 0x22, 0x33, 0x44, 0x77,
                       5, 1, 0, 0, 0, 0, 0x05, 0xc8,
                       3, 4, 64, 0xc0, 0, 0, 0x0e, 0x10, 0, 0, 0x07, 0x08, 0, 0, 0, 0 };
    uint8_t pfx[16]; a6(pfx, "2001:db8:6::");
    memcpy(ra + 48, pfx, 16);
    write(ta, f, mk6(f, peer, (uint8_t[]){ 0x33, 0x33, 0, 0, 0, 1 }, pll, alln, 58, 255, ra, 64));
    uint8_t ga[16]; memcpy(ga, pfx, 8); memcpy(ga + 8, ll + 8, 8);
    char gs[64]; inet_ntop(AF_INET6, ga, gs, sizeof gs);
    k = rd6(ta, r, sizeof r, 135, 2000, &off);
    CHECK(k > 0 && !memcmp(r + 22, any, 16) && !memcmp(r + off + 8, ga, 16), "DAD for the SLAAC address %s", gs);
    usleep(1500 * 1000);
    CHECK(sh("ip -6 addr show %s | grep -q 'inet6 %s/64 scope global dynamic'", nm, gs) == 0, "SLAAC address");
    CHECK(sh("ip -6 route | grep -q 'default via fe80::11:22ff:fe33:4477 dev %s'", nm) == 0 && sh("ip -6 route | grep -q '2001:db8:6::/64 dev %s'", nm) == 0, "default + prefix routes from the RA");
    /* slirp's default route on eth0 wins ties, so route the test prefix explicitly */
    CHECK(sh("ip -6 route add 2001:db8:99::/48 via fe80::11:22ff:fe33:4477 dev %s", nm) == 0, "ip -6 route add via a link-local gateway");
    CHECK(sh("ip -6 route get 2001:db8:99::1 | grep -q 'via fe80::11:22ff:fe33:4477 dev %s'", nm) == 0, "route get via the router");
    /* UDP to an off-link host: neighbour discovery for the router, then the datagram to its MAC */
    int u = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 dst = sin6_of("2001:db8:99::1", 7777);
    CHECK(sendto(u, "hello6", 6, 0, (void *)&dst, sizeof dst) == 6, "sendto off-link");
    /* the RA's source link-layer option made the router a STALE neighbour: the datagram goes
     * straight to its MAC, followed by a unicast NS to reconfirm it */
    k = rd6(ta, r, sizeof r, -17, 2000, &off);
    CHECK(k > 0 && !memcmp(r, peer, 6) && !memcmp(r + 22, ga, 16) && !memcmp(r + off + 8, "hello6", 6) && r[21] == 64, "datagram to the router's MAC from the SLAAC address");
    k = rd6(ta, r, sizeof r, 135, 2000, &off);
    CHECK(k > 0 && !memcmp(r, peer, 6) && !memcmp(r + 38, pll, 16) && !memcmp(r + off + 8, pll, 16) && r[off + 24] == 1, "unicast NS probe for the router (%zd)", k);
    uint8_t na[32] = { 136, 0, 0, 0, 0xe0, 0, 0, 0 };                 /* router, solicited, override */
    memcpy(na + 8, pll, 16); na[24] = 2; na[25] = 1; memcpy(na + 26, peer, 6);
    write(ta, f, mk6(f, peer, mac, pll, k > 0 ? r + 22 : ll, 58, 255, na, 32));
    usleep(100 * 1000);
    CHECK(sh("ip -6 neigh show dev %s | grep -q 'fe80::11:22ff:fe33:4477 lladdr 02:11:22:33:44:77 router REACHABLE'", nm) == 0, "ip -6 neigh");
    /* incoming: we answer NS for our address and echo requests (over the router) */
    uint8_t ns[32] = { 135, 0, 0, 0, 0, 0, 0, 0 };
    memcpy(ns + 8, ga, 16); ns[24] = 1; ns[25] = 1; memcpy(ns + 26, peer, 6);
    uint8_t gsn[16]; a6(gsn, "ff02::1:ff00:0"); memcpy(gsn + 13, ga + 13, 3);
    write(ta, f, mk6(f, peer, (uint8_t[]){ 0x33, 0x33, 0xff, ga[13], ga[14], ga[15] }, pll, gsn, 58, 255, ns, 32));
    k = rd6(ta, r, sizeof r, 136, 2000, &off);
    CHECK(k > 0 && !memcmp(r + 38, pll, 16) && (r[off + 4] & 0x40) && !memcmp(r + off + 8, ga, 16) && r[off + 24] == 2 && !memcmp(r + off + 26, mac, 6),
          "neighbour advertisement (%zd)", k);
    uint8_t rem[16]; a6(rem, "2001:db8:99::5");
    uint8_t ec[16] = { 128, 0, 0, 0, 0xab, 0xcd, 0, 1, 'e', 'c', 'h', 'o' };
    write(ta, f, mk6(f, peer, mac, rem, ga, 58, 60, ec, 12));
    k = rd6(ta, r, sizeof r, 129, 2000, &off);
    CHECK(k > 0 && !memcmp(r, peer, 6) && !memcmp(r + 38, rem, 16) && !memcmp(r + off + 4, ec + 4, 8), "echo reply");
    /* bad checksum: dropped silently */
    size_t n = mk6(f, peer, mac, rem, ga, 58, 60, ec, 12); f[54 + 2] ^= 0xff; write(ta, f, n);
    CHECK(rd6(ta, r, sizeof r, 129, 400, &off) < 0, "bad ICMPv6 checksum dropped");
    /* UDP to a closed port: ICMPv6 port unreachable */
    uint8_t ud[16] = { 0x30, 0x39, 0x22, 0xb8, 0, 12, 0, 0, 'n', 'o', 'n', 'e' };
    write(ta, f, mk6(f, peer, mac, rem, ga, 17, 60, ud, 12));
    k = rd6(ta, r, sizeof r, 1, 2000, &off);
    CHECK(k > 0 && r[off + 1] == 4 && !memcmp(r + off + 8 + 24, ga, 16), "port unreachable");
    /* multicast: IPV6_JOIN_GROUP sends an MLD report; datagrams to the group are delivered */
    a6(g, "ff12::1234");
    int mu = socket(AF_INET6, SOCK_DGRAM, 0), one = 1;
    setsockopt(mu, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 b6 = sin6_of("::", 5006);
    bind(mu, (void *)&b6, sizeof b6);
    struct ipv6_mreq mr = { .ipv6mr_interface = (unsigned)ifi };
    memcpy(&mr.ipv6mr_multiaddr, g, 16);
    CHECK(setsockopt(mu, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr, sizeof mr) == 0, "IPV6_JOIN_GROUP");
    k = rd6(ta, r, sizeof r, 131, 2000, &off);
    CHECK(k > 0 && !memcmp(r, (uint8_t[]){ 0x33, 0x33, 0, 0, 0x12, 0x34 }, 6) && r[20] == 0 && r[21] == 1 && !memcmp(r + 38, g, 16) && !memcmp(r + off + 8, g, 16),
          "MLD report with hop-by-hop router alert (%zd)", k);
    CHECK(sh("grep -q 'ff120000000000000000000000001234' /proc/net/igmp6") == 0, "/proc/net/igmp6 lists the group");
    uint8_t mud[16] = { 0x30, 0x39, 0x13, 0x8e, 0, 11, 0, 0, 'm', 'l', 'd' };
    write(ta, f, mk6(f, peer, (uint8_t[]){ 0x33, 0x33, 0, 0, 0x12, 0x34 }, pll, g, 17, 1, mud, 11));
    char m[16];
    CHECK(recv_str(mu, m, sizeof m, 2000) == 3 && !strcmp(m, "mld"), "datagram to the group");
    /* MLD general query: report within the max response delay */
    uint8_t mq[24] = { 130, 0, 0, 0, 0x01, 0xf4 };
    usleep(1200 * 1000);
    while (rd6(ta, r, sizeof r, 131, 100, &off) > 0) {}
    write(ta, f, mk6(f, peer, (uint8_t[]){ 0x33, 0x33, 0, 0, 0, 1 }, pll, alln, 58, 1, mq, 24));
    t0 = now_ms();
    int seen = 0;
    while ((k = rd6(ta, r, sizeof r, 131, 1500, &off)) > 0) if (!memcmp(r + off + 8, g, 16)) { seen = 1; break; }
    CHECK(seen && now_ms() - t0 < 1000, "MLD report answers a query");
    CHECK(setsockopt(mu, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mr, sizeof mr) == 0, "IPV6_LEAVE_GROUP");
    k = rd6(ta, r, sizeof r, 132, 2000, &off);
    CHECK(k > 0 && !memcmp(r + 38, allr, 16) && !memcmp(r + off + 8, g, 16), "MLD done to ff02::2");
    close(mu);
    /* sending to a link-local multicast group needs an interface; IPV6_MULTICAST_IF / scope id */
    struct sockaddr_in6 mg = sin6_of("ff02::fb", 5353);
    int ms = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(setsockopt(ms, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifi, sizeof ifi) == 0, "IPV6_MULTICAST_IF");
    CHECK(sendto(ms, "mdns", 4, 0, (void *)&mg, sizeof mg) == 4, "send to ff02::fb");
    k = rd6(ta, r, sizeof r, -17, 2000, &off);
    CHECK(k > 0 && !memcmp(r, (uint8_t[]){ 0x33, 0x33, 0, 0, 0, 0xfb }, 6) && r[21] == 1 && !memcmp(r + 22, ll, 16), "multicast frame from the link-local address, hop limit 1");
    close(ms);
    /* TCP over the link: a SYN to the router's MAC; the RST we inject makes connect fail with ECONNREFUSED */
    int tc = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    dst = sin6_of("2001:db8:99::1", 80);
    CHECK(connect(tc, (void *)&dst, sizeof dst) < 0 && errno == EINPROGRESS, "non-blocking connect");
    k = rd6(ta, r, sizeof r, -6, 2000, &off);
    CHECK(k > 0 && !memcmp(r, peer, 6) && !memcmp(r + 22, ga, 16) && r[off + 13] == 0x02 && cksum(r + off, (int)(k - off), sum16(r + 22, 32) + (uint32_t)(k - off) + 6) == 0,
          "SYN with a valid checksum (%zd)", k);
    if (k > 0) {
        uint8_t rst[20] = { 0 };
        memcpy(rst, r + off + 2, 2); memcpy(rst + 2, r + off, 2);
        uint32_t sq; memcpy(&sq, r + off + 4, 4); sq = htonl(ntohl(sq) + 1); memcpy(rst + 8, &sq, 4);
        rst[12] = 0x50; rst[13] = 0x14;
        write(ta, f, mk6(f, peer, mac, r + 38, ga, 6, 64, rst, 20));
    }
    struct pollfd tp = { tc, POLLOUT, 0 };
    poll(&tp, 1, 2000);
    int err = 0; socklen_t el = sizeof err;
    getsockopt(tc, SOL_SOCKET, SO_ERROR, &err, &el);
    CHECK(err == ECONNREFUSED, "RST: connection refused (%d)", err);
    close(tc);
    /* DAD failure: a manually added address that someone else answers for */
    CHECK(sh("ip -6 addr add 2001:db8:6::abcd/64 dev %s", nm) == 0, "add 2001:db8:6::abcd");
    uint8_t dup[16]; a6(dup, "2001:db8:6::abcd");
    k = rd6(ta, r, sizeof r, 135, 2000, &off);
    while (k > 0 && memcmp(r + off + 8, dup, 16)) k = rd6(ta, r, sizeof r, 135, 2000, &off);
    memcpy(na + 8, dup, 16); na[4] = 0x20;
    write(ta, f, mk6(f, peer, (uint8_t[]){ 0x33, 0x33, 0, 0, 0, 1 }, dup, alln, 58, 255, na, 32));
    usleep(300 * 1000);
    CHECK(k > 0 && sh("ip -6 addr show %s | grep 2001:db8:6::abcd | grep -q dadfailed", nm) == 0, "duplicate address detected");
    struct sockaddr_in6 bd = sin6_of("2001:db8:6::abcd", 0);
    int bs = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(bind(bs, (void *)&bd, sizeof bd) < 0 && errno == EADDRNOTAVAIL, "cannot bind a dadfailed address");
    close(bs); close(u);
    /* RA with router lifetime 0 removes the default route */
    ra[6] = ra[7] = 0;
    write(ta, f, mk6(f, peer, (uint8_t[]){ 0x33, 0x33, 0, 0, 0, 1 }, pll, alln, 58, 255, ra, 64));
    usleep(200 * 1000);
    CHECK(sh("ip -6 route | grep -q 'default via fe80::11:22ff:fe33:4477'") != 0, "router lifetime 0 drops the default route");
    close(ta);
    usleep(100 * 1000);
    CHECK(sh("grep -q 20010db8000600000000000000000000 /proc/net/ipv6_route") != 0, "routes gone with the device");
    OK("ipv6 over tap");
}

static const char *stage;
static void on_alarm(int sig) {
    (void)sig;
    char b[128];
    int n = snprintf(b, sizeof b, "net2test: FAIL %s timed out\n", stage);
    write(1, b, n);
    system("cat /proc/net/tcp; grep Tcp: /proc/net/snmp");
    sysctl_set("net/core/9os_lo_drop_every", 0);
    lo_mtu(65536);
    _exit(2);
}
#define RUN(fn) do { stage = #fn; alarm(120); fn(); alarm(0); } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    verbose = getenv("NETTEST_V") != NULL;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);
    const char *only = argc > 1 ? argv[1] : NULL;
#define T(fn) if (!only || !strcmp(only, #fn)) RUN(fn)
    T(sysctl_tests);
    T(tcp_loss);
    T(tun_tests);
    T(mcast_tests);
    T(netlink_tests);
    T(ipv6_lo_tests);
    T(ipv6_tap_tests);
    printf("net2test: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
