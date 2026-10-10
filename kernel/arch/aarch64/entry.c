/* aarch64 platform glue (QEMU virt): PL011 console, GICv2/v3, generic timer, PL031 RTC, PSCI. */
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
#include <kernel/pci.h>
#include <kernel/cpu.h>
#include <arch/cpu.h>
#include <arch/trapframe.h>

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
void arch_timer_active(void) {
    sysreg_write(cntv_tval_el0, tick_delta);
    sysreg_write(cntv_ctl_el0, 1);
}
void arch_timer_idle(uint64_t deadline) {
    if (deadline == UINT64_MAX) { sysreg_write(cntv_ctl_el0, 0); return; }
    uint64_t now = time_ns(), delta = deadline > now ? deadline - now : 1000;
    uint64_t ticks = delta / 1000000000 * cnt_freq +
                     (delta % 1000000000) * cnt_freq / 1000000000;
    sysreg_write(cntv_cval_el0, sysreg_read(cntvct_el0) + MAX(ticks, 1ULL));
    sysreg_write(cntv_ctl_el0, 1);
}
uint64_t arch_idle_poll_ns(void) { return input_tty ? 10000000ULL : UINT64_MAX; }
static void timer_irq(void) {
    if (input_tty && uart && this_cpu()->id == 0)
        while (!(uart[UART_FR] & (1 << 4))) tty_input(input_tty, (char)uart[UART_DR]);
    timer_tick();
}

/* ---- GICv2 / GICv3 (M28) ---- */
#define GICD_CTLR 0
#define GICD_ISENABLER (0x100 / 4)
#define GICD_IPRIORITYR (0x400 / 4)
#define GICC_CTLR 0
#define GICC_PMR (0x4 / 4)
#define GICC_IAR (0xc / 4)
#define GICC_EOIR (0x10 / 4)
/* GICv3 CPU interface system registers */
#define ICC_SRE_EL1     s3_0_c12_c12_5
#define ICC_PMR_EL1     s3_0_c4_c6_0
#define ICC_BPR1_EL1    s3_0_c12_c12_3
#define ICC_IGRPEN1_EL1 s3_0_c12_c12_7
#define ICC_IAR1_EL1    s3_0_c12_c12_0
#define ICC_EOIR1_EL1   s3_0_c12_c12_1
#define ICC_SGI1R_EL1   s3_0_c12_c11_5

static int gic_ver = 2;
static uint64_t gicr_phys, gicr_len;
static volatile uint8_t *gicr;
static uint64_t bsp_aff;
static uint32_t madt_gic_ver;      /* MPIDR affinity of the boot CPU: SPIs are routed there */

static irq_handler_t handlers[1020];
static void *handler_ctx[1020];

static uint64_t mpidr_aff(uint64_t m) { return m & 0xff00ffffffull; }
static uint64_t aff_router(uint64_t m) { return (m & 0xffffff) | ((m >> 32) & 0xff) << 32; }

void irq_register_vector(int v, irq_handler_t h, void *ctx) {
    if (v < 0 || v >= 1020) return;
    handler_ctx[v] = ctx; handlers[v] = h;
}
int irq_install(int gsi, irq_handler_t h, void *ctx) {
    if (gsi < 32 || gsi >= 1020 || !gicd) return -ENODEV;
    irq_register_vector(gsi, h, ctx);
    ((volatile uint8_t *)gicd)[0x400 + gsi] = 0xa0;            /* priority */
    if (gic_ver >= 3) {
        gicd[0x80 / 4 + gsi / 32] |= 1u << (gsi % 32);          /* group 1 */
        *(volatile uint64_t *)((volatile uint8_t *)gicd + 0x6000 + 8 * gsi) = aff_router(bsp_aff);
    } else {
        ((volatile uint8_t *)gicd)[0x800 + gsi] = 1;            /* target cpu0 */
    }
    gicd[GICD_ISENABLER + gsi / 32] = 1u << (gsi % 32);
    return gsi;
}
void irq_eoi(void) { /* EOI is written by the dispatcher */ }
const char *arch_irq_chip(void) { return gic_ver >= 3 ? "GICv3" : "GICv2"; }
int arch_msi_alloc(irq_handler_t h, void *ctx, uint64_t *addr, uint32_t *data) { return -ENODEV; }
/* QEMU virt: INTA..INTD of the root bus are SPIs 3..6 (INTID 35..38), swizzled by slot */
int arch_pci_intx_line(struct pci_dev *d) {
    unsigned pin = pci_read8(d, 0x3d);
    if (!gicd || !pin || pin > 4 || d->bus != 0) return -1;
    return 35 + (int)((d->dev + pin - 1) % 4);
}

