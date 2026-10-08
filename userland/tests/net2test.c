/* M32b networking: /proc/sys knobs, TCP under loss (SACK, timestamps), TUN/TAP devices,
 * IPv4 forwarding (TTL, ICMP time exceeded / fragmentation needed, re-fragmentation).
 * usage: net2test   (as root) */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
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
    printf("net2test: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
