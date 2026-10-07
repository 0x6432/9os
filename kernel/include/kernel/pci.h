#pragma once
/* PCI(e) configuration space access through ECAM (ACPI MCFG). */
#include <kernel/types.h>

struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    volatile uint8_t *cfg;          /* mapped 4 KiB config space */
};

void pci_init(void);
int pci_count(void);
struct pci_dev *pci_get(int i);
struct pci_dev *pci_find(uint16_t vendor, uint16_t device);
uint8_t pci_read8(struct pci_dev *d, unsigned off);
uint16_t pci_read16(struct pci_dev *d, unsigned off);
uint32_t pci_read32(struct pci_dev *d, unsigned off);
void pci_write16(struct pci_dev *d, unsigned off, uint16_t v);
void pci_write32(struct pci_dev *d, unsigned off, uint32_t v);
/* Physical address of a memory BAR (handles 64-bit BARs); 0 if unassigned or I/O. */
paddr_t pci_bar(struct pci_dev *d, int bar, uint64_t *size);
void pci_enable(struct pci_dev *d);   /* memory decode + bus mastering */

/* M28: interrupts */
unsigned pci_find_cap(struct pci_dev *d, uint8_t id);     /* config offset, 0 if absent */
/* program MSI-X table entry `entry` with addr/data, unmask it and enable MSI-X (INTx off);
 * returns the table size or a negative errno */
int pci_msix_enable(struct pci_dev *d, unsigned entry, uint64_t addr, uint32_t data);
