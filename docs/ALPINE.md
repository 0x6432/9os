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
not a logind/udisks/FUSE/PipeWire implementation. Physical KMS/input and updated
three-architecture validation remain pending. **M40 is not complete.**

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
a separate gate. M40 is NOT complete.

Local x86 physical KMS input now passes the diagnostic11/11 gate: real QEMU
key events enter Konsole on `/dev/pts/0`, create the expected file, and appear
in a visually reviewed1280x800 physical screenshot. This is Sway DRM scanout
with nested KWin/QPainter and stock Plasma, not direct KWin DRM or accelerated
GPU rendering. The permanent gate additionally requires UID1000: a genuine
root `seatd -g video` broker grants device access to the existing video group;
KWin, Plasma and Konsole run as the normal VM user. That stronger test and
updated ALL3 validation are still pending; no complete-M40 claim.
