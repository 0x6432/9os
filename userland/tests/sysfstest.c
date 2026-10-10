/* sysfs + uevents (M34): /sys/dev/char|block links agree with stat() of /dev nodes and with the
 * evdev/netdev/block ioctls; class/bus/device links resolve; a full walk of /sys reads every
 * file and resolves every link; NETLINK_KOBJECT_UEVENT: "change" written to a uevent file is
 * delivered (source pid 0, group 1, SCM_CREDENTIALS uid 0, SEQNUM = /sys/kernel/uevent_seqnum)
 * also to unprivileged listeners, bad actions get EINVAL, non-root writes EACCES; TAP hotplug
 * sends add/remove and the /sys/class/net entry comes and goes; user multicast needs
 * CAP_NET_ADMIN and carries the sender's port id and credentials.
 * -u: udevshim end to end (daemon re-broadcast in libudev format on group 2). */
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/if_tun.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("sysfstest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fails++; } } while (0)

static int rd(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    if (n && buf[n - 1] == '\n') buf[--n] = 0;
    return (int)n;
}
static int wr(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -errno;
    ssize_t n = write(fd, s, strlen(s));
    int e = errno;
    close(fd);
    return n < 0 ? -e : 0;
}
static bool ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
static const char *link_base(const char *l, char *buf, size_t cap) {
    ssize_t n = readlink(l, buf, cap - 1);
    if (n < 0) return "";
    buf[n] = 0;
    const char *b = strrchr(buf, '/');
    return b ? b + 1 : buf;
}

/* ---------------------------------------------------------------- structure */
static void test_input(void) {
    struct stat st;
    if (stat("/dev/input/event0", &st)) { printf("sysfstest: no input devices, skipped\n"); return; }
    char l[64], rp[PATH_MAX], b[4096], p[PATH_MAX + 64];
    snprintf(l, sizeof l, "/sys/dev/char/%u:%u", major(st.st_rdev), minor(st.st_rdev));
    CHECK(realpath(l, rp) && !strncmp(rp, "/sys/devices/", 13) && ends_with(rp, "/event0"), "%s -> %s", l, rp);
    snprintf(p, sizeof p, "%s/dev", rp);
    char want[32];
    snprintf(want, sizeof want, "%u:%u", major(st.st_rdev), minor(st.st_rdev));
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, want), "dev %s", b);
    snprintf(p, sizeof p, "%s/uevent", rp);
    CHECK(rd(p, b, sizeof b) > 0 && strstr(b, "MAJOR=13\n") && strstr(b, "DEVNAME=input/event0"), "uevent: %s", b);
    char lb[PATH_MAX];
    snprintf(p, sizeof p, "%s/subsystem", rp);
    CHECK(!strcmp(link_base(p, lb, sizeof lb), "input"), "subsystem %s", lb);
    CHECK(realpath("/sys/class/input/event0", p) && !strcmp(p, rp), "class link %s", p);
    int fd = open("/dev/input/event0", O_RDONLY);
    CHECK(fd >= 0, "open event0");
    char name[256] = "";
    unsigned long ev = 0;
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    ioctl(fd, EVIOCGBIT(0, sizeof ev), &ev);
    close(fd);
    snprintf(p, sizeof p, "%s/device/name", rp);
    CHECK(rd(p, b, sizeof b) >= 0 && !strcmp(b, name), "name '%s' vs '%s'", b, name);
    snprintf(p, sizeof p, "%s/device/capabilities/ev", rp);
    CHECK(rd(p, b, sizeof b) > 0 && strtoul(b, NULL, 16) == ev, "capabilities/ev %s vs %lx", b, ev);
    snprintf(p, sizeof p, "%s/device/uevent", rp);
    CHECK(rd(p, b, sizeof b) > 0 && strstr(b, "PRODUCT=") && strstr(b, "EV=") && strstr(b, "MODALIAS=input:b"), "input uevent %s", b);
}

