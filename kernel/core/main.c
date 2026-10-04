#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/fbcon.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/slab.h>
#include <kernel/acpi.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/syscall.h>

void input_init(void);

static const char *strstr_simple(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return h;
    return nullptr;
}

static volatile int counters[3];
static void spinner(void *arg) {
    int id = (int)(uintptr_t)arg;
    uint64_t end = time_ns() + 300000000ULL;
    while (time_ns() < end) counters[id]++;   /* busy: forces preemption */
}
static void sleeper(void *arg) {
    for (int i = 0; i < 3; i++) { sleep_ns(50000000ULL); printk("  sleeper woke #%d at %lu ms\n", i, time_ns() / 1000000); }
}
static void sched_selftest(void) {
    struct thread *t[3];
    for (int i = 0; i < 3; i++) t[i] = thread_create("spin", spinner, (void *)(uintptr_t)i);
    thread_create("sleeper", sleeper, nullptr);
    sleep_ns(500000000ULL);
    pr_info("sched: spinner iterations %d / %d / %d (all should be non-zero)\n", counters[0], counters[1], counters[2]);
    assert(counters[0] && counters[1] && counters[2]);
    (void)t;
}

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
    pr_info("Limine base revision %lu, HHDM offset %p, cmdline '%s'\n", boot_revision(), (void *)hhdm_offset, boot_cmdline());
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
    acpi_early_init();
    sched_init();
    arch_init();
    arch_irq_enable();
    smp_init();
    acpi_late_init();
    if (strstr_simple(boot_cmdline(), "selftest")) sched_selftest();
    vfs_init();
    devices_init();
    initramfs_load();
    vfs_mkdir_at(nullptr, "/proc", 0555);
    vfs_mount("/proc", procfs_create_root());
    input_init();
    syscall_trace = strstr_simple(boot_cmdline(), "strace") != nullptr;
    static char init_path[128];
    const char *ip = strstr_simple(boot_cmdline(), "init=");
    if (ip) {
        size_t n = 0;
        for (ip += 5; *ip && *ip != ' ' && n < sizeof init_path - 1; ip++) init_path[n++] = *ip;
    }
    process_create_init(init_path[0] ? init_path : nullptr);
    /* kmain becomes a sleeping kernel thread */
    for (;;) sleep_ns(1000000000ULL);
}
