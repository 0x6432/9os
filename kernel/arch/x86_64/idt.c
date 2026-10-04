#include <kernel/types.h>
#include <kernel/printk.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <arch/trapframe.h>
#include <arch/gdt.h>
#include <arch/cpu.h>

struct [[gnu::packed]] idt_entry {
    uint16_t off_lo, sel;
    uint8_t ist, flags;
    uint16_t off_mid;
    uint32_t off_hi, zero;
};
static struct idt_entry idt[256];
extern const uint64_t isr_table[256];

static irq_handler_t handlers[256];
static void *handler_ctx[256];

static void set_gate(int v, uint64_t fn, uint8_t ist, uint8_t dpl) {
    idt[v] = (struct idt_entry){
        .off_lo = fn & 0xffff, .sel = KERNEL_CS, .ist = ist,
        .flags = 0x8e | (dpl << 5), .off_mid = (fn >> 16) & 0xffff, .off_hi = fn >> 32,
    };
}

void idt_load(void) {
    struct [[gnu::packed]] { uint16_t limit; uint64_t base; } r = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(r));
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) set_gate(v, isr_table[v], 0, 0);
    idt[8].ist = 1;   /* #DF */
    idt[2].ist = 2;   /* NMI */
    idt_load();
}

void irq_register_vector(int vector, irq_handler_t h, void *ctx) {
    handler_ctx[vector] = ctx;
    handlers[vector] = h;
}

static const char *exc_names[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow", "#BR bound",
    "#UD invalid opcode", "#NM device n/a", "#DF double fault", "coproc overrun",
    "#TS invalid TSS", "#NP segment not present", "#SS stack fault", "#GP general protection",
    "#PF page fault", "reserved", "#MF x87", "#AC alignment", "#MC machine check",
    "#XM simd", "#VE virtualization", "#CP control protection",
};

void dump_frame(struct trap_frame *f) {
    printk("  rip=%016lx cs=%04lx rflags=%016lx rsp=%016lx ss=%04lx\n", f->rip, f->cs, f->rflags, f->rsp, f->ss);
    printk("  rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    printk("  rsi=%016lx rdi=%016lx rbp=%016lx r8 =%016lx\n", f->rsi, f->rdi, f->rbp, f->r8);
    printk("  r9 =%016lx r10=%016lx r11=%016lx r12=%016lx\n", f->r9, f->r10, f->r11, f->r12);
    printk("  r13=%016lx r14=%016lx r15=%016lx cr2=%016lx\n", f->r13, f->r14, f->r15, read_cr2());
    /* frame-pointer backtrace for kernel faults */
    if (!trap_from_user(f)) {
        uint64_t *bp = (uint64_t *)f->rbp;
        printk("  backtrace:");
        for (int i = 0; i < 12 && bp && (uint64_t)bp >= 0xffff800000000000ULL; i++) {
            printk(" %lx", bp[1]);
            bp = (uint64_t *)bp[0];
        }
        printk("\n");
    }
}

/* weak hooks, overridden by later subsystems */
[[gnu::weak]] bool page_fault_handler(struct trap_frame *f) { return false; }
[[gnu::weak]] bool user_exception(struct trap_frame *f) { return false; }
void user_return_work(struct trap_frame *f);

void irq_eoi(void);

void trap_dispatch(struct trap_frame *f) {
    uint64_t v = f->vector;
    if (v == 0xf0) {            /* IPI: handled without the big kernel lock */
        irq_eoi();
        ipi_handle();
        return;
    }
    if (v == 32) {              /* local timer: secondary CPUs usually need no lock */
        irq_eoi();
        if (sched_tick_fast(trap_from_user(f))) return;
        bkl_enter();
        timer_tick();
        goto out;
    }
    bkl_enter();
    if (v < 32) {
        if (v == 14 && page_fault_handler(f)) goto out;
        if (trap_from_user(f) && user_exception(f)) goto out;
        printk("\nexception %lu (%s) err=%lx\n", v, exc_names[v] ? exc_names[v] : "?", f->error);
        dump_frame(f);
        panic("unhandled exception in %s mode", trap_from_user(f) ? "user" : "kernel");
    }
    if (handlers[v]) handlers[v](f, handler_ctx[v]);
    else printk("spurious interrupt vector %lu\n", v);
out:
    user_return_work(f);
    bkl_exit();
}
