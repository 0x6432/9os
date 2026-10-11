/*
 * M35: /dev/uinput (misc 10:223). A process describes an input device (UI_SET_*BIT,
 * UI_DEV_SETUP/UI_ABS_SETUP or the legacy struct uinput_user_dev write), creates it with
 * UI_DEV_CREATE (a new /dev/input/eventN, /sys/devices/virtual/input/inputN and "add"
 * uevents) and then writes struct input_event records that are reported as if the hardware
 * produced them. UI_DEV_DESTROY or closing the file hot-unplugs the device.
 * M36: LED/SND/FF events that evdev clients write reach the owner through read(); force
 * feedback uploads/erases (EVIOCSFF/EVIOCRMFF) become EV_UINPUT UI_FF_UPLOAD/UI_FF_ERASE
 * requests that the owner answers with UI_BEGIN_/UI_END_FF_UPLOAD/ERASE (the uploader sleeps
 * up to 30 s). Ioctls run under the BKL (dropped while sleeping); the read queue has a lock.
 */
#include <kernel/input.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>

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

struct uinput_ff_upload_abi { uint32_t request_id; int32_t retval; uint8_t effect[FF_EFFECT_SIZE], old[FF_EFFECT_SIZE]; };
struct uinput_ff_erase_abi { uint32_t request_id; int32_t retval; uint32_t effect_id; };
_Static_assert(sizeof(struct uinput_ff_upload_abi) == 104, "uinput_ff_upload");
_Static_assert(sizeof(struct uinput_ff_erase_abi) == 12, "uinput_ff_erase");
#define UI_FF_UPLOAD 1
#define UI_FF_ERASE 2
#define NREQ 16
#define UQLEN 128

struct ureq {
    bool used, done, begun;
    int kind, retval;
    uint32_t effect_id;
    const uint8_t *effect, *old;
};
struct uinput {
    struct input_dev *dev;
    bool created, destroyed;
    uint32_t ff_effects_max;
    spinlock_t qlock;
    struct input_event_abi q[UQLEN];
    unsigned head, tail;
    struct ureq req[NREQ];
    struct wait_queue reqwq;
};

static void queue_event(struct uinput *u, unsigned type, unsigned code, int value) {
    uint64_t ns = time_ns();
    struct input_event_abi e = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull / 1000), (uint16_t)type, (uint16_t)code, value };
    uint64_t fl = spin_lock_irqsave(&u->qlock);
    if (u->tail - u->head < UQLEN) u->q[u->tail++ % UQLEN] = e;
    spin_unlock_irqrestore(&u->qlock, fl);
    poll_notify();
}
static void to_device(struct input_dev *d, unsigned type, unsigned code, int value) { queue_event(d->drv, type, code, value); }

/* post a request to the owner and sleep until UI_END_* (or 30 s, or destroy) */
static int request(struct uinput *u, int kind, const uint8_t *effect, const uint8_t *old, uint32_t effect_id) {
    int id = -1;
    for (int i = 0; i < NREQ && id < 0; i++) if (!u->req[i].used) id = i;
    if (id < 0) return -EBUSY;
    struct ureq *r = &u->req[id];
    *r = (struct ureq){ .used = true, .kind = kind, .effect = effect, .old = old, .effect_id = effect_id };
    queue_event(u, EV_UINPUT, (unsigned)kind, id);
    uint64_t end = time_ns() + 30ull * 1000000000ull;
    while (!r->done && !u->destroyed && time_ns() < end) wait_event_timeout(&u->reqwq, 10000000ull);
    int ret = r->done ? r->retval : u->destroyed ? -ENODEV : -ETIMEDOUT;
    r->used = false;
    return ret;
}
static int ff_upload(struct input_dev *d, const uint8_t *effect, const uint8_t *old) {
    return request(d->drv, UI_FF_UPLOAD, effect, old, 0);
}
static int ff_erase(struct input_dev *d, int id) { return request(d->drv, UI_FF_ERASE, nullptr, nullptr, (uint32_t)id); }

static void uinput_free(struct input_dev *d) { kfree(d); }

static int uinput_open(struct inode *ino, struct file *f) {
    struct uinput *u = kzalloc(sizeof *u);
    if (!u) return -ENOMEM;
    u->dev = kzalloc(sizeof *u->dev);
    if (!u->dev) { kfree(u); return -ENOMEM; }
    wait_queue_init(&u->reqwq);
    f->priv = u;
    return 0;
}

