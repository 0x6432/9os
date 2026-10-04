#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* C23 compatibility for compilers that only offer -std=c2x. */
#if __STDC_VERSION__ < 202311L
#include <stdbool.h>
#ifndef nullptr
#define nullptr ((void *)0)
#endif
#endif

typedef int64_t ssize_t;
typedef int64_t off_t;
typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __packed __attribute__((packed))
#define __noreturn [[noreturn]]
#define __unused [[maybe_unused]]
