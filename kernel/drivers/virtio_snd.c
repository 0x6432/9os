/* M39: one exclusive playback PCM, S16_LE / stereo / 48 kHz, message-based virtio-snd.
 * DMA is always kernel-owned. A timeout quarantines the device and retains its DMA pages;
 * never reuse memory whose ownership may still belong to a failed device. */
#include <kernel/virtio.h>
#include <kernel/pcm_abi.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/time.h>

#define S_OK 0x8000u
#define R_INFO 0x100u
#define R_PARAMS 0x101u
#define R_PREPARE 0x102u
#define R_RELEASE 0x103u
#define R_START 0x104u
#define R_STOP 0x105u
struct snd_info { uint32_t nid, features; uint64_t formats, rates; uint8_t direction, min_ch, max_ch, pad[5]; };
struct snd_params { uint32_t code, stream, buffer, period, features; uint8_t channels, format, rate, pad; };
_Static_assert(sizeof(struct snd_info) == 32 && sizeof(struct snd_params) == 24, "virtio PCM layouts");
static const struct lock_class io_class = { "snd_io", LR_MUTEX_DEV, false };
static const struct lock_class q_class = { "snd_queue", LR_BLKDRV, false };
static struct {
    struct virtio_dev v;
    struct virtq q[4];
    struct mutex io;
    spinlock_t lock;
    struct wait_queue wq;
    paddr_t ctrl_pa, events_pa, tx_pa;
    uint8_t *ctrl, *events, *tx;
    bool present, opened, failed, ctrl_pending;
    uint32_t ctrl_len, stream, busy, submitted;
    unsigned frames[PCM_PERIODS];
    uint64_t appl, hw;
    int state;
    struct pcm_sw_params sw;
    struct device kdev;
} snd;

