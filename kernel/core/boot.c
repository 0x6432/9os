#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define REQ __attribute__((used, section(".limine_requests"))) static volatile

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[4] = LIMINE_REQUESTS_START_MARKER;
REQ uint64_t base_revision[3] = LIMINE_BASE_REVISION(6);
REQ struct limine_framebuffer_request fb_req = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };
REQ struct limine_memmap_request memmap_req = { .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };
REQ struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };
REQ struct limine_executable_address_request kaddr_req = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0 };
REQ struct limine_rsdp_request rsdp_req = { .id = LIMINE_RSDP_REQUEST_ID, .revision = 0 };
REQ struct limine_module_request module_req = { .id = LIMINE_MODULE_REQUEST_ID, .revision = 0 };
REQ struct limine_executable_cmdline_request cmdline_req = { .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0 };
REQ struct limine_dtb_request dtb_req = { .id = LIMINE_DTB_REQUEST_ID, .revision = 0 };
REQ struct limine_mp_request mp_req = { .id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = 0 };
REQ struct limine_stack_size_request stack_req = { .id = LIMINE_STACK_SIZE_REQUEST_ID, .revision = 0, .stack_size = 65536 };
__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[2] = LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;

/* base revision the bootloader actually used */
uint64_t boot_revision(void) {
    return LIMINE_LOADED_BASE_REVISION_VALID(base_revision) ? LIMINE_LOADED_BASE_REVISION(base_revision) : 0;
}

void boot_check(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) panic("limine base revision 6 not supported");
    if (!hhdm_req.response || !memmap_req.response) panic("missing limine responses");
    hhdm_offset = hhdm_req.response->offset;
}

struct limine_memmap_response *boot_memmap(void) { return memmap_req.response; }
struct limine_framebuffer *boot_framebuffer(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count == 0) return nullptr;
    return fb_req.response->framebuffers[0];
}
struct limine_executable_address_response *boot_kernel_address(void) { return kaddr_req.response; }
void *boot_rsdp(void) {
    if (!rsdp_req.response) return nullptr;
    /* physical in base revision 3 only, HHDM-virtual from revision 4 on */
    uint64_t a = (uint64_t)rsdp_req.response->address;
    return (void *)(boot_revision() >= 4 && a >= hhdm_offset ? a - hhdm_offset : a);
}
struct limine_file *boot_module(const char *cmdline) {
    if (!module_req.response) return nullptr;
    for (uint64_t i = 0; i < module_req.response->module_count; i++) {
        struct limine_file *f = module_req.response->modules[i];
        if (!cmdline || (f->string && !strcmp(f->string, cmdline))) return f;
    }
    return nullptr;
}
const char *boot_cmdline(void) {
    return cmdline_req.response ? cmdline_req.response->cmdline : "";
}
void *boot_dtb(void) { return dtb_req.response ? dtb_req.response->dtb_ptr : nullptr; }
struct limine_mp_response *boot_mp(void) { return mp_req.response; }
