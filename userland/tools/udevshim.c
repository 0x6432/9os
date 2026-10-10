/*
 * udevshim (M34): a static stand-in for systemd-udevd, enough for libudev users (libinput,
 * wlroots) on 9os. It keeps the udev database in /run/udev/data/<id> ("E:KEY=value" lines,
 * "G:/Q:" tags, "I:" init time) with the properties udev's input_id builtin would set
 * (ID_INPUT, ID_INPUT_KEYBOARD, ID_INPUT_MOUSE, ...), tags seat/uaccess, creates missing
 * /dev nodes, and in daemon mode re-broadcasts every kernel uevent to netlink group 2 in
 * libudev's monitor format (prefix "libudev", magic 0xfeedcafe, subsystem/devtype MurmurHash2
 * and a tag bloom filter for socket filters) once the database entry is written.
 *
 *   udevshim -c             coldplug: database entries for every device in /sys
 *   udevshim -d [-f]        daemon (-f: stay in the foreground)
 *   udevshim -t [ACTION]    trigger: write ACTION (add) to every /sys/.../uevent
 *   udevshim -q PATH        print the properties of a /sys or /dev path (udevadm info -q property)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define NETLINK_KOBJECT_UEVENT 15
struct sockaddr_nl_u { unsigned short family, pad; uint32_t pid, groups; };

/* ---------------------------------------------------------------- property lists */
struct props { char *kv[128]; int n; char *tags[8]; int ntags; };
static void pset(struct props *p, const char *k, const char *fmt, ...) {
    char v[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    size_t kl = strlen(k);
    for (int i = 0; i < p->n; i++)
        if (!strncmp(p->kv[i], k, kl) && p->kv[i][kl] == '=') { free(p->kv[i]); p->kv[i] = NULL; asprintf(&p->kv[i], "%s=%s", k, v); return; }
    if (p->n < 128) asprintf(&p->kv[p->n++], "%s=%s", k, v);
}
static const char *pget(const struct props *p, const char *k) {
    size_t kl = strlen(k);
    for (int i = 0; i < p->n; i++) if (!strncmp(p->kv[i], k, kl) && p->kv[i][kl] == '=') return p->kv[i] + kl + 1;
    return NULL;
}
static void ptag(struct props *p, const char *t) { if (p->ntags < 8) p->tags[p->ntags++] = (char *)t; }
static void pfree(struct props *p) { for (int i = 0; i < p->n; i++) free(p->kv[i]); p->n = p->ntags = 0; }

static int readfile(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return (int)n;
}

/* ---------------------------------------------------------------- input_id (as udev's builtin) */
struct bits { uint64_t w[12]; };
static void readbits(const char *dir, const char *name, struct bits *b) {
    char path[PATH_MAX], buf[512];
    memset(b, 0, sizeof *b);
    snprintf(path, sizeof path, "%s/capabilities/%s", dir, name);
    if (!strcmp(name, "properties")) snprintf(path, sizeof path, "%s/properties", dir);
    if (readfile(path, buf, sizeof buf) < 0) return;
    char *words[12]; int n = 0;                      /* most significant word first */
    for (char *s = strtok(buf, " "); s && n < 12; s = strtok(NULL, " ")) words[n++] = s;
    for (int i = 0; i < n; i++) b->w[n - 1 - i] = strtoull(words[i], NULL, 16);
}
static bool tb(const struct bits *b, unsigned bit) { return bit < 768 && (b->w[bit / 64] >> (bit % 64) & 1); }

static void input_props(const char *inputdir, struct props *p) {
    struct bits ev, key, rel, abs, prop;
    readbits(inputdir, "ev", &ev); readbits(inputdir, "key", &key); readbits(inputdir, "rel", &rel);
    readbits(inputdir, "abs", &abs); readbits(inputdir, "properties", &prop);
    pset(p, "ID_INPUT", "1");
    bool any_key = false;
    for (unsigned k = 1; k < 0x100; k++) if (tb(&key, k)) any_key = true;
    for (unsigned k = 0x160; k < 0x2ff; k++) if (tb(&key, k)) any_key = true;
    bool abs_xy = tb(&ev, 3) && tb(&abs, 0) && tb(&abs, 1), rel_xy = tb(&ev, 2) && tb(&rel, 0) && tb(&rel, 1);
    bool pen = tb(&key, 0x140) || tb(&key, 0x14b), finger = tb(&key, 0x145), touch = tb(&key, 0x14a);
    bool mouse_btn = tb(&key, 0x110), direct = tb(&prop, 1);
    bool joy = tb(&key, 0x120) || tb(&key, 0x130) || tb(&key, 0x2c0);
    if (abs_xy) {
        if (pen) pset(p, "ID_INPUT_TABLET", "1");
        else if (finger && !direct) pset(p, "ID_INPUT_TOUCHPAD", "1");
        else if (touch || direct) pset(p, "ID_INPUT_TOUCHSCREEN", "1");
        else if (mouse_btn) pset(p, "ID_INPUT_MOUSE", "1");
        else if (joy) pset(p, "ID_INPUT_JOYSTICK", "1");
    }
    if (rel_xy && mouse_btn) pset(p, "ID_INPUT_MOUSE", "1");
    if (!abs_xy && joy) pset(p, "ID_INPUT_JOYSTICK", "1");
    if (any_key) pset(p, "ID_INPUT_KEY", "1");
    if ((key.w[0] & 0xfffffffeull) == 0xfffffffeull) pset(p, "ID_INPUT_KEYBOARD", "1");
    if (tb(&ev, 5)) pset(p, "ID_INPUT_SWITCH", "1");
    if (tb(&prop, 6)) pset(p, "ID_INPUT_ACCELEROMETER", "1");
}

/* ---------------------------------------------------------------- devices */
/* the subsystem of a /sys device directory (basename of its "subsystem" link) */
static bool subsystem_of(const char *syspath, char *out, size_t cap) {
    char l[PATH_MAX], t[PATH_MAX];
    snprintf(l, sizeof l, "%s/subsystem", syspath);
    ssize_t n = readlink(l, t, sizeof t - 1);
    if (n <= 0) return false;
    t[n] = 0;
    const char *b = strrchr(t, '/');
    snprintf(out, cap, "%s", b ? b + 1 : t);
    return true;
}
/* uevent file + derived properties; false if not a device */
static bool collect(const char *syspath, struct props *p) {
    char buf[4096], sub[64];
    char u[PATH_MAX];
    snprintf(u, sizeof u, "%s/uevent", syspath);
    if (readfile(u, buf, sizeof buf) < 0) return false;
    if (subsystem_of(syspath, sub, sizeof sub)) pset(p, "SUBSYSTEM", "%s", sub);
    const char *dp = strstr(syspath, "/devices/");
    if (dp) pset(p, "DEVPATH", "%s", dp);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        pset(p, line, "%s", eq + 1);
    }
    return true;
}
static void derive(const char *syspath, struct props *p) {
    const char *sub = pget(p, "SUBSYSTEM");
    if (!sub) return;
    if (!strcmp(sub, "input")) {
        const char *base = strrchr(syspath, '/');
        char dir[PATH_MAX];
        if (base && !strncmp(base + 1, "event", 5)) snprintf(dir, sizeof dir, "%s/device", syspath);
        else snprintf(dir, sizeof dir, "%s", syspath);
        input_props(dir, p);
        ptag(p, "seat"); ptag(p, "uaccess");
    } else if (!strcmp(sub, "drm")) {
        ptag(p, "seat"); ptag(p, "master-of-seat"); ptag(p, "uaccess");
    } else if (!strcmp(sub, "graphics")) {
        ptag(p, "seat"); ptag(p, "uaccess");
    }
    if (p->ntags) {
        char t[256] = ":";
        for (int i = 0; i < p->ntags; i++) { strcat(t, p->tags[i]); strcat(t, ":"); }
        pset(p, "TAGS", "%s", t);
        pset(p, "CURRENT_TAGS", "%s", t);
    }
}
/* the database id of a device: c13:64, b253:1, n2, +pci:0000:00:03.0 */
static void dbid(const struct props *p, const char *syspath, char *out, size_t cap) {
    const char *ma = pget(p, "MAJOR"), *mi = pget(p, "MINOR"), *sub = pget(p, "SUBSYSTEM"), *ifi = pget(p, "IFINDEX");
    const char *name = strrchr(syspath, '/');
    name = name ? name + 1 : syspath;
    if (ma && mi) snprintf(out, cap, "%c%s:%s", sub && !strcmp(sub, "block") ? 'b' : 'c', ma, mi);
    else if (ifi && sub && !strcmp(sub, "net")) snprintf(out, cap, "n%s", ifi);
    else snprintf(out, cap, "+%s:%s", sub ? sub : "", name);
}
static uint64_t usec_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }

