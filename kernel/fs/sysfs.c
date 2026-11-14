/*
 * M34: sysfs. The tree is computed from the device registry (kernel/device.h) on every lookup
 * and readdir: each registered device contributes its directory under /sys/devices (uevent,
 * dev, attributes, subsystem/device links) plus links from /sys/class/<class>/<name> or
 * /sys/bus/<bus>/devices/<name>, /sys/dev/{char,block}/<major>:<minor> and /sys/block/<disk>.
 * A few fixed directories and global files complete it. Intermediate directories are implied
 * by the entry paths, so "capabilities/ev" or "input/input3/event3" need no explicit dirs.
 * Links are relative, as in Linux. Files are generated at open (attribute show callbacks run
 * without the registry mutex); writing an action word to "uevent" re-sends the event (coldplug).
 */
#include <kernel/device.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/cpu.h>

enum skind { K_DIR, K_ATTR, K_UEVENT, K_DEV, K_LINK, K_GLOBAL, K_POWER, K_BIN };
/* power/ of every device: runtime PM is not implemented, so it reports "unsupported" */
static const char *const power_files[] = { "power/control", "power/runtime_status", "power/async", "power/runtime_enabled",
                                           "power/runtime_active_time", "power/runtime_suspended_time" };
struct snode { int kind; struct device *dev; struct inode *parent; int idx; char path[256]; char target[200]; };

static const char *const fixed_dirs[] = {
    "/devices/virtual", "/devices/system/cpu", "/class/input", "/class/net", "/class/block", "/class/tty",
    "/class/mem", "/class/misc", "/class/graphics", "/class/drm", "/bus/pci/devices", "/bus/pci/drivers",
    "/bus/virtio/devices", "/bus/virtio/drivers", "/dev/char", "/dev/block", "/block", "/kernel", "/firmware",
    "/fs", "/module", "/power",
};
enum { G_SEQNUM, G_CPU_ONLINE, G_CPU_POSSIBLE, G_CPU_PRESENT, G_CPU_KMAX };
static const char *const global_files[] = {
    "/kernel/uevent_seqnum", "/devices/system/cpu/online", "/devices/system/cpu/possible",
    "/devices/system/cpu/present", "/devices/system/cpu/kernel_max",
};

/* ---- the entry generator */
typedef bool (*ent_cb)(void *ctx, const char *path, int kind, struct device *d, int idx, const char *target);
struct gen { ent_cb cb; void *ctx; bool stop; char p[256], t[200]; };

static void yield(struct gen *g, const char *path, int kind, struct device *d, int idx, const char *target) {
    if (!g->stop && !g->cb(g->ctx, path, kind, d, idx, target)) g->stop = true;
}
static bool is_class(struct device *d) { return d->subsys && !d->bus; }

