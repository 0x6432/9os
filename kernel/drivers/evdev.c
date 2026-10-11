/*
 * Input core and evdev. Drivers (PS/2, virtio-input) register a struct input_dev and report
 * events; every open of /dev/input/eventN (char 13:64+N) gets its own event queue in the
 * Linux struct input_event layout. Keyboards also feed the console tty (US layout) unless a
 * client grabbed the device (EVIOCGRAB) or the console keyboard is in K_OFF mode.
 */
#include <kernel/input.h>
#include <kernel/vt.h>
#include <kernel/vfs.h>
#include <kernel/tty.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/arch.h>

#define EVDEV_MAJOR 13
#define EVDEV_MINOR_BASE 64
#define MAX_INPUT 16
#define QLEN 512

struct input_event_abi { int64_t sec, usec; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct input_event_abi) == 24, "input_event layout");

struct evdev_client {
    struct list_node node;
    struct input_dev *dev;
    struct input_event_abi q[QLEN];
    unsigned head, tail;            /* tail - head = queued */
    int clockid;                    /* 0 realtime, 1 monotonic */
    uint64_t revoked;
};

static struct input_dev *devs[MAX_INPUT];
static int ndevs, next_num;
static spinlock_t devs_lock;        /* devs[] slots; evdev_open vs. (un)register */

int input_device_count(void) { return ndevs; }

/* ---------------------------------------------------------------- console keyboard */
static const char normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
};
static const char shifted[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
};

static void console_key(struct input_dev *d, unsigned code, int value) {
    bool down = value != 0;
    int kbmode = vt_kbmode();
    struct tty *tty = vt_fg_tty();
    if ((kbmode == 0 || kbmode == 2) && code < 128 && value != 2) {   /* K_RAW, K_MEDIUMRAW: keycodes */
        tty_input(tty, (char)(code | (down ? 0 : 0x80)));
    }
    switch (code) {
    case 42: case 54: d->shift = down; return;            /* KEY_LEFTSHIFT, KEY_RIGHTSHIFT */
    case 29: case 97: d->ctrl = down; return;             /* KEY_LEFTCTRL, KEY_RIGHTCTRL */
    case 56: case 100: d->alt = down; return;             /* KEY_LEFTALT, KEY_RIGHTALT */
    case 58: if (value == 1) d->caps = !d->caps; return;  /* KEY_CAPSLOCK */
    }
    if (!down || kbmode == 4 /* K_OFF */ || kbmode == 0 || kbmode == 2) return;
    if (d->alt && ((code >= 59 && code <= 68) || code == 87 || code == 88)) {   /* (Ctrl+)Alt+Fn */
        vt_kbd_switch(code >= 87 ? code - 87 + 10 : code - 59);
        return;
    }
    const char *seq = nullptr;
    switch (code) {
    case 103: seq = "\x1b[A"; break;  case 108: seq = "\x1b[B"; break;
    case 106: seq = "\x1b[C"; break;  case 105: seq = "\x1b[D"; break;
    case 102: seq = "\x1b[H"; break;  case 107: seq = "\x1b[F"; break;
    case 111: seq = "\x1b[3~"; break; case 110: seq = "\x1b[2~"; break;
    case 104: seq = "\x1b[5~"; break; case 109: seq = "\x1b[6~"; break;
    case 96: seq = "\n"; break;       case 98: seq = "/"; break;
    case 59: seq = "\x1bOP"; break;   case 60: seq = "\x1bOQ"; break;
    case 61: seq = "\x1bOR"; break;   case 62: seq = "\x1bOS"; break;
    }
    if (seq) { tty_input_str(tty, seq); return; }
    if (code >= 128) return;
    char c = d->shift ? shifted[code] : normal[code];
    if (!c) return;
    if (d->caps && c >= 'a' && c <= 'z' && !d->shift) c -= 32;
    else if (d->caps && c >= 'A' && c <= 'Z' && d->shift) c += 32;
    if (c == '\b') c = 0x7f;
    if (d->ctrl) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        else if (c == '[') c = 27;
        else if (c == '\\') c = 28;
        else if (c == ' ' || c == '@') c = 0;
    }
    if (d->alt) tty_input(tty, 27);
    tty_input(tty, c);
}

