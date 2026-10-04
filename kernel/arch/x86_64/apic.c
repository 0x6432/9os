/* Local APIC, I/O APIC, legacy PIC masking, TSC time source, LAPIC timer. */
#include <kernel/types.h>
#include <kernel/acpi.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/vmm.h>
#include <kernel/time.h>
#include <kernel/boot.h>
#include <kernel/cpu.h>
#include <arch/cpu.h>
#include <arch/trapframe.h>

#define LAPIC_ID      0x020
#define LAPIC_EOI     0x0b0
#define LAPIC_SVR     0x0f0
#define LAPIC_LVT_TMR 0x320
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR 0x390
#define LAPIC_TMR_DIV 0x3e0
#define LAPIC_ICR_LO  0x300
#define LAPIC_ICR_HI  0x310
#define LAPIC_TPR     0x080

#define VEC_TIMER    32
#define VEC_IRQ_BASE 48
#define VEC_SPURIOUS 0xff
#define VEC_IPI      0xf0

static volatile uint32_t *lapic;
static uint32_t lapic_per_tick;
static uint32_t bsp_lapic_id;
static uint64_t tsc_khz;
static uint64_t tsc_boot;

struct ioapic { volatile uint32_t *base; uint32_t gsi_base, count; };
static struct ioapic ioapics[8];
static int nioapics;
struct iso { uint8_t source; uint32_t gsi; uint16_t flags; };
static struct iso isos[16];
static int nisos;

static inline uint32_t lapic_read(uint32_t r) { return lapic[r / 4]; }
static inline void lapic_write(uint32_t r, uint32_t v) { lapic[r / 4] = v; }
void irq_eoi(void) { lapic_write(LAPIC_EOI, 0); }

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg) { io->base[0] = reg; return io->base[4]; }
static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t v) { io->base[0] = reg; io->base[4] = v; }

uint64_t time_ns(void) {
    if (!tsc_khz) return 0;
    uint64_t d = rdtsc() - tsc_boot;
    return (d / tsc_khz) * 1000000 + ((d % tsc_khz) * 1000000) / tsc_khz;
}

/* Measure the TSC against PIT channel 2 over ~10 ms. */
static void calibrate_tsc(void) {
    const uint16_t count = 11932;           /* 1193182 Hz / 100 */
    outb(0x61, (inb(0x61) & ~0x02) | 0x01); /* gate on, speaker off */
    outb(0x43, 0xb0);                       /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, count & 0xff);
    outb(0x42, count >> 8);
    uint64_t t0 = rdtsc();
    while (!(inb(0x61) & 0x20)) arch_cpu_relax();
    uint64_t t1 = rdtsc();
    tsc_khz = (t1 - t0) / 10;
    tsc_boot = t0;
    pr_info("tsc: %lu.%03lu MHz\n", tsc_khz / 1000, tsc_khz % 1000);
}

static void pic_disable(void) {
    outb(0x20, 0x11); outb(0xa0, 0x11);
    outb(0x21, 0x20); outb(0xa1, 0x28);
    outb(0x21, 4); outb(0xa1, 2);
    outb(0x21, 1); outb(0xa1, 1);
    outb(0x21, 0xff); outb(0xa1, 0xff);
}

struct [[gnu::packed]] madt { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6], oemt[8];
                              uint32_t oemrev, creator, crev; uint32_t lapic_addr, flags; uint8_t entries[]; };

static void parse_madt(void) {
    struct madt *m = acpi_find_table("APIC");
    paddr_t lapic_pa = 0xfee00000;
    if (!m) { pr_warn("apic: no MADT, assuming defaults\n"); }
    else {
        lapic_pa = m->lapic_addr;
        uint8_t *p = m->entries, *end = (uint8_t *)m + m->len;
        while (p < end) {
            uint8_t type = p[0], len = p[1];
            if (len == 0) break;
            if (type == 1 && nioapics < 8) {
                struct ioapic *io = &ioapics[nioapics++];
                io->base = vmm_map_mmio(*(uint32_t *)(p + 4), 0x20);
                io->gsi_base = *(uint32_t *)(p + 8);
                io->count = ((ioapic_read(io, 1) >> 16) & 0xff) + 1;
            } else if (type == 2 && nisos < 16) {
                isos[nisos++] = (struct iso){ p[3], *(uint32_t *)(p + 4), *(uint16_t *)(p + 8) };
            } else if (type == 5) {
                lapic_pa = *(uint64_t *)(p + 4);
            }
            p += len;
        }
    }
    lapic = vmm_map_mmio(lapic_pa, 0x1000);
    pr_info("apic: lapic %lx, %d ioapic(s), %d overrides\n", lapic_pa, nioapics, nisos);
}

