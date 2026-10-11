/*
 * udevshim (M34): a small stand-in for systemd-udevd, enough for libudev users (libinput,
 * wlroots, seatd) on 9os. It keeps the udev database in /run/udev/data/<id> ("I:" init time,
 * "E:KEY=value", "S:" links, "G:/Q:" tags) with the properties udev's builtins would set:
 *   input_id  ID_INPUT, ID_INPUT_KEY/KEYBOARD/MOUSE/TOUCHPAD/TOUCHSCREEN/TABLET/JOYSTICK/SWITCH
 *   path_id   ID_PATH, ID_PATH_TAG (pci-0000:00:03.0)
 *   blkid     ID_FS_TYPE/UUID/UUID_ENC/LABEL (ext2/3/4 superblocks)
 *   net_id    ID_NET_NAME_PATH (enp0s3), ID_NET_NAME_MAC (enx525400123456)
 *   hwdb      records from /etc/udev/hwdb.d and /lib/udev/hwdb.d (*.hwdb) matched against the
 *             modalias (and evdev:name:<name>:phys:<phys>:ev:<ev> for input)
 * then applies rules from /etc/udev/rules.d, /run/udev/rules.d, /lib/udev/rules.d and
 * /usr/lib/udev/rules.d (*.rules,
 * by file name; /etc overrides): matches ACTION KERNEL SUBSYSTEM DEVPATH DRIVER DEVTYPE NAME
 * TAG ENV{} ATTR{} KERNELS SUBSYSTEMS DRIVERS ATTRS{} (== !=, globs with |; the parent keys of a
 * rule must all match the same ancestor), assignments ENV{} SYMLINK TAG MODE OWNER GROUP RUN NAME
 * (network interfaces are renamed with SIOCSIFNAME) OPTIONS (last_rule, link_priority=,
 * string_escape=, db_persist) (= += :=), GOTO/LABEL, IMPORT{program,file,builtin,parent,cmdline,db},
 * PROGRAM/RESULT,
 * substitutions %k %n %M %m %p %N %S %c %E{} %s{} $env{} $attr{} $kernel $devpath %%.
 * Device nodes get MODE/OWNER/GROUP, persistent links (/dev/input/by-path, /dev/disk/by-path,
 * by-uuid, by-label, by-id for virtio disks with a serial) and rule SYMLINKs; several devices
 * claiming one link are arbitrated by link_priority (/run/udev/links/<link>/<id>); RUN programs
 * get the properties as environment. "udevadm hwdb --update" compiles the hwdb sources into
 * /etc/udev/hwdb.bin (9os text format, sorted and de-duplicated); once it exists only the
 * compiled database is used, as with systemd.
 *
 *   udevshim -c             coldplug: process every device in /sys
 *   udevshim -d [-f]        daemon: process kernel uevents, then re-broadcast them to netlink
 *                           group 2 in libudev's monitor format (prefix "libudev", magic
 *                           0xfeedcafe, MurmurHash2 subsystem/devtype hashes, tag bloom)
 *                           and to group 4 in the kernel's format (libudev-zero's monitor)
 *   udevshim -t [ACTION]    trigger: write ACTION (add) to every /sys/.../uevent
 *   udevshim -s [SECONDS]   settle: wait until the daemon handled every kernel event
 *   udevshim -q PATH        properties of a /sys or /dev path
 * Invoked as "udevadm": settle, trigger [--action=X], info [-q property] -n NODE | -p PATH,
 * info --export-db, monitor [--property], control (no-op), hwdb --update [-o FILE] | --test=MODALIAS.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NETLINK_KOBJECT_UEVENT 15
struct sockaddr_nl_u { unsigned short family, pad; uint32_t pid, groups; };
#define MAXP 160
#define MAXL 16

/* ---------------------------------------------------------------- device state */
struct dev {
    char syspath[PATH_MAX];
    char *kv[MAXP]; int n;
    char *tags[16]; int ntags;
    char *links[MAXL]; int nlinks;
    char *run[8]; int nrun;
    char result[1024];                        /* PROGRAM output, %c */
    int mode; uid_t uid; gid_t gid;           /* -1: unchanged */
    bool final_env[MAXP];
    int link_prio;
    bool last_rule, esc_replace, db_persist;
    char name[64];                            /* NAME= (network interfaces) */
};
static void prog_cmd(const char *in, char *out, size_t cap);
static void pset(struct dev *d, const char *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pset(struct dev *d, const char *k, const char *fmt, ...) {
    char v[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    size_t kl = strlen(k);
    for (int i = 0; i < d->n; i++)
        if (!strncmp(d->kv[i], k, kl) && d->kv[i][kl] == '=') {
            free(d->kv[i]);
            if (!*v) { d->kv[i] = d->kv[--d->n]; return; }      /* ENV{X}="": unset */
            if (asprintf(&d->kv[i], "%s=%s", k, v) < 0) d->kv[i] = d->kv[--d->n];
            return;
        }
    if (*v && d->n < MAXP && asprintf(&d->kv[d->n], "%s=%s", k, v) >= 0) d->n++;
}
static const char *pget(const struct dev *d, const char *k) {
    size_t kl = strlen(k);
    for (int i = 0; i < d->n; i++) if (!strncmp(d->kv[i], k, kl) && d->kv[i][kl] == '=') return d->kv[i] + kl + 1;
    return NULL;
}
static bool has_tag(const struct dev *d, const char *t) { for (int i = 0; i < d->ntags; i++) if (!strcmp(d->tags[i], t)) return true; return false; }
static void add_tag(struct dev *d, const char *t) { if (!has_tag(d, t) && d->ntags < 16) d->tags[d->ntags++] = strdup(t); }
static void add_link(struct dev *d, const char *l) {
    for (int i = 0; i < d->nlinks; i++) if (!strcmp(d->links[i], l)) return;
    if (d->nlinks < MAXL) d->links[d->nlinks++] = strdup(l);
}
static void dev_init(struct dev *d) { memset(d, 0, sizeof *d); d->mode = -1; d->uid = (uid_t)-1; d->gid = (gid_t)-1; }
static void dev_free(struct dev *d) {
    for (int i = 0; i < d->n; i++) free(d->kv[i]);
    for (int i = 0; i < d->ntags; i++) free(d->tags[i]);
    for (int i = 0; i < d->nlinks; i++) free(d->links[i]);
    for (int i = 0; i < d->nrun; i++) free(d->run[i]);
    dev_init(d);
}

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
static const char *sysname(const struct dev *d) { const char *b = strrchr(d->syspath, '/'); return b ? b + 1 : d->syspath; }
static bool is(const char *a, const char *b) { return a && b && !strcmp(a, b); }

/* ---------------------------------------------------------------- builtins */
struct bits { uint64_t w[12]; };
static void readbits(const char *dir, const char *name, struct bits *b) {
    char path[PATH_MAX], buf[512];
    memset(b, 0, sizeof *b);
    if (!strcmp(name, "properties")) snprintf(path, sizeof path, "%s/properties", dir);
    else snprintf(path, sizeof path, "%s/capabilities/%s", dir, name);
    if (readfile(path, buf, sizeof buf) < 0) return;
    char *words[12]; int n = 0;                      /* most significant word first */
    for (char *s = strtok(buf, " "); s && n < 12; s = strtok(NULL, " ")) words[n++] = s;
    for (int i = 0; i < n; i++) b->w[n - 1 - i] = strtoull(words[i], NULL, 16);
}
static bool tb(const struct bits *b, unsigned bit) { return bit < 768 && (b->w[bit / 64] >> (bit % 64) & 1); }

static void builtin_input_id(const char *inputdir, struct dev *d) {
    struct bits ev, key, rel, abs, prop;
    readbits(inputdir, "ev", &ev); readbits(inputdir, "key", &key); readbits(inputdir, "rel", &rel);
    readbits(inputdir, "abs", &abs); readbits(inputdir, "properties", &prop);
    pset(d, "ID_INPUT", "1");
    bool any_key = false;
    for (unsigned k = 1; k < 0x100; k++) if (tb(&key, k)) any_key = true;
    for (unsigned k = 0x160; k < 0x2ff; k++) if (tb(&key, k)) any_key = true;
    bool abs_xy = tb(&ev, 3) && tb(&abs, 0) && tb(&abs, 1), rel_xy = tb(&ev, 2) && tb(&rel, 0) && tb(&rel, 1);
    bool pen = tb(&key, 0x140) || tb(&key, 0x14b), finger = tb(&key, 0x145), touch = tb(&key, 0x14a);
    bool mouse_btn = tb(&key, 0x110), direct = tb(&prop, 1);
    bool joy = tb(&key, 0x120) || tb(&key, 0x130) || tb(&key, 0x2c0);
    if (abs_xy) {
        if (pen) pset(d, "ID_INPUT_TABLET", "1");
        else if (finger && !direct) pset(d, "ID_INPUT_TOUCHPAD", "1");
        else if (mouse_btn) pset(d, "ID_INPUT_MOUSE", "1");     /* udev: absolute mice (VMware, QEMU tablet) */
        else if (touch || direct) pset(d, "ID_INPUT_TOUCHSCREEN", "1");
        else if (joy) pset(d, "ID_INPUT_JOYSTICK", "1");
    }
    if (rel_xy && mouse_btn) pset(d, "ID_INPUT_MOUSE", "1");
    if (!abs_xy && joy) pset(d, "ID_INPUT_JOYSTICK", "1");
    if (any_key) pset(d, "ID_INPUT_KEY", "1");
    if ((key.w[0] & 0xfffffffeull) == 0xfffffffeull) pset(d, "ID_INPUT_KEYBOARD", "1");
    if (tb(&ev, 5)) pset(d, "ID_INPUT_SWITCH", "1");
    if (tb(&prop, 6)) pset(d, "ID_INPUT_ACCELEROMETER", "1");
}

/* the PCI function the device sits below: pci-0000:00:03.0 */
static bool builtin_path_id(struct dev *d) {
    const char *p = strstr(d->syspath, "/devices/pci");
    if (!p) return false;
    const char *last = NULL;
    for (const char *c = strchr(p + 9, '/'); c; c = strchr(c + 1, '/')) {
        unsigned a, b, s, f;
        char t;
        if (sscanf(c + 1, "%x:%x:%x.%x%c", &a, &b, &s, &f, &t) >= 4) last = c + 1;
    }
    if (!last) return false;
    char slot[16];
    snprintf(slot, sizeof slot, "%.12s", last);
    slot[strcspn(slot, "/")] = 0;
    pset(d, "ID_PATH", "pci-%s", slot);
    char tag[32];
    snprintf(tag, sizeof tag, "pci-%s", slot);
    for (char *c = tag; *c; c++) if (*c == ':' || *c == '.') *c = '_';
    pset(d, "ID_PATH_TAG", "%s", tag);
    return true;
}

static void builtin_blkid(struct dev *d) {
    const char *dn = pget(d, "DEVNAME");
    if (!dn) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/dev/%s", dn);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    uint8_t sb[1024];
    ssize_t n = pread(fd, sb, sizeof sb, 1024);
    close(fd);
    if (n != (ssize_t)sizeof sb || sb[56] != 0x53 || sb[57] != 0xef) return;
    uint32_t compat = sb[92] | sb[93] << 8, incompat = sb[96] | sb[97] << 8;
    pset(d, "ID_FS_TYPE", "%s", incompat & 0x40 ? "ext4" : compat & 4 ? "ext3" : "ext2");
    pset(d, "ID_FS_USAGE", "filesystem");
    const uint8_t *u = sb + 104;
    char us[40];
    snprintf(us, sizeof us, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    bool zero = true;
    for (int i = 0; i < 16; i++) if (u[i]) zero = false;
    if (!zero) { pset(d, "ID_FS_UUID", "%s", us); pset(d, "ID_FS_UUID_ENC", "%s", us); }
    char label[17];
    memcpy(label, sb + 120, 16);
    label[16] = 0;
    if (label[0]) { pset(d, "ID_FS_LABEL", "%s", label); pset(d, "ID_FS_LABEL_ENC", "%s", label); }
}

static void builtin_net_id(struct dev *d) {
    char buf[64], p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/address", d->syspath);
    unsigned m[6];
    if (readfile(p, buf, sizeof buf) > 0 && sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6 &&
        (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]))
        pset(d, "ID_NET_NAME_MAC", "enx%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    const char *path = pget(d, "ID_PATH");
    unsigned dom, bus, slot, fn;
    if (path && sscanf(path, "pci-%x:%x:%x.%x", &dom, &bus, &slot, &fn) == 4) {
        if (fn) pset(d, "ID_NET_NAME_PATH", "enp%us%uf%u", bus, slot, fn);
        else pset(d, "ID_NET_NAME_PATH", "enp%us%u", bus, slot);
    }
}

/* hwdb: "match\n KEY=VALUE\n..." records separated by blank lines. Sources: *.hwdb in
 * /etc/udev/hwdb.d (overrides same-named files), /lib/udev/hwdb.d, /usr/lib/udev/hwdb.d, in file
 * name order; later records override earlier ones for the same key. */
struct hwrec { char **m; int nm; char **kv; int nkv; };
static struct hwrec *hw; static int nhw; static bool hw_loaded;
#define HWDB_BIN "/etc/udev/hwdb.bin"
#define HWDB_MAGIC "9os-hwdb 1"
static void push(char ***v, int *n, const char *x) { *v = realloc(*v, sizeof **v * (size_t)(*n + 1)); (*v)[(*n)++] = strdup(x); }
static struct hwrec *hw_new(void) { hw = realloc(hw, sizeof *hw * (size_t)(nhw + 1)); hw[nhw] = (struct hwrec){ 0 }; return &hw[nhw++]; }
static int cmpstr(const void *a, const void *b);
static int hw_sources(char **files, int max) {
    static const char *const dirs[] = { "/etc/udev/hwdb.d", "/lib/udev/hwdb.d", "/usr/lib/udev/hwdb.d" };
    int nf = 0;
    for (int k = 0; k < 3; k++) {
        DIR *dr = opendir(dirs[k]);
        struct dirent *e;
        while (dr && (e = readdir(dr)) && nf < max) {
            size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".hwdb")) continue;
            bool dup = false;
            for (int i = 0; i < nf; i++) if (!strcmp(strrchr(files[i], '/') + 1, e->d_name)) dup = true;
            if (!dup && asprintf(&files[nf], "%s/%s", dirs[k], e->d_name) >= 0) nf++;
        }
        if (dr) closedir(dr);
    }
    qsort(files, (size_t)nf, sizeof *files, cmpstr);
    return nf;
}
/* hwdb source text or the compiled file ("M match" / "P KEY=VALUE" lines, blank between records) */
static void hw_parse(FILE *f, bool compiled) {
    char line[1024];
    struct hwrec *r = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (compiled) {
            if (!line[0]) { r = NULL; continue; }
            if (line[0] == 'M' && line[1] == ' ') { if (!r || r->nkv) r = hw_new(); push(&r->m, &r->nm, line + 2); }
            else if (line[0] == 'P' && line[1] == ' ' && r) push(&r->kv, &r->nkv, line + 2);
            continue;
        }
        if (line[0] == '#') continue;
        if (!line[0]) { r = NULL; continue; }
        if (line[0] != ' ') {
            if (!r || r->nkv) r = hw_new();
            push(&r->m, &r->nm, line);
            continue;
        }
        char *kv = line + strspn(line, " ");
        if (r && r->nm && strchr(kv, '=')) push(&r->kv, &r->nkv, kv);
    }
}
static void hw_load(void) {
    if (hw_loaded) return;
    hw_loaded = true;
    FILE *f = fopen(HWDB_BIN, "re");
    char magic[32] = "";
    if (f && fgets(magic, sizeof magic, f) && !strncmp(magic, HWDB_MAGIC, strlen(HWDB_MAGIC))) { hw_parse(f, true); fclose(f); return; }
    if (f) fclose(f);
    char *files[256];
    int nf = hw_sources(files, 256);
    for (int i = 0; i < nf; i++) {
        FILE *g = fopen(files[i], "re");
        if (g) { hw_parse(g, false); fclose(g); }
        free(files[i]);
    }
}
static void hwdb_lookup(struct dev *d, const char *key) {
    hw_load();
    for (int i = 0; i < nhw; i++) {
        bool hit = false;
        for (int k = 0; k < hw[i].nm && !hit; k++) if (!fnmatch(hw[i].m[k], key, 0)) hit = true;
        if (!hit) continue;
        for (int k = 0; k < hw[i].nkv; k++) {
            char kv[1024];
            snprintf(kv, sizeof kv, "%s", hw[i].kv[k]);
            char *eq = strchr(kv, '=');
            *eq = 0;
            pset(d, kv, "%s", eq + 1);
        }
    }
}
/* udevadm hwdb --update [-o FILE]: compile the sources (records without properties dropped) */
static int hwdb_update(const char *out) {
    hw_loaded = true;
    char *files[256];
    int nf = hw_sources(files, 256);
    for (int i = 0; i < nf; i++) {
        FILE *g = fopen(files[i], "re");
        if (g) { hw_parse(g, false); fclose(g); }
        free(files[i]);
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    mkdir("/etc/udev", 0755);
    FILE *f = fopen(tmp, "we");
    if (!f) { perror(tmp); return 1; }
    fprintf(f, "%s\n", HWDB_MAGIC);
    int n = 0;
    for (int i = 0; i < nhw; i++) {
        if (!hw[i].nkv) continue;
        for (int k = 0; k < hw[i].nm; k++) fprintf(f, "M %s\n", hw[i].m[k]);
        for (int k = 0; k < hw[i].nkv; k++) fprintf(f, "P %s\n", hw[i].kv[k]);
        fprintf(f, "\n");
        n++;
    }
    if (fclose(f) || rename(tmp, out)) { perror(out); return 1; }
    return 0;
}
static int hwdb_test(const char *key) {
    struct dev *d = calloc(1, sizeof *d);
    dev_init(d);
    hwdb_lookup(d, key);
    for (int i = 0; i < d->n; i++) printf("%s\n", d->kv[i]);
    return 0;
}

static void builtins(struct dev *d) {
    const char *sub = pget(d, "SUBSYSTEM");
    if (!sub) return;
    builtin_path_id(d);
    const char *ma = pget(d, "MODALIAS");
    if (ma) hwdb_lookup(d, ma);
    if (is(sub, "input")) {
        char dir[PATH_MAX + 8], b[256], name[128] = "", phys[64] = "", ev[32] = "", p[PATH_MAX + 32];
        bool event = !strncmp(sysname(d), "event", 5);
        snprintf(dir, sizeof dir, event ? "%s/device" : "%s", d->syspath);
        builtin_input_id(dir, d);
        snprintf(p, sizeof p, "%s/modalias", dir);
        if (readfile(p, b, sizeof b) > 0) {
            char key[300];
            snprintf(key, sizeof key, "evdev:%s", b);
            hwdb_lookup(d, key);
        }
        snprintf(p, sizeof p, "%s/name", dir); readfile(p, name, sizeof name);
        snprintf(p, sizeof p, "%s/phys", dir); readfile(p, phys, sizeof phys);
        snprintf(p, sizeof p, "%s/capabilities/ev", dir); readfile(p, ev, sizeof ev);
        char key[300];
        snprintf(key, sizeof key, "evdev:name:%s:phys:%s:ev:%s", name, phys, ev);
        hwdb_lookup(d, key);
        add_tag(d, "seat"); add_tag(d, "uaccess");
        const char *path = pget(d, "ID_PATH");
        if (event && path) {
            const char *suf = pget(d, "ID_INPUT_KEYBOARD") ? "-event-kbd" : pget(d, "ID_INPUT_MOUSE") ? "-event-mouse"
                            : pget(d, "ID_INPUT_JOYSTICK") ? "-event-joystick" : "-event";
            snprintf(p, sizeof p, "/dev/input/by-path/%s%s", path, suf);
            add_link(d, p);
        }
    } else if (is(sub, "block")) {
        builtin_blkid(d);
        const char *path = pget(d, "ID_PATH"), *pn = pget(d, "PARTN"), *v;
        char p[PATH_MAX];
        if (path) {
            if (pn) snprintf(p, sizeof p, "/dev/disk/by-path/%s-part%s", path, pn);
            else snprintf(p, sizeof p, "/dev/disk/by-path/%s", path);
            add_link(d, p);
        }
        /* by-id: virtio disks with a serial (60-persistent-storage.rules: virtio-$attr{serial}) */
        char sp[PATH_MAX + 16], ser[64];
        snprintf(sp, sizeof sp, pn ? "%s/../serial" : "%s/serial", d->syspath);
        if (strstr(d->syspath, "/virtio") && readfile(sp, ser, sizeof ser) > 0 && ser[0]) {
            for (char *c = ser; *c; c++) if (*c == ' ' || *c == '/' || (unsigned char)*c < 0x20) *c = '_';
            pset(d, "ID_SERIAL", "%s", ser);
            if (pn) snprintf(p, sizeof p, "/dev/disk/by-id/virtio-%s-part%s", ser, pn);
            else snprintf(p, sizeof p, "/dev/disk/by-id/virtio-%s", ser);
            add_link(d, p);
        }
        if ((v = pget(d, "ID_FS_UUID_ENC"))) { snprintf(p, sizeof p, "/dev/disk/by-uuid/%s", v); add_link(d, p); }
        if ((v = pget(d, "ID_FS_LABEL_ENC"))) { snprintf(p, sizeof p, "/dev/disk/by-label/%s", v); add_link(d, p); }
    } else if (is(sub, "net")) {
        builtin_net_id(d);
    } else if (is(sub, "drm")) {
        add_tag(d, "seat"); add_tag(d, "master-of-seat"); add_tag(d, "uaccess");
    } else if (is(sub, "graphics")) {
        add_tag(d, "seat"); add_tag(d, "uaccess");
    }
}

/* ---------------------------------------------------------------- rules */
static void subst(const struct dev *d, const char *in, char *out, size_t cap) {
    size_t o = 0;
    char tmp[512];
    for (const char *c = in; *c && o + 1 < cap; c++) {
        const char *rep = NULL;
        tmp[0] = 0;
        if (*c == '%' && c[1]) {
            c++;
            switch (*c) {
            case 'k': rep = sysname(d); break;
            case 'n': { const char *s = sysname(d), *e = s + strlen(s); while (e > s && e[-1] >= '0' && e[-1] <= '9') e--; rep = e; break; }
            case 'M': rep = pget(d, "MAJOR"); break;
            case 'm': rep = pget(d, "MINOR"); break;
            case 'p': rep = pget(d, "DEVPATH"); break;
            case 'N': snprintf(tmp, sizeof tmp, "/dev/%s", pget(d, "DEVNAME") ? pget(d, "DEVNAME") : ""); rep = tmp; break;
            case '%': rep = "%"; break;
            case 'S': rep = "/sys"; break;
            case 'c': rep = d->result; break;
            case 'E': if (c[1] == '{') { const char *e = strchr(c, '}'); if (e) { snprintf(tmp, sizeof tmp, "%.*s", (int)(e - c - 2), c + 2); rep = pget(d, tmp); c = e; } } break;
            case 's': if (c[1] == '{') { const char *e = strchr(c, '}'); if (e) { char a[256], p[PATH_MAX + 256];
                snprintf(a, sizeof a, "%.*s", (int)(e - c - 2), c + 2); snprintf(p, sizeof p, "%s/%s", d->syspath, a);
                if (readfile(p, tmp, sizeof tmp) >= 0) rep = tmp; c = e; } } break;
            default: rep = ""; break;
            }
        } else if (*c == '$') {
            static const char *const words[] = { "kernel", "number", "devpath", "major", "minor", "env{", "attr{", "$" };
            int w = -1;
            for (int k = 0; k < 8 && w < 0; k++) if (!strncmp(c + 1, words[k], strlen(words[k]))) w = k;
            if (w < 0) { out[o++] = *c; continue; }
            if (w == 5 || w == 6) {
                const char *s = c + 1 + strlen(words[w]), *e = strchr(s, '}');
                if (!e) { out[o++] = *c; continue; }
                char a[256];
                snprintf(a, sizeof a, "%.*s", (int)(e - s), s);
                if (w == 5) rep = pget(d, a);
                else { char p[PATH_MAX + 256]; snprintf(p, sizeof p, "%s/%s", d->syspath, a); if (readfile(p, tmp, sizeof tmp) >= 0) rep = tmp; }
                c = e;
            } else {
                c += strlen(words[w]);
                char pc[3] = { '%', "knpMm$"[w == 7 ? 5 : w == 2 ? 2 : w], 0 };
                if (w == 7) rep = "$";
                else { subst(d, pc, tmp, sizeof tmp); rep = tmp; }
            }
        } else { out[o++] = *c; continue; }
        if (rep) for (; *rep && o + 1 < cap; rep++) out[o++] = *rep;
    }
    out[o] = 0;
}
static bool glob_any(const char *pat, const char *val) {
    char p[1024];
    snprintf(p, sizeof p, "%s", pat);
    for (char *s = strtok(p, "|"); s; s = strtok(NULL, "|"))
        if (!fnmatch(s, val ? val : "", 0)) return true;
    return !*pat && (!val || !*val);
}

struct rtok { char key[64], attr[256], op[3], val[1024]; };
/* KEY{attr}op"value" ; returns pointer past it, NULL at end */
static const char *next_tok(const char *s, struct rtok *t) {
    while (*s == ' ' || *s == '\t' || *s == ',') s++;
    if (!*s || *s == '#') return NULL;
    size_t k = 0;
    while (*s && ((*s >= 'A' && *s <= 'Z') || *s == '_') && k < sizeof t->key - 1) t->key[k++] = *s++;
    t->key[k] = 0;
    t->attr[0] = 0;
    if (*s == '{') {
        const char *e = strchr(s, '}');
        if (!e) return NULL;
        snprintf(t->attr, sizeof t->attr, "%.*s", (int)(e - s - 1), s + 1);
        s = e + 1;
    }
    while (*s == ' ') s++;
    if ((s[0] == '=' || s[0] == '!' || s[0] == '+' || s[0] == ':' || s[0] == '-') && s[1] == '=') { t->op[0] = s[0]; t->op[1] = '='; t->op[2] = 0; s += 2; }
    else if (s[0] == '=') { strcpy(t->op, "="); s++; }
    else return NULL;
    while (*s == ' ') s++;
    if (*s != '"') return NULL;
    s++;
    size_t v = 0;
    while (*s && *s != '"' && v < sizeof t->val - 1) { if (*s == '\\' && s[1]) s++; t->val[v++] = *s++; }
    t->val[v] = 0;
    return *s == '"' ? s + 1 : NULL;
}

struct rules { char **lines; int n; };
static int cmpstr(const void *a, const void *b) { return strcmp(strrchr(*(char *const *)a, '/') + 1, strrchr(*(char *const *)b, '/') + 1); }
static void load_rules(struct rules *r) {
    static const char *const dirs[] = { "/etc/udev/rules.d", "/run/udev/rules.d", "/lib/udev/rules.d", "/usr/lib/udev/rules.d" };
    char *files[128]; int nf = 0;
    for (int k = 0; k < 4; k++) {
        DIR *dr = opendir(dirs[k]);
        struct dirent *e;
        while (dr && (e = readdir(dr)) && nf < 128) {
            size_t l = strlen(e->d_name);
            if (l < 7 || strcmp(e->d_name + l - 6, ".rules")) continue;
            bool dup = false;
            for (int i = 0; i < nf; i++) if (!strcmp(strrchr(files[i], '/') + 1, e->d_name)) dup = true;
            if (dup) continue;                        /* an earlier directory overrides */
            if (asprintf(&files[nf], "%s/%s", dirs[k], e->d_name) >= 0) nf++;
        }
        if (dr) closedir(dr);
    }
    qsort(files, (size_t)nf, sizeof *files, cmpstr);
    r->lines = NULL; r->n = 0;
    for (int i = 0; i < nf; i++) {
        FILE *f = fopen(files[i], "re");
        char line[2048], acc[4096] = "";
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            size_t l = strlen(line);
            bool cont = l && line[l - 1] == '\\';
            if (cont) line[l - 1] = 0;
            strncat(acc, line, sizeof acc - strlen(acc) - 1);
            if (cont) continue;
            const char *s = acc + strspn(acc, " \t");
            if (*s && *s != '#') { r->lines = realloc(r->lines, sizeof *r->lines * (size_t)(r->n + 1)); r->lines[r->n++] = strdup(s); }
            acc[0] = 0;
        }
        if (f) fclose(f);
        free(files[i]);
    }
}