static void gen_device(struct device *d, void *arg) {
    struct gen *g = arg;
    if (g->stop) return;
    yield(g, d->path, K_DIR, d, 0, nullptr);
    snprintf(g->p, sizeof g->p, "%s/uevent", d->path);
    yield(g, g->p, K_UEVENT, d, 0, nullptr);
    if (d->devt) { snprintf(g->p, sizeof g->p, "%s/dev", d->path); yield(g, g->p, K_DEV, d, 0, nullptr); }
    if (d->subsys) {
        snprintf(g->p, sizeof g->p, "%s/subsystem", d->path);
        snprintf(g->t, sizeof g->t, "/%s/%s", d->bus ? "bus" : "class", d->subsys);
        yield(g, g->p, K_LINK, d, 0, g->t);
    }
    if (is_class(d) && d->parent) {
        snprintf(g->p, sizeof g->p, "%s/device", d->path);
        yield(g, g->p, K_LINK, d, 0, d->parent->path);
    }
    for (int k = 0; d->attrs && d->attrs[k].name; k++) {
        snprintf(g->p, sizeof g->p, "%s/%s", d->path, d->attrs[k].name);
        yield(g, g->p, d->attrs[k].read ? K_BIN : K_ATTR, d, k, nullptr);
    }
    for (int k = 0; k < (int)ARRAY_SIZE(power_files); k++) {
        snprintf(g->p, sizeof g->p, "%s/%s", d->path, power_files[k]);
        yield(g, g->p, K_POWER, d, k, nullptr);
    }
    if (d->driver && d->bus) {                       /* driver links and the built-in "module" */
        char mod[32];
        size_t l = 0;
        for (; d->driver[l] && l < sizeof mod - 1; l++) mod[l] = d->driver[l] == '-' ? '_' : d->driver[l];
        mod[l] = 0;
        snprintf(g->p, sizeof g->p, "%s/driver", d->path);
        snprintf(g->t, sizeof g->t, "/bus/%s/drivers/%s", d->subsys, d->driver);
        yield(g, g->p, K_LINK, d, 0, g->t);
        snprintf(g->p, sizeof g->p, "/bus/%s/drivers/%s/%s", d->subsys, d->driver, d->name);
        yield(g, g->p, K_LINK, d, 0, d->path);
        snprintf(g->p, sizeof g->p, "/bus/%s/drivers/%s/module", d->subsys, d->driver);
        snprintf(g->t, sizeof g->t, "/module/%s", mod);
        yield(g, g->p, K_LINK, d, 0, g->t);
        snprintf(g->p, sizeof g->p, "/module/%s/drivers/%s:%s", mod, d->subsys, d->driver);
        snprintf(g->t, sizeof g->t, "/bus/%s/drivers/%s", d->subsys, d->driver);
        yield(g, g->p, K_LINK, d, 0, g->t);
    }
    if (d->subsys) {
        if (d->bus) snprintf(g->p, sizeof g->p, "/bus/%s/devices/%s", d->subsys, d->name);
        else snprintf(g->p, sizeof g->p, "/class/%s/%s", d->subsys, d->name);
        yield(g, g->p, K_LINK, d, 0, d->path);
    }
    if (d->devt) {
        snprintf(g->p, sizeof g->p, "/dev/%s/%u:%u", d->blk ? "block" : "char", (unsigned)MAJOR(d->devt), (unsigned)MINOR(d->devt));
        yield(g, g->p, K_LINK, d, 0, d->path);
    }
    if (d->blk && d->devtype && !strcmp(d->devtype, "disk")) {
        snprintf(g->p, sizeof g->p, "/block/%s", d->name);
        yield(g, g->p, K_LINK, d, 0, d->path);
    }
}

static void generate(ent_cb cb, void *ctx) {
    struct gen *g = kmalloc(sizeof *g);
    if (!g) return;
    g->cb = cb; g->ctx = ctx; g->stop = false;
    for (size_t k = 0; k < ARRAY_SIZE(fixed_dirs); k++) yield(g, fixed_dirs[k], K_DIR, nullptr, 0, nullptr);
    for (size_t k = 0; k < ARRAY_SIZE(global_files); k++) yield(g, global_files[k], K_GLOBAL, nullptr, (int)k, nullptr);
    for (int c = 0; c < ncpus; c++) {
        snprintf(g->p, sizeof g->p, "/devices/system/cpu/cpu%d", c);
        yield(g, g->p, K_DIR, nullptr, 0, nullptr);
    }
    device_for_each(gen_device, g);
    kfree(g);
}

/* ---- inodes */
static const struct inode_ops sys_iops;
static const struct file_ops sys_file_fops, sys_dir_fops;

