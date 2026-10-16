# Fast prebuilt Alpine runtime

This optional image runs **the 9os kernel** with Alpine 3.22.6 binaries. It does
not replace the source-built ports image and does not rebuild Qt, Mesa, LLVM,
musl or BusyBox. Only the kernel/image packaging is built locally.

## Build and test

Host prerequisites: Python >=3.11, GNU tar/cpio, clang/lld, xorriso, make, and the
repository's normal Limine/uACPI/QEMU setup. The image builder currently requires
an x86_64 host; aarch64/riscv64 are cross-installed and tested by PR CI, not locally.
Pillow is needed only by the host pixel checker. Alpine Sway carries a signed
`security.capability` xattr: apk2 extraction needs host `CAP_SETFCAP` (the sandbox
has it; ordinary GitHub runners do not). CI uses sudo **only for signed assembly**,
then returns cache/image ownership to the runner and restores the VM BusyBox
setuid bit before ordinary unprivileged kernel/image builds. No signature bypass,
ignored install failure, target script execution or host capability is required
inside the guest. The newc initramfs does not preserve file capability xattrs;
these headless tests do not exercise privileged DRM/seat access.

```sh
export PATH="${TOOLS_DIR:-/data/tools}/bin:$PATH"
python3 userland/alpine/build.py --arch x86_64
make -j2 ARCH=x86_64 USERLAND=alpine iso
scripts/alpine-tests.sh x86_64
```

The outputs are `userland/alpine-root-x86_64` and
`build/x86_64-alpine/9os.iso`. Ordinary `make ARCH=x86_64 iso` still uses
`userland/root-x86_64` and `build/x86_64/9os.iso`. Kernel edits require only
another `make ... USERLAND=alpine iso`; package installation need not be repeated.

The full Alpine dependency closure is substantially larger than the tailored
source image (~502 MiB installed in the current x86 package set). Tests request
2 GiB guest RAM; this is a development/compatibility lane, not an image-size
optimization. Tests use the usual passwordless root/test accounts: VM use only.

## Provenance and replay

- Minirootfs 3.22.6 archives are SHA256-pinned per architecture, including the
  native x86 bootstrap. The pins came from Alpine's release metadata over HTTPS.
- The target's SHA-pinned mini keyring verifies repository indexes and every APK. No
  `--allow-untrusted`, disabled certificate checking or target install scripts.
- Native apk cross-installs target packages with `--no-scripts --no-chown`; no
  ARM/RISC binaries are executed during installation. OpenRC, udev services,
  system-bus activation and distro post-install hooks are NOT configured.
- Keep Alpine's musl, GCC libstdc++, Qt and dependencies together. The 9os overlay
  adds boot configuration, test accounts and smoke scripts, not source-built
  shared libraries. ELF bytes remain the distribution's binaries.
- Checked-in architecture locks are replayed by default. The current x86 lock
  fixes 156 APK URLs/hashes. Where no checked-in lock exists, dependency resolution
  uses stable v3.22 repositories and requires QtDeclarative6.8.3-r0; CI archives the
  exact resulting lock. This is not a claim that an uncommitted resolver result
  is permanently frozen across future runs.
- Generated `userland/build/alpine-ARCH.lock.json` and `/etc/9os-alpine.json`
  record the entire selected APK set and hashes. Preserve the lock AND cached
  APKs for long-term reproducibility: upstream can remove superseded packages.

```sh
# Exact replay; with a populated cache this must not use the network/indexes.
python3 userland/alpine/build.py --arch x86_64 --offline
# Review a deliberate dependency refresh before replacing a checked-in lock.
python3 userland/alpine/build.py --arch x86_64 --resolve
```

The builder rejects wrong architecture/release locks, untrusted URL locations,
missing hashes, duplicate names, missing required profile packages and checksum mismatches. Existing unmarked image
directories are not overwritten. A per-architecture host lock prevents concurrent
image builders. Cache/install work stays outside git.

## What the smoke tests establish

Prebuilt DBus private sessions/replies/exit-status propagation; root and UID1000
Qt Quick offscreen software rendering; reactive QML binding changes; actual frame
swaps, item capture, PNG save and host verification of **every pixel** in the
96x64 capture. Console logs contain the PNGs. The host decoder rejects absent or
wrong-size/wrong-color captures.

The Wayland test runs the prebuilt Sway 1.10.1/wlroots 0.18.2 compositor with a
128x96 **headless pixman output** and actual Qt Quick xdg-shell clients, as root
and UID1000. It checks the mapped/visible window through Sway IPC, at least three
completed **surface-frame** callbacks per client (not unrelated display-sync
callbacks), changing QML frames, clean client/compositor exits, and every pixel of
a final green **compositor screenshot** from grim. The non-root client gets its
own 0700 runtime directory and a restricted Wayland socket, not Sway IPC access.
Host negative controls reject fake/missing/wrong-size pixels, missing callbacks,
wrong UIDs and cross-client callback borrowing. CI covers UP and x86 SMP8 boots;
non-x86 execution is CI-only.

