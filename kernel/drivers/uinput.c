/*
 * M35: /dev/uinput (misc 10:223). A process describes an input device (UI_SET_*BIT,
 * UI_DEV_SETUP/UI_ABS_SETUP or the legacy struct uinput_user_dev write), creates it with
 * UI_DEV_CREATE (a new /dev/input/eventN, /sys/devices/virtual/input/inputN and "add"
 * uevents) and then writes struct input_event records that are reported as if the hardware
 * produced them. UI_DEV_DESTROY or closing the file hot-unplugs the device. Force feedback
 * and the EV_UINPUT request protocol are not implemented. Calls run under the BKL.
 */
#include <kernel/input.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/printk.h>

#define IOC_NR(c) ((c) & 0xff)
#define IOC_TYPE(c) (((c) >> 8) & 0xff)
#define IOC_SIZE(c) (((c) >> 16) & 0x3fff)
#define UINPUT_VERSION 5
#define UINPUT_MAX_NAME 80
#define ABS_CNT (ABS_MAX + 1)

struct uinput_setup_abi { struct input_id id; char name[UINPUT_MAX_NAME]; uint32_t ff_effects_max; };
struct uinput_abs_setup_abi { uint16_t code; uint16_t pad; struct input_absinfo absinfo; };
struct uinput_user_dev_abi {
    char name[UINPUT_MAX_NAME];
    struct input_id id;
    uint32_t ff_effects_max;
    int32_t absmax[ABS_CNT], absmin[ABS_CNT], absfuzz[ABS_CNT], absflat[ABS_CNT];
};
struct input_event_abi { int64_t sec, usec; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct uinput_setup_abi) == 92, "uinput_setup");
_Static_assert(sizeof(struct uinput_abs_setup_abi) == 28, "uinput_abs_setup");
_Static_assert(sizeof(struct uinput_user_dev_abi) == 1116, "uinput_user_dev");

struct uinput {
    struct input_dev *dev;
    bool created, destroyed;
};

static void uinput_free(struct input_dev *d) { kfree(d); }

static int uinput_open(struct inode *ino, struct file *f) {
    struct uinput *u = kzalloc(sizeof *u);
    if (!u) return -ENOMEM;
    u->dev = kzalloc(sizeof *u->dev);
    if (!u->dev) { kfree(u); return -ENOMEM; }
    f->priv = u;
    return 0;
}

static void destroy(struct uinput *u) {
    if (u->created && !u->destroyed) input_unregister(u->dev);   /* ->free runs later */
    else if (!u->created) kfree(u->dev);
    u->destroyed = true;
}

static void uinput_release(struct file *f) {
    struct uinput *u = f->priv;
    if (!u) return;
    destroy(u);
    kfree(u);
}

static ssize_t uinput_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct uinput *u = f->priv;
    if (u->destroyed) return -ENODEV;
    if (!u->created) {                                   /* legacy setup */
        if (n != sizeof(struct uinput_user_dev_abi)) return -EINVAL;
        struct uinput_user_dev_abi *ud = kmalloc(sizeof *ud);
        if (!ud) return -ENOMEM;
        if (copy_from_user(ud, buf, sizeof *ud)) { kfree(ud); return -EFAULT; }
        struct input_dev *d = u->dev;
        memcpy(d->name, ud->name, sizeof d->name - 1);
        d->name[sizeof d->name - 1] = 0;
        d->id = ud->id;
        for (int i = 0; i < ABS_CNT; i++) {
            d->abs[i].minimum = ud->absmin[i]; d->abs[i].maximum = ud->absmax[i];
            d->abs[i].fuzz = ud->absfuzz[i]; d->abs[i].flat = ud->absflat[i];
        }
        kfree(ud);
        return (ssize_t)n;
    }
    if (n < sizeof(struct input_event_abi)) return -EINVAL;
    size_t done = 0;
    for (; done + sizeof(struct input_event_abi) <= n; done += sizeof(struct input_event_abi)) {
        struct input_event_abi e;
        if (copy_from_user(&e, (const char *)buf + done, sizeof e)) return done ? (ssize_t)done : -EFAULT;
        input_event(u->dev, e.type, e.code, e.value);
    }
    return (ssize_t)done;
}

