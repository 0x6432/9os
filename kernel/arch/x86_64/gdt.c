#include <kernel/types.h>
#include <kernel/string.h>
#include <arch/gdt.h>

struct tss {
    uint32_t rsvd0;
    uint64_t rsp[3];
    uint64_t rsvd1;
    uint64_t ist[7];
    uint64_t rsvd2;
    uint16_t rsvd3, iopb;
} __packed;

static struct tss tss;
static uint64_t gdt[7];
static uint8_t df_stack[16384] __attribute__((aligned(16)));
static uint8_t nmi_stack[16384] __attribute__((aligned(16)));

struct [[gnu::packed]] gdtr { uint16_t limit; uint64_t base; };

void tss_set_kernel_stack(uint64_t rsp0) { tss.rsp[0] = rsp0; }

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffff;   /* 0x08 kernel code (L) */
    gdt[2] = 0x00cf92000000ffff;   /* 0x10 kernel data */
    gdt[3] = 0x00cff2000000ffff;   /* 0x18 user data */
    gdt[4] = 0x00affa000000ffff;   /* 0x20 user code (L) */
    memset(&tss, 0, sizeof tss);
    tss.ist[0] = (uint64_t)df_stack + sizeof df_stack;
    tss.ist[1] = (uint64_t)nmi_stack + sizeof nmi_stack;
    tss.iopb = sizeof tss;
    uint64_t base = (uint64_t)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[6] = base >> 32;

    struct gdtr r = { sizeof gdt - 1, (uint64_t)gdt };
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
}