`QT_QUICK_BACKEND=software` and `QV4_FORCE_INTERPRETER=1` deliberately avoid making
an untested GPU or QV4 JIT claim. Alpine PCRE2 may still create executable mappings
under 9os's existing W^X warning policy. That warning is not a panic and does not
mean all JIT is globally disabled in Alpine binaries.

This does NOT establish Qt Quick EGL presentation, KMS scanout, hardware acceleration,
Widgets applications, complete Alpine service compatibility or KDE/Plasma. Source
Qt Quick remains separate work. **M40 remains incomplete.** The existing
source-build CI remains the reproducible custom-port/regression lane; the new
`alpine-prebuilt-runtime` workflow provides the fast compatibility lane.

## Experimental desktop profile (not a completed desktop)

`python3 userland/alpine/build.py --arch x86_64 --profile desktop` assembles a
separate signed KDE/Plasma/KWin/Konsole/Dolphin/Mesa runtime. Its checked-in
`x86_64-desktop.json` lock and generated `alpine-desktop-x86_64.lock.json` must
not be substituted for the base profile lock. `USERLAND=alpine-desktop` uses
separate kernel/image outputs. The current installed payload is about 2.5 GiB;
prefer an ext2 disk root with the smaller base initramfs for iterative debugging.
No target Qt, Mesa, LLVM or libc build is involved. `musl-dev` supplies headers
and startup objects for compiling small ABI regression programs, not libraries.

The private-bus KWin virtual/QPainter probe and real llvmpipe OpenGL Quick
root/non-root frames passed on all three architectures in the f02fa50 desktop
CI. Its original Konsole child check was insufficient: an inherited console
could satisfy it. The updated probe requires an actual `/dev/pts/` slave and
needs new current-head CI before claiming a genuine Konsole PTY on ALL3. KWin exit15 after requested SIGTERM
is its upstream signal-handler convention, not a crash. The same CI exposed a
real Plasma abort on the unsupported PI mutex capability; subsequent kernel
fixes provide an explicit unsupported result and working ordinary-mutex fallback.

The latest x86-only desktop experiment passes real system DBus (root/non-root),
eudev trigger/settle, stock Plasma desktop/panel readiness and strict compositor
capture including the genuine packaged wallpaper. Cold initialization and image
loading are bounded, not replaced with a test background. `alpine-desktop-services`
starts the real system bus as a locked messagebus identity and real eudev; it is
not a logind/udisks/FUSE/PipeWire implementation. Physical UID1000 KMS/input and ALL3 fast/full-source validation subsequently
passed at PR17 (see integrated checkpoint below). **M40 is not complete.**

The experimental desktop additionally needs host `glib-compile-schemas` and
`update-mime-database` (Ubuntu packages `libglib2.0-bin shared-mime-info`). Since
APK install scripts remain disabled, the builder explicitly generates only the
known schema and MIME caches using these HOST tools on declarative packaged data.
It never executes a target compiler/tool or arbitrary package hook. Cache hashes
are recorded in the desktop manifest. Breeze and workspace wallpapers are
explicit signed dependencies rather than assumed optional runtime files.
Private sessions update the DBus activation environment when their Wayland
socket becomes available; otherwise activated Qt helpers incorrectly select X11.

### ABI and acceptance hardening (in progress)

Linux treats `ioctl` requests as 32-bit unsigned values, even on a 64-bit
syscall ABI. Prebuilt musl can sign-extend its public `int` request; 9os now
normalizes it at the syscall boundary instead of incorrectly rejecting real
`TIOCGPTN` (and other read-direction requests). The same-source Linux oracle
and x86 guest, root and UID1000, pass all 30 checks, including garbage upper
bits, invalid pointers, PTY raw I/O and window-size round trips.

Plasma screenshot readiness now polls actual grim PPM pixels, requiring two
consecutive rendered wallpaper/panel frames, rather than assuming a fixed
sleep finishes asynchronous image loading. The independent strict PNG checker
and its black-background negative controls remain mandatory and unchanged.
Physical KMS diagnostics now require wlroots' actual `drm,libinput` backends
and an enumerated keyboard. A real PTY and rendered physical display alone
do NOT prove that keyboard input reaches Konsole; that end-to-end test remains
a separate gate, now passed at PR17. M40 is NOT complete.

Local x86 physical KMS input now passes the diagnostic11/11 gate: real QEMU
key events enter Konsole on `/dev/pts/0`, create the expected file, and appear
in a visually reviewed1280x800 physical screenshot. This is Sway DRM scanout
with nested KWin/QPainter and stock Plasma, not direct KWin DRM or accelerated
GPU rendering. The permanent gate additionally requires UID1000: a genuine
root `seatd -g video` broker grants device access to the existing video group;
KWin, Plasma and Konsole run as the normal VM user. That stronger test and
ALL3 validation passed at PR17; no complete-M40 claim.