static int next_vector = VEC_IRQ_BASE;

/* Route a GSI (or ISA IRQ, translated through overrides) to a fresh vector. */
int irq_install(int irq, irq_handler_t h, void *ctx) {
    uint32_t gsi = irq;
    uint16_t flags = 0;
    bool isa = irq < 16;
    for (int i = 0; i < nisos; i++)
        if (isos[i].source == irq) { gsi = isos[i].gsi; flags = isos[i].flags; break; }
    for (int i = 0; i < nioapics; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi < io->gsi_base || gsi >= io->gsi_base + io->count) continue;
        if (next_vector >= 0xf0) return -1;
        int vec = next_vector++;
        irq_register_vector(vec, h, ctx);
        uint64_t ent = vec;
        bool active_low = isa ? ((flags & 3) == 3) : true;
        bool level = isa ? (((flags >> 2) & 3) == 3) : true;
        if (active_low) ent |= 1 << 13;
        if (level) ent |= 1 << 15;
        uint32_t dest = bsp_lapic_id;      /* device interrupts go to the boot CPU */
        ent |= (uint64_t)dest << 56;
        uint32_t idx = gsi - io->gsi_base;
        ioapic_write(io, 0x10 + idx * 2 + 1, ent >> 32);
        ioapic_write(io, 0x10 + idx * 2, (uint32_t)ent);
        return vec;
    }
    pr_warn("irq: no ioapic for gsi %u\n", gsi);
    return -1;
}

static void timer_irq(struct trap_frame *f, void *ctx) {
    irq_eoi();
    timer_tick();
}
static void spurious_irq(struct trap_frame *f, void *ctx) {}

uint32_t lapic_id(void) { return lapic_read(LAPIC_ID) >> 24; }

void arch_send_ipi(struct cpu *c) {
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12)) arch_cpu_relax();   /* delivery pending */
    lapic_write(LAPIC_ICR_HI, (uint32_t)c->hwid << 24);
    lapic_write(LAPIC_ICR_LO, VEC_IPI);                                /* fixed, physical */
}

/* Called on every CPU: enable the LAPIC and start its periodic tick. */
void lapic_cpu_init(void) {
    wrmsr(0x1b, rdmsr(0x1b) | (1 << 11));
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, VEC_SPURIOUS | 0x100);
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, VEC_TIMER | (1 << 17));  /* periodic */
    lapic_write(LAPIC_TMR_INIT, lapic_per_tick);
}

void apic_init(void) {
    calibrate_tsc();
    pic_disable();
    parse_madt();
    wrmsr(0x1b, rdmsr(0x1b) | (1 << 11));         /* APIC global enable */
    lapic_write(LAPIC_SVR, VEC_SPURIOUS | 0x100);
    irq_register_vector(VEC_SPURIOUS, spurious_irq, nullptr);

    /* calibrate the LAPIC timer against the TSC */
    lapic_write(LAPIC_TMR_DIV, 0x3);              /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, 1 << 16);          /* masked */
    lapic_write(LAPIC_TMR_INIT, 0xffffffff);
    udelay(10000);
    uint32_t ticks = 0xffffffff - lapic_read(LAPIC_TMR_CUR);
    uint32_t per_tick = ticks / 10 * 1000 / TIMER_HZ;
    irq_register_vector(VEC_TIMER, timer_irq, nullptr);
    lapic_per_tick = per_tick;
    bsp_lapic_id = lapic_id();
    lapic_cpu_init();
    pr_info("apic: lapic timer %u ticks per %d Hz period\n", per_tick, TIMER_HZ);
}