static void test_net(void) {
    struct if_nameindex *ni = if_nameindex();
    CHECK(ni != NULL, "if_nameindex");
    int n = 0;
    for (struct if_nameindex *i = ni; i && i->if_index; i++, n++) {
        char p[128], b[64];
        snprintf(p, sizeof p, "/sys/class/net/%s/ifindex", i->if_name);
        CHECK(rd(p, b, sizeof b) > 0 && (unsigned)atoi(b) == i->if_index, "%s ifindex %s vs %u", i->if_name, b, i->if_index);
        snprintf(p, sizeof p, "/sys/class/net/%s/uevent", i->if_name);
        char w[64];
        snprintf(w, sizeof w, "INTERFACE=%s\n", i->if_name);
        CHECK(rd(p, b, sizeof b) > 0 && !strncmp(b, w, strlen(w)), "%s uevent %s", i->if_name, b);
    }
    if (ni) if_freenameindex(ni);
    char b[256], lb[PATH_MAX], rp[PATH_MAX];
    CHECK(n >= 1 && rd("/sys/class/net/lo/address", b, sizeof b) > 0 && !strcmp(b, "00:00:00:00:00:00"), "lo address %s", b);
    CHECK(realpath("/sys/class/net/lo", rp) && !strcmp(rp, "/sys/devices/virtual/net/lo"), "lo path %s", rp);
    if (!access("/sys/class/net/eth0", F_OK)) {
        CHECK(!strcmp(link_base("/sys/class/net/eth0/device/subsystem", lb, sizeof lb), "virtio"), "eth0 bus %s", lb);
        CHECK(realpath("/sys/class/net/eth0/device", rp) != NULL, "eth0 device");
        char p[PATH_MAX + 16];
        snprintf(p, sizeof p, "%s/../vendor", rp);
        CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "0x1af4"), "eth0 pci vendor %s", b);
        snprintf(p, sizeof p, "%s/../subsystem", rp);
        CHECK(!strcmp(link_base(p, lb, sizeof lb), "pci"), "pci subsystem %s", lb);
    }
}

/* driver links, power/, the binary PCI config attribute (M34 limits) */
static void test_driver(void) {
    char b[256], lb[64], p[PATH_MAX], rp[PATH_MAX];
    if (access("/sys/class/net/eth0", F_OK)) return;
    CHECK(!strcmp(link_base("/sys/class/net/eth0/device/driver", lb, sizeof lb), "virtio_net"), "eth0 driver %s", lb);
    CHECK(rd("/sys/class/net/eth0/device/uevent", b, sizeof b) > 0 && strstr(b, "DRIVER=virtio_net\n"), "DRIVER= %s", b);
    CHECK(realpath("/sys/class/net/eth0/device", rp) != NULL, "eth0 device");
    snprintf(p, sizeof p, "/sys/bus/virtio/drivers/virtio_net/%s", strrchr(rp, '/') + 1);
    CHECK(realpath(p, b) && !strcmp(b, rp), "driver's device link %s", p);
    CHECK(!strcmp(link_base("/sys/bus/virtio/drivers/virtio_net/module", lb, sizeof lb), "virtio_net"), "module %s", lb);
    *strrchr(rp, '/') = 0;                                         /* the PCI function */
    snprintf(p, sizeof p, "%s/driver", rp);
    CHECK(!strcmp(link_base(p, lb, sizeof lb), "virtio-pci"), "pci driver %s", lb);
    snprintf(p, sizeof p, "%s/power/control", rp);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "on"), "power/control %s", b);
    CHECK(wr(p, "auto") == 0 && wr(p, "sleep") == -EINVAL, "power/control writes");
    snprintf(p, sizeof p, "%s/power/runtime_status", rp);
    CHECK(rd(p, b, sizeof b) > 0 && !strcmp(b, "unsupported"), "runtime_status %s", b);
    snprintf(p, sizeof p, "%s/config", rp);
    struct stat st;
    CHECK(stat(p, &st) == 0 && st.st_size == 4096 && (st.st_mode & 0777) == 0644, "config size %lld mode %o", (long long)st.st_size, st.st_mode);
    int fd = open(p, O_RDONLY);
    unsigned char c[4096];
    CHECK(fd >= 0 && pread(fd, c, 4, 0) == 4 && c[0] == 0xf4 && c[1] == 0x1a, "config vendor %02x%02x", c[1], c[0]);
    CHECK(pread(fd, c, sizeof c, 0) == 4096 && pread(fd, c, 16, 4090) == 6 && pread(fd, c, 16, 4096) == 0, "config ranges");
    close(fd);
    pid_t pid = fork();
    if (!pid) {
        if (setgid(1000) || setuid(1000)) _exit(99);
        int f = open(p, O_RDONLY);
        _exit(f >= 0 && read(f, c, sizeof c) == 64 && read(f, c, sizeof c) == 0 ? 0 : 98);
    }
    int ws;
    waitpid(pid, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "unprivileged config read is 64 bytes (%x)", ws);
}

