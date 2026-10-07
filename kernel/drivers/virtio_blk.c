/*
 * virtio-blk (M29) over virtio-pci. One request queue (queue 0); every request is a single
 * ring descriptor pointing at an indirect table {header, data segments..., status}, so the
 * queue depth equals the ring size. Completion is interrupt driven (MSI-X on x86, INTx via the
 * IO-APIC/GIC/PLIC elsewhere): the threaded handler reaps the used ring and completes bios,
 * which lets the block layer dispatch the next requests. Devices without an interrupt are
 * polled by a kernel thread.
 *
 * Only the PCI transport is implemented: all three QEMU machines used by 9os (q35, virt with
 * PCIe on aarch64 and riscv64) expose virtio as PCIe functions, as does real virtualisation
 * hardware; virtio-mmio would add a second transport with no device that needs it.
 */
#include <kernel/virtio.h>
#include <kernel/blk.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <kernel/errno.h>

#define F_SIZE_MAX (1u << 1)
#define F_SEG_MAX  (1u << 2)
#define F_RO       (1u << 5)
#define F_BLK_SIZE (1u << 6)
#define F_FLUSH    (1u << 9)
#define F_INDIRECT (1u << 28)
enum { T_IN = 0, T_OUT = 1, T_FLUSH = 4 };

#define SLOT_SIZE 1024        /* indirect table (<= 40 descriptors) + header + status */
#define SLOT_HDR  896
#define SLOT_STAT 912
_Static_assert((BIO_MAX_VECS + 2) * 16 <= SLOT_HDR, "indirect table fits a slot");

struct vblk {
    struct virtio_dev v;
    struct virtq q;
    spinlock_t lock;
    struct blkdev *disk;
    uint8_t *slots;           /* depth * SLOT_SIZE, physically contiguous */
    paddr_t slots_pa;
    struct bio **req;         /* in-flight bio per slot */
    uint16_t *free;           /* stack of free slots */
    unsigned nfree, depth, seg_max;
    uint64_t completions;
};
static const struct lock_class drv_class = { "virtio_blk", LR_BLKDRV, false };
static struct vblk *vdevs[8];
static int nvdevs;

static bool vblk_submit(struct blkdev *disk, struct bio *b) {
    struct vblk *d = disk->driver;
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&d->lock);
    if (!d->nfree) { spin_unlock(&d->lock); arch_irq_restore(f); return false; }
    unsigned s = d->free[--d->nfree];
    uint8_t *slot = d->slots + (size_t)s * SLOT_SIZE;
    paddr_t spa = d->slots_pa + (size_t)s * SLOT_SIZE;
    struct vq_desc *t = (struct vq_desc *)slot;
    struct { uint32_t type, reserved; uint64_t sector; } *hdr = (void *)(slot + SLOT_HDR);
    hdr->type = b->op == BIO_READ ? T_IN : b->op == BIO_WRITE ? T_OUT : T_FLUSH;
    hdr->reserved = 0;
    hdr->sector = b->op == BIO_FLUSH ? 0 : b->sector;
    slot[SLOT_STAT] = 0xff;
    unsigned n = 0;
    t[n] = (struct vq_desc){ spa + SLOT_HDR, 16, VQ_NEXT, (uint16_t)(n + 1) }; n++;
    for (int i = 0; i < b->nvec; i++, n++)
        t[n] = (struct vq_desc){ b->vec[i].pa, b->vec[i].len, (uint16_t)(VQ_NEXT | (b->op == BIO_READ ? VQ_WRITE : 0)), (uint16_t)(n + 1) };
    t[n] = (struct vq_desc){ spa + SLOT_STAT, 1, VQ_WRITE, 0 }; n++;
    d->q.desc[s] = (struct vq_desc){ spa, n * 16, VQ_INDIRECT, 0 };
    d->req[s] = b;
    virtq_push(&d->q, (uint16_t)s);
    spin_unlock(&d->lock);
    arch_irq_restore(f);
    return true;
}

static const struct blk_driver_ops vblk_ops = { .submit = vblk_submit };

