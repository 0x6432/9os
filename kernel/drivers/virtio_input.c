/*
 * virtio-input (keyboard, mouse, tablet) → evdev. The device describes itself through its
 * config space (name, ids, event-type bitmaps, absolute axis ranges), which maps 1:1 onto
 * struct input_dev. Events arrive as {le16 type, code; le32 value} in device-writable buffers
 * on queue 0. M28: the device interrupt (MSI-X or INTx) wakes a per-device threaded handler
 * that drains the used ring; devices without an interrupt fall back to a polling thread.
 */
#include <kernel/virtio.h>
#include <kernel/input.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/vmm.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/arch.h>

#define NBUF 64
enum { CFG_ID_NAME = 1, CFG_ID_SERIAL, CFG_ID_DEVIDS, CFG_PROP_BITS = 0x10, CFG_EV_BITS, CFG_ABS_INFO };

struct vinput {
    struct virtio_dev v;
    struct virtq eq;
    struct { uint16_t type, code; uint32_t value; } *ev;
    paddr_t ev_pa;
    struct input_dev in;
    int ready;
};
static struct vinput *vdevs[8];
static int nvdevs;

static unsigned cfg_query(struct vinput *d, unsigned sel, unsigned sub, void *out, size_t max) {
    volatile uint8_t *c = d->v.devcfg;
    c[0] = sel; c[1] = sub;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned n = MIN((unsigned)c[2], (unsigned)max);
    for (unsigned i = 0; i < n; i++) ((uint8_t *)out)[i] = c[8 + i];
    return n;
}

static void query_bits(struct vinput *d, unsigned ev, uint64_t *bm, size_t bytes) {
    if (cfg_query(d, CFG_EV_BITS, ev, bm, bytes)) input_set_bit(d->in.evbit, ev);
}

static void drain(void *arg) {
    struct vinput *d = arg;
    if (!__atomic_load_n(&d->ready, __ATOMIC_ACQUIRE)) return;
    uint32_t id;
    uint64_t fl = arch_irq_save();      /* console translation expects irq context */
    while (virtq_pop(&d->eq, &id, nullptr)) {
        if (id < NBUF) {
            input_event(&d->in, d->ev[id].type, d->ev[id].code, (int32_t)d->ev[id].value);
            virtq_push(&d->eq, (uint16_t)id);
        }
    }
    arch_irq_restore(fl);
}

static void poll_thread(void *arg) {
    for (;;) {
        for (int i = 0; i < nvdevs; i++) if (vdevs[i]->v.irq < 0) drain(vdevs[i]);
        sleep_ns(4 * 1000000ull);
    }
}

static void probe(struct pci_dev *pd) {
    struct vinput *d = kzalloc(sizeof *d);
    if (!d || nvdevs >= (int)ARRAY_SIZE(vdevs)) return;
    if (!virtio_pci_probe(&d->v, pd, "virtio-input") || !d->v.devcfg) { kfree(d); return; }
    char nm[16];
    snprintf(nm, sizeof nm, "vinput%d", nvdevs);
    virtio_irq_setup(&d->v, nm, nullptr, drain, d);
    if (!virtq_init(&d->v, &d->eq, 0, NBUF)) { pr_err("virtio-input: no event queue\n"); return;   /* irq handler still references d */ }
    struct input_dev *in = &d->in;
    cfg_query(d, CFG_ID_NAME, 0, in->name, sizeof in->name - 1);
    struct { uint16_t bus, vendor, product, version; } ids = { BUS_VIRTUAL, 0x0627, 0, 1 };
    cfg_query(d, CFG_ID_DEVIDS, 0, &ids, sizeof ids);
    in->id = (struct input_id){ ids.bus, ids.vendor, ids.product, ids.version };
    snprintf(in->phys, sizeof in->phys, "virtio%d/input0", nvdevs);
    cfg_query(d, CFG_PROP_BITS, 0, in->propbit, sizeof in->propbit);
    query_bits(d, EV_KEY, in->keybit, sizeof in->keybit);
    query_bits(d, EV_REL, in->relbit, sizeof in->relbit);
    query_bits(d, EV_ABS, in->absbit, sizeof in->absbit);
    query_bits(d, EV_MSC, in->mscbit, sizeof in->mscbit);
    query_bits(d, EV_LED, in->ledbit, sizeof in->ledbit);
    uint64_t rep[1] = { 0 };
    if (cfg_query(d, CFG_EV_BITS, EV_REP, rep, sizeof rep)) input_set_bit(in->evbit, EV_REP);
    for (unsigned a = 0; a <= ABS_MAX; a++) {
        if (!input_test_bit(in->absbit, a)) continue;
        struct { uint32_t min, max, fuzz, flat, res; } ai = { 0 };
        cfg_query(d, CFG_ABS_INFO, a, &ai, sizeof ai);
        in->abs[a] = (struct input_absinfo){ 0, (int32_t)ai.min, (int32_t)ai.max, (int32_t)ai.fuzz, (int32_t)ai.flat, (int32_t)ai.res };
    }
    in->console_keys = input_test_bit(in->keybit, 30 /* KEY_A */);

    d->ev_pa = pmm_alloc_zeroed(0);
    d->ev = PHYS_TO_VIRT(d->ev_pa);
    for (unsigned i = 0; i < d->eq.size; i++) {
        d->eq.desc[i] = (struct vq_desc){ d->ev_pa + i * 8, 8, VQ_WRITE, 0 };
        d->eq.avail[2 + i] = i;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->eq.avail_idx = d->eq.size;
    d->eq.avail[1] = d->eq.avail_idx;
    virtio_driver_ok(&d->v);
    *d->eq.notify = 0;
    input_register(in);
    pr_info("virtio-input: %s (%s irq %d)\n", in->name, d->v.irq_mode, d->v.irq);
    __atomic_store_n(&d->ready, 1, __ATOMIC_RELEASE);
    vdevs[nvdevs++] = d;
}

void virtio_input_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *pd = pci_get(i);
        if (pd->vendor == VIRTIO_VENDOR && pd->device == VIRTIO_DEV_INPUT) probe(pd);
    }
    bool polled = false;
    for (int i = 0; i < nvdevs; i++) if (vdevs[i]->v.irq < 0) polled = true;
    if (polled) thread_create("vinput-poll", poll_thread, nullptr);
}
