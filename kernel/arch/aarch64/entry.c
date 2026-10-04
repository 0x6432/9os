/* aarch64 platform glue (QEMU virt): PL011 console, GICv2, generic timer, PL031 RTC, PSCI. */
#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/tty.h>
#include <kernel/irq.h>
#include <kernel/vmm.h>
#include <kernel/boot.h>
#include <kernel/fdt.h>
#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <arch/cpu.h>

extern char exception_vectors[];

/* defaults for QEMU virt; refined from ACPI (SPCR/MADT) or the device tree when available */
static uint64_t uart_phys = 0x09000000, gicd_phys = 0x08000000, gicc_phys = 0x08010000, rtc_phys = 0x09010000;
static volatile uint32_t *uart, *gicd, *gicc;
static bool psci_smc;

#define UART_DR 0
#define UART_FR (0x18 / 4)

void arch_debug_putc(char c) {
    if (!uart) return;
    while (uart[UART_FR] & (1 << 5)) arch_cpu_relax();
    uart[UART_DR] = (uint8_t)c;
}
static void uart_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') arch_debug_putc('\r');
        arch_debug_putc(s[i]);
    }
}

static void psci_call(uint64_t fn) {
    register uint64_t x0 __asm__("x0") = fn;
    if (psci_smc) __asm__ volatile("smc #0" : "+r"(x0) :: "memory");
    else __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}
void arch_poweroff(void) { psci_call(0x84000008); }
void arch_reboot(void) { psci_call(0x84000009); }

__noreturn void arch_halt_forever(void) {
    for (;;) __asm__ volatile("msr daifset, #2; wfi");
}

/* ---- generic timer (virtual, PPI 27) ---- */
static uint64_t cnt_freq, tick_delta;
uint64_t time_ns(void) {
    if (!cnt_freq) return 0;
    uint64_t t = sysreg_read(cntvct_el0);
    return (t / cnt_freq) * 1000000000ULL + (t % cnt_freq) * 1000000000ULL / cnt_freq;
}

static struct tty *input_tty;
static void timer_irq(void) {
    sysreg_write(cntv_tval_el0, tick_delta);
    if (input_tty && uart)
        while (!(uart[UART_FR] & (1 << 4))) tty_input(input_tty, (char)uart[UART_DR]);
    timer_tick();
}

/* ---- GICv2 ---- */
#define GICD_CTLR 0
#define GICD_ISENABLER (0x100 / 4)
#define GICD_IPRIORITYR (0x400 / 4)
#define GICC_CTLR 0
#define GICC_PMR (0x4 / 4)
#define GICC_IAR (0xc / 4)
#define GICC_EOIR (0x10 / 4)

static irq_handler_t handlers[1020];
static void *handler_ctx[1020];

void irq_register_vector(int v, irq_handler_t h, void *ctx) {
    if (v < 0 || v >= 1020) return;
    handler_ctx[v] = ctx; handlers[v] = h;
}
int irq_install(int gsi, irq_handler_t h, void *ctx) {
    if (gsi < 0 || gsi >= 1020 || !gicd) return -ENODEV;
    irq_register_vector(gsi, h, ctx);
    ((volatile uint8_t *)gicd)[0x400 + gsi] = 0xa0;            /* priority */
    if (gsi >= 32) ((volatile uint8_t *)gicd)[0x800 + gsi] = 1; /* target cpu0 */
    gicd[GICD_ISENABLER + gsi / 32] = 1u << (gsi % 32);
    return gsi;
}
static uint32_t cur_irq;
void irq_eoi(void) { /* EOI is written by the dispatcher */ }

void a64_irq(struct trap_frame *f) {
    uint32_t iar = gicc[GICC_IAR];
    uint32_t id = iar & 0x3ff;
    if (id >= 1020) return;                                     /* spurious */
    cur_irq = id;
    if (id == 27) timer_irq();
    else if (handlers[id]) handlers[id](f, handler_ctx[id]);
    else printk("spurious irq %u\n", id);
    gicc[GICC_EOIR] = iar;
}

static void gic_init(void) {
    gicd = vmm_map_mmio(gicd_phys, 0x10000);
    gicc = vmm_map_mmio(gicc_phys, 0x2000);
    gicd[GICD_CTLR] = 1;
    gicc[GICC_PMR] = 0xff;
    gicc[GICC_CTLR] = 1;
}

void input_init(void) {
    input_tty = &console_tty;
    pr_info("input: PL011 console (polled)\n");
}

/* ACPI discovery (uACPI table access is up before arch_init) */
struct [[gnu::packed]] acpi_sdt { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6], oemt[8]; uint32_t oemrev, cid, crev; };
static void acpi_discover(void) {
    struct acpi_sdt *spcr = acpi_find_table("SPCR");
    if (spcr && spcr->len >= 52) {
        uint8_t *gas = (uint8_t *)spcr + 40;                     /* base address GAS */
        uint64_t a; __builtin_memcpy(&a, gas + 4, 8);
        if (a) uart_phys = a;
    }
    struct acpi_sdt *madt = acpi_find_table("APIC");
    if (madt) {
        uint8_t *p = (uint8_t *)madt + 44, *end = (uint8_t *)madt + madt->len;
        for (; p + 2 <= end && p[1]; p += p[1]) {
            if (p[0] == 0xc) __builtin_memcpy(&gicd_phys, p + 8, 8);       /* GICD */
            if (p[0] == 0xb) { uint64_t a; __builtin_memcpy(&a, p + 32, 8); if (a) gicc_phys = a; }  /* GICC */
        }
    }
    uint32_t l;
    const char *m = fdt_find_prop("psci", "method", &l);
    if (m && m[0] == 's') psci_smc = true;
    uint64_t a;
    if (fdt_read_reg("pl011@", &a, nullptr)) uart_phys = a;
    if (fdt_read_reg("pl031@", &a, nullptr)) rtc_phys = a;
}

void arch_early_init(void) {
    sysreg_write(vbar_el1, (uint64_t)exception_vectors);
    sysreg_write(cpacr_el1, sysreg_read(cpacr_el1) | (3UL << 20));   /* FP/SIMD at EL0/EL1 */
    isb();
}

void arch_init(void) {
    acpi_discover();
    uart = vmm_map_mmio(uart_phys, 0x1000);
    console_register(uart_write);                                    /* replays the early log */
    gic_init();
    cnt_freq = sysreg_read(cntfrq_el0);
    tick_delta = cnt_freq / TIMER_HZ;
    ((volatile uint8_t *)gicd)[0x400 + 27] = 0xa0;
    gicd[GICD_ISENABLER] = 1u << 27;
    sysreg_write(cntv_tval_el0, tick_delta);
    sysreg_write(cntv_ctl_el0, 1);
    pr_info("gic: v2 dist %lx cpu %lx; timer %lu Hz; uart %lx\n", gicd_phys, gicc_phys, cnt_freq, uart_phys);
    volatile uint32_t *rtc = vmm_map_mmio(rtc_phys, 0x1000);
    boot_epoch = (int64_t)rtc[0] - (int64_t)(time_ns() / 1000000000ULL);
    pr_info("rtc: pl031 epoch %u\n", rtc[0]);
}
