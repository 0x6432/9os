/*
 * M34: device registry and uevents (see kernel/device.h). The registry is a list under a
 * sleeping mutex (taken after net_mutex: netdev_unregister removes devices with it held);
 * uevents are formatted outside the mutex and broadcast on NETLINK_KOBJECT_UEVENT.
 */
#include <kernel/device.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <stdarg.h>

static const struct lock_class dev_class = { "devices", LR_MUTEX_DEV, false };
static struct mutex dev_mtx = MUTEX_INIT(dev_mtx, &dev_class);
static struct list_node devices = LIST_INIT(devices);
static uint64_t seqnum;

uint64_t uevent_seqnum(void) { return __atomic_load_n(&seqnum, __ATOMIC_RELAXED); }

int dev_printf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : n >= (int)cap ? (int)cap - 1 : n;
}

void uenv_add(struct uenv *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t room = sizeof e->buf - e->len;
    int n = vsnprintf(e->buf + e->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n + 1 > room) return;          /* full: drop the variable */
    e->len += (size_t)n + 1;
    e->n++;
}

static bool is_class(struct device *d) { return d->subsys && !d->bus; }

static void build_path(struct device *d) {
    char *p = d->path;
    size_t cap = sizeof d->path;
    if (d->parent) {
        struct device *pa = d->parent;
        /* class devices below a device of another subsystem sit in a "<class>/" glue dir */
        if (is_class(d) && !(is_class(pa) && !strcmp(pa->subsys, d->subsys)))
            snprintf(p, cap, "%s/%s/%s", pa->path, d->subsys, d->name);
        else snprintf(p, cap, "%s/%s", pa->path, d->name);
    } else if (is_class(d)) snprintf(p, cap, "/devices/virtual/%s/%s", d->subsys, d->name);
    else snprintf(p, cap, "/devices/%s", d->name);
}

void device_uevent_env(struct device *d, struct uenv *e) {
    if (d->devt) {
        uenv_add(e, "MAJOR=%u", (unsigned)MAJOR(d->devt));
        uenv_add(e, "MINOR=%u", (unsigned)MINOR(d->devt));
        if (d->devname[0]) uenv_add(e, "DEVNAME=%s", d->devname);
    }
    if (d->devtype) uenv_add(e, "DEVTYPE=%s", d->devtype);
    if (d->uevent) d->uevent(d, e);
}

static const char *const actions[] = { "add", "remove", "change", "move", "online", "offline", "bind", "unbind" };

int device_uevent(struct device *d, const char *action) {
    bool ok = false;
    for (size_t k = 0; k < ARRAY_SIZE(actions); k++) if (!strcmp(action, actions[k])) ok = true;
    if (!ok) return -EINVAL;
    struct uenv *e = kmalloc(sizeof *e);
    if (!e) return -ENOMEM;
    e->len = 0; e->n = 0;
    uenv_add(e, "%s@%s", action, d->path);
    uenv_add(e, "ACTION=%s", action);
    uenv_add(e, "DEVPATH=%s", d->path);
    if (d->subsys) uenv_add(e, "SUBSYSTEM=%s", d->subsys);
    device_uevent_env(d, e);
    uenv_add(e, "SEQNUM=%lu", __atomic_add_fetch(&seqnum, 1, __ATOMIC_RELAXED));
    netlink_uevent_broadcast(e->buf, e->len);
    kfree(e);
    return 0;
}

void device_add(struct device *d) {
    if (d->added) return;
    build_path(d);
    mutex_lock(&dev_mtx);
    list_add_tail(&devices, &d->node);
    d->added = true;
    mutex_unlock(&dev_mtx);
    device_uevent(d, "add");
}

void device_del(struct device *d) {
    if (!d->added) return;
    mutex_lock(&dev_mtx);
    list_del(&d->node);
    list_init(&d->node);
    d->added = false;
    mutex_unlock(&dev_mtx);
    device_uevent(d, "remove");
}

void device_for_each(void (*cb)(struct device *d, void *ctx), void *ctx) {
    mutex_lock(&dev_mtx);
    list_for_each(it, &devices) cb(list_entry(it, struct device, node), ctx);
    mutex_unlock(&dev_mtx);
}