static void fail_locked(void) {
    snd.failed = true;
    snd.v.common->device_status |= 128; /* FAILED; no reset/reuse of outstanding buffers */
}
static void reap(void *arg) {
    bool notify = false;
    uint32_t id, len;
    uint64_t f = spin_lock_irqsave(&snd.lock);
    while (virtq_pop(&snd.q[0], &id, &len)) {
        if (id != 0 || !snd.ctrl_pending) { fail_locked(); continue; }
        snd.ctrl_len = len; snd.ctrl_pending = false; notify = true;
    }
    while (virtq_pop(&snd.q[2], &id, &len)) {
        unsigned slot = id / 3;
        if (id % 3 || slot >= PCM_PERIODS || !(snd.submitted & (1u << slot))) { fail_locked(); continue; }
        uint32_t st; memcpy(&st, snd.tx + slot * PAGE_SIZE + 8, 4);
        if (len < 8 || st != S_OK) { pr_warn("snd: bad tx status id=%u len=%u status=%x\n", id, len, st); fail_locked(); }
        snd.busy &= ~(1u << slot); snd.submitted &= ~(1u << slot); snd.hw += snd.frames[slot]; notify = true;
    }
    while (virtq_pop(&snd.q[1], &id, &len)) {
        if (id >= snd.q[1].size) { fail_locked(); continue; }
        uint32_t code, stream;
        memcpy(&code, snd.events + id * 8, 4); memcpy(&stream, snd.events + id * 8 + 4, 4);
        if (len >= 8 && code == 0x1101 && stream == snd.stream) { fail_locked(); notify = true; }
        if (!snd.failed) virtq_push(&snd.q[1], (uint16_t)id);
    }
    spin_unlock_irqrestore(&snd.lock, f);
    if (notify) { wake_up(&snd.wq); poll_notify(); }
}
static void poll_thread(void *arg) { for (;;) { reap(nullptr); sleep_ns(1000000ull); } }
static bool dead(void) { return __atomic_load_n(&snd.failed, __ATOMIC_ACQUIRE); }
static int timeout(void) {
    uint64_t f = spin_lock_irqsave(&snd.lock); fail_locked(); spin_unlock_irqrestore(&snd.lock, f);
    pr_err("virtio-snd: request timed out; DMA buffers quarantined\n"); return -ETIMEDOUT;
}
/* io mutex serializes the single control DMA buffer. */
static int control(const void *req, unsigned size, void *out, unsigned out_size) {
    if (dead()) return -EIO;
    memcpy(snd.ctrl, req, size); memset(snd.ctrl + 64, 0, 4 + out_size);
    uint64_t f = spin_lock_irqsave(&snd.lock);
    snd.q[0].desc[0] = (struct vq_desc){ snd.ctrl_pa, size, VQ_NEXT, 1 };
    snd.q[0].desc[1] = (struct vq_desc){ snd.ctrl_pa + 64, 4 + out_size, VQ_WRITE, 0 };
    snd.ctrl_pending = true; virtq_push(&snd.q[0], 0);
    spin_unlock_irqrestore(&snd.lock, f);
    uint64_t end = time_ns() + 3000000000ull;
    while (__atomic_load_n(&snd.ctrl_pending, __ATOMIC_ACQUIRE) && !dead()) {
        reap(nullptr);
        if (time_ns() >= end) return timeout();
        wait_event_timeout(&snd.wq, 1000000ull);
    }
    if (dead()) return -EIO;
    uint32_t status; memcpy(&status, snd.ctrl + 64, 4);
    if (snd.ctrl_len < 4 + out_size) return -EIO;
    if (status != S_OK) return status == 0x8002 ? -ENOTSUP : -EINVAL;
    if (out_size) memcpy(out, snd.ctrl + 68, out_size);
    return 0;
}
static int command(unsigned code) { uint32_t req[2] = { code, snd.stream }; return control(req, sizeof req, nullptr, 0); }
static void defaults(void) {
    memset(&snd.sw, 0, sizeof snd.sw);
    snd.sw.period_step = 1; snd.sw.avail_min = PCM_PERIOD; snd.sw.xfer_align = 1;
    snd.sw.start_threshold = PCM_PERIOD; snd.sw.stop_threshold = PCM_BUFFER;
    snd.sw.boundary = 1ull << 60;
}
static int start(void) {
    if (snd.state != PCM_PREPARED) return -EBADFD;
    int r = command(R_START);
    if (r) return r;
    snd.state = PCM_RUNNING;
    /* Prepared data stays kernel-owned until START. DROP can discard it without
     * racing a device's control queue against an unconsumed TX notification. */
    uint64_t f = spin_lock_irqsave(&snd.lock);
    for (unsigned slot = 0; slot < PCM_PERIODS; slot++) if (snd.busy & (1u << slot)) {
        snd.submitted |= 1u << slot; virtq_push(&snd.q[2], (uint16_t)(slot * 3));
    }
    spin_unlock_irqrestore(&snd.lock, f);
    return 0;
}
static int empty_queue(bool drain) {
    if (snd.state < PCM_PREPARED) return 0;
    if (drain && snd.busy && snd.state == PCM_PREPARED) { int r = start(); if (r) return r; }
    if (drain) {
        uint64_t end = time_ns() + 4000000000ull;
        while (__atomic_load_n(&snd.busy, __ATOMIC_ACQUIRE) && !dead()) {
            reap(nullptr); if (time_ns() >= end) return timeout();
            wait_event_timeout(&snd.wq, 1000000ull);
        }
    }
    if (dead()) return -EIO;
    int r = snd.state == PCM_RUNNING ? command(R_STOP) : 0;
    if (!r) r = command(R_RELEASE); /* returns every pending TX buffer before completion */
    uint64_t end = time_ns() + 1000000000ull;
    while (!r && __atomic_load_n(&snd.submitted, __ATOMIC_ACQUIRE) && !dead()) {
        reap(nullptr);
        if (time_ns() >= end) return timeout();
        wait_event_timeout(&snd.wq, 1000000ull);
    }
    if (!r && dead()) r = -EIO;
    if (!r) {
        uint64_t f = spin_lock_irqsave(&snd.lock);
        snd.busy = 0; snd.appl = snd.hw; /* discard only unsent prepared data */
        spin_unlock_irqrestore(&snd.lock, f); snd.state = PCM_SETUP;
    }
    return r;
}
static int pcm_open(struct inode *ino, struct file *f) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EINVAL;
    mutex_lock(&snd.io);
    int r = !snd.present ? -ENODEV : dead() ? -EIO : snd.opened ? -EBUSY : 0;
    if (!r) { snd.opened = true; snd.state = PCM_OPEN; defaults(); snd.appl = snd.hw = 0; }
    mutex_unlock(&snd.io); return r;
}
static void pcm_close(struct file *f) {
    mutex_lock(&snd.io); empty_queue(false); snd.opened = false; snd.state = PCM_OPEN; mutex_unlock(&snd.io);
}
static int copy_in(void *to, uint64_t from, size_t n) { return copy_from_user(to, (void *)from, n) ? -EFAULT : 0; }
static int copy_out(uint64_t to, const void *from, size_t n) { return copy_to_user((void *)to, from, n) ? -EFAULT : 0; }
static unsigned available(void) {
    uint64_t f = spin_lock_irqsave(&snd.lock); uint64_t used = snd.appl - snd.hw;
    spin_unlock_irqrestore(&snd.lock, f); return used < PCM_BUFFER ? PCM_BUFFER - (unsigned)used : 0;
}
static ssize_t transfer(struct file *f, const void *buf, size_t n) {
    if (snd.state != PCM_PREPARED && snd.state != PCM_RUNNING) return -EBADFD;
    if (n % PCM_FRAME_BYTES) return -EINVAL;
    size_t done = 0;
    uint64_t end = time_ns() + 4000000000ull;
    while (done < n && !dead()) {
        reap(nullptr);
        uint64_t fl = spin_lock_irqsave(&snd.lock);
        unsigned slot = 0; while (slot < PCM_PERIODS && (snd.busy & (1u << slot))) slot++;
        spin_unlock_irqrestore(&snd.lock, fl);
        if (slot == PCM_PERIODS) {
            if (f->flags & O_NONBLOCK) return done ? (ssize_t)done : -EAGAIN;
            if (snd.state == PCM_PREPARED) return done ? (ssize_t)done : -EAGAIN; /* explicit-start mode */
            if (time_ns() >= end) { timeout(); break; }
            wait_event_timeout(&snd.wq, 1000000ull); continue;
        }
        size_t count = MIN(n - done, PCM_PERIOD * PCM_FRAME_BYTES);
        uint8_t *page = snd.tx + slot * PAGE_SIZE;
        if (copy_from_user(page + 64, (const uint8_t *)buf + done, count)) return done ? (ssize_t)done : -EFAULT;
        memcpy(page, &snd.stream, 4); memset(page + 8, 0, 8);
        fl = spin_lock_irqsave(&snd.lock);
        unsigned h = slot * 3; paddr_t pa = snd.tx_pa + slot * PAGE_SIZE;
        snd.q[2].desc[h] = (struct vq_desc){ pa, 4, VQ_NEXT, h + 1 };
        snd.q[2].desc[h + 1] = (struct vq_desc){ pa + 64, count, VQ_NEXT, h + 2 };
        snd.q[2].desc[h + 2] = (struct vq_desc){ pa + 8, 8, VQ_WRITE, 0 };
        snd.frames[slot] = count / PCM_FRAME_BYTES; snd.busy |= 1u << slot;
        snd.appl += count / PCM_FRAME_BYTES;
        if (snd.state == PCM_RUNNING) { snd.submitted |= 1u << slot; virtq_push(&snd.q[2], (uint16_t)h); }
        uint64_t queued = snd.appl - snd.hw;
        spin_unlock_irqrestore(&snd.lock, fl);
        done += count; end = time_ns() + 4000000000ull;
        if (snd.state == PCM_PREPARED && queued >= snd.sw.start_threshold) { int r = start(); if (r) return done ? (ssize_t)done : r; }
    }
    return done ? (ssize_t)done : dead() ? -EIO : 0;
}
static ssize_t pcm_write(struct file *f, const void *buf, size_t n, off_t *off) {
    mutex_lock(&snd.io); ssize_t r = dead() ? -EIO : transfer(f, buf, n); mutex_unlock(&snd.io); return r;
}
/* Fixed-format constraint intersection: never silently accept an unsupported request. */
static int refine(struct pcm_hw_params *p) {
    const unsigned masks[3] = { 3, 2, 0 }; /* RW_INTERLEAVED, S16_LE, STD */
    for (unsigned i = 0; i < 3; i++) {
        unsigned val = masks[i]; if (!(p->masks[i].bits[val / 32] & (1u << (val % 32)))) return -EINVAL;
        memset(&p->masks[i], 0, sizeof p->masks[i]); p->masks[i].bits[val / 32] = 1u << (val % 32);
    }
    const unsigned vals[12] = { 16, 32, 2, PCM_RATE, 5333, PCM_PERIOD, PCM_PERIOD * 4, PCM_PERIODS, 42666, PCM_BUFFER, PCM_BUFFER * 4, 0 };
    for (unsigned i = 0; i < 12; i++) {
        struct pcm_interval *v = &p->intervals[i]; unsigned val = vals[i];
        if ((v->flags & 8) || val < v->min || val > v->max || (val == v->min && (v->flags & 1)) || (val == v->max && (v->flags & 2))) return -EINVAL;
        *v = (struct pcm_interval){ val, val, 4 };
    }
    p->rmask = 0; p->cmask = 0xffffffffu; p->info = 0x100; p->msbits = 16;
    p->rate_num = PCM_RATE; p->rate_den = 1; p->fifo_size = 0;
    memset(p->mres, 0, sizeof p->mres); memset(p->ires, 0, sizeof p->ires);
    memset(p->sync, 0, sizeof p->sync); memset(p->reserved, 0, sizeof p->reserved);
    return 0;
}
static int pcm_ioctl_locked(struct file *f, uint64_t cmd, uint64_t arg) {
    int r;
    switch (cmd) {
    case PCM_IO_VERSION: { unsigned v = PCM_VERSION; return copy_out(arg, &v, sizeof v); }
    case PCM_IO_INFO: {
        struct pcm_info v = { .subdevices_count = 1, .subdevices_avail = 0 };
        strcpy((char *)v.id, "9os-virtio"); strcpy((char *)v.name, "virtio-snd playback"); return copy_out(arg, &v, sizeof v);
    }
    case PCM_IO_HW_REFINE: case PCM_IO_HW_PARAMS: {
        struct pcm_hw_params p; if ((r = copy_in(&p, arg, sizeof p))) return r;
        if ((r = refine(&p))) return r;
        if (cmd == PCM_IO_HW_PARAMS) {
            if (snd.state != PCM_OPEN && snd.state != PCM_SETUP) return -EBADFD;
            struct snd_params req = { R_PARAMS, snd.stream, PCM_BUFFER * 4, PCM_PERIOD * 4, 0, 2, 5, 7, 0 };
            if ((r = control(&req, sizeof req, nullptr, 0))) return r;
            snd.state = PCM_SETUP; defaults();
        }
        return copy_out(arg, &p, sizeof p);
    }
    case PCM_IO_SW_PARAMS: {
        if (snd.state == PCM_OPEN) return -EBADFD;
        struct pcm_sw_params p; if ((r = copy_in(&p, arg, sizeof p))) return r;
        if (!p.avail_min || p.avail_min > PCM_BUFFER || !p.start_threshold || p.boundary < PCM_BUFFER || p.boundary > (1ull << 62) || p.period_step != 1 || p.stop_threshold != PCM_BUFFER || p.tstamp_mode || p.silence_size || p.silence_threshold) return -EINVAL;
        snd.sw = p; memset(snd.sw.reserved, 0, sizeof snd.sw.reserved); return copy_out(arg, &snd.sw, sizeof snd.sw);
    }
    case PCM_IO_PREPARE:
        if (snd.state == PCM_OPEN) return -EBADFD;
        if ((r = empty_queue(false))) return r;
        if ((r = command(R_PREPARE))) return r;
        snd.appl = snd.hw = 0; snd.state = PCM_PREPARED; return 0;
    case PCM_IO_START: return start();
    case PCM_IO_DROP: return snd.state < PCM_PREPARED ? -EBADFD : empty_queue(false);
    case PCM_IO_DRAIN: return snd.state < PCM_PREPARED ? -EBADFD : empty_queue(true);
    case PCM_IO_HW_FREE:
        if ((r = empty_queue(false))) return r;
        snd.state = PCM_OPEN; return 0;
    case PCM_IO_STATUS: {
        struct pcm_status p = { .state = snd.state };
        uint64_t fl = spin_lock_irqsave(&snd.lock);
        p.appl_ptr = snd.appl % snd.sw.boundary; p.hw_ptr = snd.hw % snd.sw.boundary;
        p.delay = snd.appl - snd.hw; spin_unlock_irqrestore(&snd.lock, fl);
        p.avail = available(); p.avail_max = PCM_BUFFER;
        uint64_t ns = time_ns(); p.tstamp = (struct pcm_time){ ns / 1000000000ull, ns % 1000000000ull };
        return copy_out(arg, &p, sizeof p);
    }
    case PCM_IO_DELAY: { long delay = PCM_BUFFER - available(); return copy_out(arg, &delay, sizeof delay); }
    case PCM_IO_HWSYNC: reap(nullptr); return 0;
    case PCM_IO_SYNC_PTR: {
        struct pcm_sync_ptr p; if ((r = copy_in(&p, arg, sizeof p))) return r;
        /* RW-only: userspace cannot advance application pointers through mmap sync. */
        if (!(p.flags & 2) && p.c.control.appl_ptr != snd.appl % snd.sw.boundary) return -EINVAL;
        if (!(p.flags & 4)) { if (!p.c.control.avail_min || p.c.control.avail_min > PCM_BUFFER) return -EINVAL; snd.sw.avail_min = p.c.control.avail_min; }
        memset(&p, 0, sizeof p); p.s.status.state = snd.state;
        uint64_t fl = spin_lock_irqsave(&snd.lock);
        p.s.status.hw_ptr = snd.hw % snd.sw.boundary; p.c.control.appl_ptr = snd.appl % snd.sw.boundary;
        spin_unlock_irqrestore(&snd.lock, fl); p.c.control.avail_min = snd.sw.avail_min;
        return copy_out(arg, &p, sizeof p);
    }
    case PCM_IO_WRITEI: {
        struct pcm_xferi x; if ((r = copy_in(&x, arg, sizeof x))) return r;
        if (x.frames > (1ull << 30)) return -EINVAL;
        ssize_t n = transfer(f, x.buf, x.frames * PCM_FRAME_BYTES);
        x.result = n < 0 ? n : n / PCM_FRAME_BYTES;
        r = copy_out(arg, &x, sizeof x); return r ? r : n < 0 ? (int)n : 0;
    }
    default: return -ENOTTY;
    }
}
static int pcm_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    cmd = (uint32_t)cmd; /* musl ioctl(int request) sign-extends high-bit requests */
    mutex_lock(&snd.io); reap(nullptr);
    int r = dead() ? -EIO : pcm_ioctl_locked(f, cmd, arg); mutex_unlock(&snd.io); return r;
}
static unsigned pcm_poll(struct file *f) {
    if (dead()) return POLLERR;
    int state = __atomic_load_n(&snd.state, __ATOMIC_ACQUIRE);
    if (state != PCM_PREPARED && state != PCM_RUNNING) return POLLERR;
    uint64_t fl = spin_lock_irqsave(&snd.lock);
    bool slot = snd.busy != (1u << PCM_PERIODS) - 1;
    spin_unlock_irqrestore(&snd.lock, fl);
    return slot && available() >= __atomic_load_n(&snd.sw.avail_min, __ATOMIC_RELAXED) ? POLLOUT | POLLWRNORM : 0;
}
static const struct file_ops pcm_ops = { .open = pcm_open, .release = pcm_close, .write = pcm_write, .ioctl = pcm_ioctl, .poll = pcm_poll };

