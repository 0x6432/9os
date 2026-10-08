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

Done in M16: COW fork, `MAP_SHARED` (anonymous + tmpfs/memfd), AF_UNIX with `SCM_RIGHTS`, `socketpair`, `epoll`, `eventfd`, `timerfd`, `signalfd`, `memfd_create`. Done in M17: ptys (`/dev/ptmx` + `/dev/pts`), evdev input (virtio-input, PS/2) and VT/KD ioctls. Done in M19: dynamic linking (ld-musl, dlopen) and inotify. Done in M20: page cache for private mappings, ports framework, Lua/SQLite/libffi/expat/libwayland. Done in M21: wayland-protocols, pixman, libxkbcommon, libdrm and the `wlkms` KMS Wayland compositor with xdg-shell clients. Done in M22: `wlterm` Wayland terminal. Still open below: storage, networking, the rest of the graphics userland.

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

## Phase E: From "Linux-compatible toy" to "small real kernel" (M23–M40)

Gaps found when comparing 9os with Linux, turned into milestones that are possible and worth it for
a hobby kernel. Order = payoff first (each one unblocks the next ones). Out of scope on purpose:
NUMA, cgroups v2 controllers, eBPF, live patching, hundreds of real-hardware drivers.

| # | Milestone | Content | Unblocks |
|---|-----------|---------|----------|
| M23 ✅ | **CI boot tests** | QEMU boot of every arch in GitHub Actions, run the in-tree tests (libctest, cowtest, ipctest, ptytest, dyntest, inotifytest, mapprivtest, wltest, drmdemo), fail the build on regressions; release only if green | safe refactoring |
| M24 ✅ | **Fine-grained locking** | Replace the big kernel lock: per-subsystem spinlocks/mutexes (sched, pmm, slab, mm per process, VFS inode/dentry, fd table, tty, drivers), sleeping mutexes, lock-order rules, a debug lock checker | real SMP scaling |
| M25 ✅ | **Per-CPU scheduling** | Per-CPU run queues + load balancing/work stealing, `sched_setaffinity`, `nice`, SCHED_FIFO/RR, per-CPU slab/pmm caches, CPU time accounting (`times`, `getrusage`, `/proc/<pid>/stat`), tickless idle | top/htop, compositor latency |
| M26 ✅ | **VMM v2** | VMA tree (augmented RB/maple-like), reverse mapping (file pages via i_mmap; anon rmap deferred until there is swap), page refcount/mapcount, LRU lists, page-cache reclaim, OOM killer, `mremap`, `madvise(DONTNEED/FREE)`, `mlock`, `msync`, stack guard gaps, 2 MiB pages for the direct map | big programs (Mesa, Qt), stability under memory pressure |
| M27 ✅ | **Hardening** | SMEP/SMAP (x86), PAN/PXN (aarch64), SUM discipline (riscv), `copy_*_user` fixups via exception tables, stack canaries, ASLR (mmap/stack/PIE base), W^X checks | robustness |
| M28 ✅ | **Interrupt-driven I/O** | PLIC (riscv) and GICv2/v3 (aarch64) for virtio + UART input, MSI-X on x86, threaded IRQ handlers; drop the polling kthreads | lower latency, less CPU |
| M29 ✅ | **Block layer + virtio-blk** | bio/request queue, buffer cache, partition table (GPT/MBR), virtio-blk (PCI; mmio dropped — every target machine has PCIe) | storage |
| M30 ✅ | **ext2 + unified page cache** | ext2 read/write, page cache for every fs with write-back, `fsync`, root on disk (`root=/dev/vda1`), dentry/inode caches with negative entries | persistence, git, package installs |
| M31 ✅ | **Users and permissions** | uid/gid checks in VFS, `setuid` exec, capabilities subset, `/etc/passwd` login (`getty` + `login`), umask | multi-user, sane daemons |
| M32 ✅ | **Networking** | virtio-net, own IPv4/TCP/UDP/ARP/ICMP stack (DHCP via udhcpc), `AF_INET` sockets, loopback, `/etc/resolv.conf` | ping, curl, wget, ssh |
| M33 | **ptrace + POSIX timers** | `ptrace` (gdb, strace), `timer_create`, robust futexes, `clone3` extras, `waitid`, file locks (`flock`, `fcntl` locks), xattrs on tmpfs/ext2 | debugging, toolkits |
| M34 | **sysfs + uevents** | `/sys/class`, `/sys/devices`, `/sys/dev/char`, netlink `NETLINK_KOBJECT_UEVENT`, a static udev db shim | libinput, wlroots, KDE |
| M35 | **Input/seat stack** | xkeyboard-config, libevdev, mtdev, libinput, seatd ports | wlroots |
| M36 | **DRM atomic + PRIME** | properties, atomic commits, dma-buf/PRIME fds, zero-copy virtio-gpu scanout (resource per dumb buffer) | wlroots/Sway |
| M37 | **Sway + foot** | wlroots, tinywl, Sway, freetype/fontconfig/fcft, foot | a real desktop |
| M38 | **Mesa softpipe → llvmpipe** | EGL/GLES2/GBM in software | GL apps, Qt Quick |
| M39 | **Audio** | virtio-snd or Intel HDA, minimal ALSA PCM ABI | sound |
| M40 | **KDE groundwork** | DBus, Qt6 (qtbase, qtwayland, qtdeclarative), namespaces/`unshare` subset as needed, more `/proc` | KDE Plasma attempts |

Smaller ports to slot in when the kernel side allows it: make, tcc, ncurses → nano/vim/htop, Doom (fbdev/evdev), Python, git (after M30), curl/OpenSSL (after M32).

## Phase D: Polish
- Modular refactor (see the R0–R7 plan in HANDOFF): initcalls, driver model, `SYSCALL_DEFINE`, `register_filesystem`, a kbuild-style tree.
- CI: build all arches plus scripted QEMU smoke tests (expect-style).
- A real `init` (BusyBox init today), users and permissions, and `/etc/passwd` login.
