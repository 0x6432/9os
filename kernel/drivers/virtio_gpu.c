/*
 * virtio-gpu (2D only) over virtio-pci "modern" transport, polled (no interrupts).
 * Creates one B8G8R8X8 resource backed by physically contiguous RAM, attaches it to
 * scanout 0 and exposes it as the system framebuffer (fbcon + /dev/fb0). A kernel
 * thread pushes damage to the host (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH) at ~60 Hz.
 */
#include <kernel/pci.h>
#include <kernel/boot.h>
#include <kernel/fbcon.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/arch.h>

#define VIRTIO_VENDOR 0x1af4
#define VIRTIO_GPU_DEV 0x1050

/* virtio-pci capability types */
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

struct virtio_common {
    uint32_t device_feature_select, device_feature, driver_feature_select, driver_feature;
    uint16_t msix_config, num_queues;
    uint8_t device_status, config_generation;
    uint16_t queue_select, queue_size, queue_msix_vector, queue_enable, queue_notify_off;
    uint32_t queue_desc_lo, queue_desc_hi, queue_driver_lo, queue_driver_hi, queue_device_lo, queue_device_hi;
};   /* naturally aligned; not packed so every field is a single MMIO access of its own width */
_Static_assert(sizeof(struct virtio_common) == 56, "virtio common cfg layout");

struct vq_desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define VQ_NEXT 1
#define VQ_WRITE 2
#define QSIZE 16

struct gpu_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id; uint8_t ring_idx, pad[3]; };
struct gpu_rect { uint32_t x, y, w, h; };

enum {
    CMD_GET_DISPLAY_INFO = 0x100, CMD_RESOURCE_CREATE_2D, CMD_RESOURCE_UNREF, CMD_SET_SCANOUT,
    CMD_RESOURCE_FLUSH, CMD_TRANSFER_TO_HOST_2D, CMD_RESOURCE_ATTACH_BACKING,
    RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO,
};
#define FMT_B8G8R8X8 2

static volatile struct virtio_common *common;
static volatile uint16_t *notify;
static struct vq_desc *desc;
static volatile uint16_t *avail;      /* flags, idx, ring[QSIZE] */
static volatile uint16_t *used;       /* flags, idx, {u32 id, u32 len}[QSIZE] */
static uint16_t avail_idx, used_seen;
static uint8_t *cmdbuf; static paddr_t cmdbuf_pa;   /* request at 0, response at 2048 */

static struct limine_framebuffer vfb;
static uint32_t width, height;
static volatile bool dirty;

static bool gpu_cmd(size_t req_len, size_t resp_len) {
    desc[0] = (struct vq_desc){ cmdbuf_pa, (uint32_t)req_len, VQ_NEXT, 1 };
    desc[1] = (struct vq_desc){ cmdbuf_pa + 2048, (uint32_t)resp_len, VQ_WRITE, 0 };
    avail[2 + avail_idx % QSIZE] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    avail[1] = ++avail_idx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *notify = 0;
    for (uint64_t spins = 0; used[1] == used_seen; spins++) {
        if (spins > 50000000ull) { pr_err("virtio-gpu: command timeout\n"); return false; }
        arch_cpu_relax();
    }
    used_seen = used[1];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct gpu_hdr *r = (struct gpu_hdr *)(cmdbuf + 2048);
    return r->type == RESP_OK_NODATA || r->type == RESP_OK_DISPLAY_INFO;
}

static void *req(uint32_t type, size_t len) {
    memset(cmdbuf, 0, 4096);
    ((struct gpu_hdr *)cmdbuf)->type = type;
    return cmdbuf;
}

static void gpu_flush(void) {
    struct { struct gpu_hdr h; struct gpu_rect r; uint64_t off; uint32_t res, pad; } *t =
        req(CMD_TRANSFER_TO_HOST_2D, sizeof *t);
    t->r = (struct gpu_rect){ 0, 0, width, height }; t->res = 1;
    gpu_cmd(sizeof *t, sizeof(struct gpu_hdr));
    struct { struct gpu_hdr h; struct gpu_rect r; uint32_t res, pad; } *fl = req(CMD_RESOURCE_FLUSH, sizeof *fl);
    fl->r = (struct gpu_rect){ 0, 0, width, height }; fl->res = 1;
    gpu_cmd(sizeof *fl, sizeof(struct gpu_hdr));
}

