/*
 * virtio-gpu (2D only) over virtio-pci "modern" transport.
 * Creates one B8G8R8X8 resource backed by physically contiguous RAM, attaches it to
 * scanout 0 and exposes it as the system framebuffer (fbcon + /dev/fb0). A kernel
 * thread pushes damage to the host (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH), at most ~60 Hz.
 * M28: command completion is interrupt-driven (the flush thread sleeps until the used ring
 * advances) and the thread only wakes for damage: the fbcon hook runs under the console
 * lock, so it raises an irq_work that does the wake_up. Only an mmap'd /dev/fb0 without
 * explicit damage ioctls keeps a periodic flush.
 */
#include <kernel/virtio.h>
#include <kernel/boot.h>
#include <kernel/fbcon.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <kernel/spinlock.h>
#include <kernel/irq.h>
#include <kernel/time.h>
#include <kernel/errno.h>

#define QSIZE 16

struct gpu_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx, pad[3]; };
struct gpu_rect { uint32_t x, y, w, h; };

enum {
    CMD_GET_DISPLAY_INFO = 0x100, CMD_RESOURCE_CREATE_2D, CMD_RESOURCE_UNREF, CMD_SET_SCANOUT,
    CMD_RESOURCE_FLUSH, CMD_TRANSFER_TO_HOST_2D, CMD_RESOURCE_ATTACH_BACKING,
    RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO,
};
#define FMT_B8G8R8X8 2

static struct virtio_dev vdev;
static struct virtq vq;
static uint8_t *cmdbuf; static paddr_t cmdbuf_pa;   /* request at 0, response at 2048 */

static struct limine_framebuffer vfb;
static uint32_t width, height;
static spinlock_t dlock;
static uint32_t dx0 = UINT32_MAX, dy0 = UINT32_MAX, dx1, dy1;   /* pending damage box */

static struct wait_queue cmd_wq, flush_wq;
static bool irq_ok;          /* interrupt delivered at least once: sleep instead of spinning */
static bool flush_pending;
static uint64_t cmd_irqs, cmd_sleeps;
static struct thread *flush_thr;   /* the only caller that may sleep */

static int gpu_irq(void *ctx) {
    __atomic_store_n(&irq_ok, true, __ATOMIC_RELEASE);
    cmd_irqs++;
    wake_up(&cmd_wq);
    return IRQ_HANDLED;
}

static bool used_ready(void) { return vq.used[1] != vq.used_seen; }

static bool gpu_cmd(size_t req_len, size_t resp_len) {
    vq.desc[0] = (struct vq_desc){ cmdbuf_pa, (uint32_t)req_len, VQ_NEXT, 1 };
    vq.desc[1] = (struct vq_desc){ cmdbuf_pa + 2048, (uint32_t)resp_len, VQ_WRITE, 0 };
    virtq_push(&vq, 0);
    if (__atomic_load_n(&irq_ok, __ATOMIC_ACQUIRE) && current == flush_thr) {
        uint64_t deadline = time_ns() + 2000000000ull;
        while (!used_ready()) {
            uint64_t g = sched_wait_lock();
            if (used_ready()) { sched_wait_unlock(g); break; }
            cmd_sleeps++;
            wait_event_timeout_locked(&cmd_wq, 20 * 1000000ull, g);   /* a lost irq costs 20 ms */
            if (time_ns() > deadline) { pr_err("virtio-gpu: command timeout\n"); return false; }
        }
    }
    for (uint64_t spins = 0; !virtq_pop(&vq, nullptr, nullptr); spins++) {
        if (spins > 50000000ull) { pr_err("virtio-gpu: command timeout\n"); return false; }
        arch_cpu_relax();
    }
    struct gpu_hdr *r = (struct gpu_hdr *)(cmdbuf + 2048);
    return r->type == RESP_OK_NODATA || r->type == RESP_OK_DISPLAY_INFO;
}

static void *req(uint32_t type, size_t len) {
    memset(cmdbuf, 0, 4096);
    ((struct gpu_hdr *)cmdbuf)->type = type;
    return cmdbuf;
}

static void gpu_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct { struct gpu_hdr h; struct gpu_rect r; uint64_t off; uint32_t res, pad; } *t =
        req(CMD_TRANSFER_TO_HOST_2D, sizeof *t);
    t->r = (struct gpu_rect){ x, y, w, h }; t->off = (uint64_t)y * width * 4 + x * 4; t->res = 1;
    gpu_cmd(sizeof *t, sizeof(struct gpu_hdr));
    struct { struct gpu_hdr h; struct gpu_rect r; uint32_t res, pad; } *fl = req(CMD_RESOURCE_FLUSH, sizeof *fl);
    fl->r = (struct gpu_rect){ x, y, w, h }; fl->res = 1;
    gpu_cmd(sizeof *fl, sizeof(struct gpu_hdr));
}

static bool periodic(void) { return fb_graphics_active() && !fb_explicit_damage; }   /* mmap'd fbdev */

