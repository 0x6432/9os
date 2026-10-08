# 9os — Handoff

_Updated after every milestone. Read this first when picking up the project._

## Current state: M23 and M25 complete; M24 (fine-grained locking) remains in progress — per-CPU run-queue locks and allocator caches, periodic balancing, CPU accounting and deadline-driven idle on all three architectures

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
| M13 aarch64 | ✅ EL1, TTBR0 (user) / TTBR1 (kernel) 4-level paging, GICv2, virtual generic timer, PL011 (polled input), PL031 RTC, PSCI poweroff, svc syscalls, rt_sigframe with fpsimd_context, TPIDR_EL0 TLS; BusyBox, libctest, Bash all pass |
| M14 SMP | ✅ up to 16 CPUs on all arches: big kernel lock (ticket), IPIs, TLB shootdown, per-CPU current/idle/timers, lock-free tick on secondary CPUs; `make SCHED=rr\|mlfq` |
| M15 Graphics base | ✅ `/dev/fb0` (Linux fbdev ABI + mmap), PCI ECAM enumeration, virtio-gpu 2D driver (riscv64/aarch64 display), `fbdemo`, `schedtest` |
| M16 POSIX IPC base | ✅ COW fork, shared mappings across fork, memfd/tmpfs `MAP_SHARED`, AF_UNIX (stream/seqpacket/dgram, SCM_RIGHTS, SO_PEERCRED), epoll, eventfd, timerfd, signalfd, batched TLB flushes; `cowtest`, `ipctest` |
| M17 Terminals + input | ✅ `/dev/ptmx` + `/dev/pts/N`, generic virtio-pci layer, virtio-input keyboard/tablet + PS/2 → evdev `/dev/input/eventN`, VT/KD ioctls for seatd-style sessions; `ptytest`, `evtest` |
| M18 KMS-lite | ✅ `/dev/dri/card0`: legacy KMS (1 connector/encoder/CRTC/primary plane), GEM dumb buffers + mmap, ADDFB/ADDFB2/RMFB, SETCRTC, PAGE_FLIP with flip events, DIRTYFB, WAIT_VBLANK; `drmdemo` |
| M19 Dynamic linking + inotify | ✅ `PT_INTERP` → musl `libc.so` as `/lib/ld-musl-<arch>.so.1`, shared libs + `dlopen`, `membarrier`; inotify with VFS hooks; `dyntest`, `inotifytest` |
| M20 Ports + libwayland | ✅ page cache for private file mappings + exec (3.4× faster exec), `userland/ports` (meson/autotools cross helpers): Lua 5.4, SQLite 3.47 (FTS5), libffi, expat, wayland 1.23.1; `wltest`, `mapprivtest` |
| M21 Wayland compositor | ✅ ports wayland-protocols, pixman, libxkbcommon, libdrm (`modetest -M 9os`, `vbltest`); DRM SET_VERSION, 60 Hz deadline vblank, clipped DIRTYFB, rect damage → virtio-gpu partial transfers; `wlkms` compositor + `wlclient` |
| M23 CI boot tests | ✅ `scripts/qemu-test.py` (expect-style serial driver, panic detection, per-command exit status), `scripts/ci-tests.sh` (13 checks) run for all arches in GitHub Actions before a release; found and fixed a console race and a missing `__clear_cache` on riscv64 |
| M24 Locking (in progress) | ◐ `sched_lock` for run queue/sleep list/wait queues (held across the switch), BKL dropped on switch and retaken after, idle without BKL, `thread_interrupt()`; lock-free syscall fast path; per-mm lock, atomic page refcounts, page faults + user copies without BKL; IRQ-safe console lock; lock-free read/write for pipes + eventfd + AF_UNIX (unix_lock); futex with hashed bucket locks; `sysbench`, `faulttest`, `pipetest`, `futextest`, `efdtest`, `socktest` |
| M25 Scheduling | ✅ independent per-CPU run-queue locks, affinity, nice + FIFO/RR, CPU accounting, periodic busy-CPU balancing, bounded per-CPU PMM/slab caches, deadline-driven tickless idle |
| M22 Wayland terminal | ✅ `wlterm`: pty + shell, 8x16 font, ANSI/VT subset (cursor motion, erase, insert/delete, SGR 16 colours, DSR), US keymap from evdev codes; wlkms renders real title text; `scripts/qemu-type.py` types into the guest via the QEMU monitor |

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

## Environment
- Limine **11.4.1** (binary branch `v11.4.1-binary`), protocol header from limine-protocol trunk, **base revision 6**
  (RSDP is HHDM-virtual from rev 4; aarch64 CPACR_EL1=0 at entry so `kmain_entry` enables FP/SIMD).
- QEMU comes from the distro package manager (`scripts/setup-env.sh` handles apt/pacman/dnf/brew) — no source build.
  UEFI firmware (edk2) is located automatically for riscv64/aarch64 (`FW_CODE=` overrides) and padded to the pflash size.
- `make iso` refuses to build with an empty `userland/root-<arch>`: run `ARCH=<arch> userland/build-all.sh` first.
- The agent sandbox has been wiped four times; push WIP often. (There, QEMU 11.1 comes from Alpine edge packages via apk.static,
  plus `qemu-hw-display-virtio-gpu{,-pci}`; the wrappers in /data/tools/bin set `QEMU_MODULE_DIR`.)

## Next steps
1. M14 SMP (Limine MP request; per-CPU data, run queues, IPIs, TLB shootdown).
2. Interrupt-driven input on riscv64 (PLIC) / aarch64 (PL011 IRQ via GIC); GICv3.
3. COW fork, sockets (AF_UNIX), virtio-blk + a disk filesystem.
4. Known gaps: no COW fork (eager copy), single CPU, no sockets, tmpfs only.

## M14 (in progress): SMP + selectable scheduler
- Done and tested on x86_64, riscv64, and aarch64 with `-smp 4`: BusyBox, libctest, and `smptest` (pthreads speed-up, getcpu spread, mutex/atomic counters) all pass.
- `kernel/core/smp.c`: `struct cpu cpus[]` (`kernel/include/kernel/cpu.h`), Limine MP bring-up (`boot_mp()`), and a **big kernel lock**. It is a ticket lock with recursive depth tracked per thread. It is taken on every trap/syscall entry and released on return to user mode and in the idle halt. `bkl_enter` locks with IRQs off; this fixed an IRQ-during-spin race. IPIs (`IPI_TLB_FLUSH`, `IPI_RESCHED`) run without the BKL, and CPUs spinning for the BKL poll them. `tlb_shootdown(root, va)` works as follows: x86 sends LAPIC IPI vector 0xf0, riscv uses SBI RFENCE, and aarch64 needs nothing (`tlbi ...is`). Panics stop the other CPUs.
- Per-arch `current`: x86 uses `%gs:24` (struct cpu), riscv uses `tp` (sscratch = thread while in user mode; trap.S uses `arch_thread.ktop/uscratch` at offsets 8/16), and aarch64 uses `TPIDR_EL1`. `this_cpu()` is `current->cpu` on riscv/aarch64. AP trampolines switch to `cpu->kernel_sp` before touching the MMU. x86 has a per-CPU GDT/TSS and a per-CPU LAPIC timer. aarch64 sets up the GIC CPU interface, SGIs, and the timer PPI per CPU.
- Scheduler (`core/sched.c`): one global run queue. The policy is chosen at build time: `make SCHED=rr` (default) or `make SCHED=mlfq` (4 levels with 5/10/20/40 ms quanta, demotion on allotment use, boost every 500 ms). The `on_cpu`/`sched_finish_switch` handoff guards against running a thread whose context is still live. `/proc/sched`, per-CPU `/proc/stat`, `/proc/cpuinfo`, `sched_getaffinity`, and `getcpu` are implemented.
- `make run SMP=N` (default 4) and `scripts/smoke-test.sh ARCH WAIT cmd...` (env STEP, TIMEOUT, SMP).
- **Next steps:** (1) Handle the timer tick on non-boot CPUs without the BKL. Idle CPUs ticking at 1 kHz contend for the lock, so the riscv/aarch64 TCG speed-up is poor. Plan: split `sched_tick` into a lock-free per-CPU part and a cpu0 global part. (2) Framebuffer: `/dev/fb0` (FBIOGET_V/FSCREENINFO + mmap) and `userland/demos/fbdemo.c` (lines, Mandelbrot, Julia, plasma); mkroot already builds `userland/demos/*.c`. (3) Test MLFQ behaviour. (4) Continue with docs/ROADMAP.md.

## M15: framebuffer graphics, PCI, virtio-gpu, MLFQ check
- `kernel/drivers/fbdev.c`: `/dev/fb0` (char 29:0) with `FBIOGET_VSCREENINFO`/`FBIOPUT_VSCREENINFO` (fixed mode)/`FBIOGET_FSCREENINFO`/`FBIOPAN_DISPLAY`/`FBIOBLANK`, read/write and `mmap`. `file_ops.mmap(f, off, len, &pa)` returns physical pages; `sys_mmap` maps them as a `VMA_PHYS` VMA (write-combining, never freed, shared across fork). fbcon pauses while fb0 is open and repaints on the last close.
- `kernel/drivers/pci.c`: ECAM from ACPI MCFG (first 8 buses), device table, BAR decode/sizing, `pci_enable`. Logged at boot (`dmesg | grep pci`).
- `kernel/drivers/virtio_gpu.c`: virtio-pci modern transport (common/notify caps), one polled control queue, GET_DISPLAY_INFO → RESOURCE_CREATE_2D (B8G8R8X8) → ATTACH_BACKING (one contiguous buddy block, max 4 MiB, so up to 1280x800) → SET_SCANOUT. A `vgpu-flush` kernel thread does TRANSFER_TO_HOST_2D + RESOURCE_FLUSH (≤60 Hz, event-driven since M28) while the fb is dirty (`fb_damage()` from fbcon/fbdev write) or a client has fb0 open. It replaces the boot framebuffer via `boot_set_framebuffer()` and fbcon re-attaches. Used only when there is no firmware framebuffer, or with `virtiogpu` on the command line.
- Why: on riscv64/aarch64 Limine refuses QEMU `ramfb` ("Framebuffer page-level overlap") and edk2 has no GOP for virtio-gpu/bochs. The Makefile now adds `QEMU_GPU=-device virtio-gpu-pci` there (override with `QEMU_GPU=`). NOTE: changing PCI devices renumbers edk2 boot entries; delete `build/<arch>/fw-vars.fd` if the firmware falls into the EFI shell.
- MMIO structs must not be `packed` (on riscv clang splits packed stores into byte accesses, which virtio ignores).
- Bugs fixed: `bkl_acquire_idle()` spun with IRQs enabled → a timer IRQ took a second ticket → whole-system deadlock (seen as a hang during AP bring-up on aarch64 -smp 4). Kernel threads now take TIDs from 1<<22 so init stays PID 1 (busybox init exits otherwise).
- `userland/demos/fbdemo.c`: scenes `lines circles mandel julia plasma fern sierpinski` (`-d secs`, `-j threads`; fractals are rendered by one pthread per CPU). `userland/tests/schedtest.c`: wake-up latency of a sleeper vs N hogs. Results with -smp 1 and 3 hogs: RR avg 21 ms, MLFQ avg 0.86 ms.
- Screenshots in tests: add `QEMUEXTRA="-monitor unix:/tmp/mon.sock,server,nowait"` and send `screendump /tmp/x.ppm` to the socket.
- To run a test with the MLFQ build, pass `SCHED=mlfq` in the environment of `scripts/smoke-test.sh` (otherwise `make run` rebuilds with the default).