/* ---------------------------------------------------------------- event delivery */
static void client_push(struct evdev_client *c, unsigned type, unsigned code, int value, uint64_t mono) {
    uint64_t ns = c->clockid == 0 ? mono + (uint64_t)boot_epoch * 1000000000ull : mono;
    struct input_event_abi e = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull / 1000), type, code, value };
    if (c->tail - c->head >= QLEN) {           /* overflow: drop the queue, tell the client */
        c->head = c->tail;
        e.type = EV_SYN; e.code = SYN_DROPPED; e.value = 0;
    }
    c->q[c->tail++ % QLEN] = e;
}

void input_event(struct input_dev *d, unsigned type, unsigned code, int value) {
    if (type > EV_MAX || !input_test_bit(d->evbit, type)) return;
    if (type == EV_KEY) {
        if (code > KEY_MAX) return;
        bool was = input_test_bit(d->keystate, code);
        if (value && was) value = 2;                       /* autorepeat */
        if (!value && !was) return;
        if (value) d->keystate[code / 64] |= 1ull << (code % 64);
        else d->keystate[code / 64] &= ~(1ull << (code % 64));
    } else if (type == EV_ABS && code <= ABS_MAX) {
        d->abs[code].value = value;
    } else if (type == EV_SW) {                            /* only changes are reported */
        if (code > SW_MAX || !input_test_bit(d->swbit, code)) return;
        bool was = input_test_bit(d->swstate, code);
        if (!!value == was) return;
        if (value) d->swstate[code / 64] |= 1ull << (code % 64);
        else d->swstate[code / 64] &= ~(1ull << (code % 64));
    } else if (type == EV_SND) {
        if (code > SND_MAX || !input_test_bit(d->sndbit, code)) return;
        if (value) d->sndstate[0] |= 1ull << code; else d->sndstate[0] &= ~(1ull << code);
    } else if (type == EV_FF) {
        return;                                            /* force feedback goes to the device only */
    }
    uint64_t mono = time_ns();
    uint64_t fl = spin_lock_irqsave(&d->lock);
    if (type == EV_SYN && code == SYN_REPORT) {            /* like Linux: no empty frames */
        if (!d->npending) { spin_unlock_irqrestore(&d->lock, fl); return; }
        d->npending = 0;
    } else d->npending++;
    bool grabbed = d->grab != nullptr;
    list_for_each(it, &d->clients) {
        struct evdev_client *c = list_entry(it, struct evdev_client, node);
        if (c->revoked || (grabbed && d->grab != c)) continue;
        client_push(c, type, code, value, mono);
    }
    spin_unlock_irqrestore(&d->lock, fl);
    if (type == EV_KEY && d->console_keys && !grabbed) console_key(d, code, value);
    if (type == EV_SYN) poll_notify();
}

