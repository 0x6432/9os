#pragma once
#include <kernel/types.h>
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1b
#define USER_CS   0x23
void gdt_init(void);                 /* boot CPU */
void gdt_init_cpu(int cpu);          /* load this CPU's GDT + TSS */
void tss_set_kernel_stack(int cpu, uint64_t rsp0);
