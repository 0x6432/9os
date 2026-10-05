# 9os roadmap: SMP, scheduling, graphics, and the road to Sway/KDE

This file covers the work after M13 (BusyBox + Bash running on x86_64, riscv64
and aarch64). Each milestone is small enough to land, test on all three arches,
push, and back up on its own. **Bold** items are the current focus.

## Phase A: Multiprocessor and scheduling (M14–M16)

| M | Goal | Notes |
|---|------|-------|
| **M14** ✅ | **SMP bring-up** | Limine MP request; `struct cpu` per CPU (`this_cpu()`, `current` per CPU); per-CPU idle threads, TSS/GDT (x86), timers (LAPIC timer / SBI timer / generic timer PPI); IPIs (LAPIC ICR / SBI IPI / GIC SGI) for reschedule and TLB shootdown (x86 IPI, riscv SBI RFENCE, aarch64 `tlbi ...is`). The kernel is serialised by a **big kernel lock** (Linux 2.0 style): user code runs in parallel on all CPUs, and kernel entry takes the BKL. `nosmp` on the command line limits boot to one CPU. |
| **M14** ✅ | **Pluggable scheduler policy** | `make SCHED=rr` (default) or `make SCHED=mlfq` (`-DCONFIG_SCHED_MLFQ`). The policy interface is `enqueue/pick_next/tick/yield`. MLFQ has 4 levels with quanta 5/10/20/40 ms: a thread drops a level when it uses its whole slice, and all threads are boosted to the top level every 500 ms (anti-starvation). |
| **M15** ✅ | **Framebuffer device + demos** | `/dev/fb0` with the Linux fbdev ABI (`FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, `mmap`); the kernel console steps aside while a client has fb0 mapped (`KDSETMODE KD_GRAPHICS`). `/bin/fbdemo` draws lines, the Mandelbrot set, a Julia set, plasma and a Sierpinski triangle. riscv64/aarch64: Limine rejects `ramfb`, so the kernel drives **virtio-gpu** itself (PCI ECAM + virtio-pci, 2D scanout, 60 Hz flush thread). ✅ done (M15) |
| M15 | Fine-grained locking | Shrink the BKL: give the scheduler run queue, pmm/slab, page tables, VFS inodes, pipes and tty their own spinlocks or mutexes. Add per-CPU run queues with work stealing, plus CPU affinity (`sched_setaffinity`). |
| M16 | Interrupt controllers | riscv PLIC + virtio-mmio IRQs, aarch64 GICv3, x86 MSI. Remove polled input. |

## Phase B: A "real" POSIX base (M16–M22)

Done in M16: COW fork, `MAP_SHARED` (anonymous + tmpfs/memfd), AF_UNIX with `SCM_RIGHTS`, `socketpair`, `epoll`, `eventfd`, `timerfd`, `signalfd`, `memfd_create`. Done in M17: ptys (`/dev/ptmx` + `/dev/pts`), evdev input (virtio-input, PS/2) and VT/KD ioctls. Done in M19: dynamic linking (ld-musl, dlopen) and inotify. Done in M20: page cache for private mappings, ports framework, Lua/SQLite/libffi/expat/libwayland. Still open below: storage, networking, the rest of the graphics userland.

These features are what most ported software needs, in rough order of value:

1. **mm**: copy-on-write fork, `MAP_SHARED` file and anonymous mappings, `mprotect`, `madvise`, `mremap`, page cache.
2. ✅ (M19) **Dynamic linking**: `PT_INTERP` with `ld-musl` (to load shared musl, libdrm and Mesa `.so` files), `dlopen`.
3. **IPC**: AF_UNIX sockets (stream + dgram, `SCM_RIGHTS` fd passing, which Wayland requires), `socketpair`, `epoll`, `eventfd`, `timerfd`, `signalfd`, `memfd_create`, POSIX shm (`/dev/shm` on tmpfs), futex `PI`/robust lists.
4. ✅ **ttys**: `/dev/ptmx` + devpts (needed for terminals in the GUI: foot, konsole). Done in M17.
5. **Storage**: virtio-blk (PCI and mmio), an ext2 driver (read/write), and a persistent root disk.
6. **Networking** (optional, later): virtio-net and a small TCP/IP stack (lwIP port), AF_INET sockets.
7. **Easy software first**: ports that use few syscalls, used to test each step: `lua`, `sqlite3`, `tcc`, `make`, `vim`/`nano` (ncurses), `doom` (fbdev + evdev: the classic first graphical port), `ffplay`-less tools, `htop` (procfs).

## Phase C: Graphics stack (M23+, low priority but planned)

```
 apps (foot, weston-terminal, Qt/KDE)        <- Wayland clients
 Sway (wlroots)  /  KWin                     <- compositor
 wlroots: DRM backend, libinput, libseat     <- needs /dev/dri/card0, evdev, seatd
 Mesa: llvmpipe/softpipe (EGL, GLES2, GBM)   <- software rendering first
 libdrm  -> 9os DRM/KMS (dumb buffers)       <- kernel
 evdev (/dev/input/eventN) <- virtio-input / PS/2 / USB HID
```

Kernel steps:
1. fbdev (M15), then ✅ (M18: legacy KMS + dumb buffers + page flip; atomic/PRIME still open) **DRM/KMS-lite**: `/dev/dri/card0` with the ioctls libdrm and wlroots use (`DRM_IOCTL_VERSION`, `GET_CAP`, `MODE_GETRESOURCES/GETCONNECTOR/GETENCODER/GETCRTC/SETCRTC`, `MODE_CREATE_DUMB/MAP_DUMB/DESTROY_DUMB`, `MODE_ADDFB2/RMFB`, `MODE_PAGE_FLIP` + vblank events, `PRIME` fd export as memfd-like objects, atomic modesetting later). The backend is the Limine/ramfb framebuffer first, then **virtio-gpu** (2D resources plus a scanout; virgl 3D much later).
2. ✅ (M17, minus udev metadata) **evdev**: `/dev/input/event*` with `EVIOCG*` ioctls and `struct input_event` streams from PS/2, virtio-input and the riscv/aarch64 UART. libinput also needs udev-ish metadata, so provide a static `/run/udev/data` shim or patch libinput's udev dependency (eudev-lite).
3. **Seat/session**: `seatd` (builtin backend) needs `VT_*`/`KD*` ioctls on `/dev/tty0` (basic single-VT versions done in M17).
4. **Userland build**: cross-compile with the musl toolchain: libffi, expat, libxml2, wayland, wayland-protocols, libxkbcommon, pixman, libdrm, mesa (`-Dgallium-drivers=softpipe,llvmpipe -Dplatforms=wayland`; llvmpipe needs LLVM, so softpipe first), libinput + mtdev + libevdev, seatd, wlroots, sway (+ json-c, pcre2, pango/cairo, which can be stubbed with `-Dtray=disabled` etc.), foot (fcft, freetype, fontconfig).
5. **KDE Plasma** is last and much larger: Qt6 (qtbase + qtwayland + qtdeclarative), KF6 frameworks, KWin (needs a working libinput, logind-like DBus APIs, `/sys`), DBus, polkit-free setup. Realistically it needs sysfs, a udev-compatible device database, inotify, `/proc/self/*`, many more fs features and a lot of memory.

Prerequisite syscall count estimate: BusyBox+Bash ~170 (done); Weston/Sway ~230 (AF_UNIX, epoll, memfd, timerfd, signalfd, eventfd, `ppoll`, `recvmsg/sendmsg` with cmsg, `mmap MAP_SHARED`); KDE ~280+ (inotify, `statx`, `name_to_handle_at`, `getrandom`, `prctl`, `sched_*`, sysfs).

## Phase D: Polish
- Modular refactor (see the R0–R7 plan in HANDOFF): initcalls, driver model, `SYSCALL_DEFINE`, `register_filesystem`, a kbuild-style tree.
- CI: build all arches plus scripted QEMU smoke tests (expect-style).
- A real `init` (BusyBox init today), users and permissions, and `/etc/passwd` login.