static ssize_t uinput_read(struct file *f, void *buf, size_t n, off_t *off) {
    return -EAGAIN;                                      /* no EV_UINPUT/force-feedback requests */
}

static unsigned uinput_poll(struct file *f) { return POLLOUT | POLLWRNORM; }

static int set_bit_in(uint64_t *bm, unsigned max, uint64_t bit) {
    if (bit > max) return -EINVAL;
    input_set_bit(bm, (unsigned)bit);
    return 0;
}

static int uinput_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct uinput *u = f->priv;
    struct input_dev *d = u->dev;
    if (IOC_TYPE(cmd) != 'U') return -ENOTTY;
    unsigned nr = IOC_NR(cmd), len = IOC_SIZE(cmd);
    if (nr == 45) { unsigned v = UINPUT_VERSION; return copy_to_user((void *)arg, &v, sizeof v) ? -EFAULT : 0; }
    if (u->destroyed) return -ENODEV;
    if (nr == 44) {                                      /* UI_GET_SYSNAME(len) */
        if (!u->created) return -ENOENT;
        char s[16];
        int l = snprintf(s, sizeof s, "input%d", d->num) + 1;
        if ((unsigned)l > len) return -ENOBUFS;
        return copy_to_user((void *)arg, s, (size_t)l) ? -EFAULT : l;
    }
    if (nr == 2) { if (!u->created) return -EINVAL; destroy(u); return 0; }   /* UI_DEV_DESTROY */
    if (u->created) return -EINVAL;                      /* everything else configures */
    switch (nr) {
    case 1: {                                            /* UI_DEV_CREATE */
        if (!d->name[0]) return -EINVAL;
        d->free = uinput_free;
        int r = input_register(d);
        if (r) return r;
        u->created = true;
        return 0;
    }
    case 3: {                                            /* UI_DEV_SETUP */
        struct uinput_setup_abi st;
        if (copy_from_user(&st, (void *)arg, sizeof st)) return -EFAULT;
        memcpy(d->name, st.name, sizeof d->name - 1);
        d->name[sizeof d->name - 1] = 0;
        d->id = st.id;
        return 0;
    }
    case 4: {                                            /* UI_ABS_SETUP */
        struct uinput_abs_setup_abi a;
        if (copy_from_user(&a, (void *)arg, sizeof a)) return -EFAULT;
        if (a.code > ABS_MAX) return -ERANGE;
        d->abs[a.code] = a.absinfo;
        input_set_bit(d->absbit, a.code);
        return 0;
    }
    case 100: return set_bit_in(d->evbit, EV_MAX, arg);
    case 101: return set_bit_in(d->keybit, KEY_MAX, arg);
    case 102: return set_bit_in(d->relbit, REL_MAX, arg);
    case 103: return set_bit_in(d->absbit, ABS_MAX, arg);
    case 104: return set_bit_in(d->mscbit, MSC_MAX, arg);
    case 105: return set_bit_in(d->ledbit, LED_MAX, arg);
    case 106: case 107: case 109: return 0;              /* SND, FF, SW bits: not modelled */
    case 108: {                                          /* UI_SET_PHYS */
        char p[sizeof d->phys];
        long r = strncpy_from_user(p, (const char *)arg, sizeof p - 1);
        if (r < 0) return -EFAULT;
        p[r] = 0;
        memcpy(d->phys, p, sizeof p);
        return 0;
    }
    case 110: return set_bit_in(d->propbit, INPUT_PROP_MAX, arg);
    }
    return -ENOTTY;
}

static const struct file_ops uinput_fops = {
    .open = uinput_open, .read = uinput_read, .write = uinput_write, .ioctl = uinput_ioctl,
    .poll = uinput_poll, .release = uinput_release,
};

void chrdev_register(unsigned major, unsigned minor, const struct file_ops *ops);
void uinput_init(void) { chrdev_register(10, 223, &uinput_fops); }