static void test_block(void) {
    struct stat st, sp;
    if (stat("/dev/vda", &st)) { printf("sysfstest: no disks, skipped\n"); return; }
    int fd = open("/dev/vda", O_RDONLY);
    uint64_t bytes = 0;
    CHECK(fd >= 0 && ioctl(fd, BLKGETSIZE64, &bytes) == 0, "BLKGETSIZE64");
    close(fd);
    char b[64], rp[PATH_MAX], rp2[PATH_MAX];
    CHECK(rd("/sys/class/block/vda/size", b, sizeof b) > 0 && strtoull(b, NULL, 10) * 512 == bytes, "size %s vs %llu", b, (unsigned long long)bytes);
    CHECK(realpath("/sys/block/vda", rp) && realpath("/sys/class/block/vda", rp2) && !strcmp(rp, rp2), "/sys/block/vda %s", rp);
    if (!stat("/dev/vda1", &sp)) {
        char l[64];
        snprintf(l, sizeof l, "/sys/dev/block/%u:%u", major(sp.st_rdev), minor(sp.st_rdev));
        CHECK(realpath(l, rp2) && ends_with(rp2, "/vda/vda1") && !strncmp(rp2, rp, strlen(rp)), "%s -> %s", l, rp2);
        CHECK(rd("/sys/class/block/vda1/partition", b, sizeof b) > 0 && !strcmp(b, "1"), "partition %s", b);
        CHECK(rd("/sys/class/block/vda1/uevent", b, sizeof b) > 0 && strstr(b, "DEVTYPE=partition"), "part uevent %s", b);
    }
}

static void test_misc(void) {
    char b[4096];
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    char w[32];
    if (n > 1) snprintf(w, sizeof w, "0-%ld", n - 1); else snprintf(w, sizeof w, "0");
    CHECK(rd("/sys/devices/system/cpu/online", b, sizeof b) > 0 && !strcmp(b, w), "cpu online %s vs %s", b, w);
    CHECK(rd("/proc/mounts", b, sizeof b) > 0 && strstr(b, "sysfs /sys sysfs"), "mounts");
    DIR *d = opendir("/sys/class/mem");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d))) if (!strcmp(e->d_name, "null")) found = e->d_type == DT_LNK ? 1 : 2;
    if (d) closedir(d);
    CHECK(found == 1, "class/mem/null as DT_LNK (%d)", found);
    struct stat st;
    CHECK(stat("/sys/class/nope", &st) < 0 && errno == ENOENT, "ENOENT");
    CHECK(open("/sys/class/mem/null/uevent/x", O_RDONLY) < 0 && errno == ENOTDIR, "ENOTDIR");
    CHECK(wr("/sys/class/mem/null/dev", "1") < 0, "read-only attribute write");
}

/* every directory listed once per name, every file readable, every link resolvable */
static int nfiles, ndirs, nlinks;
static void walk(const char *dir, int depth) {
    DIR *d = opendir(dir);
    CHECK(d != NULL, "opendir %s", dir);
    if (!d) return;
    char seen[256][64];
    int ns = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        for (int k = 0; k < ns; k++) CHECK(strcmp(seen[k], e->d_name), "duplicate %s/%s", dir, e->d_name);
        if (ns < 256) snprintf(seen[ns++], 64, "%s", e->d_name);
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        CHECK(lstat(p, &st) == 0, "lstat %s", p);
        if (S_ISDIR(st.st_mode)) { ndirs++; CHECK(e->d_type == DT_DIR, "d_type %s", p); if (depth < 12) walk(p, depth + 1); }
        else if (S_ISLNK(st.st_mode)) {
            nlinks++;
            CHECK(stat(p, &st) == 0, "dangling link %s", p);
        } else {
            nfiles++;
            char b[4096];
            int fd = open(p, O_RDONLY);
            CHECK(fd >= 0, "open %s", p);
            if (fd >= 0) {
                ssize_t r = read(fd, b, sizeof b);
                CHECK(r >= 0 || errno == EINVAL, "read %s", p);
                close(fd);
            }
        }
    }
    closedir(d);
}

