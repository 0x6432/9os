/* virtio-pci modern transport: capability discovery, feature negotiation, split virtqueues. */
#include <kernel/virtio.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/vmm.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <kernel/irq.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>

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
        if (type < CAP_COMMON || type > CAP_DEVICE) continue;
        if (type == CAP_ISR && v->isr) continue;
        paddr_t pa = pci_bar(d, bar, nullptr);
        if (!pa) continue;
        volatile uint8_t *va = vmm_map_mmio(pa + off, MAX(len, 4096u));
        if (type == CAP_COMMON) v->common = (volatile struct virtio_common *)va;
        else if (type == CAP_DEVICE) v->devcfg = va;
        else if (type == CAP_ISR) v->isr = va;
        else { v->notify_base = va; v->notify_mult = pci_read32(d, p + 16); }
    }
    return v->common && v->notify_base;
}

bool virtio_pci_probe(struct virtio_dev *v, struct pci_dev *d, const char *who) {
    return virtio_pci_probe_features(v, d, who, 0, nullptr);
}

static int a_vdevice(struct device *k, char *b, size_t n) {
    struct pci_dev *d = k->priv;
    return dev_printf(b, n, "0x%04x\n", d->device >= 0x1040 ? d->device - 0x1040 : pci_read16(d, 0x2e));
}
static int a_vvendor(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x%04x\n", ((struct pci_dev *)k->priv)->vendor); }
static const struct dev_attr virtio_attrs[] = {
    { "device", 0444, a_vdevice, nullptr }, { "vendor", 0444, a_vvendor, nullptr }, { nullptr, 0, nullptr, nullptr },
};
static void virtio_uevent(struct device *k, struct uenv *e) {
    char b[16];
    int l = a_vdevice(k, b, sizeof b);
    b[l > 0 ? l - 1 : 0] = 0;
    unsigned id = 0;
    for (const char *c = b + 2; *c; c++) id = id * 16 + (unsigned)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
    uenv_add(e, "MODALIAS=virtio:d%08Xv%08X", id, ((struct pci_dev *)k->priv)->vendor);
}
static int nvirtio;

bool virtio_pci_probe_features(struct virtio_dev *v, struct pci_dev *d, const char *who,
                               uint32_t want, uint32_t *got) {
    v->pci = d;
    v->kdev = kzalloc(sizeof *v->kdev);
    if (v->kdev) {
        struct device *k = v->kdev;
        snprintf(k->name, sizeof k->name, "virtio%d", nvirtio++);
        k->parent = &d->kdev; k->subsys = "virtio"; k->bus = true;
        k->attrs = virtio_attrs; k->uevent = virtio_uevent; k->priv = d;
        static const char *const drv[][2] = { { "virtio-net", "virtio_net" }, { "virtio-blk", "virtio_blk" },
                                              { "virtio-input", "virtio_input" }, { "virtio-gpu", "virtio_gpu" }, { "virtio-snd", "virtio_snd" } };
        for (size_t i = 0; i < ARRAY_SIZE(drv); i++) if (!strcmp(who, drv[i][0])) k->driver = drv[i][1];
        if (!d->kdev.driver) { d->kdev.driver = "virtio-pci"; device_uevent(&d->kdev, "bind"); }
        device_add(k);
    }
    v->msix_vec = 0xffff;
    v->irq = -1;
    v->irq_mode = "polled";
    pci_enable(d);
    if (!find_caps(v, d)) { pr_err("%s: missing virtio-pci capabilities\n", who); return false; }
    volatile struct virtio_common *c = v->common;
    c->device_status = 0;
    while (c->device_status) arch_cpu_relax();
    c->device_status = 1 | 2;                      /* ACKNOWLEDGE | DRIVER */
    c->device_feature_select = 0;
    uint32_t have = c->device_feature & want;
    if (got) *got = have;
    c->driver_feature_select = 0; c->driver_feature = have;
    c->driver_feature_select = 1; c->driver_feature = 1;   /* VIRTIO_F_VERSION_1 */
    c->device_status = 1 | 2 | 8;                  /* FEATURES_OK */
    if (!(c->device_status & 8)) { pr_err("%s: features rejected\n", who); return false; }
    return true;
}

static int virtio_hard(void *arg) {
    struct virtio_dev *v = arg;
    if (v->msix_vec == 0xffff) {                /* shared level line: is it ours? (read acks) */
        if (!v->isr || !(*v->isr & 3)) return IRQ_NONE;
    }
    return v->hard ? v->hard(v->ctx) : IRQ_WAKE_THREAD;
}

bool virtio_irq_setup(struct virtio_dev *v, const char *name, int (*hard)(void *ctx),
                      void (*thread_fn)(void *ctx), void *ctx) {
    v->hard = hard; v->thread_fn = thread_fn; v->ctx = ctx;
    uint64_t addr; uint32_t data;
    if (pci_find_cap(v->pci, 0x11)) {
        int vec = irq_request_msi(name, virtio_hard, thread_fn, v, &addr, &data);
        if (vec >= 0 && pci_msix_enable(v->pci, 0, addr, data) > 0) {
            v->msix_vec = 0;
            v->common->msix_config = 0xffff;
            v->irq = vec; v->irq_mode = "MSI-X";
            return true;
        }
    }
    int line = arch_pci_intx_line(v->pci);
    if (line >= 0 && v->isr && irq_request(line, name, virtio_hard, thread_fn, v) >= 0) {
        v->irq = line; v->irq_mode = "INTx";
        return true;
    }
    return false;
}

bool virtq_init(struct virtio_dev *v, struct virtq *q, unsigned index, unsigned size) {
    volatile struct virtio_common *c = v->common;
    c->queue_select = index;
    unsigned max = c->queue_size;
    if (!max) return false;
    if (size > max) size = max;
    if (size > VQ_MAX) size = VQ_MAX;
    /* desc[size] | avail (6 + 2 size) | used (6 + 8 size, 4-aligned): one page up to 128 */
    unsigned aoff = 16 * size, uoff = (aoff + 6 + 2 * size + 3) & ~3u;
    paddr_t qpa = pmm_alloc_zeroed(0);
    if (!qpa) return false;
    uint8_t *m = PHYS_TO_VIRT(qpa);
    q->size = size; q->index = index;
    q->desc = (struct vq_desc *)m; q->avail = (uint16_t *)(m + aoff); q->used = (uint16_t *)(m + uoff);
    q->avail_idx = q->used_seen = 0;
    c->queue_size = size;
    c->queue_desc_lo = (uint32_t)qpa; c->queue_desc_hi = qpa >> 32;
    c->queue_driver_lo = (uint32_t)(qpa + aoff); c->queue_driver_hi = (qpa + aoff) >> 32;
    c->queue_device_lo = (uint32_t)(qpa + uoff); c->queue_device_hi = (qpa + uoff) >> 32;
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)c->queue_notify_off * v->notify_mult);
    if (v->msix_vec != 0xffff) {
        c->queue_msix_vector = v->msix_vec;
        if (c->queue_msix_vector != v->msix_vec) pr_warn("virtio: queue %u: no MSI-X vector\n", index);
    }
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
