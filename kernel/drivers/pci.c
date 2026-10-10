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
#include <kernel/errno.h>
#include <kernel/cred.h>

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

unsigned pci_find_cap(struct pci_dev *d, uint8_t id) {
    if (!(pci_read16(d, 6) & 0x10)) return 0;
    int guard = 48;
    for (unsigned p = pci_read8(d, 0x34) & 0xfc; p && guard--; p = pci_read8(d, p + 1) & 0xfc)
        if (pci_read8(d, p) == id) return p;
    return 0;
}

int pci_msix_enable(struct pci_dev *d, unsigned entry, uint64_t addr, uint32_t data) {
    unsigned cap = pci_find_cap(d, 0x11);
    if (!cap) return -ENODEV;
    uint16_t ctrl = pci_read16(d, cap + 2);
    unsigned n = (ctrl & 0x7ff) + 1;
    if (entry >= n) return -EINVAL;
    uint32_t tbl = pci_read32(d, cap + 4);
    paddr_t pa = pci_bar(d, tbl & 7, nullptr);
    if (!pa) return -ENODEV;
    volatile uint32_t *t = vmm_map_mmio(pa + (tbl & ~7u), ALIGN_UP(n * 16, 4096));
    if (!t) return -ENOMEM;
    pci_write16(d, cap + 2, ctrl | 0xc000);            /* enable + function mask while programming */
    for (unsigned i = 0; i < n; i++) t[i * 4 + 3] = 1;  /* mask every vector */
    t[entry * 4 + 0] = (uint32_t)addr;
    t[entry * 4 + 1] = addr >> 32;
    t[entry * 4 + 2] = data;
    t[entry * 4 + 3] = 0;
    pci_write16(d, 4, pci_read16(d, 4) | 0x400);       /* INTx disable */
    pci_write16(d, cap + 2, (ctrl | 0x8000) & ~0x4000);
    return (int)n;
}

void pci_enable(struct pci_dev *d) { pci_write16(d, 4, pci_read16(d, 4) | 0x6); }


/* ---- M34: sysfs */
static struct device pci_root = { .name = "pci0000:00" };
static struct pci_dev *pdev(struct device *k) { return k->priv; }
static int a_vendor(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x%04x\n", pdev(k)->vendor); }
static int a_device(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x%04x\n", pdev(k)->device); }
static int a_class(struct device *k, char *b, size_t n) {
    struct pci_dev *d = pdev(k);
    return dev_printf(b, n, "0x%02x%02x%02x\n", d->class_code, d->subclass, d->prog_if);
}
static int a_svendor(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x%04x\n", pci_read16(pdev(k), 0x2c)); }
static int a_sdevice(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x%04x\n", pci_read16(pdev(k), 0x2e)); }
static int a_revision(struct device *k, char *b, size_t n) { return dev_printf(b, n, "0x%02x\n", pci_read8(pdev(k), 8)); }
static int a_irq(struct device *k, char *b, size_t n) { return dev_printf(b, n, "%u\n", pci_read8(pdev(k), 0x3c)); }
static int a_modalias(struct device *k, char *b, size_t n) {
    struct pci_dev *d = pdev(k);
    return dev_printf(b, n, "pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X\n", d->vendor, d->device, pci_read16(d, 0x2c),
                      pci_read16(d, 0x2e), d->class_code, d->subclass, d->prog_if);
}
/* config space: 4 KiB (ECAM); unprivileged readers see the 64-byte header, the rest reads 0 */
static ssize_t a_config(struct device *k, char *b, size_t n, uint64_t off) {
    if (!capable(CAP_SYS_ADMIN)) {                   /* as Linux: only the standard header */
        if (off >= 64) return 0;
        if (n > 64 - off) n = (size_t)(64 - off);
    }
    for (size_t i = 0; i < n; i++) b[i] = (char)pci_read8(pdev(k), (unsigned)(off + i));
    return (ssize_t)n;
}
static const struct dev_attr pci_attrs[] = {
    { "config", 0644, nullptr, nullptr, a_config, 4096 },
    { "vendor", 0444, a_vendor, nullptr }, { "device", 0444, a_device, nullptr }, { "class", 0444, a_class, nullptr },
    { "subsystem_vendor", 0444, a_svendor, nullptr }, { "subsystem_device", 0444, a_sdevice, nullptr },
    { "revision", 0444, a_revision, nullptr }, { "irq", 0444, a_irq, nullptr }, { "modalias", 0444, a_modalias, nullptr },
    { nullptr, 0, nullptr, nullptr },
};
static void pci_uevent(struct device *k, struct uenv *e) {
    struct pci_dev *d = pdev(k);
    uenv_add(e, "PCI_CLASS=%X", (unsigned)d->class_code << 16 | (unsigned)d->subclass << 8 | d->prog_if);
    uenv_add(e, "PCI_ID=%04X:%04X", d->vendor, d->device);
    uenv_add(e, "PCI_SUBSYS_ID=%04X:%04X", pci_read16(d, 0x2c), pci_read16(d, 0x2e));
    uenv_add(e, "PCI_SLOT_NAME=%s", k->name);
    char m[96];
    int l = a_modalias(k, m, sizeof m);
    if (l > 0 && m[l - 1] == '\n') m[l - 1] = 0;
    uenv_add(e, "MODALIAS=%s", m);
}

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
    if (ndevs) device_add(&pci_root);
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        struct device *k = &d->kdev;
        snprintf(k->name, sizeof k->name, "0000:%02x:%02x.%u", d->bus, d->dev, d->fn);
        k->parent = &pci_root; k->subsys = "pci"; k->bus = true;
        k->attrs = pci_attrs; k->uevent = pci_uevent; k->priv = d;
        device_add(k);
    }
}
