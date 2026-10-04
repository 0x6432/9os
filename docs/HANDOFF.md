# 9os — Handoff

_Updated after every milestone. Read this first when picking up the project._

## Current state: M12 complete — x86_64 and riscv64 both boot BusyBox + Bash

| Milestone | Status |
|-----------|--------|
| M0 Plan & skeleton | ✅ |
| M1 Boot & console | ✅ serial COM1 + framebuffer console (8x16 font, ANSI subset) |
| M2 CPU setup | ✅ GDT (kernel/user/TSS), IDT w/ 256 stubs, IST for #DF/NMI, register dump + backtrace |
| M3 Physical memory | ✅ buddy allocator (orders 0..10), self-test |
| M4 Virtual memory & heap | ✅ own PML4, NX/WP, PAT WC for framebuffer, slab + kmalloc, self-test |
| M5 ACPI & timers | ✅ uACPI 6.1.1 (tables + namespace, power button → S5), LAPIC/IOAPIC, TSC via PIT, 1 kHz LAPIC timer |
| M6 Threads & scheduler | ✅ kernel threads, fxsave/fxrstor + FS base per thread, round robin (10 ms quantum), sleep list, wait queues, zombie reaping in idle |
| M7 User mode & syscalls | ✅ ring 3, `syscall` entry → common trap frame, per-process page tables + VMAs (demand-zero), ELF64 loader (static/PIE), SysV stack, fork/wait/exit |
| M8 VFS & initramfs | ✅ VFS (path walk, symlinks, mounts), tmpfs, newc cpio initramfs, /dev nodes (null, zero, random, console/tty), pipes, TTY line discipline, PS/2 + serial input |
| M9 Linux ABI core (musl) | ✅ musl 1.2.5 built with clang (`userland/build-musl.sh`), `userland/musl-cc` wrapper, `tests/libctest.c` passes (malloc/mmap, stdio, dirs, symlinks, pipes, fork/wait, signals, clocks) |
| M10 BusyBox | ✅ static BusyBox 1.36.1: busybox init + inittab, ash with job control, Ctrl-C, vi, pipes, tar/gzip, awk, ps/free (procfs), RTC wall clock |
| M11 Bash | ✅ static Bash 5.2.37: loops, $(...), <(...) via /dev/fd, here-docs, jobs/wait, indexed+assoc arrays, recursion, Ctrl-C, exit back to ash |
| M12 riscv64 | ✅ Limine/UEFI (edk2) boot, Sv48, SBI timer+console (polled input), goldfish RTC, uACPI tables, ecall syscalls (asm-generic numbers), sigreturn trampoline page, FPU (D) context; BusyBox, libctest, Bash all pass |
| M13 aarch64 | ⏳ in progress (sources written in kernel/arch/aarch64, not yet booted) |

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

## Scheduler notes (M6)
- `kernel/core/sched.c`: single FIFO run queue; `__schedule()` must run with IRQs off.
- Preemption: `sched_tick()` (from `timer_tick`) sets `need_resched`; `trap_exit_hook` → `trap_exit_hook_sched()` switches before `iretq`.
- `wait_event(q)` / `wake_up(q)`; `wait_until(q, cond)` macro; `sleep_ns()`.
- `thread_exit()` → zombie list, freed by idle thread. Boot context becomes thread "kmain" (tid 0).
- x86: `switch.S` saves callee-saved regs; new threads start in `x86_thread_trampoline` (r12=fn, r13=arg).
- `arch_switch_mm()` is a weak hook for address-space switching (M7).

## User mode / process notes (M7–M8)
- Every thread's kernel stack top holds its user `struct trap_frame` (`thread_user_frame(t)`); kernel-mode
  start frames are placed below it. Syscalls build the same frame (vector 0x100) and return via `trap_return`/`iretq`.
- `user_return_work(f)` (core/syscall.c) runs on every return: reschedule, SIGALRM, signal delivery, mask restore.
- Syscall table `core/syscall.c` is **generated** from `arch/x86_64/include/arch/unistd.h` + `sys_*` functions
  (the python snippet lives in `scripts/gen-syscalls.py`). Missing syscalls → `-ENOSYS` with a one-time warning.
- Kernel cmdline: `init=/path`, `strace` (log every syscall), `selftest` (scheduler test).
- Memory: `kernel/mm/mm.c` VMAs, demand-zero faults, `mm_write()` for writing into inactive address spaces,
  fork = eager copy (COW is future work). `copy_{from,to}_user` validate against VMAs and pre-fault.
