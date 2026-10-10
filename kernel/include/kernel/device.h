#pragma once
/*
 * M34: a minimal driver model behind sysfs and uevents. Drivers embed a struct device, fill in
 * the name, subsystem, parent and (for nodes in /dev) devt/devname, then device_add() it: the
 * device appears under /sys/devices (path derived from the parent chain), /sys/class or
 * /sys/bus, /sys/dev/{char,block} and an "add" uevent goes to NETLINK_KOBJECT_UEVENT
 * listeners. Devices are never freed (device_del only unlinks them), so attribute callbacks
 * may run on a device that is being removed.
 */
#include <kernel/types.h>
#include <kernel/list.h>

struct device;
struct uenv { char buf[2048]; size_t len; int n; };   /* "KEY=value\0" strings */
void uenv_add(struct uenv *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

struct dev_attr {
    const char *name;            /* may contain '/' (capabilities/ev, statistics/rx_bytes) */
    uint16_t mode;               /* 0444 or 0644 */
    int (*show)(struct device *d, char *buf, size_t cap);           /* bytes written */
    int (*store)(struct device *d, const char *buf, size_t n);      /* 0 or -errno */
};

struct device {
    struct list_node node;
    struct device *parent;
    const char *subsys;          /* class or bus name; nullptr: none (pci0000:00) */
    bool bus;                    /* subsys is a bus (/sys/bus/X/devices) rather than a class */
    const char *devtype;         /* DEVTYPE= (disk, partition, drm_minor) */
    char name[32];
    char devname[40];            /* node below /dev ("input/event0"), "" if none */
    uint64_t devt;               /* MKDEV, 0 if none */
    bool blk;                    /* devt is a block device */
    const struct dev_attr *attrs;    /* terminated by a null name */
    void (*uevent)(struct device *d, struct uenv *e);   /* subsystem-specific variables */
    void *priv;
    char path[192];              /* /devices/... (set by device_add) */
    bool added;
};

void device_add(struct device *d);
void device_del(struct device *d);       /* "remove" uevent; may be called under net_mutex */
int device_uevent(struct device *d, const char *action);   /* emit; -EINVAL for a bad action */
void device_uevent_env(struct device *d, struct uenv *e);   /* the variables of the uevent file */
int dev_printf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* registry walk for sysfs (device mutex held: cb must not sleep on other locks) */
void device_for_each(void (*cb)(struct device *d, void *ctx), void *ctx);
uint64_t uevent_seqnum(void);

/* netlink.c: deliver a uevent message to NETLINK_KOBJECT_UEVENT group 1 */
void netlink_uevent_broadcast(const char *msg, size_t len);

struct inode *sysfs_create_root(void);
void chrdev_sysfs(struct device *k, const char *cls, const char *name, const char *devname, unsigned ma, unsigned mi,
                  struct device *parent);
struct device *virtio_gpu_kdev(void);    /* the virtio-gpu device, nullptr if not driven */