static uint64_t path_ino(const char *p) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ULL;
    return h | 2;
}
static uint32_t kind_mode(int kind, struct device *d, int idx) {
    switch (kind) {
    case K_DIR: return S_IFDIR | 0755;
    case K_LINK: return S_IFLNK | 0777;
    case K_UEVENT: return S_IFREG | 0644;
    case K_ATTR: case K_BIN: return S_IFREG | (d->attrs[idx].mode ? d->attrs[idx].mode : 0444);
    case K_POWER: return S_IFREG | (idx == 0 ? 0644 : 0444);
    default: return S_IFREG | 0444;
    }
}
static struct inode *snew(int kind, const char *path, struct device *d, int idx, const char *target) {
    struct inode *i = inode_alloc(kind_mode(kind, d, idx));
    if (!i) return nullptr;
    struct snode *s = kzalloc(sizeof *s);
    if (!s) { iput(i); return nullptr; }
    s->kind = kind; s->dev = d; s->idx = idx;           /* d: a reference handed over by the caller */
    strncpy(s->path, path, sizeof s->path - 1);
    if (target) strncpy(s->target, target, sizeof s->target - 1);
    i->priv = s;
    i->iops = &sys_iops;
    i->fops = S_ISDIR(i->mode) ? &sys_dir_fops : &sys_file_fops;
    i->dev = 4;
    i->ino = *path ? path_ino(path) : 1;
    i->uid = i->gid = 0;
    i->nlink = S_ISDIR(i->mode) ? 2 : 1;
    /* The registry regenerates each lookup; no directory entry owns this inode.
     * Keep stat's link count, but release the inode and its device at last iput. */
    i->i_state |= I_EPHEMERAL;
    i->size = kind == K_BIN ? d->attrs[idx].size : S_ISREG(i->mode) ? 4096 : 0;
    return i;
}

struct lk { const char *full; size_t flen; bool found; int kind; struct device *d; int idx; char target[200]; };
static bool lk_cb(void *ctx, const char *path, int kind, struct device *d, int idx, const char *target) {
    struct lk *l = ctx;
    if (strncmp(path, l->full, l->flen)) return true;
    if (!path[l->flen]) {
        l->found = true; l->kind = kind; l->d = d; l->idx = idx;
        if (d) device_get(d);                        /* under the registry mutex: d is alive */
        if (target) strncpy(l->target, target, sizeof l->target - 1);
        return false;
    }
    if (path[l->flen] == '/') { l->found = true; l->kind = K_DIR; l->d = nullptr; return false; }
    return true;
}

static int s_lookup(struct inode *dir, const char *name, struct inode **out) {
    struct snode *ds = dir->priv;
    if (ds->kind != K_DIR) return -ENOTDIR;
    struct lk *l = kzalloc(sizeof *l);
    char *full = kmalloc(512);
    if (!l || !full) { kfree(l); kfree(full); return -ENOMEM; }
    snprintf(full, 512, "%s/%s", ds->path, name);
    l->full = full; l->flen = strlen(full);
    int r = -ENOENT;
    if (l->flen < 255) {
        generate(lk_cb, l);
        if (l->found) {
            struct inode *i = snew(l->kind, full, l->d, l->idx, l->target);
            if (!i) { if (l->d) device_put(l->d); }
            if (!i) r = -ENOMEM;
            else {
                /* "..", getcwd and open directory fds can outlive this lookup. */
                if (S_ISDIR(i->mode)) {
                    iget(dir); i->parent = dir;
                    ((struct snode *)i->priv)->parent = dir;
                }
                *out = i; r = 0;
            }
        }
    }
    kfree(full); kfree(l);
    return r;
}

