#include <kernel/arch.h>
#include <kernel/printk.h>
#include <arch/cpu.h>

void serial_init(void);
void serial_register_console(void);
void kmain(void);
void gdt_init(void);
void idt_init(void);
void apic_init(void);

uint8_t fpu_initial_state[512] __attribute__((aligned(16)));

/* Enable SSE for user mode (the kernel itself is built without SSE). */
static void fpu_init(void) {
    write_cr0((read_cr0() & ~(1UL << 2)) | (1UL << 1));       /* clear EM, set MP */
    write_cr4(read_cr4() | (1UL << 9) | (1UL << 10));         /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("fninit; fxsave64 (%0)" :: "r"(fpu_initial_state) : "memory");
    *(uint32_t *)(fpu_initial_state + 24) = 0x1f80;           /* MXCSR default */
}

__noreturn void arch_halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

void arch_early_init(void) {
    serial_init();
    serial_register_console();
    gdt_init();
    idt_init();
    fpu_init();
}

void arch_init(void) {
    apic_init();
}

/* Limine jumps here with a valid stack, interrupts disabled, in long mode. */
__noreturn void kmain_entry(void) {
    kmain();
    arch_halt_forever();
}
