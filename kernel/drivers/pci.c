/*
 * PCI Express enumeration via ECAM. The base comes from the ACPI MCFG table (all three
 * architectures boot through UEFI/ACPI or provide MCFG under QEMU). Firmware has already
 * assigned BARs, so we only discover devices and read their resources.
 */
#include <kernel/pci.h>
#include <kernel/acpi.h>
#include <kernel/vmm.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>

#define MAX_PCI 64
static struct pci_dev devs[MAX_PCI];
static int ndevs;

struct mcfg_entry { uint64_t base; uint16_t seg; uint8_t bus_start, bus_end; uint32_t rsvd; } __attribute__((packed));

uint8_t pci_read8(struct pci_dev *d, unsigned off) { return d->cfg[off]; }
uint16_t pci_read16(struct pci_dev *d, unsigned off) { return *(volatile uint16_t *)(d->cfg + off); }
uint32_t pci_read32(struct pci_dev *d, unsigned off) { return *(volatile uint32_t *)(d->cfg + off); }
void pci_write16(struct pci_dev *d, unsigned off, uint16_t v) { *(volatile uint16_t *)(d->cfg + off) = v; }
void pci_write32(struct pci_dev *d, unsigned off, uint32_t v) { *(volatile uint32_t *)(d->cfg + off) = v; }

int pci_count(void) { return ndevs; }
struct pci_dev *pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : nullptr; }
struct pci_dev *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device) return &devs[i];
    return nullptr;
}

paddr_t pci_bar(struct pci_dev *d, int bar, uint64_t *size) {
    unsigned off = 0x10 + bar * 4;
    uint32_t lo = pci_read32(d, off);
    if (lo & 1) return 0;                               /* I/O space */
    bool is64 = ((lo >> 1) & 3) == 2;
    uint64_t addr = lo & ~0xfull;
    if (is64) addr |= (uint64_t)pci_read32(d, off + 4) << 32;
    if (size) {
        uint16_t cmd = pci_read16(d, 4);
        pci_write16(d, 4, cmd & ~3);                    /* disable decode while sizing */
        pci_write32(d, off, 0xffffffff);
        uint64_t m = pci_read32(d, off) & ~0xfull;
        pci_write32(d, off, lo);
        if (is64) {
            uint32_t hi = pci_read32(d, off + 4);
            pci_write32(d, off + 4, 0xffffffff);
            m |= (uint64_t)pci_read32(d, off + 4) << 32;
            pci_write32(d, off + 4, hi);
        } else m |= 0xffffffff00000000ull;
        pci_write16(d, 4, cmd);
        *size = m ? ~m + 1 : 0;
    }
    return addr;
}

void pci_enable(struct pci_dev *d) { pci_write16(d, 4, pci_read16(d, 4) | 0x6); }


void pci_init(void) {
    uint8_t *mcfg = acpi_find_table("MCFG");
    if (!mcfg) { pr_info("pci: no MCFG table\n"); return; }
    uint32_t len = *(uint32_t *)(mcfg + 4);
    struct mcfg_entry *e = (struct mcfg_entry *)(mcfg + 44);
    int nent = (len - 44) / sizeof *e;
    for (int i = 0; i < nent; i++, e++) {
        if (e->seg != 0) continue;
        unsigned last = MIN((unsigned)e->bus_end, e->bus_start + 7u);   /* QEMU uses few buses */
        for (unsigned bus = e->bus_start; bus <= last; bus++) {
            paddr_t bpa = e->base + ((uint64_t)(bus - e->bus_start) << 20);
            uint8_t *bva = vmm_map_mmio(bpa, 1 << 20);
            if (!bva) continue;
            for (unsigned dev = 0; dev < 32; dev++) {
                for (unsigned fn = 0; fn < 8; fn++) {
                    volatile uint8_t *cfg = bva + (dev << 15) + (fn << 12);
                    uint16_t vendor = *(volatile uint16_t *)cfg;
                    if (vendor == 0xffff) { if (fn == 0) break; continue; }
                    if (ndevs < MAX_PCI) {
                        struct pci_dev *d = &devs[ndevs++];
                        d->bus = bus; d->dev = dev; d->fn = fn; d->cfg = cfg;
                        d->vendor = vendor;
                        d->device = *(volatile uint16_t *)(cfg + 2);
                        d->prog_if = cfg[9]; d->subclass = cfg[10]; d->class_code = cfg[11];
                        pr_info("pci: %02x:%02x.%u %04x:%04x class %02x%02x\n", bus, dev, fn,
                                d->vendor, d->device, d->class_code, d->subclass);
                    }
                    if (fn == 0 && !(cfg[0xe] & 0x80)) break;  /* single function */
                }
            }
        }
    }
}
