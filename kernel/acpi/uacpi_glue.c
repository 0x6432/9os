/* uACPI kernel API implementation. */
#include <kernel/types.h>
#include <kernel/acpi.h>
#include <kernel/boot.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/vmm.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>
#include <kernel/irq.h>
#include <kernel/arch.h>
#include <kernel/sched.h>

#ifdef HAVE_UACPI
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    void *r = boot_rsdp();
    if (!r) return UACPI_STATUS_NOT_FOUND;
    *out = (uacpi_phys_addr)r;
    return UACPI_STATUS_OK;
}
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) { return vmm_map_mmio(addr, len); }
void uacpi_kernel_unmap(void *addr, uacpi_size len) {}

void uacpi_kernel_log(uacpi_log_level lvl, const uacpi_char *msg) {
    static const char *names[] = { "", "error", "warn", "info", "trace", "debug" };
    if (lvl > UACPI_LOG_INFO) return;
    printk("[uacpi %s] %s", lvl < 6 ? names[lvl] : "?", msg);
}

/* PCI configuration space through legacy port I/O (mechanism #1). */
#ifdef __x86_64__
struct pci_dev_handle { uacpi_pci_address a; };
static uint32_t pci_addr(uacpi_pci_address *a, uacpi_size off) {
    return 0x80000000u | (a->bus << 16) | (a->device << 11) | (a->function << 8) | (off & 0xfc);
}
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) {
    struct pci_dev_handle *h = kmalloc(sizeof *h);
    h->a = a; *out = h;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { kfree(h); }
#define PCI_RW(bits, type, inop, outop) \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size off, type *v) { \
    outl(0xcf8, pci_addr(&((struct pci_dev_handle *)h)->a, off)); \
    *v = inop(0xcfc + (off & (4 - sizeof(type)))); return UACPI_STATUS_OK; } \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size off, type v) { \
    outl(0xcf8, pci_addr(&((struct pci_dev_handle *)h)->a, off)); \
    outop(0xcfc + (off & (4 - sizeof(type))), v); return UACPI_STATUS_OK; }
PCI_RW(8, uacpi_u8, inb, outb)
PCI_RW(16, uacpi_u16, inw, outw)
PCI_RW(32, uacpi_u32, inl, outl)

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    *out = (uacpi_handle)base; return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) {}
#define IO_RW(bits, type, inop, outop) \
uacpi_status uacpi_kernel_io_read##bits(uacpi_handle h, uacpi_size off, type *v) { \
    *v = inop((uint16_t)((uintptr_t)h + off)); return UACPI_STATUS_OK; } \
uacpi_status uacpi_kernel_io_write##bits(uacpi_handle h, uacpi_size off, type v) { \
    outop((uint16_t)((uintptr_t)h + off), v); return UACPI_STATUS_OK; }
IO_RW(8, uacpi_u8, inb, outb)
IO_RW(16, uacpi_u16, inw, outw)
IO_RW(32, uacpi_u32, inl, outl)
#else
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) { return UACPI_STATUS_UNIMPLEMENTED; }
void uacpi_kernel_pci_device_close(uacpi_handle h) {}
#define PCI_RW(bits, type) \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size off, type *v) { *v = 0; return UACPI_STATUS_UNIMPLEMENTED; } \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size off, type v) { return UACPI_STATUS_UNIMPLEMENTED; }
PCI_RW(8, uacpi_u8) PCI_RW(16, uacpi_u16) PCI_RW(32, uacpi_u32)
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) { return UACPI_STATUS_UNIMPLEMENTED; }
void uacpi_kernel_io_unmap(uacpi_handle h) {}
#define IO_RW(bits, type) \
uacpi_status uacpi_kernel_io_read##bits(uacpi_handle h, uacpi_size off, type *v) { *v = 0; return UACPI_STATUS_UNIMPLEMENTED; } \
uacpi_status uacpi_kernel_io_write##bits(uacpi_handle h, uacpi_size off, type v) { return UACPI_STATUS_UNIMPLEMENTED; }
IO_RW(8, uacpi_u8) IO_RW(16, uacpi_u16) IO_RW(32, uacpi_u32)
#endif

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size); }
void uacpi_kernel_free(void *mem, uacpi_size size) { kfree(mem); }

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return time_ns(); }
void uacpi_kernel_stall(uacpi_u8 usec) { udelay(usec); }
void uacpi_kernel_sleep(uacpi_u64 msec) { udelay(msec * 1000); }