void ipi_handle(void);
void user_return_work(struct trap_frame *f);

static inline uint32_t gic_ack(void) {
    if (gic_ver >= 3) { uint32_t v = (uint32_t)sysreg_read(ICC_IAR1_EL1); return v; }
    return gicc[GICC_IAR];
}
static inline void gic_eoi(uint32_t iar) {
    if (gic_ver >= 3) { sysreg_write(ICC_EOIR1_EL1, iar); isb(); }
    else gicc[GICC_EOIR] = iar;
}

void a64_irq(struct trap_frame *f) {
    uint32_t iar = gic_ack();
    uint32_t id = iar & 0xffffff;
    if (gic_ver < 3) id &= 0x3ff;
    if (id >= 1020) return;                                     /* spurious */
    if (id < 16) {                                              /* SGI = IPI, no BKL */
        gic_eoi(iar);
        ipi_irq();
        return;
    }
    if (id == 27) {                                             /* timer: usually lock-free on APs */
        sysreg_write(cntv_tval_el0, tick_delta);
        if (sched_tick_fast(trap_from_user(f))) { gic_eoi(iar); return; }
    }
    bkl_enter();
    if (id == 27) timer_irq();
    else if (handlers[id]) handlers[id](f, handler_ctx[id]);
    else printk("spurious irq %u\n", id);
    gic_eoi(iar);
    user_return_work(f);
    bkl_exit();
}

/* GICv3: this CPU's redistributor (matched by affinity) */
static volatile uint8_t *gicr_find(uint64_t aff) {
    if (!gicr) return nullptr;
    for (uint64_t off = 0; off + 0x20000 <= gicr_len;) {
        volatile uint8_t *rd = gicr + off;
        uint64_t typer = *(volatile uint64_t *)(rd + 8);
        uint64_t a = typer >> 32;                               /* aff3.aff2.aff1.aff0 */
        uint64_t want = (aff & 0xffffff) | ((aff >> 32) & 0xff) << 24;
        if (a == want) return rd;
        if (typer & (1 << 4)) break;                            /* Last */
        off += (typer & (1 << 1)) ? 0x40000 : 0x20000;          /* VLPIS: GICv4 frames */
    }
    return nullptr;
}

/* per-CPU GIC interface, SGIs and the timer PPI (banked registers) */
static void gic_cpu_init(void) {
    if (gic_ver >= 3) {
        uint64_t aff = mpidr_aff(sysreg_read(mpidr_el1));
        this_cpu()->arch_data[0] = aff;
        volatile uint8_t *rd = gicr_find(aff);
        if (!rd) { pr_err("gic: no redistributor for mpidr %lx\n", aff); return; }
        volatile uint32_t *waker = (volatile uint32_t *)(rd + 0x14);
        *waker &= ~(1u << 1);                                   /* ProcessorSleep */
        while (*waker & (1u << 2)) arch_cpu_relax();            /* ChildrenAsleep */
        volatile uint8_t *sgi = rd + 0x10000;
        *(volatile uint32_t *)(sgi + 0x80) = 0xffffffff;        /* SGIs/PPIs: group 1 */
        for (int i = 0; i < 16; i++) sgi[0x400 + i] = 0x80;
        sgi[0x400 + 27] = 0xa0;
        *(volatile uint32_t *)(sgi + 0x100) = 0xffffu | (1u << 27);
        sysreg_write(ICC_SRE_EL1, sysreg_read(ICC_SRE_EL1) | 1);
        isb();
        sysreg_write(ICC_PMR_EL1, 0xff);
        sysreg_write(ICC_BPR1_EL1, 0);
        sysreg_write(ICC_IGRPEN1_EL1, 1);
        isb();
        return;
    }
    gicc[GICC_PMR] = 0xff;
    gicc[GICC_CTLR] = 1;
    for (int i = 0; i < 16; i++) ((volatile uint8_t *)gicd)[0x400 + i] = 0x80;
    ((volatile uint8_t *)gicd)[0x400 + 27] = 0xa0;
    gicd[GICD_ISENABLER] = 0xffffu | (1u << 27);
    this_cpu()->arch_data[0] = ((volatile uint8_t *)gicd)[0x800] & 0xff;   /* own target mask */
}

