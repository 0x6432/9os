#include <kernel/sched.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <kernel/spinlock.h>
#include <kernel/cpu.h>

#define MAX_CONSOLES 4
static void (*consoles[MAX_CONSOLES])(const char *, size_t);
static int nconsoles;
static spinlock_t printk_lock = SPINLOCK_INIT;

/* kernel log ring buffer: replayed to late consoles, read by syslog(2)/dmesg */
#define LOG_SIZE 32768
static char logbuf[LOG_SIZE];
static uint64_t log_pos;            /* total bytes ever logged */

static void log_append(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) logbuf[(log_pos + i) % LOG_SIZE] = s[i];
    log_pos += n;
}

/* Copy the retained log (oldest first) into buf; returns bytes copied. */
size_t log_read(char *buf, size_t len) {
    uint64_t start = log_pos > LOG_SIZE ? log_pos - LOG_SIZE : 0;
    size_t n = 0;
    for (uint64_t i = start; i < log_pos && n < len; i++) buf[n++] = logbuf[i % LOG_SIZE];
    return n;
}

void console_register(void (*write)(const char *, size_t)) {
    if (nconsoles >= MAX_CONSOLES) return;
    uint64_t f = spin_lock_irqsave(&printk_lock);
    uint64_t start = log_pos > LOG_SIZE ? log_pos - LOG_SIZE : 0;
    for (uint64_t i = start; i < log_pos;) {      /* replay earlier messages */
        size_t off = i % LOG_SIZE, chunk = MIN(LOG_SIZE - off, log_pos - i);
        write(logbuf + off, chunk);
        i += chunk;
    }
    consoles[nconsoles++] = write;
    spin_unlock_irqrestore(&printk_lock, f);
}

/* Console output can come from any CPU and from interrupt handlers (tty echo) that interrupt a
 * write on the same CPU, so the console drivers (fbcon state, UART) are serialised here with
 * interrupts off. A nested entry on the owning CPU (exception during output, panic) skips the lock. */
static spinlock_t console_lock = SPINLOCK_INIT;
static volatile int console_owner = -1;
extern volatile bool panicking;
void console_write(const char *s, size_t n) {
    uint64_t f = arch_irq_save();
    int me = this_cpu()->id;
    bool nested = console_owner == me || panicking;
    if (!nested) { spin_lock(&console_lock); console_owner = me; }
    for (int i = 0; i < nconsoles; i++) consoles[i](s, n);
    if (!nested) { console_owner = -1; spin_unlock(&console_lock); }
    arch_irq_restore(f);
}

void vprintk(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    uint64_t f = arch_irq_save();
    bool locked = !panicking;
    if (locked) spin_lock(&printk_lock);
    log_append(buf, n);
    console_write(buf, n);
    if (locked) spin_unlock(&printk_lock);
    arch_irq_restore(f);
}

void printk(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}

volatile bool panicking;
void smp_stop_others(void);
void panic(const char *fmt, ...) {
    arch_irq_disable();
    panicking = true;
    smp_stop_others();
    printk("\n\x1b[41;97m KERNEL PANIC \x1b[0m (cpu %d) ", this_cpu()->id);
    va_list ap; va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
    printk("\n");
    arch_halt_forever();
}
