#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/fbcon.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/slab.h>

void kmain(void) {
    arch_early_init();
    boot_check();
    fbcon_init();
    printk("\x1b[1;36m9os\x1b[0m booting (" 
#if defined(__x86_64__)
           "x86_64"
#elif defined(__aarch64__)
           "aarch64"
#else
           "riscv64"
#endif
           ", C%ld)\n", __STDC_VERSION__);
    pr_info("HHDM offset %p, cmdline '%s'\n", (void *)hhdm_offset, boot_cmdline());
    struct limine_memmap_response *mm = boot_memmap();
    uint64_t usable = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++)
        if (mm->entries[i]->type == LIMINE_MEMMAP_USABLE) usable += mm->entries[i]->length;
    pr_info("%lu memory map entries, %lu MiB usable\n", mm->entry_count, usable >> 20);
    pmm_init();
    pmm_selftest();
    vmm_init();
    slab_init();
    slab_selftest();
    arch_init();
    pr_info("nothing left to do, halting\n");
}
