#pragma once
/* virtio over the PCI "modern" (1.0+) transport, polled split virtqueues. */
#include <kernel/types.h>
#include <kernel/pci.h>

#define VIRTIO_VENDOR 0x1af4
#define VIRTIO_DEV_GPU   0x1050
#define VIRTIO_DEV_INPUT 0x1052

struct virtio_common {
    uint32_t device_feature_select, device_feature, driver_feature_select, driver_feature;
    uint16_t msix_config, num_queues;
    uint8_t device_status, config_generation;
    uint16_t queue_select, queue_size, queue_msix_vector, queue_enable, queue_notify_off;
    uint32_t queue_desc_lo, queue_desc_hi, queue_driver_lo, queue_driver_hi, queue_device_lo, queue_device_hi;
};   /* naturally aligned; not packed so every field is a single MMIO access of its own width */

struct vq_desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define VQ_NEXT  1
#define VQ_WRITE 2
#define VQ_MAX   64            /* desc/avail/used rings of a queue fit in one page */

struct virtq {
    uint16_t size, index;
    struct vq_desc *desc;
    volatile uint16_t *avail;  /* flags, idx, ring[size] */
    volatile uint16_t *used;   /* flags, idx, {u32 id, u32 len}[size] */
    uint16_t avail_idx, used_seen;
    volatile uint16_t *notify;
};

struct virtio_dev {
    struct pci_dev *pci;
    volatile struct virtio_common *common;
    volatile uint8_t *notify_base;
    uint32_t notify_mult;
    volatile uint8_t *devcfg;   /* device-specific configuration */
};

/* reset, ACKNOWLEDGE|DRIVER, negotiate VIRTIO_F_VERSION_1 only, FEATURES_OK */
bool virtio_pci_probe(struct virtio_dev *v, struct pci_dev *d, const char *who);
bool virtq_init(struct virtio_dev *v, struct virtq *q, unsigned index, unsigned size);
void virtio_driver_ok(struct virtio_dev *v);
/* make descriptor chain `head` available and notify the device */
void virtq_push(struct virtq *q, uint16_t head);
/* next used element, if any */
bool virtq_pop(struct virtq *q, uint32_t *id, uint32_t *len);