static bool is_parent_key(const char *k) {
    return !strcmp(k, "KERNELS") || !strcmp(k, "SUBSYSTEMS") || !strcmp(k, "DRIVERS") || !strcmp(k, "ATTRS");
}
/* one parent key against one device directory */
static bool parent_tok_at(const struct dev *d, const char *cur, const struct rtok *t) {
    char pat[1024], buf[1024], p[PATH_MAX + 256], l[PATH_MAX];
    subst(d, t->val, pat, sizeof pat);
    const char *val = NULL;
    if (t->key[0] == 'K') val = strrchr(cur, '/') + 1;
    else if (t->key[0] == 'A') { snprintf(p, sizeof p, "%s/%s", cur, t->attr); if (readfile(p, buf, sizeof buf) >= 0) val = buf; }
    else {
        snprintf(p, sizeof p, "%s/%s", cur, t->key[0] == 'S' ? "subsystem" : "driver");
        ssize_t n = readlink(p, l, sizeof l - 1);
        if (n > 0) { l[n] = 0; val = strrchr(l, '/') ? strrchr(l, '/') + 1 : l; }
    }
    if (t->key[0] == 'A' && !val) return false;           /* a missing attribute matches neither way */
    return glob_any(pat, val) == (t->op[0] == '=');
}
/* as udev: walk up from the device itself; one device must satisfy every parent key of the rule */
static bool match_parents(const struct dev *d, const struct rtok *t, int n) {
    char cur[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", d->syspath);
    while (!strncmp(cur, "/sys/devices/", 13)) {
        bool all = true;
        for (int i = 0; i < n && all; i++) if (!parent_tok_at(d, cur, &t[i])) all = false;
        if (all) return true;
        *strrchr(cur, '/') = 0;
    }
    return false;
}
static bool match_tok(struct dev *d, const struct rtok *t, const char *action) {
    const char *v = NULL;
    char buf[1024];
    if (!strcmp(t->key, "ACTION")) v = action;
    else if (!strcmp(t->key, "KERNEL")) v = sysname(d);
    else if (!strcmp(t->key, "SUBSYSTEM")) v = pget(d, "SUBSYSTEM");
    else if (!strcmp(t->key, "DEVPATH")) v = pget(d, "DEVPATH");
    else if (!strcmp(t->key, "DRIVER")) v = pget(d, "DRIVER");
    else if (!strcmp(t->key, "DEVTYPE")) v = pget(d, "DEVTYPE");
    else if (!strcmp(t->key, "NAME")) v = pget(d, "INTERFACE") ? pget(d, "INTERFACE") : pget(d, "DEVNAME");
    else if (!strcmp(t->key, "ENV")) v = pget(d, t->attr);
    else if (!strcmp(t->key, "ATTR") || !strcmp(t->key, "SYSCTL")) {
        char p[PATH_MAX + 256];
        if (t->key[0] == 'S') { snprintf(p, sizeof p, "/proc/sys/%s", t->attr); for (char *c = p + 10; *c; c++) if (*c == '.') *c = '/'; }
        else snprintf(p, sizeof p, "%s/%s", d->syspath, t->attr);
        v = readfile(p, buf, sizeof buf) >= 0 ? buf : NULL;
    } else if (!strcmp(t->key, "TAG")) {
        bool any = false;
        for (int i = 0; i < d->ntags; i++) if (glob_any(t->val, d->tags[i])) any = true;
        return any == (t->op[0] == '=');
    } else if (!strcmp(t->key, "TEST")) {
        char p[PATH_MAX], q[PATH_MAX + 1100];
        subst(d, t->val, p, sizeof p);
        if (p[0] == '/') snprintf(q, sizeof q, "%s", p); else snprintf(q, sizeof q, "%s/%s", d->syspath, p);
        return (access(q, F_OK) == 0) == (t->op[0] == '=');
    } else return t->op[0] != '=';                 /* unknown match key: never matches with == */
    char pat[1024];
    subst(d, t->val, pat, sizeof pat);
    return glob_any(pat, v) == (t->op[0] == '=');
}

static void escape_replace(char *v, bool keep_space) {   /* string_escape=replace / udev's default for links */
    for (char *c = v; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || strchr("#+-.:=@_/", *c) ||
              (keep_space && *c == ' ') || (unsigned char)*c >= 0x80)) *c = '_';
}
static void apply_tok(struct dev *d, const struct rtok *t) {
    char v[1024];
    subst(d, t->val, v, sizeof v);
    if (!strcmp(t->key, "OPTIONS")) {
        char *save;
        for (char *o = strtok_r(v, ",", &save); o; o = strtok_r(NULL, ",", &save)) {
            while (*o == ' ') o++;
            if (!strcmp(o, "last_rule")) d->last_rule = true;
            else if (!strncmp(o, "link_priority=", 14)) d->link_prio = atoi(o + 14);
            else if (!strcmp(o, "string_escape=replace")) d->esc_replace = true;
            else if (!strcmp(o, "string_escape=none")) d->esc_replace = false;
            else if (!strcmp(o, "db_persist")) d->db_persist = true;
            /* watch, nowatch, static_node=, log_level=: no inotify watches / static nodes here */
        }
        return;
    }
    if (!strcmp(t->key, "NAME")) {
        if (d->esc_replace) escape_replace(v, false);
        snprintf(d->name, sizeof d->name, "%s", v);
        return;
    }
    if (!strcmp(t->key, "SYMLINK")) escape_replace(v, true);   /* udev replaces unsafe characters */
    bool add = t->op[0] == '+', final = t->op[0] == ':';
    if (!strcmp(t->key, "ENV")) pset(d, t->attr, "%s", v);
    else if (!strcmp(t->key, "SYMLINK")) {
        if (!add) { for (int i = 0; i < d->nlinks; i++) free(d->links[i]); d->nlinks = 0; }
        char *save;
        for (char *s = strtok_r(v, " ", &save); s; s = strtok_r(NULL, " ", &save)) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, s[0] == '/' ? "%s" : "/dev/%s", s);
            add_link(d, p);
        }
    } else if (!strcmp(t->key, "TAG")) {
        if (t->op[0] == '-') { for (int i = 0; i < d->ntags; i++) if (!strcmp(d->tags[i], v)) { free(d->tags[i]); d->tags[i] = d->tags[--d->ntags]; } }
        else add_tag(d, v);
    } else if (!strcmp(t->key, "MODE")) d->mode = (int)strtol(v, NULL, 8);
    else if (!strcmp(t->key, "OWNER")) { struct passwd *p = getpwnam(v); d->uid = p ? p->pw_uid : (uid_t)strtoul(v, NULL, 10); }
    else if (!strcmp(t->key, "GROUP")) { struct group *g = getgrnam(v); d->gid = g ? g->gr_gid : (gid_t)strtoul(v, NULL, 10); }
    else if (!strcmp(t->key, "RUN")) {
        if (!add) { for (int i = 0; i < d->nrun; i++) free(d->run[i]); d->nrun = 0; }
        if (!strcmp(t->attr, "builtin")) return;      /* kmod etc.: nothing to load here */
        char c[1024];
        prog_cmd(v, c, sizeof c);
        if (d->nrun < 8) d->run[d->nrun++] = strdup(c);
    }
    (void)final;
}