static void flush_thread(void *arg) {
    for (;;) {
        sleep_ns(16 * 1000000ull);
        if (dirty || fb_graphics_active()) { dirty = false; gpu_flush(); }
    }
}

static void vgpu_damage_flush(void) { dirty = true; }

static bool find_caps(struct pci_dev *d) {
    if (!(pci_read16(d, 6) & 0x10)) return false;
    uint32_t notify_mult = 0; volatile uint8_t *nbase = nullptr;
    for (unsigned p = pci_read8(d, 0x34) & 0xfc; p; p = pci_read8(d, p + 1) & 0xfc) {
        if (pci_read8(d, p) != 0x09) continue;
        unsigned type = pci_read8(d, p + 3), bar = pci_read8(d, p + 4);
        uint32_t off = pci_read32(d, p + 8), len = pci_read32(d, p + 12);
        if (type != CAP_COMMON && type != CAP_NOTIFY) continue;
        paddr_t pa = pci_bar(d, bar, nullptr);
        if (!pa) continue;
        volatile uint8_t *va = vmm_map_mmio(pa + off, MAX(len, 4096u));
        if (type == CAP_COMMON) common = (volatile struct virtio_common *)va;
        else { nbase = va; notify_mult = pci_read32(d, p + 16); }
    }
    if (!common || !nbase) return false;
    common->queue_select = 0;
    notify = (volatile uint16_t *)(nbase + (uint32_t)common->queue_notify_off * notify_mult);
    return true;
}

void virtio_gpu_init(void) {
    struct pci_dev *d = pci_find(VIRTIO_VENDOR, VIRTIO_GPU_DEV);
    if (!d) return;
    if (boot_framebuffer() && !strstr(boot_cmdline(), "virtiogpu")) {
        pr_info("virtio-gpu: present, using firmware framebuffer instead\n");
        return;
    }
    pci_enable(d);
    if (!find_caps(d)) { pr_err("virtio-gpu: missing virtio-pci capabilities\n"); return; }

    common->device_status = 0;
    while (common->device_status) ;
    common->device_status = 1 | 2;                      /* ACKNOWLEDGE | DRIVER */
    common->driver_feature_select = 0; common->driver_feature = 0;
    common->driver_feature_select = 1; common->driver_feature = 1;   /* VIRTIO_F_VERSION_1 */
    common->device_status = 1 | 2 | 8;                  /* FEATURES_OK */
    if (!(common->device_status & 8)) { pr_err("virtio-gpu: features rejected\n"); return; }

    paddr_t qpa = pmm_alloc_zeroed(0);
    uint8_t *q = PHYS_TO_VIRT(qpa);
    desc = (struct vq_desc *)q; avail = (uint16_t *)(q + 1024); used = (uint16_t *)(q + 2048);
    common->queue_select = 0;
    if (common->queue_size < QSIZE) { pr_err("virtio-gpu: queue too small\n"); return; }
    common->queue_size = QSIZE;
    common->queue_desc_lo = (uint32_t)qpa; common->queue_desc_hi = qpa >> 32;
    common->queue_driver_lo = (uint32_t)(qpa + 1024); common->queue_driver_hi = (qpa + 1024) >> 32;
    common->queue_device_lo = (uint32_t)(qpa + 2048); common->queue_device_hi = (qpa + 2048) >> 32;
    common->queue_enable = 1;
    common->device_status = 1 | 2 | 8 | 4;              /* DRIVER_OK */
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
    gpu_flush();
    thread_create("vgpu-flush", flush_thread, nullptr);
    pr_info("virtio-gpu: %ux%u framebuffer at 0x%lx (%lu KiB)\n", width, height, fbpa, size / 1024);
}
