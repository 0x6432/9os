/* riscv64 platform glue: SBI console, timer, RTC, input polling, boot entry. */
#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/tty.h>
#include <kernel/irq.h>
#include <kernel/vmm.h>
#include <kernel/boot.h>
#include <kernel/fdt.h>
#include <kernel/errno.h>
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
void riscv_timer_irq(void) {
    set_timer(rdtime() + tick_delta);
    if (input_tty) for (int c; (c = sbi_getc()) >= 0;) tty_input(input_tty, (char)c);
    timer_tick();
}

/* ---- interrupt controller stubs (no PLIC driver yet: console input is polled) ---- */
void irq_register_vector(int vector, irq_handler_t h, void *ctx) {}
int irq_install(int gsi, irq_handler_t h, void *ctx) { return -ENODEV; }
void irq_eoi(void) {}

void input_init(void) {
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
    /* user memory access from S-mode, FPU usable (user state saved eagerly on switch) */
    csr_set(sstatus, SSTATUS_SUM | SSTATUS_FS_INITIAL);
}

void arch_init(void) {
    uint32_t tb;
    if (fdt_read_u32("cpus", "timebase-frequency", &tb) && tb) timebase_hz = tb;
    tick_delta = timebase_hz / TIMER_HZ;
    pr_info("timer: timebase %lu Hz, SBI%s%s%s\n", timebase_hz, have_time ? " TIME" : "",
            have_dbcn ? " DBCN" : "", have_legacy_getc ? " legacy-getchar" : "");
    set_timer(rdtime() + tick_delta);
    csr_set(sie, SIE_STIE);
    rtc_init();
}

/* Limine enters here in S-mode with a valid stack and interrupts disabled. */
__noreturn void kmain_entry(void) {
    kmain();
    arch_halt_forever();
}