/* PROGRAM / IMPORT{program} / RUN: a bare name is looked up in /usr/lib/udev and /lib/udev,
 * the program gets the device properties as its environment */
static bool collect(const char *syspath, struct dev *d);
static void dbid(const struct dev *d, char *out, size_t cap);
static void prog_cmd(const char *in, char *out, size_t cap) {
    char w[256];
    size_t n = strcspn(in, " \t");
    snprintf(w, sizeof w, "%.*s", (int)n, in);
    if (!strchr(w, '/')) {
        static const char *const dirs[] = { "/usr/lib/udev", "/lib/udev" };
        for (int k = 0; k < 2; k++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", dirs[k], w);
            if (!access(p, X_OK)) { snprintf(out, cap, "%s/%s", dirs[k], in); return; }
        }
    }
    snprintf(out, cap, "%s", in);
}
static void child_env(const struct dev *d, const char *action) {
    clearenv();
    for (int k = 0; k < d->n; k++) putenv(d->kv[k]);
    if (action) setenv("ACTION", action, 1);
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
}
/* run and capture stdout; true if it exited 0 */
static bool run_capture(const struct dev *d, const char *cmd, char *out, size_t cap) {
    char c[2048];
    prog_cmd(cmd, c, sizeof c);
    int pp[2];
    if (pipe(pp)) return false;
    pid_t p = fork();
    if (p == 0) {
        dup2(pp[1], 1);
        close(pp[0]); close(pp[1]);
        child_env(d, pget(d, "ACTION"));
        execl("/bin/sh", "sh", "-c", c, (char *)NULL);
        _exit(127);
    }
    close(pp[1]);
    size_t o = 0;
    ssize_t r;
    while ((r = read(pp[0], out + o, cap - 1 - o)) > 0) { o += (size_t)r; if (o + 1 >= cap) break; }
    out[o] = 0;
    close(pp[0]);
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
/* KEY=VALUE lines (optional quotes) into properties */
static void import_lines(struct dev *d, char *text, const char *glob) {
    char *save;
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '#' || !*l) continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }
        if (glob && fnmatch(glob, l, 0)) continue;
        pset(d, l, "%s", v);
    }
}
static void hwdb_lookup(struct dev *d, const char *key);
/* IMPORT{program|file|builtin|parent|cmdline}: true on success */
static struct dev *cur_old;                          /* db entry of the event being processed */
static bool do_import(struct dev *d, const struct rtok *t) {
    char v[1024];
    subst(d, t->val, v, sizeof v);
    static char buf[16384];
    if (!strcmp(t->attr, "program")) {
        if (!run_capture(d, v, buf, sizeof buf)) return false;
        import_lines(d, buf, NULL);
        return true;
    }
    if (!strcmp(t->attr, "file")) {
        int n = readfile(v, buf, sizeof buf);
        if (n < 0) return false;
        import_lines(d, buf, NULL);
        return true;
    }
    if (!strcmp(t->attr, "builtin")) {               /* input_id, path_id, blkid, net_id run anyway */
        if (!strncmp(v, "hwdb", 4)) {
            const char *a = v + 4;
            while (*a == ' ') a++;
            char k[512];
            snprintf(k, sizeof k, "%s", *a == '\'' || *a == '"' ? a + 1 : a);
            size_t kl = strlen(k);
            if (kl && (k[kl - 1] == '\'' || k[kl - 1] == '"')) k[kl - 1] = 0;
            int before = d->n;
            if (*k && strncmp(k, "--", 2)) hwdb_lookup(d, k);
            return d->n > before || !*k || !strncmp(k, "--", 2);
        }
        return true;
    }
    if (!strcmp(t->attr, "parent")) {                /* properties of the nearest parent's db entry */
        char pp[PATH_MAX], id[160], path[PATH_MAX], line[1200];
        snprintf(pp, sizeof pp, "%s", d->syspath);
        bool ok = false;
        while (!ok && strrchr(pp, '/') && strlen(pp) > 13) {
            *strrchr(pp, '/') = 0;
            char ue[PATH_MAX + 8];
            snprintf(ue, sizeof ue, "%s/uevent", pp);
            if (access(ue, F_OK)) continue;
            struct dev *pd = malloc(sizeof *pd);
            dev_init(pd);
            if (collect(pp, pd)) {
                char l[PATH_MAX + 16], tg[PATH_MAX];
                snprintf(l, sizeof l, "%s/subsystem", pp);
                ssize_t n = readlink(l, tg, sizeof tg - 1);
                if (n > 0) { tg[n] = 0; pset(pd, "SUBSYSTEM", "%s", strrchr(tg, '/') ? strrchr(tg, '/') + 1 : tg); }
                dbid(pd, id, sizeof id);
                snprintf(path, sizeof path, "/run/udev/data/%s", id);
                FILE *f = fopen(path, "re");
                if (f) {
                    ok = true;
                    while (fgets(line, sizeof line, f)) {
                        if (strncmp(line, "E:", 2)) continue;
                        line[strcspn(line, "\n")] = 0;
                        import_lines(d, line + 2, v);
                    }
                    fclose(f);
                }
            }
            dev_free(pd); free(pd);
        }
        return ok;
    }
    if (!strcmp(t->attr, "cmdline")) {
        int n = readfile("/proc/cmdline", buf, sizeof buf);
        if (n <= 0) return false;
        for (char *w = strtok(buf, " \n"); w; w = strtok(NULL, " \n")) {
            size_t l = strlen(v);
            if (!strncmp(w, v, l) && (w[l] == '=' || !w[l])) { pset(d, v, "%s", w[l] ? w + l + 1 : "1"); return true; }
        }
        return false;
    }
    if (!strcmp(t->attr, "db")) {                    /* a property of the device's previous event */
        const char *o = cur_old ? pget(cur_old, v) : NULL;
        if (!o) return false;
        pset(d, v, "%s", o);
        return true;
    }
    return false;
}

