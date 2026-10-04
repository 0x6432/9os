#include <kernel/arch.h>
#include <kernel/printk.h>

#define COM1 0x3f8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x0B);
}

void arch_debug_putc(char ch) {
    while (!(inb(COM1 + 5) & 0x20)) arch_cpu_relax();
    outb(COM1, ch);
}

bool serial_can_read(void) { return inb(COM1 + 5) & 1; }
char serial_getc(void) { return inb(COM1); }

static void serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') arch_debug_putc('\r');
        arch_debug_putc(s[i]);
    }
}

void serial_register_console(void) { console_register(serial_write); }