static void destroy(struct uinput *u) {
    bool was = u->destroyed;
    u->destroyed = true;
    wake_up(&u->reqwq);                                  /* pending FF requests fail */
    if (u->created && !was) { u->dev->drv = nullptr; u->dev->to_device = nullptr; u->dev->ff_upload = nullptr;
        u->dev->ff_erase = nullptr; input_unregister(u->dev); }   /* ->free runs later */
    else if (!u->created && !was) kfree(u->dev);
}

static void uinput_release(struct file *f) {
    struct uinput *u = f->priv;
    if (!u) return;
    destroy(u);
    for (int i = 0; i < NREQ; i++)                        /* sleeping uploaders still use u */
        while (u->req[i].used) wait_event_timeout(&u->reqwq, 5000000ull);
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
        u->ff_effects_max = ud->ff_effects_max;
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
    struct uinput *u = f->priv;
    if (n < sizeof(struct input_event_abi)) return -EINVAL;
    for (;;) {
        if (!u->created || u->destroyed) return -ENODEV;
        uint64_t fl = spin_lock_irqsave(&u->qlock);
        if (u->head != u->tail) {
            struct input_event_abi tmp[16];
            size_t cnt = 0, max = n / sizeof tmp[0];
            while (cnt < max && cnt < 16 && u->head != u->tail) tmp[cnt++] = u->q[u->head++ % UQLEN];
            spin_unlock_irqrestore(&u->qlock, fl);
            return copy_to_user(buf, tmp, cnt * sizeof tmp[0]) ? -EFAULT : (ssize_t)(cnt * sizeof tmp[0]);
        }
        spin_unlock_irqrestore(&u->qlock, fl);
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        int r = wait_event_timeout(&poll_wq, 50000000ull);
        if (r && r != -ETIMEDOUT) return r;
    }
}

static unsigned uinput_poll(struct file *f) {
    struct uinput *u = f->priv;
    unsigned m = POLLOUT | POLLWRNORM;
    if (u->created && u->head != u->tail) m |= POLLIN | POLLRDNORM;
    if (u->destroyed) m |= POLLHUP;
    return m;
}

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
    if (nr >= 200 && nr <= 203) {                        /* UI_BEGIN/END_FF_UPLOAD/ERASE */
        if (!u->created) return -EINVAL;
        bool up = nr <= 201, begin = nr == 200 || nr == 202;
        size_t sz = up ? sizeof(struct uinput_ff_upload_abi) : sizeof(struct uinput_ff_erase_abi);
        uint8_t buf[sizeof(struct uinput_ff_upload_abi)];
        if (copy_from_user(buf, (void *)arg, sz)) return -EFAULT;
        uint32_t rid; memcpy(&rid, buf, 4);
        if (rid >= NREQ || !u->req[rid].used || u->req[rid].done || u->req[rid].kind != (up ? UI_FF_UPLOAD : UI_FF_ERASE)) return -EINVAL;
        struct ureq *r = &u->req[rid];
        if (begin) {
            if (up) {
                struct uinput_ff_upload_abi *a = (void *)buf;
                memcpy(a->effect, r->effect, FF_EFFECT_SIZE);
                if (r->old) memcpy(a->old, r->old, FF_EFFECT_SIZE); else memset(a->old, 0, FF_EFFECT_SIZE);
                a->retval = 0;
            } else {
                struct uinput_ff_erase_abi *a = (void *)buf;
                a->effect_id = r->effect_id; a->retval = 0;
            }
            r->begun = true;
            return copy_to_user((void *)arg, buf, sz) ? -EFAULT : 0;
        }
        if (!r->begun) return -EINVAL;
        int32_t rv; memcpy(&rv, buf + 4, 4);
        r->retval = rv;
        r->done = true;
        wake_up(&u->reqwq);
        return 0;
    }
    if (u->created) return -EINVAL;                      /* everything else configures */
    switch (nr) {
    case 1: {                                            /* UI_DEV_CREATE */
        if (!d->name[0]) return -EINVAL;
        if (input_test_bit(d->evbit, EV_FF) && !u->ff_effects_max) return -EINVAL;
        d->ff_max = (int)MIN(u->ff_effects_max, (uint32_t)FF_SLOTS);
        d->drv = u; d->to_device = to_device;
        if (input_test_bit(d->evbit, EV_FF)) { d->ff_upload = ff_upload; d->ff_erase = ff_erase; }
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
        u->ff_effects_max = st.ff_effects_max;
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
    case 106: return set_bit_in(d->sndbit, SND_MAX, arg);
    case 107: return set_bit_in(d->ffbit, FF_MAX, arg);
    case 109: return set_bit_in(d->swbit, SW_MAX, arg);
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