static void run_rules(const struct rules *r, struct dev *d, const char *action) {
    for (int i = 0; i < r->n; i++) {
        const char *s = r->lines[i];
        struct rtok t;
        bool ok = true;
        const char *p = s;
        struct rtok pk[8];
        int npk = 0;
        /* first pass: every match key must hold */
        while (ok && (p = next_tok(p, &t))) {
            bool assign = !strcmp(t.op, "=") || !strcmp(t.op, "+=") || !strcmp(t.op, ":=") || !strcmp(t.op, "-=");
            if (!strcmp(t.key, "IMPORT")) { if (do_import(d, &t) != (t.op[0] != '!')) ok = false; continue; }
            if (!strcmp(t.key, "PROGRAM")) {               /* runs in order; fails the rule on non-zero exit */
                char c[1024];
                subst(d, t.val, c, sizeof c);
                if (!run_capture(d, c, d->result, sizeof d->result)) ok = false;
                d->result[strcspn(d->result, "\n")] = 0;
                continue;
            }
            if (!strcmp(t.key, "RESULT")) {
                char pat[1024];
                subst(d, t.val, pat, sizeof pat);
                if (glob_any(pat, d->result) != (t.op[0] == '=')) ok = false;
                continue;
            }
            if (!assign && is_parent_key(t.key)) { if (npk < 8) pk[npk++] = t; continue; }
            if (!assign && !match_tok(d, &t, action)) ok = false;
        }
        if (ok && npk && !match_parents(d, pk, npk)) ok = false;
        if (!ok) continue;
        const char *go = NULL;
        p = s;
        while ((p = next_tok(p, &t))) {
            bool assign = !strcmp(t.op, "=") || !strcmp(t.op, "+=") || !strcmp(t.op, ":=") || !strcmp(t.op, "-=");
            if (!assign) continue;
            if (!strcmp(t.key, "GOTO")) { go = strdup(t.val); continue; }
            if (!strcmp(t.key, "LABEL") || !strcmp(t.key, "IMPORT") || !strcmp(t.key, "PROGRAM")) continue;
            apply_tok(d, &t);
        }
        if (d->last_rule) { if (go) free((void *)go); break; }   /* OPTIONS+="last_rule" */
        if (go) {                                   /* jump to the LABEL="go" line */
            for (int j = i + 1; j < r->n; j++) {
                struct rtok u;
                const char *q = r->lines[j];
                bool hit = false;
                while ((q = next_tok(q, &u))) if (!strcmp(u.key, "LABEL") && !strcmp(u.val, go)) hit = true;
                if (hit) { i = j; break; }
            }
            free((void *)go);
        }
    }
}

