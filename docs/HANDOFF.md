# 9os — Handoff

_Updated after every milestone. Read this first when picking up the project._

## Current state: M23 complete, M24 (fine-grained locking) in progress — CI boots and tests every arch; scheduler has its own lock, the BKL is dropped across context switches and a set of syscalls runs lock-free

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
| M24 Locking (in progress) | ◐ `sched_lock` for run queue/sleep list/wait queues (held across the switch), BKL dropped on switch and retaken after, idle without BKL, `thread_interrupt()`; lock-free syscall fast path; per-mm lock, atomic page refcounts, page faults + user copies without BKL; IRQ-safe console lock; `sysbench`, `faulttest` |
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
- `kernel/drivers/virtio_gpu.c`: virtio-pci modern transport (common/notify caps), one polled control queue, GET_DISPLAY_INFO → RESOURCE_CREATE_2D (B8G8R8X8) → ATTACH_BACKING (one contiguous buddy block, max 4 MiB, so up to 1280x800) → SET_SCANOUT. A `vgpu-flush` kernel thread does TRANSFER_TO_HOST_2D + RESOURCE_FLUSH every 16 ms while the fb is dirty (`fb_damage()` from fbcon/fbdev write) or a client has fb0 open. It replaces the boot framebuffer via `boot_set_framebuffer()` and fbcon re-attaches. Used only when there is no firmware framebuffer, or with `virtiogpu` on the command line.
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
- **virtio-input** (`kernel/drivers/virtio_input.c`): PCI 1af4:1052; capabilities are read from the config space (ID_NAME/DEVIDS/PROP_BITS/EV_BITS/ABS_INFO); one `vinput-poll` kthread drains event queues every 4 ms. The Makefile adds `QEMU_INPUT=-device virtio-keyboard-pci -device virtio-tablet-pci` on all arches (override with `QEMU_INPUT=`; delete `build/<arch>/fw-vars.fd` after changing devices on riscv/aarch64). x86 PS/2 keyboard is an evdev device too ("AT Translated Set 2 keyboard").
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

## M24: fine-grained locking (in progress)
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
- Next: slab/kmalloc locks (already spinlocked; audit callers), per-file/pipe/socket locks so read/write/poll can leave the BKL, then per-CPU run queues (M25).

## CI/CD
- `.github/workflows/release.yml`: on every push to `main` (docs/markdown-only changes are ignored), on PRs (build only) and manually (`workflow_dispatch`, optional `ports: false` → `NO_PORTS=1`). A `stamp` job fixes one UTC timestamp, a matrix builds x86_64/riscv64/aarch64 on ubuntu-24.04 (clang 18; `scripts/fetch-deps.sh`, `userland/build-all.sh`, `make iso`; downloads cached via `TOOLS_DIR`), and `release` publishes `9os-<YYYYMMDD-HHMMSS>` with `9os-<ts>-<arch>.iso` + `SHA256SUMS`. Build scripts accept `TOOLS_DIR` (default `/data/tools`). First release: `9os-20261005-114810`.

## Next steps (see docs/ROADMAP.md)
1. xkeyboard-config data so libxkbcommon can compile real keymaps (wl_keyboard XKB_V1 keymaps for toolkits); a terminal client (foot needs fcft/freetype/fontconfig) or a tiny own one.
2. DRM properties/atomic + PRIME for wlroots; libinput/libevdev/mtdev + a udev shim; seatd; then tinywl/wlroots and Sway.
3. Mesa softpipe (EGL/GLES2/GBM) after that; fine-grained locking + per-CPU run queues; interrupt-driven virtio (PLIC/GIC), virtio-blk + ext2; CPU time accounting (`times()` still reports 0).