/* ---------------------------------------------------------------- uevents */
static int uev_socket(unsigned groups) {
    int s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = groups };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_PASSCRED, &on, sizeof on);
    return s;
}
struct msg { char buf[8192]; int len; struct sockaddr_nl from; struct ucred cred; bool has_cred; };
static int uev_recv(int s, struct msg *m, int ms) {
    struct pollfd pf = { s, POLLIN, 0 };
    if (poll(&pf, 1, ms) != 1) return -1;
    char cb[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec iov = { m->buf, sizeof m->buf - 1 };
    struct msghdr h = { .msg_name = &m->from, .msg_namelen = sizeof m->from, .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    m->len = (int)recvmsg(s, &h, 0);
    if (m->len < 0) return -1;
    m->buf[m->len] = 0;
    m->has_cred = false;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) { memcpy(&m->cred, CMSG_DATA(c), sizeof m->cred); m->has_cred = true; }
    return m->len;
}
static const char *uev_get(const struct msg *m, const char *key) {
    size_t kl = strlen(key);
    for (int o = 0; o < m->len; o += (int)strlen(m->buf + o) + 1)
        if (!strncmp(m->buf + o, key, kl) && m->buf[o + kl] == '=') return m->buf + o + kl + 1;
    return NULL;
}
/* next message whose header starts with prefix (other devices may send events meanwhile) */
static bool uev_wait(int s, struct msg *m, const char *prefix) {
    for (int i = 0; i < 20; i++) {
        if (uev_recv(s, m, 2000) < 0) return false;
        if (!strncmp(m->buf, prefix, strlen(prefix))) return true;
    }
    return false;
}

static void test_uevent(void) {
    int s = uev_socket(1);
    CHECK(s >= 0, "uevent socket");
    if (s < 0) return;
    struct msg *m = malloc(sizeof *m);
    CHECK(wr("/sys/class/mem/null/uevent", "change") == 0, "write change");
    CHECK(uev_wait(s, m, "change@/devices/virtual/mem/null"), "change event: %s", m->buf);
    char sq[32];
    rd("/sys/kernel/uevent_seqnum", sq, sizeof sq);
    const char *v;
    CHECK((v = uev_get(m, "ACTION")) && !strcmp(v, "change"), "ACTION");
    CHECK((v = uev_get(m, "SUBSYSTEM")) && !strcmp(v, "mem"), "SUBSYSTEM");
    CHECK((v = uev_get(m, "DEVPATH")) && !strcmp(v, "/devices/virtual/mem/null"), "DEVPATH");
    CHECK((v = uev_get(m, "MAJOR")) && !strcmp(v, "1") && (v = uev_get(m, "MINOR")) && !strcmp(v, "3"), "MAJOR/MINOR");
    CHECK((v = uev_get(m, "DEVNAME")) && !strcmp(v, "null"), "DEVNAME");
    CHECK((v = uev_get(m, "SEQNUM")) && !strcmp(v, sq), "SEQNUM %s vs %s", v ? v : "-", sq);
    CHECK(m->from.nl_pid == 0 && m->from.nl_groups == 1, "source %u/%u", m->from.nl_pid, m->from.nl_groups);
    CHECK(m->has_cred && m->cred.uid == 0 && m->cred.pid == 0, "SCM_CREDENTIALS");
    CHECK(wr("/sys/class/mem/null/uevent", "bogus") == -EINVAL, "bad action EINVAL");
    CHECK(wr("/sys/class/mem/null/uevent", "add 1234-uuid SYNTH_ARG_X=1\n") == 0 && uev_wait(s, m, "add@/devices/virtual/mem/null"), "add with uuid");

    /* unprivileged: may listen, may not trigger */
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (!pid) {
        if (setgid(1000) || setuid(1000)) _exit(99);
        int c = uev_socket(1);
        if (c < 0) _exit(98);
        if (wr("/sys/class/mem/zero/uevent", "change") != -EACCES) _exit(97);
        struct sockaddr_nl to = { .nl_family = AF_NETLINK, .nl_groups = 2 };
        if (sendto(c, "x", 1, 0, (struct sockaddr *)&to, sizeof to) >= 0 || errno != EPERM) _exit(96);
        write(p[1], "r", 1);
        struct msg *cm = malloc(sizeof *cm);
        _exit(uev_wait(c, cm, "change@/devices/virtual/mem/zero") ? 0 : 95);
    }
    char ch;
    close(p[1]);
    if (read(p[0], &ch, 1) == 1) wr("/sys/class/mem/zero/uevent", "change");
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "unprivileged listener (%x)", st);

    /* TAP hotplug */
    int t = open("/dev/net/tun", O_RDWR);
    struct ifreq q = { 0 };
    strcpy(q.ifr_name, "sysft%d");
    q.ifr_flags = IFF_TAP | IFF_NO_PI;
    CHECK(t >= 0 && ioctl(t, TUNSETIFF, &q) == 0, "TUNSETIFF");
    char pre[128], path[128], b[64];
    snprintf(pre, sizeof pre, "add@/devices/virtual/net/%s", q.ifr_name);
    CHECK(uev_wait(s, m, pre), "tap add: %s", m->buf);
    CHECK((v = uev_get(m, "INTERFACE")) && !strcmp(v, q.ifr_name), "INTERFACE");
    CHECK((v = uev_get(m, "IFINDEX")) && (unsigned)atoi(v) == if_nametoindex(q.ifr_name), "IFINDEX");
    snprintf(path, sizeof path, "/sys/class/net/%s/ifindex", q.ifr_name);
    CHECK(rd(path, b, sizeof b) > 0 && (unsigned)atoi(b) == if_nametoindex(q.ifr_name), "tap ifindex %s", b);
    /* rename (down only): "move" with DEVPATH_OLD, sysfs follows */
    int ctl = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq rq = { 0 };
    char oldname[IFNAMSIZ];
    strcpy(oldname, q.ifr_name);
    strcpy(rq.ifr_name, oldname);
    strcpy(rq.ifr_newname, "sysfren0");
    CHECK(ioctl(ctl, SIOCSIFNAME, &rq) == 0, "SIOCSIFNAME");
    CHECK(uev_wait(s, m, "move@/devices/virtual/net/sysfren0"), "move: %s", m->buf);
    snprintf(pre, sizeof pre, "/devices/virtual/net/%s", oldname);
    CHECK((v = uev_get(m, "DEVPATH_OLD")) && !strcmp(v, pre), "DEVPATH_OLD %s", v ? v : "-");
    CHECK((v = uev_get(m, "INTERFACE")) && !strcmp(v, "sysfren0"), "INTERFACE after move");
    snprintf(path, sizeof path, "/sys/class/net/%s", oldname);
    CHECK(access(path, F_OK) < 0 && access("/sys/class/net/sysfren0/ifindex", F_OK) == 0, "sysfs renamed");
    CHECK(realpath("/sys/class/net/sysfren0", path) && !strcmp(path, "/sys/devices/virtual/net/sysfren0"), "renamed path %s", path);
    strcpy(rq.ifr_name, "sysfren0");
    strcpy(rq.ifr_newname, "lo");
    CHECK(ioctl(ctl, SIOCSIFNAME, &rq) < 0 && errno == EEXIST, "rename to lo EEXIST");
    rq.ifr_flags = IFF_UP;
    CHECK(ioctl(ctl, SIOCSIFFLAGS, &rq) == 0, "up");
    strcpy(rq.ifr_newname, "sysfren1");
    CHECK(ioctl(ctl, SIOCSIFNAME, &rq) < 0 && errno == EBUSY, "rename while up EBUSY");
    int held = open("/sys/class/net/sysfren0/address", O_RDONLY);  /* an open attribute pins the device */
    CHECK(held >= 0, "open address");
    close(t);
    CHECK(uev_wait(s, m, "remove@/devices/virtual/net/sysfren0"), "tap remove: %s", m->buf);
    CHECK(access("/sys/class/net/sysfren0", F_OK) < 0 && errno == ENOENT, "tap gone from sysfs");
    CHECK(read(held, b, sizeof b) >= 0 || errno == ENODEV, "read of a removed device's attribute");
    close(held);
    /* the name is free again: a new TAP gets it (and a new ifindex) */
    for (int round = 0; round < 3; round++) {
        t = open("/dev/net/tun", O_RDWR);
        memset(&q, 0, sizeof q);
        strcpy(q.ifr_name, "sysfren0");
        q.ifr_flags = IFF_TAP | IFF_NO_PI;
        CHECK(t >= 0 && ioctl(t, TUNSETIFF, &q) == 0 && !strcmp(q.ifr_name, "sysfren0"), "re-create");
        CHECK(uev_wait(s, m, "add@/devices/virtual/net/sysfren0"), "re-add: %s", m->buf);
        close(t);
        CHECK(uev_wait(s, m, "remove@/devices/virtual/net/sysfren0"), "re-remove: %s", m->buf);
    }
    close(ctl);

    /* user multicast to group 2 */
    int a = uev_socket(0), g2 = uev_socket(2), g1 = uev_socket(1);
    struct sockaddr_nl to = { .nl_family = AF_NETLINK, .nl_groups = 2 }, me;
    socklen_t ml = sizeof me;
    CHECK(a >= 0 && g2 >= 0 && g1 >= 0, "sockets");
    CHECK(sendto(a, "hello", 6, 0, (struct sockaddr *)&to, sizeof to) == 6, "sendto group 2");
    getsockname(a, (struct sockaddr *)&me, &ml);
    CHECK(uev_recv(g2, m, 2000) == 6 && !strcmp(m->buf, "hello") && m->from.nl_pid == me.nl_pid && me.nl_pid &&
          m->from.nl_groups == 2 && m->has_cred && m->cred.pid == getpid() && m->cred.uid == 0,
          "group 2 delivery (pid %u/%u groups %u)", m->from.nl_pid, me.nl_pid, m->from.nl_groups);
    while (uev_recv(g1, m, 100) >= 0) CHECK(strcmp(m->buf, "hello"), "group 1 got the group 2 message");
    close(a); close(g2); close(g1);
    close(s);
    free(m);
}

