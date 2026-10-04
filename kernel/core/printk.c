#include <kernel/printk.h>
#include <kernel/arch.h>
#include <kernel/spinlock.h>

#define MAX_CONSOLES 4
static void (*consoles[MAX_CONSOLES])(const char *, size_t);
static int nconsoles;
static spinlock_t printk_lock = SPINLOCK_INIT;

void console_register(void (*write)(const char *, size_t)) {
    if (nconsoles < MAX_CONSOLES) consoles[nconsoles++] = write;
}

void console_write(const char *s, size_t n) {
    for (int i = 0; i < nconsoles; i++) consoles[i](s, n);
}

void vprintk(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    uint64_t f = spin_lock_irqsave(&printk_lock);
    console_write(buf, n);
    spin_unlock_irqrestore(&printk_lock, f);
}

void printk(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...) {
    arch_irq_disable();
    printk("\n\x1b[41;97m KERNEL PANIC \x1b[0m ");
    va_list ap; va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
    printk("\n");
    arch_halt_forever();
}