- VFS: `kernel/fs/vfs.c` (inodes are refcounted with `iget/iput`, freed when `nlink==0 && refcount==0`),
  `tmpfs.c` (page-vector files), `initramfs.c`, `pipe.c`, `devices.c`; TTY in `drivers/tty.c`.
- Signals: `core/signal.c` + `arch/x86_64/signal_frame.c` (Linux `rt_sigframe` layout), SA_RESTART via
  `frame_restart_syscall`, job-control stop/continue.
- Processes: `core/process.c` — clone flags CLONE_VM/VFORK/THREAD/SETTLS/*TID handled; pid == main thread tid.

## Userland toolchain (M9)
- `userland/build-musl.sh` → `userland/sysroot/<arch>` (not committed; rebuild in ~30 s).
- `userland/musl-cc` → clang + lld, static, links crt1/crti/crtn + libc.a + libgcc.a (x86_64).
- Test: `userland/musl-cc -O2 -o userland/root/sbin/init userland/tests/libctest.c && make iso run`.

## BusyBox notes (M10)
- `userland/build-busybox.sh` (defconfig + static; TC/SEEDRNG disabled; host UAPI headers copied into the sysroot).
- `userland/mkroot.sh` assembles `userland/root` (busybox + applet symlinks from `busybox.links`, `skel/` files, libctest).
  Then `make iso` packs it as the cpio initramfs.
- procfs (`kernel/fs/procfs.c`): /proc/{self,<pid>/{stat,status,cmdline,comm,maps,fd/,cwd,exe},meminfo,uptime,mounts,...}.
  fd/cwd/exe are "magic links" (`inode_ops.follow_link`).
- RTC (`arch/x86_64/rtc.c`) sets `boot_epoch` for CLOCK_REALTIME.

## Bash notes (M11)
- `userland/build-bash.sh` builds `userland/build/bash/bash` (static, `--without-bash-malloc --enable-static-link`, musl-cc).
- `userland/mkroot.sh` copies it to `/bin/bash` automatically when present. Run `bash` from ash, or boot with `init=/bin/bash`.

## Multi-arch layout (M12)
- Build: `make ARCH=<x86_64|riscv64|aarch64> iso`, run: `make ARCH=... run QEMUFLAGS="-display none"`
  (non-x86 runs boot via edk2 pflash firmware copied to `build/<arch>/fw-*.fd` by the `firmware` target).
- Userland per arch: `ARCH=riscv64 userland/build-all.sh` (musl → compiler-rt builtins → BusyBox → Bash → `userland/root-<arch>`).
- Arch interface (each `kernel/arch/<arch>/`): `include/arch/{cpu,trapframe,syscall,thread,stat,unistd}.h`,
  linker.ld, entry (console/timer/irq/rtc/poweroff), trap dispatch, paging, thread switch + FPU, signal frame.
  Generic code uses `SC_*`, `FRAME_*`, `FRAME_IS_SYSCALL`, `frame_restart_syscall`, `ARCH_PLATFORM`, optional `ARCH_SIGTRAMP_CODE`.
- `unistd.h` for riscv64/aarch64 is generated from `<asm-generic/unistd.h>`; `scripts/gen-syscalls.py` builds one table for all arches.
- Kernel log ring buffer (32 KiB) replays to late consoles and backs `dmesg`/syslog(2).
- riscv64 notes: sscratch holds the kernel stack top while in U-mode; SUM is always set; A/D bits preset in PTEs;
  `fence.i` on every return to user; console input polled from the 1 kHz timer (no PLIC driver yet).

## Environment recovery
The sandbox can be wiped. `scripts/setup-env.sh` reinstalls host packages, rebuilds QEMU 9.2.3 into /data/tools/qemu,
fetches Limine + uACPI and builds all userlands.

## Next steps (M13 aarch64)
1. Boot `make ARCH=aarch64 run` (QEMU virt, GICv2, cortex-a72); debug vectors/paging (TTBR0 user, TTBR1 kernel, MAIR indices).
2. Signal frame (Linux rt_sigframe with fpsimd_context), then BusyBox + Bash.
3. M14 SMP; PLIC/virtio drivers; COW fork; sockets; disk filesystem.
4. Known gaps: no COW fork (eager copy), single CPU, no sockets, tmpfs only.
