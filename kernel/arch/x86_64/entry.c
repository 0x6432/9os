#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/cpu.h>
#include <kernel/boot.h>
#include <arch/cpu.h>
#include <arch/gdt.h>

void serial_init(void);
void serial_register_console(void);
void kmain(void);
void gdt_init(void);
void idt_init(void);
void apic_init(void);
void syscall_init(void);
void rtc_init(void);
void idt_load(void);
void x86_set_cpu_base(struct cpu *c);
void x86_ap_paging_init(void);
void lapic_cpu_init(void);

uint8_t fpu_initial_state[512] __attribute__((aligned(16)));

/* Enable SSE for user mode (the kernel itself is built without SSE). */
static void fpu_enable(void) {
    write_cr0((read_cr0() & ~(1UL << 2)) | (1UL << 1));       /* clear EM, set MP */
    write_cr4(read_cr4() | (1UL << 9) | (1UL << 10));         /* OSFXSR, OSXMMEXCPT */
}

static void fpu_init(void) {
    fpu_enable();
    __asm__ volatile("fninit; fxsave64 (%0)" :: "r"(fpu_initial_state) : "memory");
    *(uint32_t *)(fpu_initial_state + 24) = 0x1f80;           /* MXCSR default */
}

__noreturn void arch_halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

void arch_early_init(void) {
    x86_set_cpu_base(&cpus[0]);
    serial_init();
    serial_register_console();
    gdt_init();
    idt_init();
    fpu_init();
    syscall_init();
}

void arch_init(void) {
    apic_init();
    rtc_init();
}

int arch_cpu_hw_index(void) { return 0; }
void x86_ap_trampoline(void);

/* Application processors start here (Limine MP), on a bootloader stack with interrupts off. */
__noreturn void x86_ap_entry(struct limine_mp_info *info) {
    struct cpu *c = (struct cpu *)info->extra_argument;
    x86_set_cpu_base(c);
    x86_ap_paging_init();
    gdt_init_cpu(c->id);
    idt_load();
    fpu_enable();
    syscall_init();
    lapic_cpu_init();
    smp_ap_main(c);
}

void arch_ap_boot(struct cpu *c, void *mp_info) {
    struct limine_mp_info *info = mp_info;
    info->extra_argument = (uint64_t)c;
    __atomic_store_n(&info->goto_address, (limine_goto_address)x86_ap_trampoline, __ATOMIC_SEQ_CST);
}

/* Limine jumps here with a valid stack, interrupts disabled, in long mode. */
__noreturn void kmain_entry(void) {
    kmain();
    arch_halt_forever();
}