At75e3d01, fast CI38817891542 passes all three architectures: base19/19,
UP8/8 (x86SMP8 8/8), desktop18/18 and physical UID1000 KMS/keyboard11/11.
These updated results include the strengthened genuine-PTY check, real stock
wallpaper/panel captures and typed `id -u == 1000` proof. Full source-regression
CI38817891502 also passed ALL3: main115/115, disk20+6, clean fscks/shutdown,
audio2+2, UP9 and MLFQ12 (x86SMP8 10). PR17 was reviewed and merged at
1d2a11d8a823e0601968704994ac8aaa24a7f4bc, retaining its branch.
One local normal-user repeat timed out during Plasma initialization; follow-up
work removes repeated heavyweight Qt bus-query processes while preserving the
same deadline/acceptance, and isolates the normal user's bus environment.

### Real Dolphin file-model follow-up (not yet integrated)

The normal user's genuine Dolphin window is queried through its packaged
AT-SPI accessibility bridge, using a tiny `desktopa11y` C consumer and the
existing signed libatspi binary. No Qt, KDE or accessibility library is rebuilt.
The observer matches the real application's PID and exact directory List
role/name/description, then enumerates every direct ListItem model row. It
prunes unrelated menus and fails on remote errors, oversized/cyclic trees,
missing required names or present forbidden names. Seventeen host mock controls
validate rejection logic only; they are explicitly NOT desktop evidence.

An isolated x86 UID1000 KWin/QPainter session passed the real before/after
model check: before.txt present initially, after.txt/new.txt absent; after
rename/create, after.txt/new.txt present and before.txt absent. Dolphin's real
window is active and exits cleanly through its public DBus quit method. The first combined physical KMS run passed16/16 at7857039, including the
actual UID1000 keyboard/Konsole PTY, genuine UDisks disk/partition/root
filesystem enumeration, both model states, a visually reviewed QEMU screenshot
showing after.txt/new.txt, clean application exit/shutdown and lockdep0. A
second cold-boot repeat and broader updated suites are in progress. Non-x86
validation of this follow-up remains pending PR CI. `isItemVisibleInAnyView` is not used
as a file-existence oracle: upstream intentionally checks the containing
folder even when a child is nonexistent or not loaded.

### Mountinfo and GLib epoll compatibility

Real packaged UDisks exposed two kernel gaps rather than a KDE build problem:
missing /proc/self/mountinfo caused its fatal startup error, then repeatedly
delivered procfs EPOLLIN|EPOLLET caused GLib's initial-event drain to spin.
Mount snapshots now report actual mount topology, IDs, paths, devices, rw/ro
flags and filesystem sources without fixed-buffer truncation. Mount changes
wake poll with Linux's per-open POLLPRI|POLLERR acknowledgment behavior. A
fresh tmpfs /run prevents stale service sockets/PIDs across disk-root reboots.

Epoll now latches permanently readable procfs bits for ET registrations and
uses the same eligibility for nested peeks. This is NOT complete dynamic-input
EPOLLET parity: that historical level approximation still needs producer-side
edge tracking. Mountinfo describes the current single global namespace, not
bind mounts, private namespaces or a chroot-relative view. The same C test
runs on Linux and 9os, including real mount mutations in an isolated host
namespace; guest root and UID1000 checks plus source disk reboot tests pass.
UDisks acceptance is read-only enumeration, not privileged mounting, formatting
or authorization. Direct KWin DRM, hardware acceleration, logind and the
remaining source Qt Quick rendering scope are not claimed. M40 remains open.

### Nested presentation-feedback compatibility (validation in progress)

The packaged KWin6.3.6 / wlroots0.18.2 nested stack can stop rendering after
KWin commits a buffer with no damage and requests `wp_presentation` feedback.
A real failing 9os protocol trace has an outstanding feedback request, idle
compositors and Qt frame-callback timeouts. The same unmodified packaged stack
also failed the unchanged150-second wallpaper/panel gate on REAL HOST LINUX
(headless Sway); it is not evidence of a new 9os syscall failure. An earlier
Linux run reached the actual Dolphin model but its immediate outer capture
still showed only Konsole; that immediate image alone is not a full35-second
presentation-deadline negative control.

The Plasma harness now explicitly defaults to `ALPINE_PLASMA_PRESENTATION=callback`.
`wayland-presentation-proxy` hides only the optional presentation-time registry
advertisement on KWin's OUTER connection, selecting KWin's existing standard
`wl_surface.frame` fallback. It does not patch packaged libraries/QML, change
images, alter the inner KWin application connection or weaken any readiness,
model, physical typing, fresh-image/OCR or cleanup gate. Native protocol mode
remains available as `ALPINE_PLASMA_PRESENTATION=native` for comparisons.
This is a userland compatibility workaround, NOT a claim that the upstream
presentation implementation or all kernel ABIs are fixed.

The proxy preserves ordered Wayland byte messages and SCM_RIGHTS, handles
fragmentation/short writes, closes transferred rights and propagates signals
and child status. It is NOT a security boundary. `--passthrough` is a transport
control. Same-source `python3 /usr/share/9os/wayland-proxy-selftest.py` runs16
wire/FD/process controls; these are NOT GUI proof. Root and UID1000 controls
are added to the desktop suite (28 gates). A private Linux protocol-filter
control passed actual wallpaper/Konsole/Dolphin and the unchanged independent
fresh-image OCR immediately, but 9os and non-x86 compatibility validation is
still pending. M40 remains open.
