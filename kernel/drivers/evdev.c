/*
 * Input core and evdev. Drivers (PS/2, virtio-input) register a struct input_dev and report
 * events; every open of /dev/input/eventN (char 13:64+N) gets its own event queue in the
 * Linux struct input_event layout. Keyboards also feed the console tty (US layout) unless a
 * client grabbed the device (EVIOCGRAB) or the console keyboard is in K_OFF mode.
 */
#include <kernel/input.h>
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
static int ndevs;
int console_kbmode = 1;             /* K_XLATE */

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
    switch (code) {
    case 42: case 54: d->shift = down; return;            /* KEY_LEFTSHIFT, KEY_RIGHTSHIFT */
    case 29: case 97: d->ctrl = down; return;             /* KEY_LEFTCTRL, KEY_RIGHTCTRL */
    case 56: case 100: d->alt = down; return;             /* KEY_LEFTALT, KEY_RIGHTALT */
    case 58: if (value == 1) d->caps = !d->caps; return;  /* KEY_CAPSLOCK */
    }
    if (!down || console_kbmode == 4 /* K_OFF */) return;
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
    if (seq) { tty_input_str(&console_tty, seq); return; }
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
    if (d->alt) tty_input(&console_tty, 27);
    tty_input(&console_tty, c);
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
    }
    uint64_t mono = time_ns();
    uint64_t fl = spin_lock_irqsave(&d->lock);
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

void input_register(struct input_dev *d) {
    if (ndevs >= MAX_INPUT) return;
    list_init(&d->clients);
    d->lock = (spinlock_t){ 0 };
    input_set_bit(d->evbit, EV_SYN);
    if (!d->rep[0]) { d->rep[0] = 250; d->rep[1] = 33; }
    d->index = ndevs;
    devs[ndevs++] = d;
    char path[32];
    snprintf(path, sizeof path, "/dev/input/event%d", d->index);
    vfs_mkdir_at(nullptr, "/dev/input", 0755);
    vfs_mknod_at(nullptr, path, S_IFCHR | 0660, MKDEV(EVDEV_MAJOR, EVDEV_MINOR_BASE + d->index));
    pr_info("input: %s as %s\n", d->name, path);
}

/* ---------------------------------------------------------------- /dev/input/eventN */
static int evdev_open(struct inode *ino, struct file *f) {
    int idx = (int)MINOR(ino->rdev) - EVDEV_MINOR_BASE;
    if (idx < 0 || idx >= ndevs) return -ENODEV;
    struct evdev_client *c = kzalloc(sizeof *c);
    if (!c) return -ENOMEM;
    c->dev = devs[idx];
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
    kfree(c);
}

static ssize_t evdev_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct evdev_client *c = f->priv;
    if (n < sizeof(struct input_event_abi)) return -EINVAL;
    for (;;) {
        if (c->revoked) return -ENODEV;
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

static ssize_t evdev_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct evdev_client *c = f->priv;
    size_t done = 0;
    for (; done + sizeof(struct input_event_abi) <= n; done += sizeof(struct input_event_abi)) {
        struct input_event_abi e;
        if (copy_from_user(&e, (const char *)buf + done, sizeof e)) return done ? (ssize_t)done : -EFAULT;
        if (e.type == EV_LED && e.code <= LED_MAX) {
            if (e.value) input_set_bit(c->dev->ledstate, e.code);
            else c->dev->ledstate[0] &= ~(1ull << e.code);
        }
    }
    return done;
}

static unsigned evdev_poll(struct file *f) {
    struct evdev_client *c = f->priv;
    if (c->revoked) return POLLERR | POLLHUP;
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
    if (c->revoked) return -ENODEV;
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
    case 0x1a: case 0x1b: { uint64_t z[2] = { 0, 0 }; return put_bits(arg, len, z, 8); } /* GSND, GSW */
    case 0x84: { int v = 0; return copy_to_user((void *)arg, &v, sizeof v); }            /* GEFFECTS */
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

static const struct file_ops evdev_ops = {
    .open = evdev_open, .release = evdev_release, .read = evdev_read, .write = evdev_write,
    .poll = evdev_poll, .ioctl = evdev_ioctl,
};

void evdev_register_chrdev(void) { chrdev_register(EVDEV_MAJOR, CHRDEV_ANY_MINOR, &evdev_ops); }
