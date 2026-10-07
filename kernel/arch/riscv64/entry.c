/* riscv64 platform glue: SBI console, timer, RTC, PLIC + ns16550 console input, boot entry. */
#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/tty.h>
#include <kernel/irq.h>
#include <kernel/vmm.h>
#include <kernel/boot.h>
#include <kernel/fdt.h>
#include <kernel/errno.h>
#include <kernel/sched.h>
#include <kernel/pci.h>
#include <kernel/cpu.h>
#include <arch/cpu.h>

void kmain(void);
void trap_entry(void);

#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIME 0x54494D45
#define SBI_EXT_SRST 0x53525354
#define SBI_EXT_DBCN 0x4442434E

static bool have_dbcn, have_time, have_legacy_getc;

static bool sbi_probe(long ext) { return sbi_call(SBI_EXT_BASE, 3, ext, 0, 0).value != 0; }

void arch_debug_putc(char c) {
    if (have_dbcn) sbi_call(SBI_EXT_DBCN, 2, (unsigned char)c, 0, 0);
    else sbi_call(0x01, 0, (unsigned char)c, 0, 0);       /* legacy console_putchar */
}

static void sbi_console_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') arch_debug_putc('\r');
        arch_debug_putc(s[i]);
    }
}

static int sbi_getc(void) {
    if (have_legacy_getc) {
        long c = sbi_call(0x02, 0, 0, 0, 0).error;          /* legacy console_getchar returns in a0 */
        return c < 0 ? -1 : (int)c;
    }
    return -1;
}

__noreturn void arch_halt_forever(void) {
    for (;;) { csr_write(sie, 0); __asm__ volatile("csrci sstatus, 2; wfi"); }
}

void arch_poweroff(void) { sbi_call(SBI_EXT_SRST, 0, 0, 0, 0); }
void arch_reboot(void) { sbi_call(SBI_EXT_SRST, 0, 1, 0, 0); }

/* ---- timer ---- */
static uint64_t timebase_hz = 10000000;   /* QEMU virt default; overridden from the device tree */
static uint64_t tick_delta;

uint64_t time_ns(void) {
    uint64_t t = rdtime();
    return (t / timebase_hz) * 1000000000ULL + (t % timebase_hz) * 1000000000ULL / timebase_hz;
}

static void set_timer(uint64_t when) {
    if (have_time) sbi_call(SBI_EXT_TIME, 0, (long)when, 0, 0);
    else sbi_call(0x00, 0, (long)when, 0, 0);
}

static struct tty *input_tty;
void riscv_timer_rearm(void) { set_timer(rdtime() + tick_delta); }
void arch_timer_active(void) { riscv_timer_rearm(); }
void arch_timer_idle(uint64_t deadline) {
    if (deadline == UINT64_MAX) { set_timer(UINT64_MAX); return; }
    uint64_t now = time_ns(), delta = deadline > now ? deadline - now : 1000;
    uint64_t ticks = delta / 1000000000 * timebase_hz +
                     (delta % 1000000000) * timebase_hz / 1000000000;
    set_timer(rdtime() + MAX(ticks, 1ULL));
}
/* console input is polled only without a PLIC/UART interrupt (SBI getchar fallback) */
uint64_t arch_idle_poll_ns(void) { return input_tty ? 10000000ULL : UINT64_MAX; }
void riscv_timer_irq(void) {
    if (input_tty && this_cpu()->id == 0) for (int c; (c = sbi_getc()) >= 0;) tty_input(input_tty, (char)c);
    timer_tick();
}

/* ---- PLIC (M28): every source is routed to the boot hart's S-mode context ---- */
#define PLIC_NSRC 128
static volatile uint32_t *plic;
static unsigned plic_ctx;
static irq_handler_t handlers[PLIC_NSRC];
static void *handler_ctx[PLIC_NSRC];

static void plic_init(void) {
    uint64_t base = 0x0c000000;
    if (!fdt_read_reg("plic@", &base, nullptr)) fdt_read_reg("interrupt-controller@c", &base, nullptr);
    plic = vmm_map_mmio(base, 0x400000);
    if (!plic) return;
    plic_ctx = 2 * (unsigned)this_cpu()->hwid + 1;          /* QEMU virt: M then S context per hart */
    for (unsigned i = 1; i < PLIC_NSRC; i++) plic[i] = 0;    /* priority 0 = masked */
    for (unsigned w = 0; w < PLIC_NSRC / 32; w++) plic[(0x2000 + plic_ctx * 0x80) / 4 + w] = 0;
    plic[(0x200000 + plic_ctx * 0x1000) / 4] = 0;            /* threshold */
    csr_set(sie, SIE_SEIE);
    pr_info("plic: at %lx, S-mode context %u\n", base, plic_ctx);
}

