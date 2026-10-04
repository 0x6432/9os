/* virtio-pci modern transport: capability discovery, feature negotiation, split virtqueues. */
#include <kernel/virtio.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/vmm.h>
#include <kernel/printk.h>
#include <kernel/arch.h>

_Static_assert(sizeof(struct virtio_common) == 56, "virtio common cfg layout");

#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

static bool find_caps(struct virtio_dev *v, struct pci_dev *d) {
    if (!(pci_read16(d, 6) & 0x10)) return false;
    for (unsigned p = pci_read8(d, 0x34) & 0xfc; p; p = pci_read8(d, p + 1) & 0xfc) {
        if (pci_read8(d, p) != 0x09) continue;
        unsigned type = pci_read8(d, p + 3), bar = pci_read8(d, p + 4);
        uint32_t off = pci_read32(d, p + 8), len = pci_read32(d, p + 12);
        if (type != CAP_COMMON && type != CAP_NOTIFY && type != CAP_DEVICE) continue;
        paddr_t pa = pci_bar(d, bar, nullptr);
        if (!pa) continue;
        volatile uint8_t *va = vmm_map_mmio(pa + off, MAX(len, 4096u));
        if (type == CAP_COMMON) v->common = (volatile struct virtio_common *)va;
        else if (type == CAP_DEVICE) v->devcfg = va;
        else { v->notify_base = va; v->notify_mult = pci_read32(d, p + 16); }
    }
    return v->common && v->notify_base;
}

bool virtio_pci_probe(struct virtio_dev *v, struct pci_dev *d, const char *who) {
    v->pci = d;
    pci_enable(d);
    if (!find_caps(v, d)) { pr_err("%s: missing virtio-pci capabilities\n", who); return false; }
    volatile struct virtio_common *c = v->common;
    c->device_status = 0;
    while (c->device_status) arch_cpu_relax();
    c->device_status = 1 | 2;                      /* ACKNOWLEDGE | DRIVER */
    c->driver_feature_select = 0; c->driver_feature = 0;
    c->driver_feature_select = 1; c->driver_feature = 1;   /* VIRTIO_F_VERSION_1 */
    c->device_status = 1 | 2 | 8;                  /* FEATURES_OK */
    if (!(c->device_status & 8)) { pr_err("%s: features rejected\n", who); return false; }
    return true;
}

bool virtq_init(struct virtio_dev *v, struct virtq *q, unsigned index, unsigned size) {
    volatile struct virtio_common *c = v->common;
    c->queue_select = index;
    unsigned max = c->queue_size;
    if (!max) return false;
    if (size > max) size = max;
    if (size > VQ_MAX) size = VQ_MAX;
    paddr_t qpa = pmm_alloc_zeroed(0);
    if (!qpa) return false;
    uint8_t *m = PHYS_TO_VIRT(qpa);
    q->size = size; q->index = index;
    q->desc = (struct vq_desc *)m; q->avail = (uint16_t *)(m + 1024); q->used = (uint16_t *)(m + 2048);
    q->avail_idx = q->used_seen = 0;
    c->queue_size = size;
    c->queue_desc_lo = (uint32_t)qpa; c->queue_desc_hi = qpa >> 32;
    c->queue_driver_lo = (uint32_t)(qpa + 1024); c->queue_driver_hi = (qpa + 1024) >> 32;
    c->queue_device_lo = (uint32_t)(qpa + 2048); c->queue_device_hi = (qpa + 2048) >> 32;
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)c->queue_notify_off * v->notify_mult);
    c->queue_enable = 1;
    return true;
}

void virtio_driver_ok(struct virtio_dev *v) { v->common->device_status = 1 | 2 | 8 | 4; }

void virtq_push(struct virtq *q, uint16_t head) {
    q->avail[2 + q->avail_idx % q->size] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail[1] = ++q->avail_idx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify = q->index;
}

bool virtq_pop(struct virtq *q, uint32_t *id, uint32_t *len) {
    if (q->used[1] == q->used_seen) return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    volatile uint32_t *e = (volatile uint32_t *)(q->used + 2) + 2 * (q->used_seen % q->size);
    if (id) *id = e[0];
    if (len) *len = e[1];
    q->used_seen++;
    return true;
}