/* ---------------------------------------------------------------- database, nodes, links */
static void dbid(const struct dev *d, char *out, size_t cap) {
    const char *ma = pget(d, "MAJOR"), *mi = pget(d, "MINOR"), *sub = pget(d, "SUBSYSTEM"), *ifi = pget(d, "IFINDEX");
    if (ma && mi) snprintf(out, cap, "%c%s:%s", is(sub, "block") ? 'b' : 'c', ma, mi);
    else if (ifi && is(sub, "net")) snprintf(out, cap, "n%s", ifi);
    else snprintf(out, cap, "+%s:%s", sub ? sub : "", sysname(d));
}
static uint64_t usec_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static void mkdirs(void) {
    mkdir("/run", 0755); mkdir("/run/udev", 0755); mkdir("/run/udev/data", 0755); mkdir("/run/udev/tags", 0755);
    int fd = open("/run/udev/control", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}
static void mkparents(char *path) {
    for (char *s = path + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(path, 0755); *s = '/'; }
}
/* relative link target from the link's directory to /dev/<devname> */
static void link_create(const char *link, const char *devname) {
    char l[PATH_MAX], t[PATH_MAX] = "";
    snprintf(l, sizeof l, "%s", link);
    mkparents(l);
    int depth = 0;
    for (const char *c = link + 5; *c; c++) if (*c == '/') depth++;       /* below /dev */
    for (int i = 0; i < depth; i++) strcat(t, "../");
    strncat(t, devname, sizeof t - strlen(t) - 1);
    char cur[PATH_MAX];
    ssize_t n = readlink(link, cur, sizeof cur - 1);
    if (n >= 0) { cur[n] = 0; if (!strcmp(cur, t)) return; unlink(link); }
    if (symlink(t, link) < 0 && errno != EEXIST) fprintf(stderr, "udevshim: %s: %s\n", link, strerror(errno));
}
/* /run/udev/links/<link with '/' as \x2f>/<id> = "priority devname": the claimant with the
 * highest link_priority owns the link; on a tie the current owner keeps it, else the newest */
static void link_dir(const char *link, char *out, size_t cap) {
    size_t o = snprintf(out, cap, "/run/udev/links/");
    for (const char *c = link + 5; *c && o + 5 < cap; c++) {
        if (*c == '/') { memcpy(out + o, "\\x2f", 4); o += 4; } else out[o++] = *c;
    }
    out[o] = 0;
}
static void link_update(const char *link, const char *prefer) {
    char dir[PATH_MAX], path[PATH_MAX + 200], best[PATH_MAX] = "", cur[PATH_MAX] = "";
    link_dir(link, dir, sizeof dir);
    ssize_t cn = readlink(link, cur, sizeof cur - 1);
    if (cn > 0) cur[cn] = 0; else cur[0] = 0;
    const char *curbase = strrchr(cur, '/') ? strrchr(cur, '/') + 1 : cur;
    int bestp = 0; bool have = false, best_cur = false, best_pref = false;
    DIR *dr = opendir(dir);
    struct dirent *e;
    while (dr && (e = readdir(dr))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        char buf[PATH_MAX + 32];
        if (readfile(path, buf, sizeof buf) <= 0) continue;
        int prio = 0; char dn[PATH_MAX];
        if (sscanf(buf, "%d %4095s", &prio, dn) != 2) continue;
        const char *dnb = strrchr(dn, '/') ? strrchr(dn, '/') + 1 : dn;
        bool is_cur = cur[0] && !strcmp(dnb, curbase), is_pref = prefer && !strcmp(e->d_name, prefer);
        bool better = !have || prio > bestp || (prio == bestp && !best_cur && (is_cur || (is_pref && !best_pref)));
        if (better) { have = true; bestp = prio; snprintf(best, sizeof best, "%s", dn); best_cur = is_cur; best_pref = is_pref; }
    }
    if (dr) closedir(dr);
    if (have) link_create(link, best);
    else { unlink(link); rmdir(dir); }
}
static void link_claim(const char *link, const char *id, int prio, const char *devname) {
    char dir[PATH_MAX], path[PATH_MAX + 200];
    link_dir(link, dir, sizeof dir);
    mkdir("/run/udev/links", 0755);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s", dir, id);
    FILE *f = fopen(path, "we");
    if (f) { fprintf(f, "%d %s\n", prio, devname); fclose(f); }
    link_update(link, id);
}
static void link_drop(const char *link, const char *id) {
    char dir[PATH_MAX], path[PATH_MAX + 200];
    link_dir(link, dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, id);
    if (unlink(path) && errno == ENOENT && access(dir, F_OK)) { unlink(link); return; }   /* pre-registry link */
    link_update(link, NULL);
}
static void db_old(const char *id, struct dev *old) {
    char path[PATH_MAX], line[1200];
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    FILE *f = fopen(path, "re");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "S:", 2)) { char p[PATH_MAX]; snprintf(p, sizeof p, "/dev/%s", line + 2); add_link(old, p); }
        else if (!strncmp(line, "I:", 2)) pset(old, "USEC_INITIALIZED", "%s", line + 2);
        else if (!strncmp(line, "G:", 2)) add_tag(old, line + 2);
        else if (!strncmp(line, "E:", 2) && strchr(line + 2, '=')) {
            char *eq = strchr(line + 2, '=');
            *eq = 0;
            pset(old, line + 2, "%s", eq + 1);
        }
        else if (!strncmp(line, "L:", 2)) old->link_prio = atoi(line + 2);
    }
    if (f) fclose(f);
}
static void db_write(const char *id, struct dev *d) {
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return;
    fprintf(f, "I:%s\n", pget(d, "USEC_INITIALIZED") ? pget(d, "USEC_INITIALIZED") : "1");
    if (d->link_prio) fprintf(f, "L:%d\n", d->link_prio);
    for (int i = 0; i < d->nlinks; i++) fprintf(f, "S:%s\n", d->links[i] + 5);
    static const char *const skip[] = { "ACTION", "DEVPATH", "SUBSYSTEM", "SEQNUM", "MAJOR", "MINOR", "DEVNAME", "DEVTYPE",
                                        "IFINDEX", "INTERFACE", "TAGS", "CURRENT_TAGS", "USEC_INITIALIZED", "PRODUCT", "NAME",
                                        "PHYS", "PROP", "EV", "KEY", "REL", "ABS", "MSC", "LED", "MODALIAS", "PARTN", "DRIVER",
                                        "DEVLINKS", "DEVPATH_OLD" };
    for (int i = 0; i < d->n; i++) {
        bool k = true;
        for (size_t s = 0; s < sizeof skip / sizeof *skip; s++) {
            size_t l = strlen(skip[s]);
            if (!strncmp(d->kv[i], skip[s], l) && d->kv[i][l] == '=') k = false;
        }
        if (k) fprintf(f, "E:%s\n", d->kv[i]);
    }
    for (int i = 0; i < d->ntags; i++) fprintf(f, "G:%s\nQ:%s\n", d->tags[i], d->tags[i]);
    fprintf(f, "V:1\n");
    fclose(f);
    rename(tmp, path);
    for (int i = 0; i < d->ntags; i++) {
        snprintf(path, sizeof path, "/run/udev/tags/%s", d->tags[i]);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "/run/udev/tags/%s/%s", d->tags[i], id);
        int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    }
}
static void db_remove(const char *id) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    unlink(path);
    DIR *dr = opendir("/run/udev/tags");
    struct dirent *e;
    while (dr && (e = readdir(dr))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/run/udev/tags/%s/%s", e->d_name, id);
        unlink(path);
    }
    if (dr) closedir(dr);
}
static void node_update(const struct dev *d, bool create) {
    const char *dn = pget(d, "DEVNAME"), *ma = pget(d, "MAJOR"), *mi = pget(d, "MINOR");
    if (!dn || !ma || !mi) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/dev/%s", dn);
    struct stat st;
    if (lstat(path, &st) && create) {
        mkparents(path);
        mknod(path, (is(pget(d, "SUBSYSTEM"), "block") ? S_IFBLK : S_IFCHR) | (d->mode >= 0 ? d->mode : 0660), makedev(atoi(ma), atoi(mi)));
    }
    if (d->mode >= 0) chmod(path, (mode_t)d->mode);
    if (d->uid != (uid_t)-1 || d->gid != (gid_t)-1) if (chown(path, d->uid, d->gid)) {}
}
static void run_programs(const struct dev *d, const char *action) {
    for (int i = 0; i < d->nrun; i++) {
        pid_t p = fork();
        if (p == 0) {
            child_env(d, action);
            execl("/bin/sh", "sh", "-c", d->run[i], (char *)NULL);
            _exit(127);
        }
        if (p > 0) waitpid(p, NULL, 0);
    }
}

