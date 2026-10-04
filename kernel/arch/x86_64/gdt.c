/* x86_64 GDT and TSS: one GDT/TSS pair per CPU (the TSS holds the per-CPU ring-0 stack). */
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/boot.h>
#include <arch/gdt.h>

struct tss {
    uint32_t rsvd0;
    uint64_t rsp[3];
    uint64_t rsvd1;
    uint64_t ist[7];
    uint64_t rsvd2;
    uint16_t rsvd3, iopb;
} __packed;

static struct tss tss[MAX_CPUS];
static uint64_t gdt[MAX_CPUS][7];
static uint8_t df_stack[16384] __attribute__((aligned(16)));
static uint8_t nmi_stack[16384] __attribute__((aligned(16)));

struct [[gnu::packed]] gdtr { uint16_t limit; uint64_t base; };

void tss_set_kernel_stack(int cpu, uint64_t rsp0) { tss[cpu].rsp[0] = rsp0; }

void gdt_init_cpu(int cpu) {
    uint64_t *g = gdt[cpu];
    struct tss *t = &tss[cpu];
    g[0] = 0;
    g[1] = 0x00af9a000000ffff;   /* 0x08 kernel code (L) */
    g[2] = 0x00cf92000000ffff;   /* 0x10 kernel data */
    g[3] = 0x00cff2000000ffff;   /* 0x18 user data */
    g[4] = 0x00affa000000ffff;   /* 0x20 user code (L) */
    memset(t, 0, sizeof *t);
    if (cpu == 0) {
        t->ist[0] = (uint64_t)df_stack + sizeof df_stack;
        t->ist[1] = (uint64_t)nmi_stack + sizeof nmi_stack;
    } else {
        /* boot CPU's allocator is up by the time APs start */
        t->ist[0] = (uint64_t)PHYS_TO_VIRT(pmm_alloc_pages(2)) + 16384;
        t->ist[1] = (uint64_t)PHYS_TO_VIRT(pmm_alloc_pages(2)) + 16384;
    }
    t->iopb = sizeof *t;
    uint64_t base = (uint64_t)t, limit = sizeof *t - 1;
    g[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
           (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    g[6] = base >> 32;

    struct gdtr r = { 7 * 8 - 1, (uint64_t)g };
    /* reloading GS would clear the GS base, so save and restore it around the reload */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "movw $0x28, %%ax\n ltr %%ax\n"
        :: "m"(r) : "rax", "memory");
    __asm__ volatile("wrmsr" :: "c"(0xC0000101), "a"(lo), "d"(hi));
}

void gdt_init(void) { gdt_init_cpu(0); }
