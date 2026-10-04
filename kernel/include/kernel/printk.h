#pragma once
#include <kernel/types.h>
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(const char *fmt, va_list ap);
void console_write(const char *s, size_t n);
void console_register(void (*write)(const char *, size_t));
__noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define pr_info(fmt, ...)  printk("[\x1b[32m info\x1b[0m] " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)  printk("[\x1b[33m warn\x1b[0m] " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)   printk("[\x1b[31merror\x1b[0m] " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) do { if (0) printk(fmt, ##__VA_ARGS__); } while (0)
#define assert(c) do { if (unlikely(!(c))) panic("assertion failed: %s at %s:%d", #c, __FILE__, __LINE__); } while (0)
