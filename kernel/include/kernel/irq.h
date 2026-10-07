#pragma once
/*
 * Interrupts. Low level (arch): vectors/GSIs routed to irq_handler_t, called with IRQs off
 * under the BKL. M28 adds a generic layer on top (core/irq.c):
 *  - irq_request(): a (shareable, level-triggered) interrupt line — GSI on x86 (IO-APIC), SPI
 *    INTID on aarch64 (GICv2/v3), PLIC source on riscv64 — with a hard handler (runs in IRQ
 *    context, must quiet the device and return IRQ_NONE/HANDLED/WAKE_THREAD) and an optional
 *    thread handler run by a dedicated kernel thread "irq/N-name";
 *  - irq_request_msi(): the same on a message-signalled vector (x86 MSI-X), returns the
 *    address/data pair to program into the device;
 *  - irq_work: a callback raised from any context (even with spinlocks held, e.g. from the
 *    console) that runs from a self-IPI once interrupts are enabled again;
 *  - /proc/interrupts.
 */
#include <kernel/types.h>
struct trap_frame;
struct pci_dev;
typedef void (*irq_handler_t)(struct trap_frame *f, void *ctx);
void irq_register_vector(int vector, irq_handler_t h, void *ctx);
/* Route a legacy/global interrupt line to a handler. Returns the vector used. */
int irq_install(int gsi, irq_handler_t h, void *ctx);
void irq_eoi(void);

enum { IRQ_NONE = 0, IRQ_HANDLED = 1, IRQ_WAKE_THREAD = 2 };
typedef int (*irq_hard_t)(void *ctx);
typedef void (*irq_thread_t)(void *ctx);
int irq_request(int line, const char *name, irq_hard_t hard, irq_thread_t thread_fn, void *ctx);
int irq_request_msi(const char *name, irq_hard_t hard, irq_thread_t thread_fn, void *ctx,
                    uint64_t *addr, uint32_t *data);
int irq_proc_read(char *buf, size_t max);

/* arch hooks */
const char *arch_irq_chip(void);
/* allocate a message-signalled vector: -ENODEV if the platform has no MSI support */
int arch_msi_alloc(irq_handler_t h, void *ctx, uint64_t *addr, uint32_t *data);
/* interrupt line of a PCI function's INTx pin, -1 if none / unknown */
int arch_pci_intx_line(struct pci_dev *d);

struct irq_work {
    void (*fn)(struct irq_work *w);
    struct irq_work *next;
    int pending;
};
void irq_work_queue(struct irq_work *w);
void irq_work_run(void);    /* from the IPI vector, the timer tick and the idle loop */
void irq_work_enable(void); /* after SMP/IPI setup: start raising self-IPIs */
void ipi_irq(void);     /* every arch's IPI vector */
void ipi_service(void); /* the same without accounting (idle loop) */
