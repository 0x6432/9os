# 9os — Implementation Plan

9os is a 64-bit hobby operating system written in C23 (freestanding, clang/lld),
booted by **Limine**, using **uACPI** for ACPI and **musl** as the userspace libc.
The kernel exposes the **Linux syscall ABI** so unmodified static musl binaries
(BusyBox, then Bash) can run. Targets: **x86_64 first**, then **riscv64** and **aarch64**.

## Ground rules

- Language: C23 (`-std=c23` when the compiler supports it, otherwise `-std=c2x`).
- 64-bit only. Higher-half kernel, Limine HHDM for physical memory access.
- Physical memory: **buddy allocator** (orders 0..MAX_ORDER, 4 KiB pages).
- Kernel heap: **slab allocator** (power-of-two caches 16 B..2 KiB, larger allocations go straight to buddy).
- Scheduler: preemptive **round robin** with a fixed time slice driven by the timer interrupt.
- Syscalls: Linux numbering / calling convention per architecture; deviate only where documented in `docs/SYSCALLS.md`.
- Arch-specific code lives only in `kernel/arch/<arch>/`; everything else is portable.
- Every milestone ends with: green build, QEMU smoke test, `docs/HANDOFF.md` update,
  git push to `0x6432/9os`, and `backup_m<N>.zip`.

## Repository layout

```
kernel/
  arch/x86_64/      boot glue, GDT/IDT/TSS, ISR stubs, paging, APIC, syscall entry
  arch/riscv64/     (M11)
  arch/aarch64/     (M12)
  core/             kmain, printk, panic, timer, scheduler, syscalls
  mm/               buddy (pmm), vmm, slab, kmalloc
  fs/               vfs, tmpfs, initramfs (cpio/ustar), devfs, pipes
  drivers/          serial, framebuffer console, ps2 keyboard, tty
  acpi/             uACPI kernel API glue
  lib/              string, formatted printing, list helpers
  include/          kernel headers
third_party/        limine.h (vendored), uACPI (fetched, pinned)
userland/           test programs, initramfs content, musl/busybox/bash build scripts
scripts/            fetch-deps.sh, mkiso.sh, run-qemu.sh
docs/               PLAN.md, HANDOFF.md, SYSCALLS.md
```

## Milestones

| # | Milestone | Deliverable / exit criteria |
|---|-----------|------------------------------|
| M0 | Plan & skeleton | This plan, Makefile, linker script, dep fetch script, empty kernel boots to `hlt` |
| M1 | Boot & console | Limine boots kernel, serial (COM1) + framebuffer text console, `printk`, `panic` |
| M2 | CPU setup | GDT + TSS, IDT, exception handlers with register dump, IST for #DF |
| M3 | Physical memory | Buddy allocator over Limine memory map, self-test |
| M4 | Virtual memory & heap | Own page tables (kernel PML4), map/unmap API, slab allocator + `kmalloc/kfree` |
| M5 | ACPI & timers | uACPI integration (tables mode + full), LAPIC, IOAPIC, HPET/PIT-calibrated LAPIC timer |
| M6 | Threads & scheduler | Kernel threads, context switch, round robin preemption, sleep/wakeup, wait queues |
| M7 | User mode & syscalls | Ring 3, `syscall/sysret`, per-process address spaces, ELF64 loader, `write`/`exit` |
| M8 | VFS & initramfs | VFS layer, tmpfs, cpio initramfs from Limine module, devfs (`/dev/console`, `/dev/null`) |
| M9 | Linux ABI core | `open/read/write/close/lseek/mmap/munmap/brk/ioctl/arch_prctl/set_tid_address/uname/getpid...` — static musl hello world runs |
| M10 | Processes | `fork/clone/execve/wait4/exit_group`, signals, pipes, `dup2`, TTY line discipline + PS/2 keyboard → **BusyBox sh** interactive |
| M11 | Bash | Remaining syscalls for bash (`poll/select`, `getcwd/chdir`, `stat` family, `getdents64`, job control basics) → **Bash** runs |
| M12 | riscv64 port | Limine on RISC-V (UEFI), SBI console, Sv48 paging, PLIC/timer, `ecall` syscalls, BusyBox |
| M13 | aarch64 port | Limine on AArch64 (UEFI), PL011, 4-level paging, GICv3, generic timer, `svc` syscalls, BusyBox |
| M14 | SMP & polish | Bring-up of APs via Limine MP request, per-CPU run queues, locking audit |

## Key design notes

### Boot (Limine)
Kernel is an ELF64 linked at `0xffffffff80000000`. Requests used: base revision,
framebuffer, memory map, HHDM, executable address, RSDP, modules (initramfs), MP (later).

### Buddy allocator
- Per-zone free lists for orders 0..10 (4 KiB .. 4 MiB).
- `struct page` array covering all usable physical memory (allocated from the memory map itself).
- Allocation splits larger blocks; free coalesces with buddy `pfn ^ (1 << order)`.

### Slab allocator
- `kmem_cache` with per-cache partial/full/empty slab lists, one page (or more for big objects) per slab.
- Free list stored inside free objects. Slab header at the start of the slab page.
- `kmalloc` sizes 16..2048 use slab caches; larger go to buddy with a size header.

### Scheduler (round robin)
- Single global run queue (per-CPU in M14), FIFO.
- Timer tick at 1000 Hz, quantum = 10 ticks. On expiry the current thread is put at the tail.
- Idle thread per CPU.

### Syscalls
- x86_64: `syscall` instruction, `rax` = number, args `rdi, rsi, rdx, r10, r8, r9`.
- riscv64: `ecall`, `a7` number, `a0..a5` args. aarch64: `svc #0`, `x8` number, `x0..x5`.
- Unimplemented syscalls return `-ENOSYS` and are logged once.

### Userspace
- musl built with the same clang (`--target=<arch>-linux-musl`) as a static libc.
- BusyBox and Bash built statically against that musl; packed into a cpio initramfs.