/* finish a device after its uevent variables are in d: builtins, rules, db, nodes, links */
static struct rules rules;
static void process(struct dev *d, const char *action) {
    char id[160];
    pset(d, "ACTION", "%s", action);
    if (!pget(d, "SUBSYSTEM")) {
        char l[PATH_MAX + 16], t[PATH_MAX];
        snprintf(l, sizeof l, "%s/subsystem", d->syspath);
        ssize_t n = readlink(l, t, sizeof t - 1);
        if (n > 0) { t[n] = 0; pset(d, "SUBSYSTEM", "%s", strrchr(t, '/') ? strrchr(t, '/') + 1 : t); }
    }
    struct dev *old = calloc(1, sizeof *old);
    dev_init(old);
    dbid(d, id, sizeof id);
    db_old(id, old);
    if (!strcmp(action, "remove")) {
        for (int i = 0; i < old->nlinks; i++) link_drop(old->links[i], id);
        db_remove(id);
        run_rules(&rules, d, action);
        run_programs(d, action);
        dev_free(old); free(old);
        return;
    }
    builtins(d);
    cur_old = old;
    run_rules(&rules, d, action);
    cur_old = NULL;
    if (is(pget(d, "SUBSYSTEM"), "net") && d->name[0] && pget(d, "INTERFACE") && strcmp(d->name, pget(d, "INTERFACE")) &&
        !strcmp(action, "add")) {                     /* NAME=: rename the interface */
        struct ifreq ifr = { 0 };
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", pget(d, "INTERFACE"));
        snprintf(ifr.ifr_newname, sizeof ifr.ifr_newname, "%s", d->name);
        int sk = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (sk >= 0 && ioctl(sk, SIOCSIFNAME, &ifr) == 0) {
            pset(d, "INTERFACE", "%s", d->name);
            char *slash = strrchr(d->syspath, '/');
            if (slash) snprintf(slash + 1, sizeof d->syspath - (size_t)(slash + 1 - d->syspath), "%s", d->name);
            const char *dp = pget(d, "DEVPATH");
            if (dp && strrchr(dp, '/')) { char np[PATH_MAX]; snprintf(np, sizeof np, "%.*s/%s", (int)(strrchr(dp, '/') - dp), dp, d->name); pset(d, "DEVPATH", "%s", np); }
        } else fprintf(stderr, "udevshim: rename %s to %s: %s\n", ifr.ifr_name, d->name, strerror(errno));
        if (sk >= 0) close(sk);
    }
    pset(d, "USEC_INITIALIZED", "%s", pget(old, "USEC_INITIALIZED") ? pget(old, "USEC_INITIALIZED") : "");
    if (!pget(d, "USEC_INITIALIZED")) pset(d, "USEC_INITIALIZED", "%llu", (unsigned long long)usec_now());
    if (d->ntags) {
        char t[512] = ":";
        for (int i = 0; i < d->ntags; i++) { strncat(t, d->tags[i], sizeof t - strlen(t) - 2); strcat(t, ":"); }
        pset(d, "TAGS", "%s", t);
        pset(d, "CURRENT_TAGS", "%s", t);
    }
    if (d->nlinks) {
        char t[2048] = "";
        for (int i = 0; i < d->nlinks; i++) { if (i) strcat(t, " "); strncat(t, d->links[i], sizeof t - strlen(t) - 2); }
        pset(d, "DEVLINKS", "%s", t);
    }
    for (int i = 0; i < old->nlinks; i++) {          /* links no longer wanted */
        bool keep = false;
        for (int k = 0; k < d->nlinks; k++) if (!strcmp(old->links[i], d->links[k])) keep = true;
        if (!keep) link_drop(old->links[i], id);
    }
    node_update(d, !strcmp(action, "add") || !strcmp(action, "change"));
    if (pget(d, "DEVNAME")) for (int i = 0; i < d->nlinks; i++) link_claim(d->links[i], id, d->link_prio, pget(d, "DEVNAME"));
    db_write(id, d);
    run_programs(d, action);
    dev_free(old); free(old);
}

