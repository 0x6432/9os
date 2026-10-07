#include <kernel/uaccess.h>
#include <kernel/mm.h>
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
#include <kernel/pagecache.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/syscall.h>

void input_init(void);
void fbdev_init(void);
void drm_init(void);
void virtio_gpu_init(void);
void evdev_register_chrdev(void);
void virtio_input_init(void);
void virtio_blk_init(void);
#include <kernel/pci.h>
#include <kernel/irq.h>

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
    stack_guard_init();
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
    harden_init();
    slab_init();
    slab_selftest();
    acpi_early_init();
    sched_init();
    arch_init();
    arch_irq_enable();
    smp_init();
    irq_work_enable();
    pmm_enable_cpu_caches();
    pmm_cache_selftest();
    slab_enable_cpu_caches();
    slab_cpu_selftest();
    acpi_late_init();
    if (strstr_simple(boot_cmdline(), "selftest")) sched_selftest();
    vfs_init();
    devices_init();
    pci_init();
    virtio_gpu_init();
    evdev_register_chrdev();
    virtio_input_init();
    writeback_init();
    ext2_init();
    virtio_blk_init();
    fbcon_init();          /* (re)attach the console if a GPU driver provided a framebuffer */
    fbdev_init();
    drm_init();
    initramfs_load();
    vfs_mkdir_at(nullptr, "/proc", 0555);
    vfs_mount("/proc", procfs_create_root());
    input_init();
    const char *ra = strstr_simple(boot_cmdline(), "root=");
    if (ra) {               /* root=/dev/vdXN [rootfstype=ext2]: boot from a disk */
        static char rootdev[64];
        size_t n = 0;
        for (ra += 5; *ra && *ra != ' ' && n < sizeof rootdev - 1; ra++) rootdev[n++] = *ra;
        int r = vfs_mount_root(rootdev, "ext2");
        if (r) pr_err("vfs: cannot mount root %s (%d), staying on the initramfs\n", rootdev, r);
    }
    mm_pressure_init();    /* kswapd: page-cache reclaim below the low watermark */
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