## M16: COW fork and event-loop IPC
- **COW** (`kernel/mm/mm.c`): `mm_clone` shares every present page and write-protects private writable ones in both address spaces (page `refcount`++). `mm_handle_fault` on a present page + write → `cow_break` (copy if refcount > 1, else just re-enable write). `user_range_ok(write)` and `mm_write` break COW before the kernel writes, so kernel stores never hit a shared page. `mm_protect` keeps shared private pages read-only. `MAP_SHARED` anonymous memory is now really shared after fork. Counters: `/proc/vmstat` (`cow_shared/copied/reused`).
- **TLB batching** (`vmm_batch_begin/end`, `tlb_batched(va)` in every arch's map/unmap/protect): fork, exit and big munmaps skip per-page invalidations and do one full flush on all CPUs at the end (x86 IPI → CR3 reload, riscv SBI RFENCE whole space, aarch64 `tlbi vmalle1is`). Fork of a 32 MiB process: aarch64 777 → 25 ms.
- **memfd / shared files**: `file_ops.mmap_page(f, pgoff, &pa)` (tmpfs implements it): `MAP_SHARED` file mappings map the tmpfs pages themselves (extra page ref per mapping; tmpfs frees with `page_put_pa`). `memfd_create` makes an unlinked tmpfs inode. `MAP_PRIVATE` file mappings are still snapshots.
- **`kernel/fs/anonfd.c`**: eventfd(2) (semaphore mode), timerfd (MONOTONIC/REALTIME/BOOTTIME, ABSTIME, intervals; checked from the cpu0 tick via `timerfd_tick`), signalfd(4) (dequeues from thread/process pending; signals blocked by the first thread are now queued even when their default action is "ignore", and SIGCHLD with SIG_DFL is sent so signalfd/sigwait can see it), epoll (create/create1/ctl/wait/pwait/pwait2, ONESHOT, nesting, EPOLLET = level-triggered input + edge-triggered EPOLLOUT; items hold no file reference and vanish when the fd no longer refers to the same file).
- **`kernel/net/unix.c`**: AF_UNIX stream/seqpacket/dgram; filesystem names (S_IFSOCK inode created by bind) and abstract names; listen/accept4/connect (connect completes when queued, like Linux); socketpair; sendmsg/recvmsg with SCM_RIGHTS (fds ride on the chunk that starts the message; stream reads never merge across such a chunk) and SCM_CREDENTIALS with SO_PASSCRED; SO_PEERCRED/SO_TYPE/SO_ERROR/...; shutdown; MSG_PEEK/DONTWAIT/NOSIGNAL/CMSG_CLOEXEC/TRUNC; FIONREAD. 256 KiB per-socket receive limit. AF_INET is still EAFNOSUPPORT.
- Syscall plumbing: x86_64 `unistd.h` now carries the full musl syscall number list; `scripts/gen-syscalls.py` also scans `fs/anonfd.c` and `net/*.c`. **Edit `kernel/core/syscall.c.in`, never the generated `syscall.c`** (`thread_first_return` had been lost that way and is now in the template).
- Tests (all pass on x86_64, riscv64, aarch64 with -smp 4): `cowtest`, `ipctest` (72 checks; `ipctest unix` runs one group, `V=1` prints steps), plus `libctest`, `smptest`.

## M17: ptys and evdev
- **PTYs** (`kernel/drivers/pty.inc`, included by `tty.c`): `struct tty` has an `output(t, s, n, may_block)` hook (console → `console_write`, pty slave → master's ring) and `drv`/`hup`. `/dev/ptmx` (5:2) allocates a pair and creates `/dev/pts/N` (136:N, `CHRDEV_ANY_MINOR`); the slave uses the normal line discipline (ICANON/ECHO/ISIG, ONLCR on output). `TIOCGPTN`, `TIOCSPTLCK`, `TIOCGPTPEER`, `TIOCSWINSZ` (→ SIGWINCH to the pgrp). Closing the master hangs up the slave (EIO/0 reads, SIGHUP to the session) and removes the node; the pair is freed when both sides are closed. Opening a tty without `O_NOCTTY` as a session leader without one makes it the controlling tty; `/dev/tty` resolves to `p->ctty`.
- **virtio** (`kernel/drivers/virtio.c`, `include/kernel/virtio.h`): `virtio_pci_probe` (caps incl. device cfg, reset, VERSION_1, FEATURES_OK), `virtq_init` (≤64 entries, one page), `virtq_push/pop`, `virtio_driver_ok`. virtio-gpu now uses it.
- **Input core + evdev** (`kernel/drivers/evdev.c`, `include/kernel/input.h`): drivers fill a `struct input_dev` (bitmaps, absinfo, ids) and call `input_register` → `/dev/input/eventN` (char 13:64+N) and `input_event()` (IRQ-safe; per-device spinlock). Each open gets a 512-event queue of 24-byte `struct input_event` (SYN_DROPPED on overflow), `EVIOCGVERSION/GID/GNAME/GPHYS/GUNIQ/GPROP/GBIT/GKEY/GLED/GABS/SABS/GREP/GRAB/REVOKE/SCLOCKID`. Key autorepeat value 2 is synthesised. Keyboards also feed the console tty (US map, moved from ps2.c) unless grabbed or `KDSKBMODE K_OFF`.
- **virtio-input** (`kernel/drivers/virtio_input.c`): PCI 1af4:1052; capabilities are read from the config space (ID_NAME/DEVIDS/PROP_BITS/EV_BITS/ABS_INFO); each device's interrupt (MSI-X on x86, INTx elsewhere, M28) wakes a threaded handler `irq/N-vinputK` that drains its event queue (`vinput-poll` remains only as a fallback for devices without an interrupt). The Makefile adds `QEMU_INPUT=-device virtio-keyboard-pci -device virtio-tablet-pci` on all arches (override with `QEMU_INPUT=`; delete `build/<arch>/fw-vars.fd` after changing devices on riscv/aarch64). x86 PS/2 keyboard is an evdev device too ("AT Translated Set 2 keyboard").
- **VT/KD ioctls** on the console: `KDGKBTYPE`, `KDSETMODE/KDGETMODE` (KD_GRAPHICS pauses fbcon), `KDGKBMODE/KDSKBMODE`, `VT_OPENQRY/GETMODE/SETMODE/GETSTATE/RELDISP/ACTIVATE/WAITACTIVE` (single VT).
- Tests: `ptytest` (13 checks: interactive sh on a pty, ONLCR, stty size, backspace editing, Ctrl-C, raw mode, hangup) passes on all arches. `evtest [secs]` lists devices and prints events; drive it with the QEMU monitor (`sendkey a`, `mouse_button 1`; HMP `mouse_move` does not move absolute axes without a display).

## M18: DRM/KMS-lite
- `kernel/drivers/drm.c`, char 226:0 `/dev/dri/card0` (created when there is a 32 bpp system framebuffer). Object IDs: connector 31 (type VIRTUAL, always connected, one preferred mode = scanout size @60), encoder 32, CRTC 33, plane 34. Driver name `9os` (so Mesa would pick kms_swrast).
- Dumb buffers are order-0 pages (refcount 1 owned by the BO); `MAP_DUMB` offset = `handle << 28`; `mmap(MAP_SHARED)` goes through `file_ops.mmap_page`, so munmap/exit drop page refs like tmpfs. GEM_CLOSE == DESTROY_DUMB. Objects are global tables (64 each) tagged with the owning open file and freed on close.
- Scanout is a copy (`present()`) of the client fb into the real framebuffer on SETCRTC, DIRTYFB and page-flip completion, followed by `fb_damage()` (virtio-gpu flush). A `drm-vblank` kthread (started on first open) ticks at 60 Hz, completes a pending flip and queues a 32-byte `drm_event_vblank` (type FLIP_COMPLETE) that `read()` returns; `poll` reports POLLIN. SETCRTC pauses fbcon (`fbcon_set_graphics`), disabling the CRTC or closing the fd restores it.
- Caps: DUMB_BUFFER, PREFERRED_DEPTH 24, TIMESTAMP_MONOTONIC, CRTC_IN_VBLANK_EVENT; `SET_CLIENT_CAP` accepts UNIVERSAL_PLANES, rejects ATOMIC (clients fall back to legacy). No properties, no hw cursor (CURSOR → ENXIO), formats XR24/AR24 only, LINEAR modifier only.
- `drmdemo [frames]` (raw ioctls, no libdrm): enumerate, 2 dumb buffers, SETCRTC, animated page flipping with events; 25 checks pass on all arches (x86 ~31 fps, riscv/aarch64 ~13 fps under TCG — drawing dominates).
- Ideas: zero-copy virtio-gpu scanout (one resource per dumb BO + SET_SCANOUT on flip), PRIME export (dma-buf as memfd-like fd), atomic modesetting with a few properties (wlroots prefers it but has a legacy path).

## M19: dynamic linking and inotify
- **ELF loader** (`kernel/core/exec.c`): `load_elf(..., is_interp, ..., &base, &interp)`. A program with `PT_INTERP` gets the interpreter (must be ET_DYN) loaded at a free range reserved through the mmap allocator; execution starts at the interpreter's entry with `AT_BASE` = its load bias and `AT_ENTRY`/`AT_PHDR` describing the program. Static binaries are unchanged.
- **Userland**: `userland/build-musl-shared.sh` (run by `build-all.sh` after compiler-rt) builds musl's `libc.so` with `LIBCC` = libgcc/compiler-rt builtins. `mkroot.sh` installs it as `/lib/libc.so` + `/lib/ld-musl-$ARCH.so.1` symlink, builds every `userland/dynlib/*.c` into `/lib/<name>.so`, and every `userland/dyntests/*.c` as a dynamic PIE (`DYNAMIC=1 musl-cc ...` links with Scrt1.o, `-pie`, `-dynamic-linker`). The `-shared` mode of musl-cc only links (compile with `musl-cc -fPIC -c` first; it would otherwise pick host headers).
- **Page cache for private mappings** (follow-up): `mmap(MAP_PRIVATE)` of a file whose fops have `mmap_page` (tmpfs) maps the cached pages read-only with an extra reference; the first write takes the COW path (the cache's reference forces a copy), pages past EOF are demand-zero. `load_elf` maps read-only PT_LOAD segments the same way (a page already present because segments share it falls back to copying). Counters `pagecache_private_mapped` / `pagecache_exec_mapped` in `/proc/vmstat`. Exec of 20 `/bin/true` (busybox) on x86: 2.38 s → 0.69 s. `mapprivtest` (10 checks) passes on all arches. Note: unwritten private pages see later `write()`s to the file (allowed by POSIX).
- `membarrier` (QUERY/REGISTER/GLOBAL/PRIVATE_EXPEDITED: fence + reschedule IPI to other CPUs) — musl's dlopen uses it.
- **inotify** (end of `kernel/fs/anonfd.c`): watches pin their inode; hooks in `vfs.c`/`sys_fs.c` (`fsnotify_dirent/inode/file/path/unlinked`, no-ops while `fsnotify_nwatches == 0`): IN_CREATE (open O_CREAT, mknod, mkdir, symlink, link), IN_OPEN, IN_ACCESS, IN_MODIFY (write, truncate), IN_CLOSE_WRITE/NOWRITE, IN_ATTRIB (chmod/chown/utimensat, link count), IN_MOVED_FROM/TO with a shared cookie + IN_MOVE_SELF, IN_DELETE + IN_DELETE_SELF/IN_IGNORED, IN_ISDIR, ONESHOT, MASK_ADD/MASK_CREATE, ONLYDIR, DONT_FOLLOW, identical-event coalescing, 16384-event queue with IN_Q_OVERFLOW, FIONREAD. Directory-entry events for files opened by path use `f->path` to find the parent.
- Tests: `dyntest` (11 checks: DT_NEEDED lib with constructor, TLS from a .so incl. new threads, dlopen/dlsym/dlerror, dl_iterate_phdr) and `inotifytest` (29 checks) pass on x86_64, riscv64, aarch64.

## M20: ports and libwayland
- `userland/build-ports.sh` builds `$PORTS` (default, in dependency order: `lua sqlite libffi expat wayland wltest`) with `userland/ports/<name>.sh`; `build-all.sh` runs it unless `NO_PORTS=1`. Ports install into `userland/build/ports-root-$ARCH/usr` (`DESTDIR`), which `mkroot.sh` copies to `/usr/bin` + `/usr/lib` of the root fs. Sources are cached in `/data/tools/dl` (`fetch URL`).
- `userland/ports/common.sh`: `musl-dcc` (= `DYNAMIC=1 musl-cc`, dynamic PIEs), `mkso`, a generated meson cross file (`c = musl-dcc`, pkg-config limited to the ports root via `pkg_config_libdir` + `sys_root`) and `meson_port SRC BUILD opts...`; `autotools_port SRC opts...` (`--host=$ARCH-linux-musl`, `LIBS=<libgcc|compiler-rt builtins>` because libtool links with `-nostdlib`). Host-side tools (`wayland-scanner`) go to `/data/tools/host` and are passed with a meson native file (`[built-in options] pkg_config_path`).
- `musl-cc -shared` now links against the sysroot (crti/crtn, `-lc`, builtins) instead of host defaults.
- compiler-rt builtins now include `__clear_cache` (libffi closures); riscv64 gets `riscv_flush_icache(2)` (`kernel/arch/riscv64/user.c`, local `fence.i`).
- Ports: Lua 5.4.7 (`liblua.so.5.4`, `lua`, `luac`), SQLite 3.47.2 (`libsqlite3.so.0`, `sqlite3` with FTS5 + math), libffi 3.4.6, expat 2.6.4, wayland 1.23.1 (client, server, cursor, egl libs).
- `wltest` (`userland/ports/src/wltest.c`): a libwayland server (wl_shm + minimal wl_compositor/wl_surface/wl_region, timer and SIGCHLD event sources, client create/destroy listeners) and a forked client (registry, memfd pool through SCM_RIGHTS, ARGB8888 buffer, attach/damage/frame/commit, buffer release, roundtrips). Server reads the pixels via `wl_shm_buffer`. Passes on all three arches.

## M21: Wayland compositor
- Ports (`userland/ports/*.sh`, in `build-ports.sh` ALL): wayland-protocols 1.38, pixman 0.44.2, libxkbcommon 1.7.0 (needs host bison; no xkeyboard-config data yet), libdrm 2.4.123 (core only + test programs: `modetest -M 9os`, `vbltest`), `wlkms` (compositor + `wlclient`). xdg-shell glue is generated with the host `wayland-scanner` in `wlkms.sh`.
- Kernel DRM: `SET_VERSION`; a vblank kthread with a 60 Hz deadline; WAIT_VBLANK events are queued until the target sequence; `MODE_DIRTYFB` honours clip rects (`present_rect`), and `fb_damage_rect()` feeds a damage box into virtio-gpu so only the dirty rectangle is transferred/flushed. `fb_explicit_damage` is set while a DRM master owns the CRTC (fbdev mmap clients still get full-screen 60 Hz flushes).
- `wlkms [seconds]` (`userland/ports/src/wlkms.c`): DRM master on `/dev/dri/card0`, one dumb buffer, pixman compositing clipped to a damage region (background, shadows, title bars, close box, cursor), `drmModeDirtyFB` with the damage rects, frame callbacks paced by vblank events (+16 ms idle timer). Globals: wl_compositor v5, wl_shm, xdg_wm_base v5, wl_seat v7 (pointer + keyboard, `WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP`), wl_output v3. Input from grabbed `/dev/input/event*` (tablet abs → cursor, rel mice, keys). Click raises/focuses, drag a title bar to move, close box sends `xdg_toplevel.close`, Ctrl+Alt+Backspace quits. Prints frames, fps, repainted Mpixels and ms/frame at exit.
- `XDG_RUNTIME_DIR=/run/user/0` comes from `/etc/profile` (wlkms creates it if unset).
- `wlclient [seconds] [w h]`: xdg toplevel with two shm buffers, animated pattern on frame callbacks, click → palette change, `q` quits; prints fps.
- Test: `wlkms 16 > /tmp/k.log 2>&1 &`, `wlclient 10 &`, `wlclient 9 200 150 &` then screendump through the QEMU monitor (`QEMUEXTRA="-monitor unix:/tmp/mon.sock,server,nowait"`, `python3 scripts/qemu-monitor.py "screendump /tmp/x.ppm"`). x86_64 (TCG): ~40 fps for one client; riscv64/aarch64 (TCG) 4–6 fps, bound by emulation speed.

## M22: wlterm
- `wlterm [-e command] [cols rows]` (`userland/ports/src/wlterm.c`, built by `wlkms.sh` with `kernel/drivers/font8x16.c`): opens `/dev/ptmx`, forks `/bin/sh` (login) or `sh -c command` with `TERM=linux` on the slave, sets `TIOCSWINSZ`; event loop with `wl_display_prepare_read` + `poll` on the display fd and the pty; redraws the cell grid into one of two shm buffers when dirty and no frame callback is pending; exits after the child's output ends. Keyboard: raw evdev codes (the compositor sends `NO_KEYMAP`), shift/ctrl tracked locally, arrows/home/end/del/pgup/pgdn as VT sequences. No key repeat, no scrollback, no scroll regions yet.
- `wlkms` draws window titles with the 8x16 font and sends `wl_keyboard.enter` to newly mapped windows.
- Interactive test: boot with `QEMUEXTRA="-monitor unix:/tmp/mon.sock,server,nowait"`, run `wlkms 40 &` and `wlterm &`, then `python3 scripts/qemu-type.py /tmp/mon.sock 'ls /\n'` and screendump. Verified on x86_64 and aarch64 (riscv64 builds the same binaries).

## M23: CI boot tests
- `scripts/qemu-test.py ARCH [--smp N] [--sched rr|mlfq] [--boot-timeout S] [--timeout S] [--log F] CMD...` boots `make run` with `-display none`, waits for the shell banner/prompt, runs each command as `CMD; echo __RCi=$?`, fails a command on a non-zero status or `FAIL` in its output, and fails the run on any panic/exception text. Exit status 0 = all passed.
- `scripts/ci-tests.sh ARCH` = libctest cowtest ipctest ptytest inotifytest dyntest mapprivtest smptest + a bash array test + a gzip pipe + wltest (if ports are built) + drmdemo; log in `build/test-ARCH.log`.
- CI runs it after building each ISO (Ubuntu qemu-system-* + qemu-efi-aarch64/riscv64); the release job only runs if all three pass.
- `userland/install-uapi.sh` now runs right after musl (compiler-rt's `clear_cache.c` needs `<asm/unistd.h>`; without it libffi had an undefined `__clear_cache` on riscv64 after a clean build).
- `scripts/sandbox-setup.sh` rebuilds the agent sandbox (dnf packages, Alpine QEMU, Limine/uACPI, userland).

## M24: fine-grained locking
- Lock order: BKL → `sched_lock` → (`buddy_lock`, `pt_lock`, console lock). `sched_lock` (core/sched.c) guards the run queue, sleep list, wait queues, thread states and zombies; it is taken with IRQs off, held across `arch_switch_to` and released by the incoming thread in `sched_finish_switch()`.
- Context switches drop the outgoing thread's BKL (`bkl_drop_for_switch`, depth saved in `bkl_saved`) and the incoming thread retakes its own (`bkl_retake_after_switch`). New threads start with `bkl_saved = 1`; idle threads run with no BKL (IRQ handlers on an idle CPU take it).
- `wait_event*` check `signal_pending()` under `sched_lock`; signals wake sleepers with `thread_interrupt()` (no lost wakeups).
- Lock-free syscalls (`lockfree_names[]` in syscall.c.in): get*id, getres*id, clock_gettime/getres, gettimeofday, time, sched_yield, nanosleep, clock_nanosleep, uname, getcpu, sched_getaffinity. `copy_{to,from}_user` take the BKL for the copy when the caller does not hold it (VMAs/page tables/page refcounts are still BKL-protected). `user_return_work` takes the BKL only for signal/alarm work.
- `console_write()` is serialised by an IRQ-safe lock (owner CPU may re-enter, e.g. panic): an echo from the serial IRQ used to interrupt fbcon mid-scroll and write past the framebuffer.
- `sysbench [iters] [procs]`: getpid (lock-free) vs getppid (BKL) with 1..N processes. SMP=2 under TCG: lock-free ×2.8, BKL ×1.5. (SMP=4 on a 2-core host collapses from lock-holder preemption of vCPUs; that is a host artefact.)
- Per-mm lock (`mm_lock`/`mm_unlock` in mm/mm.c): recursive (owner CPU + depth), IRQs off, spun with `spin_lock_ipi()` (services TLB-shootdown IPIs while waiting). Taken inside `mm_map/unmap/protect/handle_fault/write`, `mm_clone` (source), `mm_put`, `user_range_ok`, the mmap populate loops (`install_page()` in sys_mm.c tolerates a racing demand fault) and `sys_mremap`. Lock order: BKL → mm->lock → pt_lock/buddy/slab → sched_lock. Never sleep under it (file reads for mmap happen outside).
- Page faults are resolved **before** the BKL on all arches (x86 `page_fault_fast()` in user.c, aarch64/riscv64 trap.c); the BKL is taken only for signals/segfaults. `copy_{to,from}_user`/`strncpy_from_user` hold only the mm lock (check + memcpy, in 64 KiB chunks), no BKL.
- Page refcounts are atomic (`page_ref_inc`, `page_ref_dec_test`, `page_put[_pa]` in pmm.h); `mm->refcount` too. TLB batching depth is per thread (`current->tlb_batch_depth`); page-table locks use `pt_lock_irqsave()` = `spin_lock_ipi`.
- `faulttest` (in ci-tests): 4 threads demand-faulting a shared region, mmap/munmap churn, clock_gettime into untouched pages, 4 forks mid-flight; passes ×3 on all arches with `--smp 4`.
- `read`/`write` are lock-free syscalls: `fd_get_ref()` takes a file reference under the per-process `fd_lock` (slot changes go through `fd_slot_set()`); file refcounts are atomic and `vfs_close()` takes the BKL for the final release. Files whose `file_ops.nobkl` is set (pipes) run without the BKL; others take it in `sys_read`/`sys_write`.
- Pipes: ring guarded by an IRQ-off `spin_lock_ipi` lock (user copies happen under it), sleeps via `wait_until_sl()` (condition re-checked under `sched_lock` → no lost wakeups), writes ≤ PIPE_BUF (4096) are atomic, SIGPIPE raised under the BKL. Kernel-pointer buffers (sendfile/splice) are memcpy'd.
- `poll_seq`: `poll_notify()` bumps it; poll/select/epoll_wait sample it before the readiness scan and sleep with `poll_wait_seq()` only if unchanged (lock-free wakers can't be missed).
- Rule: any lock held while taking an mm lock must be spun with `spin_lock_ipi()` (the mm lock holder may be waiting for TLB-shootdown acks).
- `pipetest` (in ci-tests): 2 producers/2 consumers with 64-byte atomic records, 300 poll() wakeups from another thread, 2 MiB cross-process transfer.
- Next: slab/kmalloc locks (already spinlocked; audit callers), tty/socket paths off the BKL, per-file/pipe/socket locks so read/write/poll can leave the BKL, then per-CPU run queues (M25).

## M24 part 6: futex and eventfd without the BKL; LAPIC mode-switch livelock
- `futex` is a lock-free syscall. Waiters live in 64 hashed buckets (`hash(mm, uaddr)`), each with
  an IRQ-off lock spun with `spin_lock_ipi()` because FUTEX_WAIT reads the user word (mm lock)
  under it. "Read word + enqueue" and "dequeue + set woken + wake_up" are both done under the
  bucket lock; the sleep re-checks `woken` under `sched_lock` (wait_until_sl pattern), and a
  waiter always retakes its bucket lock before returning so a waker never touches a dead stack
  frame. Lock order: bucket → mm lock → sched_lock.
- Fixed semantics: FUTEX_REQUEUE/CMP_REQUEUE used to wake waiters on *uaddr2* but never the
  ones to be requeued from *uaddr*, so a musl condvar waiter parked on its barrier
  (`unlock_requeue()` → `FUTEX_REQUEUE 0, 1`) could sleep forever. Requeue now wakes up to
  val + val2 waiters on uaddr (a requeue is reported as a spurious wakeup, which musl/glibc
  tolerate); CMP_REQUEUE checks `*uaddr == val3` under the bucket lock. Also EINVAL for
  misaligned words, bad timespecs and an empty bitset; WAIT_BITSET honours FUTEX_CLOCK_REALTIME.
- eventfd (`file_ops.nobkl`): counter updated by CAS, 8-byte copies via copy_{to,from}_user
  (memcpy for kernel buffers), blocking with `wait_until_sl(&poll_wq, ...)`; every change is
  followed by `poll_notify()`.
- **x86 LAPIC livelock (M25 tickless idle, pre-existing):** `arch_timer_active()` wrote the LVT
  periodic bit while a one-shot count (as small as 1) was still loaded. QEMU (TCG) then ran a
  periodic timer with a period of a few bus cycles; its main loop re-fired it forever while
  holding the BQL (host strace: endless `write(eventfd)`), so no vCPU reached the following
  TMR_INIT write and the whole guest froze. Both mode switches now stop the counter first
  (TMR_INIT = 0). Reproduced in 1 of ~3 `futextest` runs at `-smp 4`; 12/12 after the fix.
- `/proc/sched` lists every thread: `thread <tid> <name> <R|X|B|S|Z> cpu rq syscall bkl depth/saved`
  (useful to see who is blocked where when a guest test hangs).
- Debugging a frozen guest: run with `QEMU_EXTRA="-monitor unix:/tmp/mon.sock,server,nowait"`;
  if the monitor does not answer, QEMU itself is wedged — attach `gdb`/`strace` to the host process.
- Tests: `futextest` (futex mutex hammer, strict ping-pong, condvar broadcast, EAGAIN/EINVAL,
  relative/absolute/past timeouts, CMP_REQUEUE, 300 create/join for CLEARTID wakeups) and
  `efdtest` (MPMC semaphore eventfd with poll, blocking read, overflow EAGAIN/EINVAL) in ci-tests.

## M24 part 7: AF_UNIX data path without the BKL
- `sendmsg`, `recvmsg`, `sendto`, `recvfrom` are lock-free syscalls and `unix_fops` is `nobkl`
  (read/write). All socket state — usock fields, receive queues, listener backlogs and
  `bound_socks` — is guarded by one subsystem lock, `unix_lock` (IRQ-off, `spin_lock_ipi()`,
  since recv copies to user memory under it). Setup syscalls (socket/bind/listen/connect/accept/
  shutdown/[gs]etsockopt/get*name) still run under the BKL and take `unix_lock` around shared state.
- The data path holds a file reference on its own socket (`sock_file_ref()` = `fd_get_ref()`);
  peers are only dereferenced under `unix_lock` and re-resolved after every sleep, because a peer
  can be destroyed (u_release → `usock_destroy()`) as soon as the lock is dropped.
- Never under `unix_lock`: the BKL, VFS lookups (`name_inode()` resolves filesystem names under
  the BKL first, `match_locked()` then searches `bound_socks`), `vfs_close()`/`iput()` (dequeued
  chunks go on a local list and are freed after unlocking), `fd_alloc()` (SCM_RIGHTS files are
  detached under the lock and installed afterwards under the BKL — `fd_alloc` scans the table
  unlocked, so it must stay serialised by the BKL). Send chunks are allocated and filled from
  user memory before taking the lock.
- Sleeps sample `poll_seq` under `unix_lock` and call `poll_wait_seq()` after dropping it; every
  state change is followed by `poll_notify()`, so wakeups cannot be lost.
- Lock order: BKL → unix_lock → mm lock → pt/buddy/slab → sched_lock.
- Fixed a use-after-free: `recv(MSG_TRUNC)` on a datagram returned `first->len` after freeing it.
- `socktest` (ci-tests): SEQPACKET 2×2 senders/receivers with record checksums, cross-process stream
  bulk copy, 200× SCM_RIGHTS pipe passing, 150× send racing the peer's close (EPIPE), accept/connect
  churn from 3 client threads on an abstract listener, DGRAM fan-in with poll + recvfrom.

## M24 part 8: sleeping mutexes, lockdep-lite, poll/select/epoll without the BKL
- `struct mutex` (core/mutex.c, kernel/mutex.h): sleeping, non-recursive, owner-tracked; contended
  lockers block on the mutex wait queue. A context switch drops the BKL, so mutex-vs-BKL cannot
  deadlock. `mutex_trylock()` never sleeps.
- Lock classes with ranks (kernel/spinlock.h) and a debug order checker (core/lockdep.c): taking a
  lock whose rank is not above every lock held on this CPU (spinlocks) / by this thread (mutexes),
  a mutex under a spinlock, or the BKL under a classified spinlock is reported once per pair on the
  console and in `/proc/lockdep`. `ci-tests.sh` ends with `grep -q '^violations 0' /proc/lockdep`.
  Successful trylocks (`spin_trylock`, `mutex_trylock`, `mm_trylock`) skip the order check
  (`lockdep_acquire_try`) — reclaim relies on that.
- poll/ppoll/select/pselect6/epoll_* run lock-free; `->poll` methods of files not marked `nobkl`
  are called under the BKL (taken once per scan). `polltest`.

## M24 part 9: tty/pty and the fd table
- tty/pty line discipline under a per-tty IRQ-safe lock; read/write/poll are lock-free and copy
  through bounce buffers (never touch user memory under the tty lock).
- fd table under `fd_lock` (fd_alloc/close/dup*/fcntl/cloexec); `fd_get()` pins the file until
  syscall exit (`fd_borrow_release()` in the lock-free dispatch path). close/dup/dup2/dup3/fcntl
  are lock-free. `fdtest`.

## M24 part 10: tmpfs file data, VFS namespace
- tmpfs file data under a per-inode mutex; read/write/pread/pwrite/readv/writev/lseek lock-free,
  atomic inode refcounts. `filetest`.
- The VFS namespace (lookup/create/unlink/rename/link/symlink/readlink/cwd) is under a recursive
  namespace mutex; driver `->open` runs outside it. Path syscalls (open*/stat*/access/mkdir/unlink/
  rename/link/symlink/chdir/getcwd/getdents/chmod/chown/utimensat/truncate/statfs) are lock-free;
  procfs and inotify take the BKL themselves. tmpfs readdir positions are stable per-directory
  cookies (unlinking during readdir no longer skips entries). `vfstest`.

## M24 part 11: devices and ioctl
- /dev/null, zero, random, urandom and evdev are `nobkl`: output is staged in a stack buffer and
  copied with `copy_to_user`; `random_u64()` is a lock-free splitmix64 over an atomic counter;
  evdev LED bits are updated atomically.
- `ioctl` is dispatched lock-free: FIONBIO/FIOCLEX/FIONCLEX are handled atomically, driver
  `->ioctl` methods are called under the BKL (sys_fs.c). The mm syscalls are lock-free too (M26).

## M24 status / lock order
- Global order (outer → inner): BKL → sleeping mutexes (tty, epoll, VFS namespace, tmpfs inode)
  → fd table → futex buckets → pipe → tty → unix → mm->lock → tmpfs page array → inode i_mmap →
  page-cache LRU → page tables → slab → buddy/PCP → sched_lock/rq locks → console. Spinlocks that
  can be held while a TLB shootdown is issued, or spun on with IRQs off by someone who must answer
  one (mm, page array, i_mmap, LRU), are taken with `spin_lock_ipi()`.
- Still under the BKL: process lifecycle and signals (fork/exec/exit/wait/kill/sigaction), socket
  setup calls, mount, DRM/fbdev/driver ioctls, procfs file generation, inotify. The data paths
  (read/write/poll/epoll/futex/mmap/faults/path lookup) no longer take it.

## M25 part 1: CPU time accounting
- Ticks are sampled as user or system (`cpu->tick_user`, set in `sched_tick_fast()`, which every arch calls first on each timer tick) and charged by `account_tick()` to the thread, process (atomic) and CPU (`user_ticks`/`sys_ticks`).
- Precise on-CPU time: `__schedule()` adds `now - exec_start_ns` to `thread->sum_exec_ns` and `proc->sum_exec_ns` on switch-out and counts voluntary/involuntary switches; `clock_gettime(CLOCK_PROCESS_CPUTIME_ID/THREAD_CPUTIME_ID)` = sum + current slice.
- `times()`, `getrusage(SELF/CHILDREN/THREAD)`, `wait4()` rusage (`struct rusage_k` → `rusage_to_user()`), child totals accumulated into the parent on reap; minor faults counted in `mm_handle_fault`. `/proc/stat` has user/system/idle per CPU; `/proc/pid/stat` minflt/cminflt/utime/stime/cutime/cstime/num_threads; `/proc/pid/status` Threads + ctxt switches.
- `timetest` in ci-tests; bash `time` reports real user/sys.

## M25 part 2: per-CPU run queues
- `rqs[cpu]` (core/sched.c), one list per MLFQ level (one for RR), still all under `sched_lock`. `select_cpu()` places a woken thread on an idle allowed CPU (last CPU first), else its last CPU, else the shortest allowed queue; `kick_after_wake()` IPIs that CPU (or sets local resched). A preempted thread is requeued locally; `dequeue(c)` takes from the own queue, else steals the highest-priority allowed thread from the busiest queue.
- `thread->affinity` (inherited on fork/clone), real `sched_setaffinity`/`sched_getaffinity` (pid = tid; migration at next schedule). The boot CPU is now marked `online` (it never was).
- `/proc/sched` shows queue length and steals per CPU. `afftest` (in ci-tests): pinning to each CPU sticks, offline-only mask rejected, busy threads spread over all CPUs.
- Next for M25: per-CPU locks (split `sched_lock` into rq locks + a sleep-list lock), load balancing on tick for busy CPUs, nice/priorities.

## M25 part 3: nice, SCHED_FIFO/RR, accounting by time
- `nice` (`setpriority`/`getpriority`, kernel returns 20-nice) scales the quantum (`quantum_for()`: x1.25 per step, clamped 1..100 ticks); deficit round robin: a thread that overran its quantum carries the debt into the next one (`pick_reset_quantum`), `policy_skip` lets lower-nice threads run first.
- SCHED_FIFO/SCHED_RR (`sched_setscheduler/getscheduler/setparam/getparam/get_priority_max/min/rr_get_interval`): RT threads sit on a per-rq RT list sorted by `rt_prio` and always beat normal threads; RR quantum `SCHED_RR_QUANTUM` (100 ms). `wake_preempts()` decides wake-up preemption. nice/policy are inherited by `thread_alloc`; `/proc/pid/stat` priority/nice.
- Accounting is now time-weighted (`acct_ns`, `*_ns` fields) instead of counting ticks; `account_tick()` returns the ms consumed for quantum bookkeeping.
- Idle-loop fix: on x86 `sti; cli` in a loop could livelock a CPU so it never took the TLB-shootdown IPI (deadlock). Idle now only calls schedule when `cpu_has_work()` (rq length + per-rq `nr_mig` migratable count) and halts otherwise.
- Tests: `nicetest` (in ci-tests); `afftest` checks CLOCK_MONOTONIC across CPUs; `timetest` checks cpu <= wall for a fresh child. `scripts/qemu-test.py` waits for the `#` prompt before each command and appends `QEMU_EXTRA`.
- CI installs Alpine edge QEMU/edk2 (`scripts/install-qemu-alpine.sh`, also used by `sandbox-setup.sh`): Ubuntu's riscv64 firmware made Limine panic ("BSP hart does not advertise MMU support").
- Timing tests (nicetest/afftest) can be flaky when the host has fewer cores than `--smp`.

## M25 part 4: periodic balancing and affinity correctness
- The cpu0 timer tick balances normal queued threads every 20 ms under `sched_lock`,
  moving at most one thread per destination per interval. Load includes the running
  non-idle thread, so balancing works between busy CPUs, not only when an idle CPU steals.
  Affinity, policy, quantum and total runnable count are preserved. Balancing never
  targets offline CPUs or selects live switch-out contexts or RT threads.
- `/proc/sched` now reports `balances` (periodic migrations received) separately from
  `steals` (idle pull migrations). RT placement still follows the existing wake/steal paths.
- Fixed queued-affinity accounting: remove with the old mask, then reinsert with the
  new one, even when the queue does not change. Previously `nr_mig` could be stale or
  negative, hiding eligible work or causing unnecessary idle scans.
- Stealing tries subsequent donors if the largest queue has no thread allowed on the
  destination CPU; it no longer gives up despite eligible work in another queue.
- Scheduling/affinity syscall target lookup now searches process thread lists by TID
  (`process_find_thread`, BKL held), including non-leader pthreads. Previously only a
  process leader or the calling thread was found.
- `scripts/sched-host-tests.sh`: actual scheduler code with host IRQ/current/IPI shims,
  UBSan traps, 4,467 assertions each for RR and MLFQ. Covers same-queue affinity changes,
  migration accounting, multiple steal donors, busy balancing, pinned/offline/RT/live
  exclusions and 200 rounds of affinity/balance churn. Reintroducing either queue bug
  causes the suite to fail. CI runs it once on the x86_64 build job.
- `balancetest` (in the boot suite) uses FIFO guards on CPUs 0/1 and individually gated
  normal workers. It checks sibling-TID scheduling queries, widens queued masks, observes
  periodic migrations while both CPUs stay busy, then narrows the masks and verifies
  workers execute only on CPU0. It skips cleanly on a single-CPU guest.
- **Remaining after part 4:** split `sched_lock` into per-CPU run-queue locks with safe sleep/wait
  coordination, per-CPU slab/pmm caches, and tickless idle. This increment does not
  remove the global scheduler lock or complete M24/M25.

### Validation for part 4

Kernel, userland and ISO builds passed for all three architectures with clang 15.0.7.
Boot tests used QEMU 11.1.2 on a two-core host, with `NO_PORTS=1` (no `wltest`).

| Architecture / policy | Guest CPUs | Coverage | Result |
|---|---:|---|---|
| x86_64 / RR | 4 | Full boot suite | 18/18 including boot |
| x86_64 / RR | 2 | balancetest, afftest, nicetest | 4/4 including boot |
| x86_64 / MLFQ | 4 | balancetest, afftest, nicetest, timetest, faulttest, pipetest | 7/7 including boot |
| x86_64 / MLFQ | 1 | balancetest single-CPU skip | 2/2 including boot |
| riscv64 / RR | 2 | balancetest, afftest, nicetest, timetest, faulttest, pipetest | 7/7 including boot |
| aarch64 / RR | 2 | balancetest, afftest, nicetest, timetest, faulttest, pipetest | 7/7 including boot |

The full riscv64 four-CPU run was stopped after boot/libctest/cowtest/ipctest passed:
it was very slow on the two-core host while ARM userland was building. The complete
focused two-CPU run above passed after the builds settled. Full non-x86 and graphics-port
suites still need an uncongested CI run; these results are not a claim that M25 is complete.

## M25 part 5: per-CPU physical-page caches
- Freed order-0 pages may stay on the freeing CPU's cache (16 pages / 64 KiB per CPU,
  at most 2 MiB resident in caches at `MAX_CPUS=32`). Cache hits/puts avoid `buddy_lock`;
  other orders, empty-cache misses and full-cache frees use the buddy allocator.
  Caches are enabled only after `smp_init()`; bootstrap allocation remains unchanged.
- Cached pages have a distinct `PG_PCPU` flag, no slab owner, order 0 and refcount 0.
  Allocation restores flags 0/refcount 1. The flag is not `PG_FREE`, so buddy coalescing
  cannot consume a cached page. Cache flag transitions/coalescer loads are atomic.
- `free_pages` is atomic and includes cached pages. Moving a page between cache and
  buddy does not change the total. `/proc/vmstat` reports `pmm_pcpu_cached`,
  `pmm_pcpu_alloc_hits`, `pmm_pcpu_free_hits`, and `pmm_pcpu_drained`.
- On a buddy allocation miss (including higher orders), drain all CPU caches and
  retry once. This recovers remote cached pages and permits coalescing into large blocks.
  Draining detaches under a cache lock, releases it, then coalesces under the buddy lock.
  A rare-path drain lock serializes concurrent detach/transfer operations, so an OOM
  retry cannot overlook an unfinished drain's detached pages.
- Allocator lock waiters service TLB IPIs after cache enablement, including while an
  mm/page-table lock is held by the caller. Cache selection is IRQ-off to prevent
  migration; no cache lock is held while acquiring the buddy lock.
- `pmm_cache_selftest()` runs at boot (reuse/metadata/counts/bounded overflow/drain).
  `scripts/pmm-host-tests.sh` runs the actual allocator with synthetic page metadata:
  19,629 UBSan-trap checks for bootstrap behavior, local reuse, remote-cache recovery,
  cache bounds, high-order coalescing, paused overlapping drains, and four concurrent
  alloc/free/drain workers. Ten additional stress repetitions passed.
- `pcputest` (in CI's boot suite) pins up to eight workers to separate CPUs and repeatedly
  maps/touches/unmaps private pages. It checks demand-zero on reuse, page/thread isolation,
  observable cache hits, cache bounds and free-memory accounting.
- `balancetest` now uses a 60-second safety deadline for its FIFO guard instead of five,
  and explicitly checks `guard_expired`. The 400 ms balancing observation and affinity
  assertions are unchanged. A mixed-emulator run hit its `bad_cpu` assertion; a too-short
  guard can let workers run before mask narrowing under host contention. Final checks
  are run serially rather than hiding such a failure behind a timing-dependent pass.
- **Remaining M25:** per-CPU run-queue locks, per-CPU slab/object caches and tickless idle.
  Order-0 PMM caching does not remove the BKL or global scheduler/slab locks.

### Validation for part 5

Final checks ran serially on the two-core host after adding explicit FIFO-guard timeout
reporting. All three kernels built with clang 15.0.7 and booted with QEMU 11.1.2.
For non-x86 tests, userland was reused from release `9os-20261005-183051` (target
`0a3e922`), with `pcputest` and the updated `balancetest` rebuilt against each target's
musl 1.2.5 / compiler-rt 15.0.7 toolchain. This avoids rebuilding unrelated ports.

| Architecture / policy | Guest CPUs | Coverage | Result |
|---|---:|---|---|
| x86_64 / RR | 4 | Full boot suite (`NO_PORTS=1`) | 19/19 including boot |
| x86_64 / MLFQ | 4 | pcputest, faulttest, cowtest, pipetest, balancetest | 6/6 including boot |
| riscv64 / RR | 2 | pcputest, faulttest, cowtest, pipetest, balancetest | 6/6 including boot |
| aarch64 / RR | 2 | pcputest, faulttest, cowtest, pipetest, balancetest | 6/6 including boot |
| x86_64 / MLFQ | 1 | pcputest and balancetest single-CPU skip | 3/3 including boot |

Host regressions passed: PMM 19,629 checks (plus ten repeated stress runs), scheduler
4,467 checks for each of RR/MLFQ. The larger graphics-port suite and full non-x86
regressions still belong in CI; these focused results do not imply M24/M25 completion.

## M25 completion: local queue locks, slab magazines and tickless idle

This finishes the M25 scope in ROADMAP.md. It does **not** declare M24 complete or
remove the BKL from every syscall/subsystem. Earlier “remaining M25” notes above
are historical, superseded by this section.

### Scheduler concurrency

- Each cache-line-aligned run queue has its own IRQ-off, IPI-aware spinlock.
  Local yield/preemption switches take only their own queue lock; CPU0's
  sleep/wait/wake coordinator is not held across a machine context switch.
- Blocking/waking, affinity changes and remote steals use the brief coordinator.
  Balancing takes source/destination queue locks in ascending CPU order and
  revalidates placement/load. Global MLFQ boosts lock all queues in ascending order.
- The incoming thread releases its own rq lock **before** any foreign-queue kick
  and before retaking the BKL. A live foreign `on_cpu` context is skipped.
  `handoff_refs` pins the saved context while a finish-switch reader clears
  `on_cpu` and sends the post-save kick that closes the wake/switch-out race.
  Zombie reaping waits for both `on_cpu == 0` and all handoff readers to retire.
- Selecting the already-running thread leaves the BKL held: only a **real**
  context switch drops it. Eight-vCPU object churn exposed acquire/preempt/self-
  select/drop/retake livelock when even a self-selection requeued the BKL ticket.
  Host tests now model BKL nesting/saved depth and assert no drop on self-selection.
- User-syscall BKL critical sections defer **kernel-mode IRQ** preemption until
  user return/explicit blocking. Eight-vCPU stress exposed a real progress
  livelock (above); deferring syscall-critical kernel IRQ preemption also gives
  a contended syscall a chance to finish before the next involuntary switch. IRQ wakeups
  still mark resched, while user return, explicit waits/yields, BKL-free paths
  and pure kernel threads remain preemptible. The fix is shared across arches;
  host tests check the gate and CI adds an eight-CPU object/idle/page/thread test.
- Equal-priority FIFO wakeups do not force preemption; higher RT priorities do.
  Inherited policy/nice now determine a newly allocated thread's first quantum.
  Invalid priority policies and nonzero OTHER/BATCH/IDLE priorities return EINVAL.
- `/proc/sched` adds per-CPU `local`, `coordinated` and `nohz` counters while
  retaining queue/steal/balance diagnostics.

### Generic per-CPU object magazines

- Every generic cache of objects <= 2 KiB has a bounded eight-object magazine
  per CPU (not just kmalloc size classes). Cache selection and magazine access
  are IRQ-off; reuse avoids the global class lock. Oversized caches bypass it.
- A magazine or drain-detached object still reserves its slab slot, preventing
  backing memory from being returned while a cached reference exists. Magazines
  release their lock before the global free path acquires a class/buddy lock.
- The class lock is released **before** growing a slab. On page-allocation
  pressure, PMM drains PCP pages, retries, then asks slab to drain all magazines
  and trim empty slabs, drains newly released PCP pages, and retries. Registry
  serialization prevents incomplete concurrent drains from hiding free memory.
- Cache metadata is aligned to `_Alignof(struct kmem_cache)`; invalid/oversized
  cache geometry is rejected and kmalloc requests above maximum buddy size return
  NULL without size-order overflow. kzalloc still clears objects reused from a
  magazine. Boot runs the original slab test plus a magazine zero/reuse/drain test.
- `/proc/vmstat`: `slab_pcpu_cached`, `slab_pcpu_alloc_hits`,
  `slab_pcpu_free_hits`, `slab_pcpu_drained`.

### Deadline-driven idle

- x86 LAPIC idle timers switch from periodic mode to one-shot (or masked);
  RISC-V programs the SBI absolute timer; ARM programs CNTV_CVAL (or disables it).
  Active-thread scheduling restores the normal 1 kHz quantum timer.
- APs without work stop their local tick completely and wake on IPIs. CPU0 arms
  the earliest sleep/timeout, timerfd, 20 ms balance, MLFQ boost or console-poll
  deadline. RISC-V/ARM console polling is bounded to 10 ms pending M28's
  interrupt-driven I/O; existing driver-poller sleepers may impose earlier wakes.
  This is NOHZ **idle**, not tickless execution of non-idle threads.
- Remote timeout insertion or timerfd rearming kicks an idle CPU0 so an earlier
  deadline is not missed. IRQ-off timer arming plus a final queue/IPI/resched
  check closes the enqueue/WFI race; architecture WFI helpers retain their
  interrupt-safe entry sequences. RISC-V keeps global IRQs masked through WFI
  (locally enabled pending IRQs still wake it), avoiding an enable-before-WFI
  lost-IPI window. IRQs are masked again before updating idle accounting.
  A remote donor hint that only allows other CPUs does not prevent WFI forever;
  final idle entry checks local placement and actual resched/IPI notifications.
- CPU0 derives jiffies from elapsed monotonic time, not IRQ count. Idle elapsed
  time is charged before switching away from the idle thread, with fractional-ms
  carry. Sequence-protected `/proc/stat` and `/proc/sched` snapshots also include
  an idle interval while its CPU remains halted, without waking that CPU.

### Added regression coverage

- `scripts/sched-host-tests.sh`: actual scheduler implementation under both RR
  and MLFQ, UBSan traps; affinity/migration/RT bounds, queue invariants, balancing,
  live-context skipping, local/self switches, late wake handoff, FIFO wake
  priorities, deadline selection, elapsed/live idle accounting and two concurrent
  CPU-local switchers. CPU1 completes 2,000 switches while CPU0 holds the global
  coordinator; combined workers complete 6,000 machine switches per policy.
- `scripts/slab-host-tests.sh`: actual buddy + slab allocators over an aligned
  physical arena, UBSan traps; all size classes, alignment, zeroing/realloc,
  oversized geometry, remote reuse, real total-memory exhaustion, high-order
  coalescing and four concurrent alloc/free workers racing magazine/PCP drains.
- `slabtest`: pinned per-CPU pipe/eventfd/socket object churn, clean reuse,
  payload/EOF isolation and observable magazine hits.
- `idletest`: idle AP interrupt suppression, live idle-time snapshots, sleep and
  poll deadlines, relative/absolute/periodic timerfds, accumulated expirations,
  disarm, remotely armed timers and idle CPUs facing affinity-ineligible donors. Expanded `nicetest` checks invalid policies
  and RT/normal priority limits without changing the caller's policy on failure.
- CI runs all three host suites, the new guest tests in the full suite, plus
  single-CPU and MLFQ-focused guest runs on **each** architecture. Published ISO
  artifacts are copied from the default RR build before alternate-policy tests.

### Completion validation

The final matrix explicitly passes `--sched mlfq` to the QEMU runner and checks
its boot policy. Earlier results labelled MLFQ based only on a prebuilt ISO are
superseded: the runner invokes Make and otherwise rebuilds the default RR kernel.
Guest priority tests use raw sched syscalls because musl 1.2.5 intentionally
stubs its sched_setscheduler/getscheduler wrappers with ENOSYS.

All final runs were serial on the two-core development host using clang 15.0.7
and QEMU 11.1.2. Default RR builds were followed by explicitly selected MLFQ
builds; the MLFQ banner was verified in each log. Non-x86 userland was reused from
release `9os-20261005-183051` (`0a3e922`), with new/updated tests rebuilt against
the target musl 1.2.5/compiler-rt toolchains. x86 userland used `NO_PORTS=1`.

| Architecture / policy | CPUs | Coverage | Result (includes boot) |
|---|---:|---|---|
| x86_64 / RR | 4 | Full base guest suite | 21/21 |
| riscv64 / RR | 2 | Full base guest suite | 21/21 |
| aarch64 / RR | 2 | Full base guest suite | 21/21 |
| x86_64 / MLFQ | 4 | Full base guest suite | 21/21 |
| riscv64 / MLFQ | 2 | slabtest, idletest, nicetest, afftest, balancetest, pcputest, smptest, timetest | 9/9 |
| aarch64 / MLFQ | 2 | Same focused MLFQ suite | 9/9 |
| x86_64, riscv64, aarch64 / RR | 1 each | slabtest, idletest, nicetest, pcputest, balancetest | 6/6 each |
| x86_64 / RR | 8 | slabtest, idletest, afftest, balancetest, pcputest, smptest | 7/7 |
| x86_64 / MLFQ | 8 | Same high-contention suite | 7/7 |

Host results: PMM **19,629** checks; slab **65,512,082** checks (plus five
additional completed stress repetitions); scheduler **28,543** checks **per
policy**, plus five repeated final RR/MLFQ runs. All use UBSan traps. Workflow
YAML parsed, shell scripts passed `sh -n`, and `git diff --check` passed.

Reproduce the final matrix after building userland:

```sh
scripts/pmm-host-tests.sh
scripts/slab-host-tests.sh
scripts/sched-host-tests.sh
scripts/ci-tests.sh x86_64 --sched rr --smp 4
scripts/ci-tests.sh riscv64 --sched rr --smp 2
scripts/ci-tests.sh aarch64 --sched rr --smp 2
scripts/ci-tests.sh x86_64 --sched mlfq --smp 4
# Substitute each architecture for ARCH in the focused runs:
python3 scripts/qemu-test.py ARCH --sched rr --smp 1 slabtest idletest nicetest pcputest balancetest
python3 scripts/qemu-test.py ARCH --sched mlfq --smp 2 slabtest idletest nicetest afftest balancetest pcputest smptest timetest
python3 scripts/qemu-test.py x86_64 --sched mlfq --smp 8 slabtest idletest afftest balancetest pcputest smptest
```

These are local functional/stress results, not a claim of hardware validation,
NUMA/cgroup support or complete BKL removal. The graphics-port `wltest` suite
was not run locally; CI retains it when the ports userland is built. M24 and M28
remain separate milestones. The pre-existing non-x86 syscall debug-name alias
initializer warning is unchanged. GitHub Actions configuration was validated,
not remotely executed as part of this local completion run.

## CI/CD
- `.github/workflows/release.yml`: on every push to `main` (docs/markdown-only changes are ignored), on PRs (build only) and manually (`workflow_dispatch`, optional `ports: false` → `NO_PORTS=1`). A `stamp` job fixes one UTC timestamp, a matrix builds x86_64/riscv64/aarch64 on ubuntu-24.04 (clang 18; `scripts/fetch-deps.sh`, `userland/build-all.sh`, `make iso`; downloads cached via `TOOLS_DIR`), and `release` publishes `9os-<YYYYMMDD-HHMMSS>` with `9os-<ts>-<arch>.iso` + `SHA256SUMS`. Build scripts accept `TOOLS_DIR` (default `/data/tools`). First release: `9os-20261005-114810`.

## Next steps (see docs/ROADMAP.md)
1. xkeyboard-config data so libxkbcommon can compile real keymaps (wl_keyboard XKB_V1 keymaps for toolkits); a terminal client (foot needs fcft/freetype/fontconfig) or a tiny own one.
2. DRM properties/atomic + PRIME for wlroots; libinput/libevdev/mtdev + a udev shim; seatd; then tinywl/wlroots and Sway.
3. Mesa softpipe (EGL/GLES2/GBM) after that; fine-grained locking + per-CPU run queues; interrupt-driven virtio (PLIC/GIC), virtio-blk + ext2; CPU time accounting (done in M25 part 1).

## M26: VMM v2
- **VMA tree** (mm/mm.c, kernel/mm.h): VMAs live in an augmented red-black tree
  (kernel/lib/rbtree.c, host-tested by `scripts/rbtree-host-tests.sh`) keyed by start, plus an
  address-ordered list. Each node caches `gap` (free space down to the previous VMA) and
  `max_gap` (subtree maximum); `gap_search()` finds the highest fitting hole below `mmap_hint` in
  O(log n), keeping `STACK_GUARD_GAP` (1 MiB) below stack VMAs. `vma_find`/`vma_lower_bound` are
  tree descents. Adjacent compatible VMAs are merged after mmap/mprotect/mlock/brk.
- **Page state**: `struct page` gained `uflags` (PGU_LRU/REFERENCED/DIRTY/CACHE), `mapping`,
  `index` and `mapcount`; every user PTE holds one reference and one mapcount, `mm->rss` counts
  resident pages (`VmRSS`, `/proc/pid/stat`). PTE installs/removals go through
  `pte_install()`/`pte_zap()`; unmaps under TLB batching gather pages and free them only after
  the batch flush.
- **File mappings** are demand-faulted: a VMA holds a file reference and `pgoff`;
  `file_ops.fault_page` (tmpfs: `t_fault_page`) returns the page-cache page under `mm->lock`
  (atomic, no mutex). Private mappings map the cache page read-only and copy on write; pages past
  EOF are zero pages. Shared mappings map the cache page itself (writable shared pages are marked
  dirty). MAP_SHARED anonymous memory and MAP_SHARED of /dev/zero are unlinked tmpfs files
  (shmem). exec maps read-only ELF segments as private file VMAs. DRM keeps the eager
  `mmap_page` path, fbdev the physical `mmap` path (both under the BKL).
- **Reverse map**: file VMAs are on `inode->i_mmap` (spinlock); `rmap_unmap_file_page()` walks it
  and unmaps a page from every mm with `mm_trylock()` (fails on mlocked VMAs or busy mms).
  Anonymous pages have no reverse map (there is no swap device to evict them to).
- **Page cache and reclaim** (fs/tmpfs.c): initramfs files are no longer copied — the archive stays
  reserved and `tmpfs_set_backing()` makes reads/faults fill pages lazily from it (boot RAM use
  dropped by the size of the initramfs). Clean backed pages are on a global LRU (second chance via
  PGU_REFERENCED); `tmpfs_reclaim()` trylocks the inode mutex under the LRU lock (an LRU page's
  inode cannot be evicted without that mutex), unmaps via rmap, and frees the page if only the
  cache still references it. Written or unbacked pages are dirty and never reclaimed. The page
  array entries are guarded by `tf->pglock`, so faults need no mutex.
- **Pressure**: `kswapd` (started by `mm_pressure_init()`) wakes every 50 ms and reclaims up to the
  high watermark (total/32) when free memory is below total/64. Allocation failures inside a
  fault or user copy drop `mm->lock` and call `oom_retry()`: direct reclaim, PCP drain, then the
  OOM killer (largest RSS, never pid 1, one victim at a time, `SIGKILL` under the BKL). Faults
  retry after the kill; copies return -EFAULT.
- **Syscalls** (core/sys_mm.c, all lock-free): mmap (MAP_SHARED_VALIDATE, FIXED_NOREPLACE,
  POPULATE, LOCKED, access checks → EACCES), munmap, mprotect, `mremap` (in-place grow/shrink,
  MAYMOVE, FIXED, DONTUNMAP), `madvise` (DONTNEED/FREE/REMOVE zap, WILLNEED/POPULATE_READ/WRITE
  prefault, the rest validated no-ops), `mlock`/`mlock2(MLOCK_ONFAULT)`/`munlock`/`mlockall`
  (CURRENT/FUTURE/ONFAULT)/`munlockall` (VMA_LOCKED, populated, never reclaimed, `VmLck`), `msync`
  (validation only: tmpfs is the backing store and shared mappings are coherent), `mincore`, brk
  (`[heap]` VMA).
- `/proc/pid/maps` shows offsets, shared/private, file inode/path, `[heap]`/`[stack]`;
  `/proc/meminfo` Cached; `/proc/vmstat` page-cache, fault, reclaim, OOM and rmap counters.
- 2 MiB pages for the direct map already existed (`vmm_map_range` uses 2 MiB leaves).
- `vmtest` (ci-tests): mremap variants, madvise on anon and private file mappings, shared file
  coherence, msync/mincore errors, mlock accounting, shared anon and /dev/zero across fork,
  FIXED_NOREPLACE, 1500 split-and-merged VMAs, the stack guard gap, and memory pressure: a hog is
  OOM-killed while page-cache pages are reclaimed, after which binaries checksum the same and run.
- Known limits: no swap and no anonymous rmap; truncating a file does not unmap pages already
  mapped past the new EOF (they stay valid but detached from the cache); `MADV_REMOVE` does not
  punch holes; mlock has no RLIMIT_MEMLOCK.

## M27: Hardening
- **User-access gate** (kernel/uaccess.h, mm/uaccess.c, `arch/uaccess.h`): every kernel
  dereference of a user pointer sits between `user_access_begin()`/`user_access_end()`. The window
  nests per thread (`thread.uaccess`) and is re-applied by `sched_finish_switch()` (a driver may
  sleep inside it). x86: SMEP+SMAP in CR4 when CPUID has them, the window is `STAC`/`CLAC`;
  interrupt entry clears EFLAGS.AC (user mode may set it; an interrupted copy gets it back from
  its iret frame) and SYSCALL's FMASK already does. aarch64: PAN (ID_AA64MMFR1) with
  SCTLR.SPAN = 0 so every exception entry sets PSTATE.PAN; user PTEs are PXN, kernel PTEs UXN.
  riscv64: SSTATUS.SUM is no longer always on — it is cleared at boot, on every trap from user
  mode and outside the window (S-mode can never execute U pages). QEMU now runs x86 with
  `-cpu qemu64,+smep,+smap,+rdrand` and aarch64 with `-cpu cortex-a76` (PAN; override
  `QEMU_CPU=`).
- Users of the window: `copy_{to,from}_user`/`strncpy_from_user` and the BKL driver path of
  read/write/pread64/pwrite64 (sys_fs.c `uaccess_call`). Those four syscalls now reject buffers
  outside the user half with `access_ok()` up front — the file-op copy helpers (tty, pipe, tmpfs,
  eventfd, devices) treat kernel addresses as in-kernel buffers (sendfile, exec), so before this
  `write(1, kernel_addr, n)` leaked kernel memory.
- **Fault triage** (`kernel_fault_check`, called by each arch before the lock-free fault path):
  a kernel-mode instruction fetch from a user address panics (SMEP/PXN); a kernel data access to
  a user address with the window closed is counted (`uaccess_violations`), fixed up if its PC is
  in the exception table, else panics with "SMAP/PAN/SUM violation". Also fixed: a user-mode x86
  #PF on a kernel address now raises SIGSEGV instead of panicking.
- **Exception table**: `__copy_user(dst, src, n)` (arch `uaccess.S`: rep movs / ld-sd / ldr-str
  loops) returns the bytes not copied; its loads/stores have `__ex_table` entries (linker scripts
  collect them in rodata, `__start/__stop___ex_table`). `user_copy()` now runs it under the mm
  lock *without* prefaulting: absent/COW pages are faulted in place by the ordinary fault path
  (recursive mm lock); with `thread.pagefault_disabled` set that path makes one attempt and never
  reclaims/OOM-kills, so bad addresses and allocation failures land in the fixup. The remainder
  goes through the old slow path (VMA permission check + prefault, OOM retry outside the lock;
  `user_copy_slowpath` in /proc/vmstat). `kernel_fault_fixup()` is the last resort before the
  BKL/panic for any kernel-mode fault.
- **Stack protector**: `-fstack-protector-strong` (x86 `-mstack-protector-guard=global`; the
  others default to the global guard). `stack_guard_init()` (no_stack_protector) randomizes
  `__stack_chk_guard` from `arch_entropy()` (RDTSC+RDRAND / CNTVCT+RNDR / rdtime) first thing in
  kmain, low byte zero; `__stack_chk_fail` panics. `CONFIG_HARDEN=1` is in CONFIG_FLAGS so the
  config stamp forces a full rebuild.
- **ASLR** (exec.c, cmdline `norandmaps` disables): PIE base `0x400000 + rand(2^24 pages)`
  (aligned to the largest PT_LOAD p_align), mmap base `USER_MMAP_BASE - rand(2^28 pages)` (so
  ld.so/libc and every non-fixed mmap move), stack top `USER_TOP - rand(2^22 pages)` plus a
  sub-page sp offset (the sigreturn trampoline sits below the stack), brk start
  `+ rand(2^13 pages)`. AT_RANDOM comes from `random_u64()`, whose state is now mixed with
  `arch_entropy()` at boot (`rng_mix`). fork inherits the layout.
- **W^X**: the boot CPU audits the kernel half of the page tables (`arch_kernel_wx_pages`, all
  three arches) and panics on any writable+executable kernel page (0 today: text RX, rodata R,
  data/physmap/MMIO RW NX). User W+X mappings (mmap, mprotect, ELF segments, segments widened to
  share a page) go through `wx_check()`: counted and logged (first 4) by default, refused with
  -EACCES under `wx=strict`, ignored with `wx=off`.
- `/proc/hardening`: features, stack guard state, randomize_va_space, W^X policy and the
  kernel_wx_pages / wx_mappings / wx_denied / extable_fixups / uaccess_violations counters.
- `hardentest` (dynamic PIE, ci-tests): three exec'd children must differ in PIE text, stack,
  mmap, brk, libc and AT_RANDOM; bad and kernel pointers to write/open/fstatat/read (pipe, tmpfs,
  /dev/zero, tty) return EFAULT and go through the exception table; reads into PROT_READ pages
  fail, reads into fresh demand-zero pages work; the W+X counter moves for RWX but not for RX.
- Fixed on the way: two CPUs shooting down each other's TLBs with IRQs off deadlocked in
  `arch_tlb_remote()` (each waited for the other's ack; seen as a faulttest hang with munmap on one
  CPU and an exiting mm's gather flush on another). The ack wait now serves its own pending IPIs.
- Known limits: no KASLR (the kernel stays at 0xffffffff80000000); no per-thread kernel stack
  canaries or shadow stacks/CET/BTI/PAC; the in-kernel buffer convention of the file-op helpers
  (kernel address = kernel buffer) remains, guarded by `access_ok()` at the four syscall entries.

## M28: Interrupt-driven I/O
- **Generic IRQ layer** (`kernel/core/irq.c`, `kernel/irq.h`): `irq_request(line, name, hard,
  thread_fn, ctx)` puts an action on a shareable level-triggered line (x86 GSI via the IO-APIC,
  aarch64 SPI INTID, riscv PLIC source); `irq_request_msi()` allocates a message-signalled vector
  (x86) and returns the address/data pair. The hard handler runs in IRQ context under the BKL and
  must quiet the device (returning IRQ_NONE/HANDLED/WAKE_THREAD); a thread handler gets its own
  kernel thread `irq/N-name` (wait_until_sl on a pending flag, so it is lost-wakeup-free and
  reruns if the line fired while it worked). The dispatcher counts per line and per CPU, then EOIs
  (x86; the GIC/PLIC dispatchers complete after the handler returns). The old raw
  `irq_install()` is now only the arch backend; PS/2, the x86 COM1 RX and the ACPI SCI moved to
  `irq_request` so they show up in `/proc/interrupts`.
- **irq_work**: `irq_work_queue()` is callable from any context (even under the console lock);
  it pushes onto a lock-free list and raises a self-IPI (`IPI_WORK`) once `irq_work_enable()` ran
  after SMP bring-up; the list is also drained from the timer tick and the idle loop. Every arch's
  IPI vector now calls `ipi_irq()` (accounting + `ipi_handle()` + irq_work); spin loops still use
  plain `ipi_handle()` and never run irq_work.
- **x86**: `arch_msi_alloc` takes vectors from the IO-APIC allocator (dest = BSP, fixed, edge);
  `pci_msix_enable()` (pci.c, with `pci_find_cap`) programs one table entry, masks the rest and
  turns INTx off. `arch_pci_intx_line` returns -1 (no _PRT parsing), so x86 virtio needs MSI-X.
- **riscv64**: PLIC driver (FDT `plic@`/`interrupt-controller@c`, default 0x0c000000; all
  sources routed to the boot hart's S context `2*hartid+1`, claim/complete loop for scause
  `1<<63|9`, SIE.SEIE on the BSP only). Console input comes from the ns16550 RX interrupt (FDT
  `serial@` reg/interrupts, default 0x10000000 / source 10) while output stays on SBI; SBI
  getchar polling remains only if there is no PLIC/UART.
- **aarch64**: GICv3 next to GICv2 — version from the MADT GICD entry, else a GICR range (MADT
  type 0xE) or the FDT `arm,gic-v3` compatible (GICD_PIDR2 lies outside the 4 KiB GICv2
  distributor and faults). v3: ARE + Group 1, SPIs routed with IROUTER to the BSP affinity,
  per-CPU redistributor found by GICR_TYPER affinity (woken via GICR_WAKER), SGI/PPI setup in the
  SGI frame, ICC_* system registers for ack/EOI/PMR, IPIs through ICC_SGI1R. Test with
  `QEMU_MACHINE_aarch64=virt,gic-version=3` (riscv also has `QEMU_MACHINE_riscv64`). PL011 RX/RT
  interrupt (SPCR GSIV, default INTID 33) replaces the 1 kHz console poll in the timer tick.
- On riscv/aarch64 `arch_idle_poll_ns()` is now UINT64_MAX (no 10 ms idle wakeups for console
  input): idle APs report 0 ticks per idle second in irqtest.
- **PCI INTx** on QEMU virt (root bus): aarch64 INTID `35 + (slot + pin - 1) % 4`, riscv PLIC
  source `32 + (slot + pin - 1) % 4`.
- **virtio** (`virtio.c`): maps the ISR capability; `virtio_irq_setup(v, name, hard, thread, ctx)`
  (before `virtq_init`) prefers one MSI-X vector shared by all queues (`queue_msix_vector = 0`,
  `msix_config = NO_VECTOR`) and falls back to the INTx line, whose hard handler reads ISR (which
  acks) and returns IRQ_NONE when the device was not the source. virtio-input: per-device thread
  handler drains the event ring (the poll thread starts only for devices without an IRQ).
  virtio-gpu: the completion interrupt wakes `gpu_cmd()` in the flush thread (20 ms timeout
  fallback; the init path still spins); the flush thread sleeps until damage arrives (fbcon's hook
  runs under the console lock, so it raises an irq_work that does the wake_up), then waits 16 ms to
  batch; only an mmap'd fbdev without explicit damage flushes periodically. fbcon_set_graphics()
  now always damages so the flusher notices mode changes.
- `/proc/interrupts`: per-CPU LOC (timer ticks), IPI and DEV counters, then one row per line:
  number (or `vNN` for MSI vectors), count, chip (IO-APIC / MSI-X / GICv2 / GICv3 / PLIC),
  actions with handled counts and thread runs, unhandled count.
- `irqtest` (ci-tests): the console UART line exists on a real controller and has fired, every
  virtio-input line has a thread handler, virtio-gpu interrupts increase when the console is
  written (riscv/aarch64), and idle APs stay (nearly) tickless.
- Known limits: no MSI on aarch64 (GICv2m/ITS) or riscv (IMSIC/APLIC), so virtio there shares
  INTx lines; all device interrupts go to the boot CPU (no affinity/balancing); x86 without
  MSI-X stays polled; `drm-vblank` remains a timer-driven emulated vblank and kswapd stays a
  kernel thread by design.

## M29: Block layer + virtio-blk
Built together with M30 and shaped by what comes after it (a disk root that git, package
managers and a self-hosted toolchain can live on), not just by "read sectors".
- **Block core** (`kernel/block/blk.c`, `kernel/blk.h`): `struct blkdev` registry (64 entries:
  whole disks and their partitions; `whole`, `start`, `nr_sectors`, `max_vecs`, `readonly`).
  `bio_alloc/bio_add/submit_bio/bio_complete/submit_bio_wait`: a bio is a list of page segments
  for one contiguous sector range (READ/WRITE/FLUSH); `submit_bio` remaps partitions, checks bounds
  and RO, and inserts into the disk's queue sorted by sector; the dispatcher (`pick()`) is C-LOOK
  and never reorders across a FLUSH (a barrier: everything submitted before it completes first).
  Completion runs `end_io` in the driver's thread-IRQ context and refills the device. Waiting is
  uninterruptible (a signal must not leave a half-written page cache page).
- **Buffer cache = the page cache of the device inode**: each disk/partition has an inode whose
  `address_space` (bdev_aops) caches whole pages. `pg->private` is a per-sector dirty mask, so
  writepage only writes the dirty runs of sectors (a 1 KiB ext2 block dirtied ≠ 4 KiB written).
  Metadata helpers for filesystems: `bread` (read + pin), `bget_new` (no read: freshly allocated
  block), `bdirty`, `brelse`, `bforget` (drop dirty state of a freed block), `blk_sync`. Because
  file data (M30) and metadata live in the same LRU/write-back machinery, there is one reclaim and
  one writeback policy for everything; there is no separate buffer_head layer.
- **/dev/vdX, /dev/vdXN** (major 254, minor disk·16 + part): read/write/pread/pwrite go through the
  cache with sector masks, `lseek(SEEK_END)`, ioctls BLKGETSIZE64/BLKGETSIZE/BLKSSZGET/BLKPBSZGET/
  BLKFLSBUF/BLKRRPART(EBUSY), fsync flushes. GPT and MBR (primary only) scanned at registration;
  `/proc/partitions` and `/proc/diskstats` (Linux format, so busybox/util-linux tools work).
- **virtio-blk** (`drivers/virtio_blk.c`): PCI only (modern 0x1042, transitional 0x1001) — every
  supported platform (q35, riscv virt, aarch64 virt) gives us PCIe, so virtio-mmio would be a
  second transport for no machine we boot. Requires `VIRTIO_RING_F_INDIRECT_DESC`: one slot per
  request (indirect table + header + status in 1 KiB), so queue depth = ring size (128) regardless
  of segment count. Negotiates SIZE_MAX/SEG_MAX/RO/BLK_SIZE/FLUSH; completions are reaped in the
  M28 threaded IRQ handler (MSI-X on x86, INTx on riscv/aarch64), or a `vblk-poll` thread without
  an IRQ. virtio.c gained `virtio_pci_probe_features()`, rings up to 128 entries laid out in one
  page, and `VIRTQ_DESC_F_INDIRECT`.
- Tools/tests: `scripts/mkdisk.sh OUT MB [DIR] [BLOCKSIZE]` (GPT, one ext2 partition at 1 MiB,
  `mke2fs -O ^dir_index,^resize_inode -d DIR`; needs e2fsprogs + sfdisk, now in sandbox-setup and
  the CI workflow); `qemu-test.py --disk IMG` (repeatable, vda, vdb…) and `--cmdline`; Makefile
  `CMDLINE=` is appended to a generated `build/ARCH/limine.conf`. x86 QEMU runs with `-boot d`
  (SeaBIOS otherwise tries to boot the GPT disk and hangs). `blktest DEV` (raw scratch disk: ioctls,
  unaligned I/O, EOF, concurrent writers, fsync, /proc files; `-w/-v DEV SEED` for persistence
  across boots).
- Known limits: no virtio-mmio, no extended MBR partitions, no BLKRRPART rescan, single queue per
  disk, no I/O priorities or per-process accounting, no discard/write-zeroes.

## M30: ext2 + unified page cache
- **Unified page cache** (`mm/filemap.c`, commit "M30 prep"): radix-tree `address_space` per
  inode, async `readpage` + readahead, dirty/writeback accounting with a `writeback` thread,
  LRU reclaim of clean pages (dirty ones are written first), rmap-based mkclean for shared
  mappings, and `FLT_IO` faults: a fault that needs I/O records inode+index, drops mm->lock,
  waits for the page (`fault_io_run()`), then retries. If reading needs memory that is not there
  the fault goes down the normal reclaim/OOM path (vmtest's OOM hog on a disk root used to get
  SIGSEGV). tmpfs, block devices and ext2 all sit on it. `mapping_detach()` (with `no_writeback`)
  takes a mapping away from writeback safely for eviction/umount.
- **VFS**: `super_block`/`super_ops`/`fs_type` (`fs_register`), inode cache per sb (hash, ≤4096
  unused inodes on an LRU, pruned, `evict_inode` on last put; directories pin their parent), a
  dentry cache (1024 buckets, ≤8192 entries, positive and negative, for `SB_DCACHE` filesystems;
  invalidated by create/mknod/unlink/symlink/link/rename, purged on rmdir/umount), a real mount
  table (`/proc/mounts`, `/proc/filesystems`), `mount(2)`/`umount2(2)` (root only; MS_RDONLY,
  MS_REMOUNT, MNT_DETACH; EBUSY when anything inside is in use — unused cached inodes are pruned
  first), `mark_inode_dirty`, `fsync`/`fdatasync`/`syncfs`/`sync`, per-fs `statfs`, `msync(MS_SYNC)`
  writing back. `reboot(2)` runs `vfs_shutdown()`: sync, then remount every disk fs read-only so
  the next mount is clean.
- **ext2** (`fs/ext2.c`): read/write rev0/rev1, 1–4 KiB blocks, filetype, sparse_super,
  large_file (unknown ro_compat → read-only, unknown incompat → refused). Block/inode allocation
  with group-local goals, direct/1/2/3-indirect maps, holes, truncate, async readpage, writepage
  that allocates for blocks dirtied through shared mmap, all directory operations incl. rename of
  directories (`..` retargeted, nlinks), fast and slow symlinks, hard links, unlink-while-open
  (freed on evict), `s_state` VALID/ERROR handling. Inodes are written through to the buffer cache
  on change (metadata reaches disk at the next writeback/sync); the header comment documents the
  locking (per-inode `map` mutex for block maps, `fs->alloc` for bitmaps/counters).
- **Disk root**: `root=/dev/vdXN` on the kernel command line mounts that partition as `/` after
  the drivers came up; the boot tmpfs's `/dev`, `/tmp` and `/proc` become mounts on the new root
  (created there if missing), so device nodes and runtime state keep working without a devtmpfs.
- ci-tests now boots three times: the main suite with an empty ext2 disk (vda: `ext2test
  /dev/vda1 /mnt`, mounting/umounting itself) and a raw scratch disk (vdb: `blktest`); then
  `root=/dev/vda1` on an image of the userland root (libc/dyn/mmap/file/vfs/vm tests and
  `ext2test -d` on the disk root, writing data), and a second disk-root boot that verifies the data
  and that the fs was cleanly shut down; host `e2fsck -fn` must pass on every image afterwards.
- Why this shape (broader context): git and package managers need rename atomicity, fsync,
  hard links, mmap of files and many small files — all exercised by ext2test; keeping one page
  cache for data and metadata keeps the memory-pressure story single (M27/M28 work carries over);
  the inode/dentry caches and uid/gid/mode on disk are the base for M31 (permissions) and the
  inode/sb layering for xattrs/other filesystems later.
- Known limits: no journaling and no orphan list (a crash may need e2fsck; unlinked-open inodes
  leak until fsck), no htree directories (linear scans), no xattrs/ACLs, superblock backups and
  group descriptor backups are not updated, atime is never updated (effectively `noatime`),
  `st_blocks` derived from size, the VFS namespace mutex is held across directory I/O (one
  directory operation at a time system-wide), no bind/move mounts.
- Fixed along the way: riscv64 routed the PLIC to hart 0's S context even when OpenSBI's lottery
  picked another boot hart (cpus[0].hwid was only filled in by smp_init, after plic_init), so on
  ~1 in 4 boots no device interrupt arrived at all — invisible until a boot-time disk mount
  waited for one. The boot hart now comes from Limine's RISC-V BSP hart-id request. Umount no
  longer reports EBUSY because of cached unused directories (they pin their parent).
- Lock order additions: VFS namespace mutex → per-inode `bmap` mutex (LR_MUTEX_BMAP) → icache
  mutex (LR_MUTEX_ICACHE) → `fs->alloc` (LR_MUTEX_FSALLOC) → … → icache spinlock (LR_ICACHE, under
  mm locks, above the page cache) … → block queue (LR_BLKQ) → driver lock (LR_BLKDRV) → sched.

## M31: Users and permissions
- **Credentials** (`core/cred.c`, `kernel/cred.h`): an immutable, refcounted `struct cred`
  (real/effective/saved/fs uid and gid, up to 64 sorted supplementary groups, the five
  capability sets, securebits). Like Linux there are two views: every thread has a *subjective*
  cred (`current->cred`, what it acts as; `cred_override()` swaps it temporarily) and the process
  keeps the *objective* one (`p->cred`, last committed by one of its threads; read under the leaf
  `cred` spinlock with `proc_cred()` for kill/`/proc`/sched/capget). Changes are always
  `cred_prepare()` → edit → `cred_commit()`; musl runs set*id on every thread (`__synccall`), so
  POSIX process-wide semantics fall out. Kernel threads always run as `init_cred` (root, all caps).
- **Syscalls** (`core/sys_cred.c`): get/set{,e,re,res,fs}{uid,gid}, get/setgroups (CAP_SETGID),
  capget/capset (v1/v2/v3, Linux's rules: permitted only shrinks, effective ⊆ permitted,
  inheritable bounded), prctl KEEPCAPS / CAPBSET_READ / CAPBSET_DROP / GET_SECUREBITS / CAP_AMBIENT
  (read-only). Uid changes apply Linux's "root is special" capability fix-ups (losing all root
  ids drops permitted+effective unless KEEPCAPS; euid 0↔non-0 clears/restores effective; fsuid
  changes toggle the filesystem caps). `clock_settime` needs CAP_SYS_TIME.
- **VFS DAC** (`fs/vfs.c`): `inode_permission()` = owner/group/other class against fsuid,
  fsgid + supplementary groups, then CAP_DAC_OVERRIDE / CAP_DAC_READ_SEARCH (root executes only
  files with some x bit), EROFS for writes on read-only mounts. Search permission on every path
  component; open checks read/write (O_TRUNC = write, acc mode 3 = both); create/mknod/symlink/
  link need W+X on the parent; unlink/rmdir/rename add the sticky-directory rule (owner of the
  file or directory, or CAP_FOWNER) and EBUSY on mount points; moving a directory to another
  parent needs write on it (its `..` changes); protected_hardlinks (link only what you own or
  could read+write, never setuid/setgid-exec files). chmod/chown/utimensat/truncate follow POSIX
  (owner or CAP_FOWNER; chown needs CAP_CHOWN except chgrp by the owner into one of its groups;
  setgid silently dropped when the group is not yours; chown and unprivileged writes/truncates
  clear setuid/setgid). New inodes get fsuid and fsgid, or the directory's group under a setgid
  directory (subdirectories inherit the bit). umask was already applied at create.
- **exec**: the file is opened `O_PATH` and needs only execute permission (an `--x` binary
  runs; the interpreter `ld.so` likewise). setuid/setgid bits switch effective + saved + fs ids;
  capabilities follow Linux without file caps: root (real or effective) gets
  `inheritable | bounding` as permitted, effective if euid is 0; others get the ambient set.
  The auxv carries AT_UID/EUID/GID/EGID and AT_SECURE (ids changed or caps gained), so musl
  ignores LD_* for setuid programs. The new cred is committed at the point of no return.
- **Other checks**: kill/tgkill (sender ruid/euid vs target ruid/suid, CAP_KILL, SIGCONT within
  the session; `kill(-1/-pgrp)` reports EPERM if all were denied), nice/setpriority/sched_*/
  setaffinity on others' threads or raising priority/RT (CAP_SYS_NICE; EACCES for nice like
  Linux), mount/umount2/sethostname (CAP_SYS_ADMIN), reboot (CAP_SYS_BOOT + magic numbers),
  chroot (CAP_SYS_CHROOT), mknod of devices (CAP_MKNOD), connecting to a unix socket (write on
  the socket inode), SO_PEERCRED/SCM_CREDENTIALS (effective ids). `/proc/<pid>` is owned by the
  process's euid/egid, `/proc/<pid>/status` shows real Uid/Gid/Groups/Cap* lines, `environ` is
  0400 and following/reading another user's `cwd`/`exe`/`fd/*` needs same ids or CAP_SYS_PTRACE.
  ptmx creates `/dev/pts/N` as the kernel, owned by the opener, group tty (5), mode 0620.
  The initramfs honours the cpio owner and exact mode (`cpio -R 0:0`, so setuid bits survive);
  `mkdisk.sh` resets the image to root ownership with debugfs (mke2fs -d copies the builder's uid).
- **Userland**: `/etc/passwd`, `/etc/group`, `/etc/shadow` (0640 root:shadow; root has no
  password like a live image, `user`/`9os` is uid 1000 with SHA-512 crypt), `/etc/securetty`,
  `/etc/shells`; init runs `getty -L 0 console` → BusyBox `login` instead of a bare root shell;
  `/bin/busybox` is setuid root (BusyBox drops privileges for every applet except su/passwd/
  login/ping…); rcS hands `/home/user` and `/run/user/1000` to the user; `/etc/profile` no longer
  forces HOME=/root. `qemu-test.py` logs in as root when it sees `login:`.
- Tests: `permtest [DIR]` (forks children with other ids: DAC classes, search, umask, sticky,
  chmod/chown rules, setgid dirs, suid stripping, setuid/setgid exec + AT_SECURE, `--x`
  binaries, caps drop/raise/bounding set, saved-id games, KEEPCAPS, fsuid, access() real vs
  effective, privileged syscalls, `/proc` ownership and link protection, protected hardlinks,
  SO_PEERCRED, pty ownership, unix-socket connect); run on tmpfs in the main boot and on ext2 in
  the disk-root boot. `logintest` drives BusyBox login on a pty (wrong password refused, session
  ids/groups/HOME/tty owner, `/etc/shadow` and `/root` unreadable, su back to root, `passwd` as
  the user rewriting `/etc/shadow` through setuid busybox, old password refused, new accepted).
- Why this shape (broader context): per-thread subjective creds + override creds are what
  NFS-style servers, access(2) and kernel-internal node creation need, and keep the door open for
  user namespaces; matching Linux's capability arithmetic exactly means unmodified su/sudo/
  doas/login/daemons (which drop to nobody) behave, and real on-disk ownership on ext2 makes a
  multi-user system persistent. Ambient capabilities and file capabilities are the natural next
  step once xattrs exist.
- Known limits: no file capabilities (no xattrs), no ACLs, no nosuid/noexec/nodev mount flags,
  no user namespaces, no ptrace-based `dumpable` logic, RLIMITs are mostly fixed (RLIMIT_NICE 0,
  RLIMIT_RTPRIO 0), PR_SET_NO_NEW_PRIVS and SECBIT locking are not implemented, setpgid/setsid
  checks are lax, device nodes created at boot keep root:root (no video/input groups yet).
- Fixed along the way: on riscv64 the ABI keeps 32-bit values sign-extended in registers, but
  musl passes unsigned arguments (uid_t -1, reboot's 0xfee1dead) zero-extended, so `uint32_t`
  syscall parameters compared wrong (`chown(f, -1, g)` changed the uid, reboot() refused its
  magic and init died at poweroff). The chown family, set*id and reboot now take 64-bit
  parameters and truncate explicitly.
- Lock order addition: the `cred` spinlock (LR_CRED = 68) is a leaf; nothing is taken under it.

## M32: Networking
- **Own IPv4 stack** (no lwIP), all in `kernel/net/` behind `kernel/net.h`:
  - `core.c`: `struct pkt` (one kmalloc'd buffer with 80 bytes of headroom for the Ethernet/IP/TCP
    headers, `data`/`len`, network/transport header pointers, pkttype), clone/free, byte rings for
    socket data, the Internet checksum (aligned 64-bit fast path), one-shot network timers
    (`ntimer_mod/del`, run by the net thread), the receive queue and the **`net` kernel thread**
    that processes every received packet and expired timer under `net_mutex`, device registry
    (`netdev_register`, by index/name), `lo` (127.0.0.1/8, MTU 65536, xmit loops straight back
    into the receive queue) and the `/proc/net` files (dev, route, arp, tcp, udp, raw, unix, snmp).
  - `arp.c`: neighbour cache (pending-packet queue, 3 retries at 1 s, 60 s expiry), Ethernet
    output, ARP request/reply, `SIOC[SGD]ARP`, `/proc/net/arp`.
  - `ip.c`: routing table (connected routes per interface address + static routes,
    longest-prefix match, `SIOCADDRT/SIOCDELRT`), output with fragmentation, `IP_HDRINCL`, input
    with validation and reassembly (32 queues, 30 s timeout), ICMP echo reply and destination
    unreachable/port unreachable, ICMP errors mapped to errnos and handed to UDP/TCP.
  - `dgram.c`: UDP (checksums, port demux incl. wildcard/connected/broadcast, ICMP errors as
    `ECONNREFUSED` on connected sockets), raw IP sockets (`ICMP_FILTER`), unprivileged ICMP
    "ping" sockets (`SOCK_DGRAM, IPPROTO_ICMP`, matched by echo id — BusyBox ping uses raw
    sockets via setuid busybox, both work), `AF_PACKET` taps (SOCK_RAW/SOCK_DGRAM, cBPF filters).
  - `tcp.c`: full RFC 793/1122 state machine, NewReno congestion control, RFC 6298 RTO with
    Karn + backoff, delayed ACKs (40 ms, every second full segment, immediately for small/out of
    order data), Nagle (TCP_NODELAY), sender/receiver SWS avoidance, persist timer for zero
    windows, keepalives (SO_KEEPALIVE/TCP_KEEPIDLE/INTVL/CNT), MSS option, listen backlog with
    unaccepted children, RST rules (RFC 2525 close-with-unread-data, data for a closed socket),
    out-of-order queue, TIME_WAIT 2 s, rate-limited ACKs for out-of-window pure ACKs.
  - `inet.c` + `socket.c`: the socket layer: `sys_socket…sys_recvmmsg` dispatch AF_UNIX to
    `unix.c` (`unix_sys_*`) and AF_INET/AF_PACKET to `inet.c`; bind/connect/listen/accept4/
    getsockname/getpeername/shutdown, send/recv{,from,msg,mmsg} with 16 KiB bounce buffers (user
    copies happen with `net_mutex` dropped), blocking/non-blocking/timeouts (SO_RCVTIMEO/
    SO_SNDTIMEO), poll/epoll (the pollmask is recomputed lock-free on every state change and
    `poll_notify()` wakes waiters), ephemeral ports 32768–60999, SO_REUSEADDR, SO_BINDTODEVICE,
    SO_BROADCAST, SO_LINGER, SO_ERROR, SO_ATTACH_FILTER, IP_TTL/TOS/HDRINCL/PKTINFO (multicast
    and PMTU options are accepted as no-ops), TCP_NODELAY/CORK/MAXSEG/KEEP*, FIONREAD/SIOCOUTQ, and the interface ioctls ifconfig/route/udhcpc need
    (SIOCGIFCONF/…FLAGS/ADDR/NETMASK/BRDADDR/MTU/HWADDR/INDEX/NAME/TXQLEN, setters need
    CAP_NET_ADMIN). Raw and packet sockets need CAP_NET_RAW; ports < 1024 CAP_NET_BIND_SERVICE.
- **virtio-net** (`drivers/virtio_net.c`): modern PCI (1af4:1041, transitional 1000), MAC +
  MTU features, 128-entry RX and TX rings of 2 KiB DMA slots (packets are copied in and out —
  simple, and fast enough: ~40 MiB/s loopback, ~6 MiB/s each way through QEMU slirp), threaded
  MSI-X/INTx interrupt with a polling fallback; registers `eth0`.
- **Locking**: everything protocol-side runs under one mutex, `net_mutex` (LR_MUTEX_NET = 21:
  after the BKL, before fd/mm locks); socket syscalls are `nobkl`. Under it only leaf locks are
  taken: the driver lock (LR_NETDRV = 67) and the receive-queue spinlock (LR_NETQ = 69, also
  taken from interrupt context). Sleeping waits (`lock_wait`) drop `net_mutex` around
  `poll_wait_seq()`, sampling the poll sequence before unlocking so no wakeup is lost.
- **Userland**: rcS brings up `lo` and, if `eth0` exists, runs `udhcpc -b` with
  `/usr/share/udhcpc/default.script` (ifconfig, default route, `/etc/resolv.conf`); `/etc/hosts`,
  `/etc/services`, `/etc/protocols`. ping, wget, nc, telnet(d), httpd, nslookup, ifconfig, route,
  ip work from BusyBox. The Makefile gives every QEMU a `-netdev user` + virtio-net-pci
  (`QEMU_NET=` to override).
- **SIGALRM fix**: `alarm()`/`setitimer()` used to fire only when the process returned to
  userspace, so a process blocked in a syscall (BusyBox ping waiting in recvfrom) never got its
  signal; an `alarm` kernel thread now delivers due timers and interrupts the sleeper.
- **Tests**: `nettest` (loopback: TCP basics/half-close/RST/EPIPE/ECONNREFUSED/non-blocking
  connect/EADDRINUSE/options, 16 MiB bulk transfer, 8 concurrent echo clients against a poll()
  server, flow control with a stalled reader, UDP incl. connected errors and broadcast, IP
  fragmentation over a 1500-byte `lo`, raw + ping ICMP sockets, privilege checks, interface
  ioctls; a per-test watchdog reports hangs). `nettest -x HOST PORT` runs a 4 MiB TCP echo and a
  UDP echo against `scripts/net-host-server.py`; `nettest -s PORT` is an echo server for
  host-driven tests. `qemu-test.py --net-test` starts the host server (guest sees it at
  10.0.2.2:@HP@) and forwards host port @FP@ to guest TCP 8080/UDP 8081; `host:CMD` steps run on
  the host. ci-tests checks DHCP (10.0.2.15, default route, resolv.conf), ping, `nettest -x`,
  wget of a small and a 1 MiB file (md5), and an incoming 1 MiB echo through the forwarded port.
- Bugs worth remembering: the active-open path did not initialise `rcv_adv`, so the "never
  shrink the window" rule advertised garbage (often zero) windows and connections stalled at
  random; a zero-window probe byte that the peer accepted was not counted in `snd_max`, so the
  peer's ACK looked like it acknowledged unsent data and both ends ACKed each other forever
  (now the probe ACK is accepted and out-of-window ACK replies are rate-limited).
- Known limits: IPv4 only (AF_INET6 → EAFNOSUPPORT), no netlink (`ip` falls back to ioctls
  where it can), no window scaling/SACK/timestamps (64 KiB windows), no multicast routing/IGMP,
  no forwarding, no TCP_FASTOPEN, one global `net_mutex`, copies in the driver, TIME_WAIT is 2 s
  instead of 60 s.