/* uevent file of a /sys device directory into d */
static bool collect(const char *syspath, struct dev *d) {
    char buf[4096], u[PATH_MAX + 16];
    snprintf(d->syspath, sizeof d->syspath, "%s", syspath);
    snprintf(u, sizeof u, "%s/uevent", syspath);
    if (readfile(u, buf, sizeof buf) < 0) return false;
    const char *dp = strstr(syspath, "/devices/");
    if (dp) pset(d, "DEVPATH", "%s", dp);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        pset(d, line, "%s", eq + 1);
    }
    return true;
}

static void for_each_device(void (*cb)(const char *syspath, void *arg), void *arg) {
    static const char *const roots[] = { "/sys/bus", "/sys/class" };
    for (int r = 0; r < 2; r++) {
        DIR *dr = opendir(roots[r]);
        struct dirent *e;
        while (dr && (e = readdir(dr))) {
            if (e->d_name[0] == '.') continue;
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, r ? "%s/%s" : "%s/%s/devices", roots[r], e->d_name);
            DIR *d2 = opendir(dir);
            struct dirent *e2;
            while (d2 && (e2 = readdir(d2))) {
                if (e2->d_name[0] == '.') continue;
                char l[PATH_MAX + 256], t[PATH_MAX];
                snprintf(l, sizeof l, "%s/%s", dir, e2->d_name);
                if (realpath(l, t)) cb(t, arg);
            }
            if (d2) closedir(d2);
        }
        if (dr) closedir(dr);
    }
}
static void cold_one(const char *syspath, void *arg) {
    struct dev *d = malloc(sizeof *d);
    dev_init(d);
    if (collect(syspath, d)) { process(d, "add"); (*(int *)arg)++; }
    dev_free(d); free(d);
}
static void trig_one(const char *syspath, void *arg) {
    char u[PATH_MAX + 16];
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
    const uint8_t *p = (const uint8_t *)s;
    for (; len >= 4; p += 4, len -= 4) {
        uint32_t k = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        k *= m; k ^= k >> 24; k *= m; h *= m; h ^= k;
    }
    switch (len) {
    case 3: h ^= (uint32_t)p[2] << 16; __attribute__((fallthrough));
    case 2: h ^= (uint32_t)p[1] << 8; __attribute__((fallthrough));
    case 1: h ^= p[0]; h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}
struct mon_hdr { char prefix[8]; uint32_t magic, header_size, properties_off, properties_len, sub_hash, devtype_hash, bloom_hi, bloom_lo; };
static void broadcast(int sk, struct dev *d) {
    static char msg[16384];
    struct mon_hdr h = { "libudev", htobe32(0xfeedcafe), sizeof h, sizeof h, 0, 0, 0, 0, 0 };
    size_t n = sizeof h;
    for (int i = 0; i < d->n; i++) {
        size_t l = strlen(d->kv[i]) + 1;
        if (n + l > sizeof msg) break;
        memcpy(msg + n, d->kv[i], l);
        n += l;
    }
    h.properties_len = (uint32_t)(n - sizeof h);
    const char *sub = pget(d, "SUBSYSTEM"), *dt = pget(d, "DEVTYPE");
    if (sub) h.sub_hash = htobe32(murmur2(sub));
    if (dt) h.devtype_hash = htobe32(murmur2(dt));
    uint64_t bloom = 0;
    for (int i = 0; i < d->ntags; i++) {
        uint32_t x = murmur2(d->tags[i]);
        bloom |= 1ull << (x & 63) | 1ull << ((x >> 6) & 63) | 1ull << ((x >> 12) & 63) | 1ull << ((x >> 18) & 63);
    }
    h.bloom_hi = htobe32((uint32_t)(bloom >> 32)); h.bloom_lo = htobe32((uint32_t)bloom);
    memcpy(msg, &h, sizeof h);
    struct sockaddr_nl_u to = { AF_NETLINK, 0, 0, 2 };
    sendto(sk, msg, n, 0, (struct sockaddr *)&to, sizeof to);
    /* group 4: the kernel's format plus udev properties, for libudev-zero's "udev" monitor */
    const char *a = pget(d, "ACTION"), *dp = pget(d, "DEVPATH");
    if (!a || !dp) return;
    n = (size_t)snprintf(msg, sizeof msg, "%s@%s", a, dp) + 1;
    for (int i = 0; i < d->n && n < sizeof msg; i++) {
        size_t l = strlen(d->kv[i]) + 1;
        if (n + l > sizeof msg) break;
        memcpy(msg + n, d->kv[i], l);
        n += l;
    }
    to.groups = 4;
    sendto(sk, msg, n, 0, (struct sockaddr *)&to, sizeof to);
}

static int uevent_socket(unsigned groups) {
    int sk = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl_u a = { AF_NETLINK, 0, 0, groups };
    if (sk < 0 || bind(sk, (struct sockaddr *)&a, sizeof a) < 0) { perror("udevshim: uevent socket"); return -1; }
    int on = 1, sz = 1 << 20;
    setsockopt(sk, SOL_SOCKET, SO_PASSCRED, &on, sizeof on);
    setsockopt(sk, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    return sk;
}
static void write_seqnum(const char *s) {
    FILE *f = fopen("/run/udev/queue-seqnum.tmp", "we");
    if (!f) return;
    fprintf(f, "%s\n", s);
    fclose(f);
    rename("/run/udev/queue-seqnum.tmp", "/run/udev/queue-seqnum");
}
static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }
static int daemon_loop(void) {
    int sk = uevent_socket(1);
    if (sk < 0) return 1;
    char sq[32] = "0";
    readfile("/sys/kernel/uevent_seqnum", sq, sizeof sq);
    write_seqnum(sq);                                /* events before we listened are coldplug's */
    FILE *pf = fopen("/run/udev/udevshim.pid", "we");
    if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    static char buf[16384];
    while (!stop) {
        struct sockaddr_nl_u from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(sk, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) { if (errno == EINTR) continue; perror("udevshim: recv"); break; }
        if (from.pid != 0 || !(from.groups & 1)) continue;         /* kernel messages only */
        buf[n] = 0;
        struct dev *d = malloc(sizeof *d);
        dev_init(d);
        for (char *s = buf + strlen(buf) + 1; s < buf + n; s += strlen(s) + 1) {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0; pset(d, s, "%s", eq + 1); *eq = '=';
        }
        const char *act = pget(d, "ACTION"), *dp = pget(d, "DEVPATH");
        if (act && dp) {
            char a[16];
            snprintf(a, sizeof a, "%s", act);
            snprintf(d->syspath, sizeof d->syspath, "/sys%s", dp);
            const char *old = pget(d, "DEVPATH_OLD");
            if (!strcmp(a, "move") && old) {           /* the db id may change (net: n<ifindex> does not) */
            }
            process(d, a);
            broadcast(sk, d);
        }
        if (pget(d, "SEQNUM")) write_seqnum(pget(d, "SEQNUM"));
        dev_free(d); free(d);
    }
    unlink("/run/udev/udevshim.pid");
    return 0;
}

static bool daemon_running(void) {
    char b[32];
    if (readfile("/run/udev/udevshim.pid", b, sizeof b) <= 0) return false;
    return kill(atoi(b), 0) == 0 || errno == EPERM;
}
static int settle(int seconds) {
    for (long t = 0; t < seconds * 100L; t++) {
        if (!daemon_running()) return 0;
        char k[32], q[32];
        if (readfile("/sys/kernel/uevent_seqnum", k, sizeof k) > 0 && readfile("/run/udev/queue-seqnum", q, sizeof q) > 0 &&
            strtoull(q, NULL, 10) >= strtoull(k, NULL, 10)) return 0;
        usleep(10000);
    }
    fprintf(stderr, "udevshim: settle timed out\n");
    return 1;
}

static int resolve(const char *arg, char *sp) {
    struct stat st;
    if (!strncmp(arg, "/sys/", 5) || !strncmp(arg, "/devices/", 9)) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s%s", arg[1] == 'd' ? "/sys" : "", arg);
        return realpath(p, sp) ? 0 : -1;
    }
    char n[PATH_MAX];
    snprintf(n, sizeof n, arg[0] == '/' ? "%s" : "/dev/%s", arg);
    if (stat(n, &st) || !(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) return -1;
    char l[64];
    snprintf(l, sizeof l, "/sys/dev/%s/%u:%u", S_ISBLK(st.st_mode) ? "block" : "char", major(st.st_rdev), minor(st.st_rdev));
    return realpath(l, sp) ? 0 : -1;
}
/* properties as the database has them (falls back to computing them) */
static int query(const char *arg, bool all) {
    char sp[PATH_MAX];
    if (resolve(arg, sp)) { fprintf(stderr, "udevshim: %s: no such device\n", arg); return 1; }
    struct dev *d = malloc(sizeof *d);
    dev_init(d);
    if (!collect(sp, d)) return 1;
    pset(d, "ACTION", "add");
    char l[PATH_MAX + 16], t[PATH_MAX];
    snprintf(l, sizeof l, "%s/subsystem", sp);
    ssize_t n = readlink(l, t, sizeof t - 1);
    if (n > 0) { t[n] = 0; pset(d, "SUBSYSTEM", "%s", strrchr(t, '/') ? strrchr(t, '/') + 1 : t); }
    char id[160], path[PATH_MAX], line[1200];
    dbid(d, id, sizeof id);
    snprintf(path, sizeof path, "/run/udev/data/%s", id);
    FILE *f = fopen(path, "re");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "E:", 2)) { char *eq = strchr(line + 2, '='); if (eq) { *eq = 0; pset(d, line + 2, "%s", eq + 1); } }
            else if (!strncmp(line, "I:", 2)) pset(d, "USEC_INITIALIZED", "%s", line + 2);
            else if (!strncmp(line, "S:", 2)) { char p[PATH_MAX]; snprintf(p, sizeof p, "/dev/%s", line + 2); add_link(d, p); }
            else if (!strncmp(line, "G:", 2)) add_tag(d, line + 2);
        }
        fclose(f);
    } else { builtins(d); run_rules(&rules, d, "add"); }
    for (int i = 0; i < d->n; i++) {
        if (!strncmp(d->kv[i], "ACTION=", 7)) continue;
        if (!strncmp(d->kv[i], "DEVNAME=", 8) && d->kv[i][8] != '/') printf("DEVNAME=/dev/%s\n", d->kv[i] + 8);
        else printf("%s\n", d->kv[i]);
    }
    if (d->nlinks) { printf("DEVLINKS="); for (int i = 0; i < d->nlinks; i++) printf("%s%s", i ? " " : "", d->links[i]); printf("\n"); }
    if (d->ntags) { printf("TAGS=:"); for (int i = 0; i < d->ntags; i++) printf("%s:", d->tags[i]); printf("\n"); }
    (void)all;
    return 0;
}
/* db id (c13:64, b254:0, n2, +pci:0000:00:03.0) back to a /sys path */
static bool id_syspath(const char *id, char *sp) {
    char p[PATH_MAX];
    if (id[0] == 'c' || id[0] == 'b') snprintf(p, sizeof p, "/sys/dev/%s/%s", id[0] == 'b' ? "block" : "char", id + 1);
    else if (id[0] == 'n') {
        DIR *dr = opendir("/sys/class/net");
        struct dirent *e;
        bool ok = false;
        while (dr && !ok && (e = readdir(dr))) {
            char q[PATH_MAX], b[32];
            if (e->d_name[0] == '.') continue;
            snprintf(q, sizeof q, "/sys/class/net/%s/ifindex", e->d_name);
            if (readfile(q, b, sizeof b) > 0 && !strcmp(b, id + 1)) { snprintf(p, sizeof p, "/sys/class/net/%s", e->d_name); ok = true; }
        }
        if (dr) closedir(dr);
        if (!ok) return false;
    } else if (id[0] == '+') {
        char sub[64];
        const char *c = strchr(id, ':');
        if (!c || c - id - 1 >= (long)sizeof sub) return false;
        memcpy(sub, id + 1, (size_t)(c - id - 1)); sub[c - id - 1] = 0;
        snprintf(p, sizeof p, "/sys/bus/%s/devices/%s", sub, c + 1);
        if (access(p, F_OK)) snprintf(p, sizeof p, "/sys/class/%s/%s", sub, c + 1);
    } else return false;
    return realpath(p, sp) != NULL;
}
/* udevadm info --export-db: P: N: S: E: G: records separated by blank lines */
static int export_db(void) {
    DIR *dr = opendir("/run/udev/data");
    struct dirent *e;
    while (dr && (e = readdir(dr))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX], sp[PATH_MAX], line[1200], ue[4096];
        if (!id_syspath(e->d_name, sp)) continue;              /* stale entry */
        printf("P: %s\n", sp + 4);
        snprintf(p, sizeof p, "%s/uevent", sp);
        if (readfile(p, ue, sizeof ue) > 0) {
            char *dn = strstr(ue, "DEVNAME=");
            if (dn && (dn == ue || dn[-1] == '\n')) { dn += 8; dn[strcspn(dn, "\n")] = 0; printf("N: %s\n", dn); }
        }
        snprintf(p, sizeof p, "/run/udev/data/%s", e->d_name);
        FILE *f = fopen(p, "re");
        while (f && fgets(line, sizeof line, f)) if (line[0] != 'I') printf("%c: %s", line[0], line + 2);
        if (f) fclose(f);
        printf("\n");
    }
    if (dr) closedir(dr);
    return 0;
}
static int monitor(bool props) {
    int sk = uevent_socket(2);
    if (sk < 0) return 1;
    static char buf[16384];
    for (;;) {
        ssize_t n = recv(sk, buf, sizeof buf - 1, 0);
        if (n < 40 || memcmp(buf, "libudev", 8)) continue;
        buf[n] = 0;
        const char *a = "", *dp = "", *sub = "";
        for (char *s = buf + 40; s < buf + n; s += strlen(s) + 1) {
            if (!strncmp(s, "ACTION=", 7)) a = s + 7;
            else if (!strncmp(s, "DEVPATH=", 8)) dp = s + 8;
            else if (!strncmp(s, "SUBSYSTEM=", 10)) sub = s + 10;
        }
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        printf("UDEV  [%ld.%06ld] %-8s %s (%s)\n", (long)t.tv_sec, t.tv_nsec / 1000, a, dp, sub);
        if (props) { for (char *s = buf + 40; s < buf + n; s += strlen(s) + 1) printf("%s\n", s); printf("\n"); }
        fflush(stdout);
    }
}