static void flush_thread(void *arg) {
    for (;;) {
        wait_until_sl(&flush_wq, __atomic_load_n(&flush_pending, __ATOMIC_ACQUIRE) || periodic());
        sleep_ns(16 * 1000000ull);              /* rate limit and batch damage: ~60 Hz */
        __atomic_store_n(&flush_pending, false, __ATOMIC_RELEASE);
        uint64_t fl = spin_lock_irqsave(&dlock);
        uint32_t x0 = dx0, y0 = dy0, x1 = MIN(dx1, width), y1 = MIN(dy1, height);
        dx0 = dy0 = UINT32_MAX; dx1 = dy1 = 0;
        spin_unlock_irqrestore(&dlock, fl);
        if (periodic()) { x0 = y0 = 0; x1 = width; y1 = height; }
        if (x1 > x0 && y1 > y0) gpu_flush_rect(x0, y0, x1 - x0, y1 - y0);
    }
}

static void flush_kick(struct irq_work *w) { wake_up(&flush_wq); }
static struct irq_work flush_work = { .fn = flush_kick };

static void vgpu_damage_flush(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    uint64_t fl = spin_lock_irqsave(&dlock);
    dx0 = MIN(dx0, x0); dy0 = MIN(dy0, y0); dx1 = MAX(dx1, x1); dy1 = MAX(dy1, y1);
    spin_unlock_irqrestore(&dlock, fl);
    if (!__atomic_exchange_n(&flush_pending, true, __ATOMIC_ACQ_REL)) irq_work_queue(&flush_work);
}

void virtio_gpu_init(void) {
    struct pci_dev *d = pci_find(VIRTIO_VENDOR, VIRTIO_DEV_GPU);
    if (!d) return;
    if (boot_framebuffer() && !strstr(boot_cmdline(), "virtiogpu")) {
        pr_info("virtio-gpu: present, using firmware framebuffer instead\n");
        return;
    }
    if (!virtio_pci_probe(&vdev, d, "virtio-gpu")) return;
    wait_queue_init(&cmd_wq); wait_queue_init(&flush_wq);
    virtio_irq_setup(&vdev, "virtio-gpu", gpu_irq, nullptr, nullptr);
    if (!virtq_init(&vdev, &vq, 0, QSIZE)) { pr_err("virtio-gpu: no control queue\n"); return; }
    virtio_driver_ok(&vdev);
    cmdbuf_pa = pmm_alloc_zeroed(0); cmdbuf = PHYS_TO_VIRT(cmdbuf_pa);

    req(CMD_GET_DISPLAY_INFO, sizeof(struct gpu_hdr));
    width = 1024; height = 768;
    if (gpu_cmd(sizeof(struct gpu_hdr), sizeof(struct gpu_hdr) + 16 * 24)) {
        struct gpu_rect *r = (struct gpu_rect *)(cmdbuf + 2048 + sizeof(struct gpu_hdr));
        if (r->w && r->h) { width = r->w; height = r->h; }
    }
    while ((uint64_t)width * height * 4 > (PAGE_SIZE << (MAX_ORDER - 1))) {   /* one buddy block */
        width = width * 3 / 4; height = height * 3 / 4;
    }
    uint64_t size = (uint64_t)width * height * 4;
    unsigned order = size_to_order(size);
    paddr_t fbpa = pmm_alloc_zeroed(order);
    if (!fbpa) { pr_err("virtio-gpu: no memory for %ux%u\n", width, height); return; }

    struct { struct gpu_hdr h; uint32_t res, fmt, w, h2; } *c2 = req(CMD_RESOURCE_CREATE_2D, sizeof *c2);
    c2->res = 1; c2->fmt = FMT_B8G8R8X8; c2->w = width; c2->h2 = height;
    if (!gpu_cmd(sizeof *c2, sizeof(struct gpu_hdr))) { pr_err("virtio-gpu: create_2d failed\n"); return; }
    struct { struct gpu_hdr h; uint32_t res, n; uint64_t addr; uint32_t len, pad; } *ab =
        req(CMD_RESOURCE_ATTACH_BACKING, sizeof *ab);
    ab->res = 1; ab->n = 1; ab->addr = fbpa; ab->len = size;
    if (!gpu_cmd(sizeof *ab, sizeof(struct gpu_hdr))) { pr_err("virtio-gpu: attach_backing failed\n"); return; }
    struct { struct gpu_hdr h; struct gpu_rect r; uint32_t scanout, res; } *ss = req(CMD_SET_SCANOUT, sizeof *ss);
    ss->r = (struct gpu_rect){ 0, 0, width, height }; ss->scanout = 0; ss->res = 1;
    if (!gpu_cmd(sizeof *ss, sizeof(struct gpu_hdr))) { pr_err("virtio-gpu: set_scanout failed\n"); return; }

    vfb = (struct limine_framebuffer){
        .address = PHYS_TO_VIRT(fbpa), .width = width, .height = height, .pitch = width * 4, .bpp = 32,
        .memory_model = LIMINE_FRAMEBUFFER_RGB,
        .red_mask_size = 8, .red_mask_shift = 16, .green_mask_size = 8, .green_mask_shift = 8,
        .blue_mask_size = 8, .blue_mask_shift = 0,
    };
    boot_set_framebuffer(&vfb);
    fb_flush_hook = vgpu_damage_flush;
    gpu_flush_rect(0, 0, width, height);
    flush_thr = thread_create("vgpu-flush", flush_thread, nullptr);
    pr_info("virtio-gpu: %ux%u framebuffer at 0x%lx (%lu KiB), %s irq %d\n", width, height, fbpa,
            size / 1024, vdev.irq_mode, vdev.irq);
}
