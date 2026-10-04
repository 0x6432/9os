#pragma once
/* Bootloader-provided information (Limine), arch independent. */
#include <kernel/types.h>
#include <limine.h>

extern uint64_t hhdm_offset;
#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + hhdm_offset))
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - hhdm_offset)

struct limine_memmap_response *boot_memmap(void);
struct limine_framebuffer *boot_framebuffer(void);
void boot_set_framebuffer(struct limine_framebuffer *fb);   /* e.g. virtio-gpu replaces/provides one */
struct limine_executable_address_response *boot_kernel_address(void);
void *boot_rsdp(void);           /* physical address or nullptr */
struct limine_file *boot_module(const char *cmdline);
const char *boot_cmdline(void);
struct limine_mp_response *boot_mp(void);
void *boot_dtb(void);             /* flattened device tree (virtual) or nullptr */
void boot_check(void);
uint64_t boot_revision(void);      /* Limine base revision used to load us */