static int usage(void) {
    fprintf(stderr, "usage: udevshim -c | -d [-f] | -t [ACTION] | -s [SECONDS] | -q PATH\n"
                    "       udevadm settle | trigger [--action=X] | info [-q property] -n NODE|-p PATH | info --export-db | monitor [--property]\n");
    return 2;
}
static int udevadm(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "settle")) {
        int to = 120;
        for (int i = 2; i < argc; i++) if (!strncmp(argv[i], "--timeout=", 10)) to = atoi(argv[i] + 10); else if (!strcmp(argv[i], "-t") && i + 1 < argc) to = atoi(argv[++i]);
        return settle(to);
    }
    if (!strcmp(cmd, "trigger")) {
        const char *a = "change";
        for (int i = 2; i < argc; i++) if (!strncmp(argv[i], "--action=", 9)) a = argv[i] + 9; else if (!strcmp(argv[i], "-c") && i + 1 < argc) a = argv[++i];
        for_each_device(trig_one, (void *)a);
        return 0;
    }
    if (!strcmp(cmd, "info")) {
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--export-db") || !strcmp(argv[i], "-e")) return export_db();
            if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-p") || !strcmp(argv[i], "--name") || !strcmp(argv[i], "--path")) && i + 1 < argc) return query(argv[i + 1], true);
            if (!strncmp(argv[i], "--name=", 7) || !strncmp(argv[i], "--path=", 7)) return query(argv[i] + 7, true);
            if (argv[i][0] == '/' ) return query(argv[i], true);
        }
        return usage();
    }
    if (!strcmp(cmd, "monitor")) {
        bool p = false;
        for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--property") || !strcmp(argv[i], "-p")) p = true;
        return monitor(p);
    }
    if (!strcmp(cmd, "control")) return 0;
    if (!strcmp(cmd, "hwdb")) {
        const char *out = HWDB_BIN;
        bool upd = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--update") || !strcmp(argv[i], "-u")) upd = true;
            else if (!strncmp(argv[i], "--output=", 9)) out = argv[i] + 9;
            else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
            else if (!strncmp(argv[i], "--test=", 7)) return hwdb_test(argv[i] + 7);
            else if (!strcmp(argv[i], "-t") && i + 1 < argc) return hwdb_test(argv[++i]);
        }
        return upd ? hwdb_update(out) : usage();
    }
    return usage();
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    load_rules(&rules);
    if (!strcmp(me, "udevadm")) return udevadm(argc, argv);
    if (argc < 2) return usage();
    mkdirs();
    if (!strcmp(argv[1], "-c")) { int n = 0; for_each_device(cold_one, &n); return n ? 0 : 1; }
    if (!strcmp(argv[1], "-t")) { for_each_device(trig_one, argc > 2 ? argv[2] : "add"); return 0; }
    if (!strcmp(argv[1], "-s")) return settle(argc > 2 ? atoi(argv[2]) : 120);
    if (!strcmp(argv[1], "-q") && argc > 2) return query(argv[2], false);
    if (!strcmp(argv[1], "-d")) {
        if (!(argc > 2 && !strcmp(argv[2], "-f"))) {
            pid_t pid = fork();
            if (pid < 0) return 1;
            if (pid) return 0;
            setsid();
        }
        return daemon_loop();
    }
    return usage();
}
