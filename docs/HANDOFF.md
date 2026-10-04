# 9os — Handoff

_Updated after every milestone. Read this first when picking up the project._

## Current state: M5 complete (x86_64)

| Milestone | Status |
|-----------|--------|
| M0 Plan & skeleton | ✅ |
| M1 Boot & console | ✅ serial COM1 + framebuffer console (8x16 font, ANSI subset) |
| M2 CPU setup | ✅ GDT (kernel/user/TSS), IDT w/ 256 stubs, IST for #DF/NMI, register dump + backtrace |
| M3 Physical memory | ✅ buddy allocator (orders 0..10), self-test |
| M4 Virtual memory & heap | ✅ own PML4, NX/WP, PAT WC for framebuffer, slab + kmalloc, self-test |
| M5 ACPI & timers | ✅ uACPI 6.1.1 (tables + namespace, power button → S5), LAPIC/IOAPIC, TSC via PIT, 1 kHz LAPIC timer |
| M6 Threads & scheduler | ⏳ next |

## Build environment used
- clang 15.0.7 / ld.lld (Amazon Linux 2023). clang 15 has no `-std=c23`, so the Makefile
  auto-detects and falls back to `-std=c2x`; `kernel/include/kernel/types.h` shims `nullptr`/`bool`.
- QEMU 9.2.3 built from source into `/data/tools/qemu` (not in the repo).
- Limine v9.x-binary (API revision 3, base revision 3).

## Code map
- `kernel/core/boot.c` – all Limine requests; `boot_*()` accessors; `hhdm_offset`.
- `kernel/core/main.c` – `kmain()` init sequence.
- `kernel/core/printk.c` – console fan-out, `printk`, `panic`.
- `kernel/lib/` – string functions, `vsnprintf`.
- `kernel/drivers/fbcon.c` – framebuffer console (supports SGR colours, cursor moves, J/K erase).
- `kernel/mm/buddy.c` – `page_alloc/page_free`, `pmm_alloc_pages`, `struct page` array (`page_array`).
- `kernel/mm/slab.c` – `kmem_cache_*`, `kmalloc/kzalloc/krealloc/kfree` (≤2 KiB slab, larger → buddy).
- `kernel/acpi/uacpi_glue.c` – uACPI kernel API (PCI via 0xCF8, port I/O, mutex/event spin+yield, work runs synchronously).
- `kernel/arch/x86_64/apic.c` – PIC mask, MADT parse, `irq_install(gsi)` → IOAPIC route, `irq_eoi`, `time_ns()` (TSC), LAPIC periodic timer (vector 32).
- `kernel/core/time.c` – `jiffies`, `timer_tick()` → weak `sched_tick()`, `udelay`.
- `kernel/arch/x86_64/` – `entry.c` (kmain_entry), `gdt.c`, `idt.c` + generated `isr.S`, `paging.c`, `serial.c`.

## Conventions
- GDT: 0x08 kcode, 0x10 kdata, 0x18 udata, 0x20 ucode, 0x28 TSS (sysret-compatible ordering).
- All physical memory is reachable at `PHYS_TO_VIRT(pa)` (HHDM).
- Upper-half PML4 entries 256..511 are pre-allocated so every user address space shares them.
- Weak hooks in `idt.c`: `page_fault_handler`, `user_exception`, `trap_exit_hook` (scheduler/signals override them).

## Testing
`make iso && qemu-system-x86_64 -M q35 -m 512M -cdrom build/x86_64/9os.iso -serial stdio`
Expected: boot banner, pmm/slab self-tests pass, "nothing left to do, halting".

## Done in M5 (kept for reference)
1. Fetch uACPI (scripts/fetch-deps.sh), implement `kernel/acpi/uacpi_glue.c` (kernel API: map/unmap,
   logging, PCI/IO access, mutex/event/spinlock stubs, interrupt install, scheduling hooks).
2. Parse MADT → LAPIC + IOAPIC; mask legacy PIC.
3. Calibrate LAPIC timer against HPET (or PIT fallback) and run it at 1000 Hz.

## Next steps (M6)
1. Replace `kernel/core/sched.c` placeholder: `struct thread` (kernel stack, saved context, state), `thread_create`.
2. `arch/x86_64/switch.S`: callee-saved register context switch.
3. Round robin run queue; `sched_tick()` decrements the quantum (10 ms) and sets need_resched; reschedule in `trap_exit_hook`.
4. Sleep queues / `wait_queue`, idle thread, `thread_exit` + reaper.