static void timer_cpu_init(void) {
    sysreg_write(cntv_tval_el0, tick_delta);
    sysreg_write(cntv_ctl_el0, 1);
}

int arch_cpu_hw_index(void) { return 0; }

void arch_send_ipi(struct cpu *c) {
    if (gic_ver >= 3) {
        uint64_t m = c->arch_data[0] ? c->arch_data[0] : mpidr_aff(c->hwid);
        uint64_t v = (1ull << (m & 0xf)) | ((m >> 8) & 0xff) << 16 | ((m >> 16) & 0xff) << 32 |
                     ((m >> 32) & 0xff) << 48;                  /* SGI 0, aff0 < 16 */
        __asm__ volatile("dsb ishst" ::: "memory");
        sysreg_write(ICC_SGI1R_EL1, v);
        isb();
        return;
    }
    uint32_t mask = c->arch_data[0] ? (uint32_t)c->arch_data[0] : 1u << c->id;
    __asm__ volatile("dsb ishst" ::: "memory");
    gicd[0xf00 / 4] = (mask << 16) | 0;                         /* GICD_SGIR: SGI 0 */
}

void a64_ap_mmu_init(void);
void a64_ap_trampoline(void);
extern char exception_vectors[];

__noreturn void a64_ap_entry(struct limine_mp_info *info) {
    struct cpu *c = (struct cpu *)info->extra_argument;
    c->arch_data[1] = 1;
    arch_set_current(c->idle);
    sysreg_write(vbar_el1, (uint64_t)exception_vectors);
    sysreg_write(oslar_el1, 0);         /* unlock the OS lock: debug exceptions (ptrace step) */
    sysreg_write(mdscr_el1, 0);
    isb();
    c->arch_data[1] = 2;
    a64_ap_mmu_init();
    c->arch_data[1] = 3;
    gic_cpu_init();
    timer_cpu_init();
    c->arch_data[1] = 4;
    smp_ap_main(c);
}

void arch_ap_boot(struct cpu *c, void *mp_info) {
    struct limine_mp_info *info = mp_info;
    info->extra_argument = (uint64_t)c;
    __atomic_store_n(&info->goto_address, (limine_goto_address)a64_ap_trampoline, __ATOMIC_SEQ_CST);
    __asm__ volatile("dsb sy; sev" ::: "memory");      /* parked APs may be waiting in WFE */
}

static void gic_init(void) {
    gicd = vmm_map_mmio(gicd_phys, 0x10000);
    /* version: MADT GICD entry, else a GICR range / FDT compatible (PIDR2 is outside the 4 KiB
     * GICv2 distributor, so it cannot be probed blindly) */
    uint32_t arch = madt_gic_ver;
    if (!arch && gicr_phys) arch = 3;
    if (!arch) {
        uint32_t l;
        const char *c = fdt_find_prop("intc@", "compatible", &l);
        if (c && l >= 10 && !__builtin_memcmp(c, "arm,gic-v3", 10)) arch = 3;
    }
    if (arch >= 3) {
        gic_ver = arch;
        if (!gicr_phys) gicr_phys = 0x080a0000;
        if (!gicr_len) gicr_len = 0x20000ull * 64;
        gicr = vmm_map_mmio(gicr_phys, gicr_len);
        bsp_aff = mpidr_aff(sysreg_read(mpidr_el1));
        gicd[GICD_CTLR] = 0;
        while (gicd[GICD_CTLR] & (1u << 31)) arch_cpu_relax();  /* RWP */
        gicd[GICD_CTLR] = 1u << 4;                              /* ARE (single security state) */
        gicd[GICD_CTLR] = (1u << 4) | 2 | 1;                    /* + EnableGrp1, EnableGrp0 */
    } else {
        gicc = vmm_map_mmio(gicc_phys, 0x2000);
        gicd[GICD_CTLR] = 1;
    }
    gic_cpu_init();
}

