/*
 * virtio-net (M32) over virtio-pci: one receive queue (0) and one transmit queue (1), no
 * offloads (no checksum/TSO/merged buffers), so every frame is a 12-byte virtio_net_hdr_v1
 * plus at most 1514 bytes. Receive and transmit buffers are fixed 2 KiB DMA slots: received
 * frames are copied into a packet buffer and the slot is reposted at once; frames to send are
 * copied into a free transmit slot. The copies cost little at VM link speeds and keep packet
 * buffers ordinary slab memory with no device ownership rules.
 *
 * Locking: d->lock (LR_NETDRV, IRQ-safe) guards both rings and the slot lists. Transmission
 * runs under net_mutex (outer); the threaded interrupt handler reaps both queues, allocates
 * packets with the lock dropped and passes them to net_rx().
 */
#include <kernel/virtio.h>
#include <kernel/net.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <kernel/errno.h>

#define VIRTIO_DEV_NET 0x1041
#define F_MTU    (1u << 3)
#define F_MAC    (1u << 5)
#define F_STATUS (1u << 16)
#define HDR_LEN 12
#define SLOT 2048
#define NSLOTS 128            /* per queue */

struct vnet {
    struct virtio_dev v;
    struct virtq rxq, txq;
    spinlock_t lock;
    struct netdev *dev;
    uint8_t *rxbuf, *txbuf;   /* NSLOTS * SLOT each, physically contiguous */
    paddr_t rxpa, txpa;
    uint16_t txfree[NSLOTS];
    unsigned ntxfree;
    uint64_t irqs;
};
static const struct lock_class drv_class = { "virtio_net", LR_NETDRV, false };
static struct vnet *vnets[4];
static int nvnets;

static void post_rx(struct vnet *d, uint16_t slot) {      /* lock held */
    d->rxq.desc[slot] = (struct vq_desc){ d->rxpa + (paddr_t)slot * SLOT, SLOT, VQ_WRITE, 0 };
    virtq_push(&d->rxq, slot);
}

static void reap_tx(struct vnet *d) {                      /* lock held */
    uint32_t id;
    while (virtq_pop(&d->txq, &id, nullptr))
        if (id < NSLOTS) d->txfree[d->ntxfree++] = (uint16_t)id;
}

static void vnet_xmit(struct netdev *nd, struct pkt *p) {
    struct vnet *d = nd->priv;
    if (p->len > SLOT - HDR_LEN) { nd->tx_dropped++; pkt_free(p); return; }
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&d->lock);
    reap_tx(d);
    if (!d->ntxfree) {
        spin_unlock(&d->lock); arch_irq_restore(f);
        nd->tx_dropped++;
        pkt_free(p);
        return;
    }
    uint16_t s = d->txfree[--d->ntxfree];
    uint8_t *b = d->txbuf + (size_t)s * SLOT;
    memset(b, 0, HDR_LEN);
    memcpy(b + HDR_LEN, p->data, p->len);
    size_t len = p->len + HDR_LEN;
    if (len < 60 + HDR_LEN) { memset(b + len, 0, 60 + HDR_LEN - len); len = 60 + HDR_LEN; }   /* pad short frames */
    d->txq.desc[s] = (struct vq_desc){ d->txpa + (paddr_t)s * SLOT, (uint32_t)len, 0, 0 };
    virtq_push(&d->txq, s);
    spin_unlock(&d->lock);
    arch_irq_restore(f);
    nd->tx_packets++;
    nd->tx_bytes += p->len;
    pkt_free(p);
}