/* readdir: unique immediate children of the directory, in generation order */
struct it { const char *pre; size_t plen; char *names; size_t len, cap; };
static bool it_cb(void *ctx, const char *path, int kind, struct device *d, int idx, const char *target) {
    struct it *c = ctx;
    if (strncmp(path, c->pre, c->plen) || path[c->plen] != '/') return true;
    const char *nm = path + c->plen + 1, *e = strchr(nm, '/');
    size_t nl = e ? (size_t)(e - nm) : strlen(nm);
    if (!nl) return true;
    for (size_t o = 0; o < c->len; ) {               /* entries: type byte, name, NUL */
        if (strlen(c->names + o + 1) == nl && !strncmp(c->names + o + 1, nm, nl)) return true;
        o += strlen(c->names + o + 1) + 2;
    }
    if (c->len + nl + 2 > c->cap) {
        size_t nc = (c->len + nl + 2) * 2;
        char *n = krealloc(c->names, nc);
        if (!n) return false;
        c->names = n; c->cap = nc;
    }
    c->names[c->len] = (char)(e ? 4 : kind == K_DIR ? 4 : kind == K_LINK ? 10 : 8);
    memcpy(c->names + c->len + 1, nm, nl);
    c->names[c->len + 1 + nl] = 0;
    c->len += nl + 2;
    return true;
}
static int s_iterate(struct inode *dir, uint64_t *pos, filldir_t fill, void *ctx) {
    struct snode *ds = dir->priv;
    struct it c = { ds->path, strlen(ds->path), nullptr, 0, 0 };
    generate(it_cb, &c);
    uint64_t idx = 0;
    char p[512];
    const char *n[2] = { ".", ".." };
    for (int k = 0; k < 2; k++, idx++) {
        if (idx < *pos) continue;
        uint64_t ino = k ? (dir->parent ? dir->parent->ino : dir->ino) : dir->ino;
        if (fill(ctx, n[k], strlen(n[k]), ino, 4, idx + 1)) goto out;
        (*pos)++;
    }
    for (size_t o = 0; o < c.len; idx++) {
        const char *nm = c.names + o + 1;
        size_t nl = strlen(nm);
        if (idx >= *pos) {
            snprintf(p, sizeof p, "%s/%s", ds->path, nm);
            if (fill(ctx, nm, nl, path_ino(p), (unsigned char)c.names[o], idx + 1)) break;
            (*pos)++;
        }
        o += nl + 2;
    }
out:
    kfree(c.names);
    return 0;
}

static int s_readlink(struct inode *i, char *buf, size_t size) {
    struct snode *s = i->priv;
    if (s->kind != K_LINK) return -EINVAL;
    char tmp[512];
    size_t n = 0;
    int depth = 0;                                   /* directories of the link's own dir */
    const char *last = strrchr(s->path, '/');
    for (const char *c = s->path; c < last; c++) if (*c == '/') depth++;
    for (int k = 0; k < depth && n + 3 < sizeof tmp; k++) { memcpy(tmp + n, "../", 3); n += 3; }
    n += (size_t)snprintf(tmp + n, sizeof tmp - n, "%s", s->target + 1);
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    size_t l = MIN(n, size);
    memcpy(buf, tmp, l);
    return (int)l;
}
static void s_evict(struct inode *i) {
    struct snode *s = i->priv;
    if (s && s->dev) device_put(s->dev);
    if (s) iput(s->parent);
    kfree(s);
}