static void reap(void *arg) {
    struct vblk *d = arg;
    for (;;) {
        struct bio *done[32];
        int st[32], n = 0;
        uint32_t id;
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&d->lock);
        while (n < 32 && virtq_pop(&d->q, &id, nullptr)) {
            if (id >= d->depth || !d->req[id]) continue;
            done[n] = d->req[id];
            st[n++] = d->slots[(size_t)id * SLOT_SIZE + SLOT_STAT] == 0 ? 0 : -EIO;
            d->req[id] = nullptr;
            d->free[d->nfree++] = (uint16_t)id;
        }
        d->completions += n;
        spin_unlock(&d->lock);
        arch_irq_restore(f);
        if (!n) break;
        for (int i = 0; i < n; i++) bio_complete(done[i], st[i]);
    }
}

static void poll_thread(void *arg) {
    for (;;) {
        for (int i = 0; i < nvdevs; i++) if (vdevs[i]->v.irq < 0) reap(vdevs[i]);
        sleep_ns(1000000ull);
    }
}

static void probe(struct pci_dev *pd) {
    if (nvdevs >= (int)ARRAY_SIZE(vdevs)) return;
    struct vblk *d = kzalloc(sizeof *d);
    if (!d) return;
    uint32_t feat = 0;
    if (!virtio_pci_probe_features(&d->v, pd, "virtio-blk", F_SIZE_MAX | F_SEG_MAX | F_RO | F_BLK_SIZE | F_FLUSH | F_INDIRECT, &feat) ||
        !d->v.devcfg) { kfree(d); return; }
    if (!(feat & F_INDIRECT)) { pr_err("virtio-blk: device lacks indirect descriptors\n"); kfree(d); return; }
    spin_lock_init_class(&d->lock, &drv_class);
    char nm[16];
    snprintf(nm, sizeof nm, "vd%c", 'a' + nvdevs);
    virtio_irq_setup(&d->v, nm, nullptr, reap, d);
    if (!virtq_init(&d->v, &d->q, 0, VQ_MAX)) { pr_err("virtio-blk: no request queue\n"); return; }
    volatile uint8_t *cfg = d->v.devcfg;
    uint64_t cap = *(volatile uint32_t *)cfg | (uint64_t)*(volatile uint32_t *)(cfg + 4) << 32;
    d->seg_max = feat & F_SEG_MAX ? *(volatile uint32_t *)(cfg + 12) : BIO_MAX_VECS;
    if (!d->seg_max || d->seg_max > BIO_MAX_VECS) d->seg_max = BIO_MAX_VECS;
    uint32_t bsz = feat & F_BLK_SIZE ? *(volatile uint32_t *)(cfg + 20) : SECTOR_SIZE;
    d->depth = d->q.size;
    unsigned order = 0;
    while ((PAGE_SIZE << order) < d->depth * SLOT_SIZE) order++;
    d->slots_pa = pmm_alloc_zeroed(order);
    d->req = kzalloc(d->depth * sizeof *d->req);
    d->free = kzalloc(d->depth * sizeof *d->free);
    if (!d->slots_pa || !d->req || !d->free) { pr_err("virtio-blk: out of memory\n"); return; }
    d->slots = PHYS_TO_VIRT(d->slots_pa);
    for (unsigned i = 0; i < d->depth; i++) d->free[d->nfree++] = (uint16_t)(d->depth - 1 - i);
    virtio_driver_ok(&d->v);
    vdevs[nvdevs++] = d;
    d->disk = blk_register_disk(nm, cap, bsz, &vblk_ops, d, d->depth);
    if (!d->disk) return;
    d->disk->max_vecs = d->seg_max;
    d->disk->readonly = feat & F_RO;
    d->disk->has_flush = feat & F_FLUSH;
    pr_info("virtio-blk: %s: %lu sectors (%lu MiB)%s%s, queue depth %u, %s irq %d\n", nm, cap, cap >> 11,
            d->disk->readonly ? ", read-only" : "", d->disk->has_flush ? ", flush" : "", d->depth, d->v.irq_mode, d->v.irq);
}

void virtio_blk_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *pd = pci_get(i);
        if (pd->vendor == VIRTIO_VENDOR && (pd->device == VIRTIO_DEV_BLK || pd->device == 0x1001)) probe(pd);   /* modern or transitional */
    }
    bool polled = false;
    for (int i = 0; i < nvdevs; i++) if (vdevs[i]->v.irq < 0) polled = true;
    if (polled) thread_create("vblk-poll", poll_thread, nullptr);
    for (int i = 0; i < nvdevs; i++) if (vdevs[i]->disk) blk_scan_partitions(vdevs[i]->disk);
}