/* ---------------------------------------------------------------- udevshim */
static uint32_t murmur2(const char *s) {
    const uint32_t mm = 0x5bd1e995;
    size_t len = strlen(s);
    uint32_t h = (uint32_t)len;
    const uint8_t *d = (const uint8_t *)s;
    for (; len >= 4; d += 4, len -= 4) {
        uint32_t k = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
        k *= mm; k ^= k >> 24; k *= mm; h *= mm; h ^= k;
    }
    switch (len) {
    case 3: h ^= (uint32_t)d[2] << 16; __attribute__((fallthrough));
    case 2: h ^= (uint32_t)d[1] << 8; __attribute__((fallthrough));
    case 1: h ^= d[0]; h *= mm;
    }
    h ^= h >> 13; h *= mm; h ^= h >> 15;
    return h;
}
static void test_udevshim(void) {
    unlink("/run/udev/control");
    unlink("/tmp/sysftest.run");
    mkdir("/run/udev", 0755);
    mkdir("/run/udev/rules.d", 0755);
    FILE *rf = fopen("/run/udev/rules.d/90-sysftest.rules", "w");
    CHECK(rf != NULL, "rules file");
    if (rf) {
        fputs("# sysfstest\n"
              "ACTION!=\"add|change\", GOTO=\"sysft_end\"\n"
              "SUBSYSTEM==\"input\", KERNEL==\"event0\", ATTRS{name}==\"?*\", SYMLINK+=\"input/sysftest\", \\\n"
              "  ENV{SYSFTEST}=\"yes-%k-$major\", TAG+=\"sysft\", RUN+=\"/bin/touch /tmp/sysftest.run\"\n"
              "SUBSYSTEM==\"input\", ENV{SYSFTEST}==\"no\", ENV{SYSFTEST_BAD}=\"1\"\n"
              "LABEL=\"sysft_end\"\n", rf);
        fclose(rf);
    }
    pid_t d = fork();
    if (!d) { execlp("udevshim", "udevshim", "-d", "-f", (char *)NULL); _exit(127); }
    for (int i = 0; i < 50 && access("/run/udev/control", F_OK); i++) usleep(20000);
    usleep(100000);                                  /* the daemon binds after creating it */
    int g2 = uev_socket(2);
    CHECK(g2 >= 0, "group 2 socket");
    struct msg *m = malloc(sizeof *m);
    CHECK(wr("/sys/class/input/event0/uevent", "change") == 0, "trigger event0");
    bool got = false;
    for (int i = 0; i < 20 && !got; i++) {
        if (uev_recv(g2, m, 3000) < 0) break;
        if (m->len < 40 || memcmp(m->buf, "libudev", 8)) continue;
        uint32_t w[8];
        memcpy(w, m->buf + 8, sizeof w);
        CHECK(be32toh(w[0]) == 0xfeedcafe && w[1] == 40 && w[2] == 40 && w[3] == (uint32_t)m->len - 40, "header");
        struct msg p = { 0 };
        memcpy(p.buf, m->buf + 40, (size_t)m->len - 40);
        p.len = m->len - 40;
        const char *a = uev_get(&p, "ACTION"), *dn = uev_get(&p, "DEVNAME");
        if (!a || strcmp(a, "change") || !dn || strcmp(dn, "input/event0")) continue;
        got = true;
        CHECK(be32toh(w[4]) == murmur2("input"), "subsystem hash");
        CHECK(uev_get(&p, "ID_INPUT") && uev_get(&p, "USEC_INITIALIZED"), "ID_INPUT/USEC_INITIALIZED");
        const char *tg = uev_get(&p, "TAGS");
        CHECK(tg && strstr(tg, ":seat:"), "TAGS %s", tg ? tg : "-");
        CHECK(m->from.nl_pid != 0 && m->from.nl_groups == 2 && m->has_cred && m->cred.uid == 0, "sender");
    }
    CHECK(got, "libudev message for event0");
    char b[4096];
    CHECK(rd("/run/udev/data/c13:64", b, sizeof b) > 0 && strstr(b, "E:ID_INPUT=1") && strstr(b, "G:seat") && !strncmp(b, "I:", 2), "db: %s", b);
    CHECK(strstr(b, "E:SYSFTEST=yes-event0-13\n") && strstr(b, "S:input/sysftest\n") && strstr(b, "G:sysft\n") && !strstr(b, "SYSFTEST_BAD"), "rules in db: %s", b);
    char rp[PATH_MAX];
    CHECK(realpath("/dev/input/sysftest", rp) && !strcmp(rp, "/dev/input/event0"), "SYMLINK %s", rp);
    DIR *bp = opendir("/dev/input/by-path");
    int nbp = 0;
    for (struct dirent *e; bp && (e = readdir(bp));) if (strstr(e->d_name, "-event")) nbp++;
    if (bp) closedir(bp);
    CHECK(nbp >= 1, "/dev/input/by-path links (%d)", nbp);
    for (int i = 0; i < 100 && access("/tmp/sysftest.run", F_OK); i++) usleep(20000);
    CHECK(access("/tmp/sysftest.run", F_OK) == 0, "RUN+=");
    CHECK(system("udevadm settle --timeout=10") == 0, "udevadm settle");
    FILE *pf = popen("udevadm info -q property -n input/event0", "r");
    size_t got_n = pf ? fread(b, 1, sizeof b - 1, pf) : 0;
    b[got_n] = 0;
    if (pf) pclose(pf);
    CHECK(strstr(b, "SYSFTEST=yes-event0-13\n") && strstr(b, "DEVNAME=/dev/input/event0\n") && strstr(b, "DEVLINKS="), "udevadm info: %s", b);
    static char db[65536];
    pf = popen("udevadm info --export-db", "r");
    got_n = 0;
    for (size_t r; pf && got_n < sizeof db - 1 && (r = fread(db + got_n, 1, sizeof db - 1 - got_n, pf)) > 0;) got_n += r;
    db[got_n] = 0;
    if (pf) pclose(pf);
    CHECK(strstr(db, "P: /devices/") && strstr(db, "\nN: input/event0\n") && strstr(db, "\nS: input/sysftest\n"), "export-db: %.300s", db);
    kill(d, SIGTERM);
    waitpid(d, NULL, 0);
    unlink("/run/udev/rules.d/90-sysftest.rules");
    close(g2);
    free(m);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-u")) test_udevshim();
    else {
        test_input();
        test_net();
        test_driver();
        test_block();
        test_misc();
        walk("/sys", 0);
        CHECK(nfiles > 50 && ndirs > 20 && nlinks > 20, "walk: %d files %d dirs %d links", nfiles, ndirs, nlinks);
        test_uevent();
    }
    if (fails) { printf("sysfstest: %d failures\n", fails); return 1; }
    printf("sysfstest: all passed (%d files, %d dirs, %d links)\n", nfiles, ndirs, nlinks);
    return 0;
}