/* ---- files */
struct sbuf { char *data; size_t len; };
static int gen_file(struct snode *s, char *b, size_t cap) {
    struct device *d = s->dev;
    switch (s->kind) {
    case K_ATTR: return d->attrs[s->idx].show ? d->attrs[s->idx].show(d, b, cap) : 0;
    case K_DEV: return dev_printf(b, cap, "%u:%u\n", (unsigned)MAJOR(d->devt), (unsigned)MINOR(d->devt));
    case K_POWER: {
        /* as Linux without runtime PM in the driver: PCI devices start "on" (pci_pm_init forbids
         * runtime PM), others "auto"; runtime_enabled reflects the forbidden state */
        bool on = d->pm_set ? d->pm_on : (d->bus && d->subsys && !strcmp(d->subsys, "pci"));
        static const char *const v[] = { "on", "unsupported", "disabled", "disabled", "0", "0" };
        if (s->idx == 0) return dev_printf(b, cap, "%s\n", on ? "on" : "auto");
        if (s->idx == 3) return dev_printf(b, cap, "%s\n", on ? "disabled & forbidden" : "disabled");
        return dev_printf(b, cap, "%s\n", v[s->idx]);
    }
    case K_UEVENT: {
        struct uenv *e = kmalloc(sizeof *e);
        if (!e) return 0;
        e->len = 0; e->n = 0;
        device_uevent_env(d, e);
        size_t n = MIN(e->len, cap);
        memcpy(b, e->buf, n);
        for (size_t k = 0; k < n; k++) if (!b[k]) b[k] = '\n';
        kfree(e);
        return (int)n;
    }
    case K_GLOBAL:
        switch (s->idx) {
        case G_SEQNUM: return dev_printf(b, cap, "%lu\n", uevent_seqnum());
        case G_CPU_KMAX: return dev_printf(b, cap, "%d\n", MAX_CPUS - 1);
        case G_CPU_POSSIBLE: case G_CPU_PRESENT: case G_CPU_ONLINE:
            return ncpus > 1 ? dev_printf(b, cap, "0-%d\n", ncpus - 1) : dev_printf(b, cap, "0\n");
        }
    }
    return 0;
}
static int sf_open(struct inode *i, struct file *f) {
    if (((struct snode *)i->priv)->kind == K_BIN) { f->priv = nullptr; return 0; }
    struct sbuf *b = kzalloc(sizeof *b);
    if (!b) return -ENOMEM;
    b->data = kmalloc(4096);
    if (!b->data) { kfree(b); return -ENOMEM; }
    b->len = (size_t)gen_file(i->priv, b->data, 4096);
    f->priv = b;
    return 0;
}
static ssize_t sf_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct snode *s = f->inode->priv;
    if (s->kind == K_BIN) {
        const struct dev_attr *a = &s->dev->attrs[s->idx];
        if (*off < 0 || (uint64_t)*off >= a->size) return 0;
        n = MIN(n, a->size - (size_t)*off);
        ssize_t r = a->read(s->dev, buf, n, (uint64_t)*off);
        if (r > 0) *off += r;
        return r;
    }
    struct sbuf *b = f->priv;
    if (!b || *off >= (off_t)b->len) return 0;
    n = MIN(n, b->len - (size_t)*off);
    memcpy(buf, b->data + *off, n);
    *off += n;
    return (ssize_t)n;
}
static ssize_t sf_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct snode *s = f->inode->priv;
    char t[128];
    if (n >= sizeof t) return -EINVAL;
    memcpy(t, buf, n);
    t[n] = 0;
    if (s->kind == K_UEVENT) {
        if (!s->dev->added) return -ENODEV;
        size_t l = 0;
        while (t[l] && t[l] != ' ' && t[l] != '\n' && t[l] != '\t') l++;
        t[l] = 0;                                    /* "add [UUID [VAR=x ...]]": the action only */
        int r = device_uevent(s->dev, t);
        if (r) return r;
    } else if (s->kind == K_POWER && s->idx == 0) {
        if (strncmp(t, "on", 2) && strncmp(t, "auto", 4)) return -EINVAL;
        s->dev->pm_on = t[0] == 'o';
        s->dev->pm_set = true;
    } else if (s->kind == K_ATTR && s->dev->attrs[s->idx].store) {
        int r = s->dev->attrs[s->idx].store(s->dev, t, n);
        if (r) return r;
    } else return -EIO;
    *off += n;
    return (ssize_t)n;
}
static void sf_release(struct file *f) {
    struct sbuf *b = f->priv;
    if (b) { kfree(b->data); kfree(b); }
}
static unsigned sf_poll(struct file *f) { return POLLIN | POLLRDNORM | POLLPRI; }

static const struct inode_ops sys_iops = { .lookup = s_lookup, .readlink = s_readlink, .iterate = s_iterate, .evict = s_evict };
static const struct file_ops sys_file_fops = { .open = sf_open, .read = sf_read, .write = sf_write, .release = sf_release, .poll = sf_poll };
static const struct file_ops sys_dir_fops = { .poll = sf_poll };

bool sysfs_inode(struct inode *i) { return i && i->iops == &sys_iops; }
struct inode *sysfs_create_root(void) {
    struct inode *i = snew(K_DIR, "", nullptr, 0, nullptr);
    return i;
}