static void vnet_reap(void *arg) {
    struct vnet *d = arg;
    d->irqs++;
    for (;;) {
        uint32_t ids[32], lens[32];
        int n = 0;
        uint64_t f = arch_irq_save();
        spin_lock_ipi(&d->lock);
        reap_tx(d);
        while (n < 32 && virtq_pop(&d->rxq, &ids[n], &lens[n])) if (ids[n] < NSLOTS) n++;
        spin_unlock(&d->lock);
        arch_irq_restore(f);
        if (!n) break;
        struct pkt *pk[32];
        for (int i = 0; i < n; i++) {                       /* slots stay ours until reposted */
            pk[i] = nullptr;
            if (lens[i] <= HDR_LEN || lens[i] > SLOT) continue;
            size_t len = lens[i] - HDR_LEN;
            pk[i] = pkt_alloc_rx(len);
            if (pk[i]) memcpy(pk[i]->data, d->rxbuf + (size_t)ids[i] * SLOT + HDR_LEN, len);
            else d->dev->rx_dropped++;
        }
        f = arch_irq_save();
        spin_lock_ipi(&d->lock);
        for (int i = 0; i < n; i++) post_rx(d, (uint16_t)ids[i]);
        spin_unlock(&d->lock);
        arch_irq_restore(f);
        for (int i = 0; i < n; i++) if (pk[i]) net_rx(d->dev, pk[i]);
    }
}

static void poll_thread(void *arg) {
    for (;;) {
        for (int i = 0; i < nvnets; i++) if (vnets[i]->v.irq < 0) vnet_reap(vnets[i]);
        sleep_ns(1000000ull);
    }
}

static void probe(struct pci_dev *pd) {
    if (nvnets >= (int)ARRAY_SIZE(vnets)) return;
    struct vnet *d = kzalloc(sizeof *d);
    if (!d) return;
    uint32_t feat = 0;
    if (!virtio_pci_probe_features(&d->v, pd, "virtio-net", F_MAC | F_MTU | F_STATUS, &feat) || !d->v.devcfg) { kfree(d); return; }
    spin_lock_init_class(&d->lock, &drv_class);
    char nm[16];
    snprintf(nm, sizeof nm, "eth%d", nvnets);
    virtio_irq_setup(&d->v, nm, nullptr, vnet_reap, d);
    if (!virtq_init(&d->v, &d->rxq, 0, NSLOTS) || !virtq_init(&d->v, &d->txq, 1, NSLOTS)) { pr_err("virtio-net: no queues\n"); return; }
    unsigned order = 0;
    while ((PAGE_SIZE << order) < NSLOTS * SLOT) order++;
    d->rxpa = pmm_alloc_zeroed(order);
    d->txpa = pmm_alloc_zeroed(order);
    if (!d->rxpa || !d->txpa) { pr_err("virtio-net: out of memory\n"); return; }
    d->rxbuf = PHYS_TO_VIRT(d->rxpa);
    d->txbuf = PHYS_TO_VIRT(d->txpa);
    for (unsigned i = 0; i < d->txq.size; i++) d->txfree[d->ntxfree++] = (uint16_t)i;
    uint8_t mac[6];
    volatile uint8_t *cfg = d->v.devcfg;
    if (feat & F_MAC) for (int i = 0; i < 6; i++) mac[i] = cfg[i];
    else { uint64_t r = random_u64(); memcpy(mac, &r, 6); mac[0] = (mac[0] & 0xfe) | 0x02; }
    int mtu = 1500;
    if (feat & F_MTU) { int m = *(volatile uint16_t *)(cfg + 10); if (m >= 68 && m < 1500) mtu = m; }
    virtio_driver_ok(&d->v);
    uint64_t f = arch_irq_save();
    spin_lock_ipi(&d->lock);
    for (unsigned i = 0; i < d->rxq.size; i++) post_rx(d, (uint16_t)i);
    spin_unlock(&d->lock);
    arch_irq_restore(f);
    d->dev = netdev_register(nm, ARPHRD_ETHER, mac, mtu, vnet_xmit, d);
    if (!d->dev) return;
    vnets[nvnets++] = d;
    pr_info("virtio-net: %s: %02x:%02x:%02x:%02x:%02x:%02x mtu %d, %s irq %d\n", nm, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            mtu, d->v.irq_mode, d->v.irq);
}

void virtio_net_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *pd = pci_get(i);
        if (pd->vendor == VIRTIO_VENDOR && (pd->device == VIRTIO_DEV_NET || pd->device == 0x1000)) probe(pd);
    }
    bool polled = false;
    for (int i = 0; i < nvnets; i++) if (vnets[i]->v.irq < 0) polled = true;
    if (polled) thread_create("vnet-poll", poll_thread, nullptr);
}
