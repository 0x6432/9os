#include <kernel/arch.h>
#include <kernel/printk.h>

void serial_init(void);
void serial_register_console(void);
void kmain(void);
void gdt_init(void);
void idt_init(void);

__noreturn void arch_halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

void arch_early_init(void) {
    serial_init();
    serial_register_console();
    gdt_init();
    idt_init();
}

void arch_init(void) {}

/* Limine jumps here with a valid stack, interrupts disabled, in long mode. */
__noreturn void kmain_entry(void) {
    kmain();
    arch_halt_forever();
}