void virtio_snd_init(void) {
    struct pci_dev *pd = nullptr;
    for (int i = 0; i < pci_count(); i++) { struct pci_dev *p = pci_get(i); if (p->vendor == VIRTIO_VENDOR && p->device == 0x1059) { pd = p; break; } }
    if (!pd) return;
    mutex_init(&snd.io, &io_class); spin_lock_init_class(&snd.lock, &q_class); wait_queue_init(&snd.wq);
    if (!virtio_pci_probe(&snd.v, pd, "virtio-snd") || !snd.v.devcfg) return;
    snd.ctrl_pa = pmm_alloc_zeroed(0); snd.events_pa = pmm_alloc_zeroed(0); snd.tx_pa = pmm_alloc_zeroed(3);
    if (!snd.ctrl_pa || !snd.events_pa || !snd.tx_pa) { snd.v.common->device_status |= 128; return; }
    snd.ctrl = PHYS_TO_VIRT(snd.ctrl_pa); snd.events = PHYS_TO_VIRT(snd.events_pa); snd.tx = PHYS_TO_VIRT(snd.tx_pa);
    virtio_irq_setup(&snd.v, "virtio-snd", nullptr, reap, nullptr);
    for (unsigned i = 0; i < 4; i++) if (!virtq_init(&snd.v, &snd.q[i], i, 32) || snd.q[i].size < (i == 2 ? PCM_PERIODS * 3 : 2)) { snd.v.common->device_status |= 128; return; }
    for (unsigned i = 0; i < snd.q[1].size; i++) {
        snd.q[1].desc[i] = (struct vq_desc){ snd.events_pa + i * 8, 8, VQ_WRITE, 0 };
        virtq_push(&snd.q[1], i);
    }
    virtio_driver_ok(&snd.v);
    unsigned streams = *(volatile uint32_t *)(snd.v.devcfg + 4); bool found = false;
    mutex_lock(&snd.io);
    for (unsigned i = 0; i < MIN(streams, 16u); i++) {
        uint32_t req[4] = { R_INFO, i, 1, sizeof(struct snd_info) }; struct snd_info info;
        if (control(req, sizeof req, &info, sizeof info)) break;
        if (!info.direction && info.min_ch <= 2 && info.max_ch >= 2 && (info.formats & (1ull << 5)) && (info.rates & (1ull << 7))) { snd.stream = i; found = true; break; }
    }
    mutex_unlock(&snd.io);
    if (!found) { pr_warn("virtio-snd: no stereo S16/48k playback stream\n"); snd.v.common->device_status |= 128; return; }
    if (snd.v.irq < 0) thread_create("snd-poll", poll_thread, nullptr);
    snd.present = true;
    chrdev_register(116, 16, &pcm_ops); vfs_mkdir_at(nullptr, "/dev/snd", 0755);
    vfs_mknod_at(nullptr, "/dev/snd/pcmC0D0p", S_IFCHR | 0660, MKDEV(116, 16));
    chrdev_sysfs(&snd.kdev, "sound", "pcmC0D0p", "snd/pcmC0D0p", 116, 16, snd.v.kdev);
    pr_info("virtio-snd: playback stream %u, stereo S16_LE 48000 Hz, %s irq %d\n", snd.stream, snd.v.irq_mode, snd.v.irq);
}