void irq_register_vector(int vector, irq_handler_t h, void *ctx) {
    if (vector <= 0 || vector >= PLIC_NSRC) return;
    handler_ctx[vector] = ctx;
    handlers[vector] = h;
}
int irq_install(int src, irq_handler_t h, void *ctx) {
    if (!plic) return -ENODEV;
    if (src <= 0 || src >= PLIC_NSRC) return -EINVAL;
    irq_register_vector(src, h, ctx);
    plic[src] = 1;
    plic[(0x2000 + plic_ctx * 0x80) / 4 + src / 32] |= 1u << (src % 32);
    return src;
}
void irq_eoi(void) { /* completion is written by the dispatcher */ }
/* supervisor external interrupt (BKL held) */
void riscv_ext_irq(struct trap_frame *f) {
    volatile uint32_t *claim = &plic[(0x200004 + plic_ctx * 0x1000) / 4];
    for (uint32_t src; (src = *claim);) {
        if (src < PLIC_NSRC && handlers[src]) handlers[src](f, handler_ctx[src]);
        else printk("plic: spurious source %u\n", src);
        *claim = src;
    }
}
const char *arch_irq_chip(void) { return "PLIC"; }
int arch_msi_alloc(irq_handler_t h, void *ctx, uint64_t *addr, uint32_t *data) { return -ENODEV; }
/* QEMU virt: INTA..INTD of the root bus are PLIC sources 32..35, swizzled by slot */
int arch_pci_intx_line(struct pci_dev *d) {
    unsigned pin = pci_read8(d, 0x3d);
    if (!plic || !pin || pin > 4 || d->bus != 0) return -1;
    return 32 + (int)((d->dev + pin - 1) % 4);
}

/* ns16550 console UART: receive interrupts (OpenSBI keeps writing through the same THR) */
static volatile uint8_t *uart;
static int uart_rx(void *ctx) {
    if (!(uart[5] & 1)) return IRQ_NONE;
    while (uart[5] & 1) tty_input(&console_tty, (char)uart[0]);
    return IRQ_HANDLED;
}

void input_init(void) {
    uint64_t base = 0x10000000;
    uint32_t src = 10;
    fdt_read_reg("serial@", &base, nullptr);
    fdt_read_u32("serial@", "interrupts", &src);
    if (plic && (uart = vmm_map_mmio(base, 0x1000))) {
        while (uart[5] & 1) (void)uart[0];
        if (irq_request((int)src, "serial", uart_rx, nullptr, nullptr) >= 0) {
            uart[4] |= 0x08;                                 /* OUT2 */
            uart[1] = 0x01;                                  /* IER: received data available */
            pr_info("input: ns16550 console at %lx, PLIC source %u (interrupt-driven)\n", base, src);
            return;
        }
    }
    input_tty = &console_tty;
    pr_info("input: SBI console (polled)\n");
}

/* goldfish RTC on QEMU virt */
static void rtc_init(void) {
    uint64_t base = 0x101000;
    fdt_read_reg("rtc@", &base, nullptr);
    volatile uint32_t *r = vmm_map_mmio(base, 0x1000);
    uint64_t lo = r[0], hi = r[1];
    uint64_t ns = hi << 32 | lo;
    boot_epoch = (int64_t)(ns / 1000000000ULL) - (int64_t)(time_ns() / 1000000000ULL);
    pr_info("rtc: goldfish at %lx, epoch %ld\n", base, (long)(ns / 1000000000ULL));
}

void arch_early_init(void) {
    have_dbcn = sbi_probe(SBI_EXT_DBCN);
    have_time = sbi_probe(SBI_EXT_TIME);
    have_legacy_getc = sbi_probe(0x02);
    console_register(sbi_console_write);
    csr_write(sscratch, 0);
    csr_write(stvec, (uint64_t)trap_entry);
    csr_write(sie, 0);
    /* FPU usable; user memory only inside user_access_begin/end (SUM, M27) (user state saved eagerly on switch) */
    csr_set(sstatus, SSTATUS_FS_INITIAL);
}

void arch_init(void) {
    uint32_t tb;
    if (fdt_read_u32("cpus", "timebase-frequency", &tb) && tb) timebase_hz = tb;
    tick_delta = timebase_hz / TIMER_HZ;
    pr_info("timer: timebase %lu Hz, SBI%s%s%s\n", timebase_hz, have_time ? " TIME" : "",
            have_dbcn ? " DBCN" : "", have_legacy_getc ? " legacy-getchar" : "");
    set_timer(rdtime() + tick_delta);
    csr_set(sie, SIE_STIE | SIE_SSIE);
    plic_init();
    rtc_init();
}

/* ---- SMP ---- */
#define SBI_EXT_IPI 0x735049
int arch_cpu_hw_index(void) { return 0; }

void riscv_ap_trampoline(void);
void arch_send_ipi(struct cpu *c) { sbi_call(SBI_EXT_IPI, 0, 1, (long)c->hwid, 0); }

__noreturn void riscv_ap_entry(struct limine_mp_info *info) {
    struct cpu *c = (struct cpu *)info->extra_argument;
    arch_set_current(c->idle);        /* so this_cpu() works before the idle thread runs */
    csr_write(sscratch, 0);
    csr_write(stvec, (uint64_t)trap_entry);
    csr_write(sie, 0);
    csr_set(sstatus, SSTATUS_FS_INITIAL);
    vmm_switch(kernel_pt);
    set_timer(rdtime() + tick_delta);
    csr_set(sie, SIE_STIE | SIE_SSIE);
    smp_ap_main(c);
}

void arch_ap_boot(struct cpu *c, void *mp_info) {
    struct limine_mp_info *info = mp_info;
    info->extra_argument = (uint64_t)c;
    __atomic_store_n(&info->goto_address, (limine_goto_address)riscv_ap_trampoline, __ATOMIC_SEQ_CST);
}

/* Limine enters here in S-mode with a valid stack and interrupts disabled. */
__noreturn void kmain_entry(void) {
    arch_set_current(nullptr);
    kmain();
    arch_halt_forever();
}