static void mkdirs(void) {
    mkdir("/run", 0755); mkdir("/run/udev", 0755); mkdir("/run/udev/data", 0755); mkdir("/run/udev/tags", 0755);
    int fd = open("/run/udev/control", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}
static void db_write(const char *id, struct props *p) {
    char path[PATH_MAX], tmp[PATH_MAX];
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return;
    const char *ui = pget(p, "USEC_INITIALIZED");
    fprintf(f, "I:%s\n", ui ? ui : "1");
    static const char *const skip[] = { "ACTION", "DEVPATH", "SUBSYSTEM", "SEQNUM", "MAJOR", "MINOR", "DEVNAME", "DEVTYPE",
                                        "IFINDEX", "INTERFACE", "TAGS", "CURRENT_TAGS", "USEC_INITIALIZED", "PRODUCT", "NAME",
                                        "PHYS", "PROP", "EV", "KEY", "REL", "ABS", "MSC", "LED", "MODALIAS", "PARTN", "DRIVER" };
    for (int i = 0; i < p->n; i++) {
        bool k = true;
        for (size_t s = 0; s < sizeof skip / sizeof *skip; s++) {
            size_t l = strlen(skip[s]);
            if (!strncmp(p->kv[i], skip[s], l) && p->kv[i][l] == '=') k = false;
        }
        if (k) fprintf(f, "E:%s\n", p->kv[i]);
    }
    for (int i = 0; i < p->ntags; i++) fprintf(f, "G:%s\nQ:%s\n", p->tags[i], p->tags[i]);
    fprintf(f, "V:1\n");
    fclose(f);
    rename(tmp, path);
    for (int i = 0; i < p->ntags; i++) {
        snprintf(path, sizeof path, "/run/udev/tags/%s", p->tags[i]);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "/run/udev/tags/%s/%s", p->tags[i], id);
        int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    }
}
static void db_remove(const char *id) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    unlink(path);
    DIR *d = opendir("/run/udev/tags");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/run/udev/tags/%s/%s", e->d_name, id);
        unlink(path);
    }
    if (d) closedir(d);
}
static void node_create(const struct props *p) {
    const char *dn = pget(p, "DEVNAME"), *ma = pget(p, "MAJOR"), *mi = pget(p, "MINOR"), *sub = pget(p, "SUBSYSTEM");
    if (!dn || !ma || !mi) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/dev/%s", dn);
    struct stat st;
    if (!lstat(path, &st)) return;
    for (char *s = path + 5; (s = strchr(s, '/')); s++) { *s = 0; mkdir(path, 0755); *s = '/'; }
    bool blk = sub && !strcmp(sub, "block");
    mknod(path, (blk ? S_IFBLK : S_IFCHR) | 0660, makedev(atoi(ma), atoi(mi)));
}