/* Mutexes: a spinlock with yielding (fine for a uniprocessor hobby kernel). */
struct umutex { volatile int locked; };
uacpi_handle uacpi_kernel_create_mutex(void) { return kzalloc(sizeof(struct umutex)); }
void uacpi_kernel_free_mutex(uacpi_handle h) { kfree(h); }
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout) {
    struct umutex *m = h;
    uint64_t deadline = time_ns() + (uint64_t)timeout * 1000000;
    while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE)) {
        if (timeout == 0) return UACPI_STATUS_TIMEOUT;
        if (timeout != 0xffff && time_ns() > deadline) return UACPI_STATUS_TIMEOUT;
        sched_yield();
    }
    return UACPI_STATUS_OK;
}
void uacpi_kernel_release_mutex(uacpi_handle h) { __atomic_store_n(&((struct umutex *)h)->locked, 0, __ATOMIC_RELEASE); }

struct uevent { volatile int64_t count; };
uacpi_handle uacpi_kernel_create_event(void) { return kzalloc(sizeof(struct uevent)); }
void uacpi_kernel_free_event(uacpi_handle h) { kfree(h); }
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout) {
    struct uevent *e = h;
    uint64_t deadline = time_ns() + (uint64_t)timeout * 1000000;
    for (;;) {
        int64_t c = __atomic_load_n(&e->count, __ATOMIC_ACQUIRE);
        if (c > 0 && __atomic_compare_exchange_n(&e->count, &c, c - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return true;
        if (timeout == 0 || (timeout != 0xffff && time_ns() > deadline)) return false;
        sched_yield();
    }
}
void uacpi_kernel_signal_event(uacpi_handle h) { __atomic_add_fetch(&((struct uevent *)h)->count, 1, __ATOMIC_RELEASE); }
void uacpi_kernel_reset_event(uacpi_handle h) { __atomic_store_n(&((struct uevent *)h)->count, 0, __ATOMIC_RELEASE); }

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return (uacpi_thread_id)(uintptr_t)(current ? current->tid : 1); }

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return arch_irq_save(); }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { arch_irq_restore(s); }

uacpi_handle uacpi_kernel_create_spinlock(void) { return kzalloc(sizeof(spinlock_t)); }
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) { return spin_lock_irqsave(h); }
void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f) { spin_unlock_irqrestore(h, f); }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL) pr_err("acpi: fatal firmware error\n");
    return UACPI_STATUS_OK;
}

struct uirq { uacpi_interrupt_handler fn; uacpi_handle ctx; };
static int uirq_trampoline(void *ctx) {
    struct uirq *u = ctx;
    return (u->fn(u->ctx) & UACPI_INTERRUPT_HANDLED) ? IRQ_HANDLED : IRQ_NONE;
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler fn,
                                                    uacpi_handle ctx, uacpi_handle *out) {
    struct uirq *u = kmalloc(sizeof *u);
    u->fn = fn; u->ctx = ctx;
    if (irq_request((int)irq, "acpi", uirq_trampoline, nullptr, u) < 0) { kfree(u); return UACPI_STATUS_INTERNAL_ERROR; }
    *out = u;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle h) {
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type t, uacpi_work_handler fn, uacpi_handle ctx) {
    fn(ctx);        /* run synchronously */
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) { return UACPI_STATUS_OK; }

static uacpi_interrupt_ret power_button(uacpi_handle ctx) {
    pr_info("acpi: power button pressed, shutting down\n");
    acpi_poweroff();
    return UACPI_INTERRUPT_HANDLED;
}

void acpi_early_init(void) {
    uacpi_status st = uacpi_initialize(0);
    if (uacpi_unlikely_error(st)) { pr_err("acpi: uacpi_initialize: %s\n", uacpi_status_to_string(st)); return; }
    pr_info("acpi: tables ready\n");
}

void *acpi_find_table(const char *sig) {
    uacpi_table t;
    if (uacpi_table_find_by_signature(sig, &t) != UACPI_STATUS_OK) return nullptr;
    return t.ptr;
}

void acpi_late_init(void) {
    uacpi_status st = uacpi_namespace_load();
    if (uacpi_unlikely_error(st)) { pr_err("acpi: namespace load: %s\n", uacpi_status_to_string(st)); return; }
    st = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(st)) { pr_err("acpi: namespace init: %s\n", uacpi_status_to_string(st)); return; }
    uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, nullptr);
    uacpi_finalize_gpe_initialization();
    pr_info("acpi: namespace loaded and initialized\n");
}

void acpi_poweroff(void) {
    uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    arch_irq_disable();
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
}
void acpi_reboot(void) { uacpi_reboot(); }
#else
void acpi_early_init(void) { pr_warn("acpi: built without uACPI\n"); }
void acpi_late_init(void) {}
void *acpi_find_table(const char *sig) { return nullptr; }
void acpi_poweroff(void) {}
void acpi_reboot(void) {}
#endif