/* PL011 receive interrupt (M28; previously polled from the timer tick) */
static uint32_t uart_gsi = 33;
#define UART_IMSC (0x38 / 4)
#define UART_MIS  (0x40 / 4)
#define UART_ICR  (0x44 / 4)
static int uart_rx(void *ctx) {
    if (!(uart[UART_MIS] & 0x50) && (uart[UART_FR] & (1 << 4))) return IRQ_NONE;
    while (!(uart[UART_FR] & (1 << 4))) tty_input(&console_tty, (char)uart[UART_DR]);
    uart[UART_ICR] = 0x50;                                      /* RXIC | RTIC */
    return IRQ_HANDLED;
}

void input_init(void) {
    if (uart && irq_request((int)uart_gsi, "pl011", uart_rx, nullptr, nullptr) >= 0) {
        uart[UART_ICR] = 0x7ff;
        uart[UART_IMSC] |= 0x50;                                /* RXIM | RTIM */
        pr_info("input: PL011 console, %s INTID %u (interrupt-driven)\n", arch_irq_chip(), uart_gsi);
        return;
    }
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
        uint32_t gsiv; __builtin_memcpy(&gsiv, (uint8_t *)spcr + 54, 4);
        if ((((uint8_t *)spcr)[52] & 8) && gsiv >= 32 && gsiv < 1020) uart_gsi = gsiv;   /* GIC type */
    }
    struct acpi_sdt *madt = acpi_find_table("APIC");
    if (madt) {
        uint8_t *p = (uint8_t *)madt + 44, *end = (uint8_t *)madt + madt->len;
        for (; p + 2 <= end && p[1]; p += p[1]) {
            if (p[0] == 0xc) { __builtin_memcpy(&gicd_phys, p + 8, 8); madt_gic_ver = p[20]; }   /* GICD */
            if (p[0] == 0xb) { uint64_t a; __builtin_memcpy(&a, p + 32, 8); if (a) gicc_phys = a; }  /* GICC */
            if (p[0] == 0xe && !gicr_phys) {                                 /* GICR discovery range */
                __builtin_memcpy(&gicr_phys, p + 4, 8);
                uint32_t l; __builtin_memcpy(&l, p + 12, 4); gicr_len = l;
            }
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
    sysreg_write(oslar_el1, 0);         /* unlock the OS lock: debug exceptions (ptrace step) */
    sysreg_write(mdscr_el1, 0);
    isb();
}

void arch_init(void) {
    acpi_discover();
    uart = vmm_map_mmio(uart_phys, 0x1000);
    console_register(uart_write);                                    /* replays the early log */
    gic_init();
    cnt_freq = sysreg_read(cntfrq_el0);
    tick_delta = cnt_freq / TIMER_HZ;
    timer_cpu_init();
    if (gic_ver >= 3) pr_info("gic: v%d dist %lx redist %lx; timer %lu Hz; uart %lx\n", gic_ver, gicd_phys, gicr_phys, cnt_freq, uart_phys);
    else pr_info("gic: v2 dist %lx cpu %lx; timer %lu Hz; uart %lx\n", gicd_phys, gicc_phys, cnt_freq, uart_phys);
    volatile uint32_t *rtc = vmm_map_mmio(rtc_phys, 0x1000);
    boot_epoch = (int64_t)rtc[0] - (int64_t)(time_ns() / 1000000000ULL);
    pr_info("rtc: pl031 epoch %u\n", rtc[0]);
}