/* full property set of a device directory (coldplug, -q) */
static bool device_props(const char *syspath, struct props *p) {
    if (!collect(syspath, p)) return false;
    derive(syspath, p);
    char id[128], path[PATH_MAX], buf[64];
    dbid(p, syspath, id, sizeof id);
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    FILE *f = fopen(path, "re");                    /* keep the original initialization time */
    char line[600];
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "I:", 2)) { snprintf(buf, sizeof buf, "%s", line + 2); buf[strcspn(buf, "\n")] = 0; pset(p, "USEC_INITIALIZED", "%s", buf); }
    if (f) fclose(f);
    if (!pget(p, "USEC_INITIALIZED")) pset(p, "USEC_INITIALIZED", "%llu", (unsigned long long)usec_now());
    return true;
}

/* walk /sys/class/X/Y and /sys/bus/X/devices/Y */
static void for_each_device(void (*cb)(const char *syspath, void *arg), void *arg) {
    static const char *const roots[] = { "/sys/class", "/sys/bus" };
    for (int r = 0; r < 2; r++) {
        DIR *d = opendir(roots[r]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, r ? "%s/%s/devices" : "%s/%s", roots[r], e->d_name);
            DIR *d2 = opendir(dir);
            struct dirent *e2;
            while (d2 && (e2 = readdir(d2))) {
                if (e2->d_name[0] == '.') continue;
                char l[PATH_MAX], t[PATH_MAX];
                snprintf(l, sizeof l, "%s/%s", dir, e2->d_name);
                if (!realpath(l, t)) continue;
                cb(t, arg);
            }
            if (d2) closedir(d2);
        }
        if (d) closedir(d);
    }
}
static void cold_one(const char *syspath, void *arg) {
    struct props p = { 0 };
    if (device_props(syspath, &p)) {
        char id[128];
        dbid(&p, syspath, id, sizeof id);
        node_create(&p);
        db_write(id, &p);
        (*(int *)arg)++;
    }
    pfree(&p);
}
static void trig_one(const char *syspath, void *arg) {
    char u[PATH_MAX];
    snprintf(u, sizeof u, "%s/uevent", syspath);
    int fd = open(u, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, arg, strlen(arg)) > 0) {}
    close(fd);
}