/* ---------------------------------------------------------------- M34: sysfs */
static struct input_dev *idev_of(struct device *k) { return k->priv; }
/* Linux bitmap format: hex longs, most significant first, leading zero longs omitted */
static int fmt_bits(char *b, size_t cap, const uint64_t *bm, unsigned nbits) {
    int words = (int)BITS_TO_U64(nbits), top = words - 1, n = 0;
    while (top > 0 && !bm[top]) top--;
    for (int w = top; w >= 0; w--)
        n += dev_printf(b + n, cap - (size_t)n, w == top ? "%lx" : " %lx", bm[w]);
    return n;
}
static const uint64_t *cap_map(struct input_dev *d, int which, unsigned *nbits) {
    static const uint64_t zero[1];
    switch (which) {
    case 0: *nbits = EV_MAX; return d->evbit;
    case 1: *nbits = KEY_MAX; return d->keybit;
    case 2: *nbits = REL_MAX; return d->relbit;
    case 3: *nbits = ABS_MAX; return d->absbit;
    case 4: *nbits = MSC_MAX; return d->mscbit;
    case 5: *nbits = LED_MAX; return d->ledbit;
    case 6: *nbits = SW_MAX; return d->swbit;
    case 7: *nbits = FF_MAX; return d->ffbit;
    case 8: *nbits = INPUT_PROP_MAX; return d->propbit;
    case 9: *nbits = SND_MAX; return d->sndbit;
    default: *nbits = 1; return zero;
    }
}
static int show_cap(struct device *k, char *b, size_t n, int which) {
    unsigned nb;
    const uint64_t *bm = cap_map(idev_of(k), which, &nb);
    int l = fmt_bits(b, n, bm, nb);
    return l + dev_printf(b + l, n - (size_t)l, "\n");
}
#define CAPF(nm, w) static int a_cap_##nm(struct device *k, char *b, size_t n) { return show_cap(k, b, n, w); }
CAPF(ev, 0) CAPF(key, 1) CAPF(rel, 2) CAPF(abs, 3) CAPF(msc, 4) CAPF(led, 5) CAPF(sw, 6) CAPF(ff, 7) CAPF(prop, 8) CAPF(snd, 9)
static int a_name(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%s\n", idev_of(k)->name); }
static int a_phys(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%s\n", idev_of(k)->phys); }
static int a_uniq(struct device *k, char *b, size_t n) { return dev_printf(b, n, "\n"); }
static int a_bustype(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%04x\n", idev_of(k)->id.bustype); }
static int a_vendor(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%04x\n", idev_of(k)->id.vendor); }
static int a_product(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%04x\n", idev_of(k)->id.product); }
static int a_version(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%04x\n", idev_of(k)->id.version); }
/* input:b0011v0001p0001eAB41-e0,1,4,11,14,k1,2,...,ramlsfw */
static int modalias(struct input_dev *d, char *b, size_t cap) {
    int n = dev_printf(b, cap, "input:b%04Xv%04Xp%04Xe%04X-", d->id.bustype, d->id.vendor, d->id.product, d->id.version);
    static const char letters[] = "ekramlsfw";
    static const int which[] = { 0, 1, 2, 3, 4, 5, 9, 7, 6 };     /* ...l(ed) s(nd) f(f) w(sw) */
    for (int s = 0; s < 9; s++) {
        n += dev_printf(b + n, cap - (size_t)n, "%c", letters[s]);
        unsigned nb;
        const uint64_t *bm = cap_map(d, which[s], &nb);
        for (unsigned i = 0; i <= nb && (size_t)n + 8 < cap; i++)
            if (input_test_bit(bm, i)) n += dev_printf(b + n, cap - (size_t)n, "%X,", i);
    }
    return n;
}
static int a_modalias(struct device *k, char *b, size_t n) {
    int l = modalias(idev_of(k), b, n - 1);
    return l + dev_printf(b + l, n - (size_t)l, "\n");
}
static const struct dev_attr input_attrs[] = {
    { "name", 0444, a_name, nullptr }, { "phys", 0444, a_phys, nullptr }, { "uniq", 0444, a_uniq, nullptr },
    { "properties", 0444, a_cap_prop, nullptr }, { "modalias", 0444, a_modalias, nullptr },
    { "id/bustype", 0444, a_bustype, nullptr }, { "id/vendor", 0444, a_vendor, nullptr },
    { "id/product", 0444, a_product, nullptr }, { "id/version", 0444, a_version, nullptr },
    { "capabilities/ev", 0444, a_cap_ev, nullptr }, { "capabilities/key", 0444, a_cap_key, nullptr },
    { "capabilities/rel", 0444, a_cap_rel, nullptr }, { "capabilities/abs", 0444, a_cap_abs, nullptr },
    { "capabilities/msc", 0444, a_cap_msc, nullptr }, { "capabilities/led", 0444, a_cap_led, nullptr },
    { "capabilities/sw", 0444, a_cap_sw, nullptr }, { "capabilities/ff", 0444, a_cap_ff, nullptr },
    { "capabilities/snd", 0444, a_cap_snd, nullptr },
    { nullptr, 0, nullptr, nullptr },
};
static void input_uevent(struct device *k, struct uenv *e) {
    struct input_dev *d = idev_of(k);
    uenv_add(e, "PRODUCT=%x/%x/%x/%x", d->id.bustype, d->id.vendor, d->id.product, d->id.version);
    uenv_add(e, "NAME=\"%s\"", d->name);
    if (d->phys[0]) uenv_add(e, "PHYS=\"%s\"", d->phys);
    static const char *const keys[] = { "EV", "KEY", "REL", "ABS", "MSC", "LED", "SND", "FF", "SW" };
    static const int map[] = { 0, 1, 2, 3, 4, 5, 9, 7, 6 };
    char *b = kmalloc(1024);
    if (!b) return;
    unsigned nb;
    fmt_bits(b, 1024, cap_map(d, 8, &nb), nb);
    uenv_add(e, "PROP=%s", b);
    for (int w = 0; w < 9; w++) {
        const uint64_t *bm = cap_map(d, map[w], &nb);
        bool any = false;
        for (unsigned i = 0; i < BITS_TO_U64(nb); i++) if (bm[i]) any = true;
        if (!any && w) continue;
        fmt_bits(b, 1024, bm, nb);
        uenv_add(e, "%s=%s", keys[w], b);
    }
    modalias(d, b, 1024);
    uenv_add(e, "MODALIAS=%s", b);
    kfree(b);
}

static void idev_release(struct device *k) {
    struct input_dev *d = idev_of(k);
    if (d->free) d->free(d);
}
static void edev_release(struct device *k) { device_put(&idev_of(k)->idev); }

int input_register(struct input_dev *d) {
    list_init(&d->clients);
    d->lock = (spinlock_t){ 0 };
    input_set_bit(d->evbit, EV_SYN);
    if (!d->rep[0]) { d->rep[0] = 250; d->rep[1] = 33; }
    uint64_t fl = spin_lock_irqsave(&devs_lock);
    int idx = -1;
    for (int i = 0; i < MAX_INPUT && idx < 0; i++) if (!devs[i]) idx = i;
    if (idx >= 0) { devs[idx] = d; ndevs++; d->index = idx; d->num = next_num++; d->gone = false; }
    spin_unlock_irqrestore(&devs_lock, fl);
    if (idx < 0) return -ENOSPC;
    char path[32];
    snprintf(path, sizeof path, "/dev/input/event%d", d->index);
    vfs_mkdir_at(nullptr, "/dev/input", 0755);
    vfs_unlink_at(nullptr, path, false);                   /* a stale node from an old owner */
    vfs_mknod_at(nullptr, path, S_IFCHR | 0660, MKDEV(EVDEV_MAJOR, EVDEV_MINOR_BASE + d->index));
    pr_info("input: %s as %s\n", d->name, path);
    struct device *k = &d->idev;
    snprintf(k->name, sizeof k->name, "input%d", d->num);
    k->parent = d->parent; k->subsys = "input"; k->attrs = input_attrs; k->uevent = input_uevent; k->priv = d;
    k->release = idev_release;
    device_add(k);
    k = &d->edev;
    snprintf(k->name, sizeof k->name, "event%d", d->index);
    snprintf(k->devname, sizeof k->devname, "input/event%d", d->index);
    k->parent = &d->idev; k->subsys = "input"; k->devt = MKDEV(EVDEV_MAJOR, EVDEV_MINOR_BASE + d->index); k->priv = d;
    k->release = edev_release;
    device_get(&d->idev);                                  /* eventN pins inputN */
    device_add(k);
    return 0;
}

/* hot-unplug (uinput): readers see ENODEV/POLLHUP, the node and sysfs entries go away, the
 * struct is freed (->free) when the last open file and sysfs inode let go */
void input_unregister(struct input_dev *d) {
    uint64_t fl = spin_lock_irqsave(&devs_lock);
    if (devs[d->index] == d) { devs[d->index] = nullptr; ndevs--; }
    spin_unlock_irqrestore(&devs_lock, fl);
    fl = spin_lock_irqsave(&d->lock);
    d->gone = true;
    d->grab = nullptr;
    spin_unlock_irqrestore(&d->lock, fl);
    poll_notify();
    char path[32];
    snprintf(path, sizeof path, "/dev/input/event%d", d->index);
    vfs_unlink_at(nullptr, path, false);
    pr_info("input: %s removed\n", d->name);
    device_del(&d->edev);
    device_del(&d->idev);
    device_put(&d->edev);
    device_put(&d->idev);
}

/* ---------------------------------------------------------------- /dev/input/eventN */
static void ff_erase_slot(struct input_dev *d, int id);
static int evdev_open(struct inode *ino, struct file *f) {
    int idx = (int)MINOR(ino->rdev) - EVDEV_MINOR_BASE;
    if (idx < 0 || idx >= MAX_INPUT) return -ENODEV;
    struct evdev_client *c = kzalloc(sizeof *c);
    if (!c) return -ENOMEM;
    uint64_t dl = spin_lock_irqsave(&devs_lock);
    c->dev = devs[idx];
    if (c->dev) device_get(&c->dev->idev);
    spin_unlock_irqrestore(&devs_lock, dl);
    if (!c->dev) { kfree(c); return -ENODEV; }
    uint64_t fl = spin_lock_irqsave(&c->dev->lock);
    list_add_tail(&c->dev->clients, &c->node);
    spin_unlock_irqrestore(&c->dev->lock, fl);
    f->priv = c;
    return 0;
}

static void evdev_release(struct file *f) {
    struct evdev_client *c = f->priv;
    if (!c) return;
    uint64_t fl = spin_lock_irqsave(&c->dev->lock);
    list_del(&c->node);
    if (c->dev->grab == c) c->dev->grab = nullptr;
    spin_unlock_irqrestore(&c->dev->lock, fl);
    for (int i = 0; i < FF_SLOTS; i++) if (c->dev->ff_owner[i] == c) ff_erase_slot(c->dev, i);
    device_put(&c->dev->idev);
    kfree(c);
}

static ssize_t evdev_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct evdev_client *c = f->priv;
    if (n < sizeof(struct input_event_abi)) return -EINVAL;
    for (;;) {
        if (c->revoked || c->dev->gone) return -ENODEV;
        uint64_t fl = spin_lock_irqsave(&c->dev->lock);
        if (c->tail != c->head) {
            size_t cnt = 0, max = n / sizeof(struct input_event_abi);
            struct input_event_abi tmp[32];
            while (cnt < max && cnt < 32 && c->head != c->tail) tmp[cnt++] = c->q[c->head++ % QLEN];
            spin_unlock_irqrestore(&c->dev->lock, fl);
            if (copy_to_user(buf, tmp, cnt * sizeof tmp[0])) return -EFAULT;
            return cnt * sizeof tmp[0];
        }
        if (f->flags & O_NONBLOCK) { spin_unlock_irqrestore(&c->dev->lock, fl); return -EAGAIN; }
        spin_unlock(&c->dev->lock);           /* interrupts stay off until we are on the queue */
        int r = wait_event_timeout(&poll_wq, 50000000ull);   /* bounded: no lost wakeup across CPUs */
        if (r == -ETIMEDOUT) r = 0;
        arch_irq_restore(fl);
        if (r) return r;
    }
}

/* as Linux's evdev_write: events are injected into the device (readers see them), LED/SND/FF
 * also go to the hardware (uinput's owner); EV_FF plays an uploaded effect or sets the gain */
static ssize_t evdev_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct evdev_client *c = f->priv;
    struct input_dev *d = c->dev;
    if (c->revoked || d->gone) return -ENODEV;
    if (n < sizeof(struct input_event_abi)) return -EINVAL;
    size_t done = 0;
    for (; done + sizeof(struct input_event_abi) <= n; done += sizeof(struct input_event_abi)) {
        struct input_event_abi e;
        if (copy_from_user(&e, (const char *)buf + done, sizeof e)) return done ? (ssize_t)done : -EFAULT;
        if (e.type > EV_MAX || !input_test_bit(d->evbit, e.type)) continue;
        if (e.type == EV_LED && e.code <= LED_MAX) {
            /* atomic: evdev runs without the BKL (LED_MAX < 64, so word 0 only) */
            if (e.value) __atomic_fetch_or(&d->ledstate[0], 1ull << e.code, __ATOMIC_RELAXED);
            else __atomic_fetch_and(&d->ledstate[0], ~(1ull << e.code), __ATOMIC_RELAXED);
        }
        if (e.type == EV_FF) {
            bool ok = e.code == FF_GAIN || e.code == FF_AUTOCENTER ? input_test_bit(d->ffbit, e.code)
                    : e.code < (unsigned)d->ff_max && e.code < FF_SLOTS && d->ff_owner[e.code];
            if (ok && d->to_device) d->to_device(d, e.type, e.code, e.value);
            continue;
        }
        if ((e.type == EV_LED || e.type == EV_SND) && d->to_device) d->to_device(d, e.type, e.code, e.value);
        input_event(d, e.type, e.code, e.value);
    }
    return (ssize_t)done;
}

static unsigned evdev_poll(struct file *f) {
    struct evdev_client *c = f->priv;
    if (c->revoked || c->dev->gone) return POLLERR | POLLHUP;
    return (c->tail != c->head ? POLLIN | POLLRDNORM : 0) | POLLOUT | POLLWRNORM;
}

static int put_bits(uint64_t arg, size_t len, const void *bits, size_t size) {
    size_t n = MIN(len, size);
    if (copy_to_user((void *)arg, bits, n)) return -EFAULT;
    return (int)n;
}

#define IOC_NR(c) ((c) & 0xff)
#define IOC_TYPE(c) (((c) >> 8) & 0xff)
#define IOC_SIZE(c) (((c) >> 16) & 0x3fff)
#define IOC_DIR(c) (((c) >> 30) & 3)

static int evdev_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct evdev_client *c = f->priv;
    struct input_dev *d = c->dev;
    if (IOC_TYPE(cmd) != 'E') return -ENOTTY;
    unsigned nr = IOC_NR(cmd), len = IOC_SIZE(cmd), dir = IOC_DIR(cmd);
    if (c->revoked || d->gone) return -ENODEV;
    switch (nr) {
    case 0x01: { int v = 0x010001; return copy_to_user((void *)arg, &v, sizeof v); }     /* GVERSION */
    case 0x02: return copy_to_user((void *)arg, &d->id, sizeof d->id);                   /* GID */
    case 0x03:                                                                            /* G/SREP */
        if (dir == 2) return copy_to_user((void *)arg, d->rep, sizeof d->rep);
        return copy_from_user(d->rep, (void *)arg, sizeof d->rep);
    case 0x06: return put_bits(arg, len, d->name, strlen(d->name) + 1);                 /* GNAME */
    case 0x07: return put_bits(arg, len, d->phys, strlen(d->phys) + 1);                 /* GPHYS */
    case 0x08: return put_bits(arg, len, "", 1);                                         /* GUNIQ */
    case 0x09: return put_bits(arg, len, d->propbit, sizeof d->propbit);                 /* GPROP */
    case 0x18: return put_bits(arg, len, d->keystate, sizeof d->keystate);               /* GKEY */
    case 0x19: return put_bits(arg, len, d->ledstate, sizeof d->ledstate);               /* GLED */
    case 0x1a: return put_bits(arg, len, d->sndstate, sizeof d->sndstate);               /* GSND */
    case 0x1b: return put_bits(arg, len, d->swstate, sizeof d->swstate);                 /* GSW */
    case 0x84: { int v = input_test_bit(d->evbit, EV_FF) ? d->ff_max : 0;                /* GEFFECTS */
        return copy_to_user((void *)arg, &v, sizeof v); }
    case 0x80: {                                                                          /* SFF */
        if (!input_test_bit(d->evbit, EV_FF) || !d->ff_upload) return -EINVAL;
        if (len != FF_EFFECT_SIZE) return -EINVAL;
        uint8_t e[FF_EFFECT_SIZE], old[FF_EFFECT_SIZE];
        if (copy_from_user(e, (void *)arg, sizeof e)) return -EFAULT;
        uint16_t type; int16_t id;
        memcpy(&type, e, 2); memcpy(&id, e + 2, 2);
        if (type > FF_MAX || !input_test_bit(d->ffbit, type) || type >= FF_GAIN) return -EINVAL;
        int lim = MIN(d->ff_max, FF_SLOTS);
        bool had = false;
        if (id == -1) {
            for (int i = 0; i < lim && id < 0; i++) if (!d->ff_owner[i]) id = (int16_t)i;
            if (id < 0) return -ENOSPC;
            memcpy(e + 2, &id, 2);
        } else {
            if (id < 0 || id >= lim || !d->ff_owner[id]) return -EINVAL;
            if (d->ff_owner[id] != c) return -EACCES;
            memcpy(old, d->ff_effect[id], sizeof old);
            had = true;
        }
        d->ff_owner[id] = c;                       /* reserve while the driver works */
        int r = d->ff_upload(d, e, had ? old : nullptr);
        if (r) { if (!had) d->ff_owner[id] = nullptr; return r; }
        memcpy(d->ff_effect[id], e, sizeof e);
        return copy_to_user((uint8_t *)arg + 2, &id, 2) ? -EFAULT : 0;
    }
    case 0x81: {                                                                          /* RMFF */
        int id = (int)arg;
        if (id < 0 || id >= MIN(d->ff_max, FF_SLOTS) || !d->ff_owner[id]) return -EINVAL;
        if (d->ff_owner[id] != c) return -EACCES;
        ff_erase_slot(d, id);
        return 0;
    }
    case 0x90: {                                                                          /* GRAB */
        uint64_t fl = spin_lock_irqsave(&d->lock);
        int r = 0;
        if (arg) { if (d->grab && d->grab != c) r = -EBUSY; else d->grab = c; }
        else { if (d->grab != c) r = -EINVAL; else d->grab = nullptr; }
        spin_unlock_irqrestore(&d->lock, fl);
        return r;
    }
    case 0x91:                                                                            /* REVOKE */
        if (arg) return -EINVAL;
        c->revoked = 1;
        if (d->grab == c) d->grab = nullptr;
        poll_notify();
        return 0;
    case 0x92: case 0x93: return 0;                                                      /* G/SMASK */
    case 0xa0: {                                                                          /* SCLOCKID */
        int id;
        if (copy_from_user(&id, (void *)arg, sizeof id)) return -EFAULT;
        if (id != 0 && id != 1 && id != 7) return -EINVAL;   /* REALTIME, MONOTONIC, BOOTTIME */
        c->clockid = id == 0 ? 0 : 1;
        return 0;
    }
    }
    if (nr >= 0x20 && nr < 0x40 && dir == 2) {                                           /* GBIT */
        unsigned ev = nr - 0x20;
        switch (ev) {
        case 0: return put_bits(arg, len, d->evbit, sizeof d->evbit);
        case EV_KEY: return put_bits(arg, len, d->keybit, sizeof d->keybit);
        case EV_REL: return put_bits(arg, len, d->relbit, sizeof d->relbit);
        case EV_ABS: return put_bits(arg, len, d->absbit, sizeof d->absbit);
        case EV_MSC: return put_bits(arg, len, d->mscbit, sizeof d->mscbit);
        case EV_LED: return put_bits(arg, len, d->ledbit, sizeof d->ledbit);
        case EV_SW: return put_bits(arg, len, d->swbit, sizeof d->swbit);
        case EV_SND: return put_bits(arg, len, d->sndbit, sizeof d->sndbit);
        case EV_FF: return put_bits(arg, len, d->ffbit, sizeof d->ffbit);
        default: { uint64_t z[2] = { 0, 0 }; return put_bits(arg, len, z, MIN(len, sizeof z)); }
        }
    }
    if (nr >= 0x40 && nr < 0x80 && dir == 2) {                                           /* GABS */
        unsigned a = nr - 0x40;
        return copy_to_user((void *)arg, &d->abs[a], MIN(len, sizeof d->abs[a]));
    }
    if (nr >= 0xc0 && nr < 0x100 && dir == 1) {                                          /* SABS */
        unsigned a = nr - 0xc0;
        struct input_absinfo ai;
        if (copy_from_user(&ai, (void *)arg, MIN(len, sizeof ai))) return -EFAULT;
        d->abs[a] = ai;
        return 0;
    }
    return -EINVAL;
}

static void ff_erase_slot(struct input_dev *d, int id) {
    if (d->ff_erase && !d->gone) d->ff_erase(d, id);
    d->ff_owner[id] = nullptr;
}

/* read/write/poll are lock-free (queue under dev->lock); ioctls take the BKL in sys_ioctl */
static const struct file_ops evdev_ops = {
    .nobkl = true,
    .open = evdev_open, .release = evdev_release, .read = evdev_read, .write = evdev_write,
    .poll = evdev_poll, .ioctl = evdev_ioctl,
};

void evdev_register_chrdev(void) { chrdev_register(EVDEV_MAJOR, CHRDEV_ANY_MINOR, &evdev_ops); }