/* ---------------------------------------------------------------- libudev monitor format */
static uint32_t murmur2(const char *s) {
    const uint32_t m = 0x5bd1e995;
    size_t len = strlen(s);
    uint32_t h = (uint32_t)len;
    const uint8_t *d = (const uint8_t *)s;
    while (len >= 4) {
        uint32_t k = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
        k *= m; k ^= k >> 24; k *= m;
        h *= m; h ^= k;
        d += 4; len -= 4;
    }
    switch (len) {
    case 3: h ^= (uint32_t)d[2] << 16; __attribute__((fallthrough));
    case 2: h ^= (uint32_t)d[1] << 8; __attribute__((fallthrough));
    case 1: h ^= d[0]; h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}
struct mon_hdr { char prefix[8]; uint32_t magic, header_size, properties_off, properties_len, sub_hash, devtype_hash, bloom_hi, bloom_lo; };
static void broadcast(int sk, struct props *p) {
    static char msg[8192];
    struct mon_hdr h = { "libudev", htobe32(0xfeedcafe), sizeof h, sizeof h, 0, 0, 0, 0, 0 };
    size_t n = sizeof h;
    for (int i = 0; i < p->n; i++) {
        size_t l = strlen(p->kv[i]) + 1;
        if (n + l > sizeof msg) break;
        memcpy(msg + n, p->kv[i], l);
        n += l;
    }
    h.properties_len = (uint32_t)(n - sizeof h);
    const char *sub = pget(p, "SUBSYSTEM"), *dt = pget(p, "DEVTYPE");
    if (sub) h.sub_hash = htobe32(murmur2(sub));
    if (dt) h.devtype_hash = htobe32(murmur2(dt));
    uint64_t bloom = 0;
    for (int i = 0; i < p->ntags; i++) {
        uint32_t x = murmur2(p->tags[i]);
        bloom |= 1ull << (x & 63) | 1ull << ((x >> 6) & 63) | 1ull << ((x >> 12) & 63) | 1ull << ((x >> 18) & 63);
    }
    h.bloom_hi = htobe32((uint32_t)(bloom >> 32)); h.bloom_lo = htobe32((uint32_t)bloom);
    memcpy(msg, &h, sizeof h);
    struct sockaddr_nl_u to = { AF_NETLINK, 0, 0, 2 };
    sendto(sk, msg, n, 0, (struct sockaddr *)&to, sizeof to);
}

static int daemon_loop(void) {
    int sk = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl_u a = { AF_NETLINK, 0, 0, 1 };
    if (sk < 0 || bind(sk, (struct sockaddr *)&a, sizeof a) < 0) { perror("udevshim: uevent socket"); return 1; }
    int on = 1;
    setsockopt(sk, SOL_SOCKET, SO_PASSCRED, &on, sizeof on);
    static char buf[8192];
    for (;;) {
        struct sockaddr_nl_u from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(sk, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) { if (errno == EINTR) continue; perror("udevshim: recv"); return 1; }
        if (from.pid != 0 || !(from.groups & 1)) continue;         /* kernel messages only */
        buf[n] = 0;
        struct props p = { 0 };
        for (char *s = buf + strlen(buf) + 1; s < buf + n; s += strlen(s) + 1) {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0; pset(&p, s, "%s", eq + 1); *eq = '=';
        }
        const char *act = pget(&p, "ACTION"), *dp = pget(&p, "DEVPATH");
        if (!act || !dp) { pfree(&p); continue; }
        char syspath[PATH_MAX], id[128];
        snprintf(syspath, sizeof syspath, "/sys%s", dp);
        if (!strcmp(act, "remove")) {
            dbid(&p, syspath, id, sizeof id);
            db_remove(id);
        } else {
            derive(syspath, &p);
            dbid(&p, syspath, id, sizeof id);
            char path[PATH_MAX], line[600];
            snprintf(path, sizeof path, "/run/udev/data/%s", id);
            FILE *f = fopen(path, "re");
            while (f && fgets(line, sizeof line, f))
                if (!strncmp(line, "I:", 2)) { line[strcspn(line, "\n")] = 0; pset(&p, "USEC_INITIALIZED", "%s", line + 2); }
            if (f) fclose(f);
            if (!pget(&p, "USEC_INITIALIZED")) pset(&p, "USEC_INITIALIZED", "%llu", (unsigned long long)usec_now());
            if (!strcmp(act, "add")) node_create(&p);
            db_write(id, &p);
        }
        broadcast(sk, &p);
        pfree(&p);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) goto usage;
    mkdirs();
    if (!strcmp(argv[1], "-c")) {
        int n = 0;
        for_each_device(cold_one, &n);
        return n ? 0 : 1;
    }
    if (!strcmp(argv[1], "-t")) { for_each_device(trig_one, argc > 2 ? argv[2] : "add"); return 0; }
    if (!strcmp(argv[1], "-q") && argc > 2) {
        char sp[PATH_MAX];
        struct stat st;
        if (!strncmp(argv[2], "/sys/", 5)) { if (!realpath(argv[2], sp)) { perror(argv[2]); return 1; } }
        else if (!stat(argv[2], &st) && (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) {
            char l[64];
            snprintf(l, sizeof l, "/sys/dev/%s/%u:%u", S_ISBLK(st.st_mode) ? "block" : "char", major(st.st_rdev), minor(st.st_rdev));
            if (!realpath(l, sp)) { perror(l); return 1; }
        } else { fprintf(stderr, "udevshim: %s: not a device\n", argv[2]); return 1; }
        struct props p = { 0 };
        if (!device_props(sp, &p)) return 1;
        for (int i = 0; i < p.n; i++) printf("%s\n", p.kv[i]);
        return 0;
    }
    if (!strcmp(argv[1], "-d")) {
        bool fg = argc > 2 && !strcmp(argv[2], "-f");
        if (!fg) {
            pid_t pid = fork();
            if (pid < 0) return 1;
            if (pid) return 0;
            setsid();
        }
        return daemon_loop();
    }
usage:
    fprintf(stderr, "usage: udevshim -c | -d [-f] | -t [ACTION] | -q PATH\n");
    return 2;
}
