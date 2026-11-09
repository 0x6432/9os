# Opt-in M40 input-chain diagnostics

`ALPINE_INPUT_CHAIN_DIAGNOSTICS=1 scripts/alpine-kms-tests.sh x86_64` is a
diagnostic-only mode. The default remains off. It passes the existing outer
Wayland, inner libwayland, and Qt platform-submission observers through the
root-to-user probe boundary, starts the existing non-grabbing evdev collector
only after both genuine readiness markers and the pre-type screenshot, and
arranges to retain their streams in one unique host log for that QEMU attempt.
The before/after/Dolphin screenshots use the same attempt-specific basename.
Streams not returned before a panic/VM exit make the attempt incomplete, as
described below.

The raw collector runs for a fixed 45 seconds. Its helper publishes the status
file only after the bounded collector exits; it has no early-stop control.
After the pre-type screenshot, the diagnostic path makes at most 10 one-second
poll waits for exactly one valid `INPUT_CAPTURE_READY` record and a matching
set of opened-device records. Each inspection snapshots at most 64 KiB of the
growing collector log, checks inspector exit status and output, and checks the
collector's atomic completion-status path before accepting readiness. This is
a bounded-poll/processing contract, **not a strict 10-second wall-clock
guarantee**: scheduling and per-inspection work can add elapsed time. The
existing QEMU per-command timeout remains unchanged. The host-side key sender
independently requires exactly one readiness witness followed by exactly one
successful `__RC<n>=0` completion for the helper's actual command index in the
current QEMU attempt log. This sender-side guard is necessary because
`qemu-test.py` continues to later commands after a failed guest command.
Missing, malformed, failed, duplicate, or out-of-order collector startup
therefore cannot authorize the original physical typing command. The
post-gate readback polls at most 600 times with 0.1-second sleeps for the
atomic status, then includes the completed raw log and status in the host log.
This polling count is not a strict wall-clock guarantee.

If a kernel panic or VM exit occurs before the final guest readback, the raw
collector and other guest streams may not be returned to the host log. Such an
attempt is incomplete; this workflow does not promise to retain every guest
stream after a panic/exit, and the checker must fail it as incomplete. The
collector has no stop/readback shortcut that would make this final step
unconditional.

`scripts/input-chain-check.py LOG` requires exactly one mode witness, collector
start/readiness, raw capture, outer trace, inner trace, and Qt trace in one
orchestrated attempt log. It checks the exact fixed key edges at each boundary,
validates raw readiness/device-count consistency and raw ABI numeric ranges,
rejects capture errors/truncation/incomplete records, and requires zero Qt
autorepeat. The outer checker confirms the outer-compositor-to-KWin wire
sequence only; it cannot prove forwarding beyond the proxy. The inner checker
is libwayland pre-listener dispatch; the Qt hook is platform submission. None
proves terminal consumption or explains a later text discrepancy.

The unique log name and checked section boundaries make this an
**orchestrated one-run artifact only**. They do not authenticate the producer
or prove independent same-boot provenance, and they do not detect a
deliberately spliced log. Do not describe a checker pass as cryptographic or
source-authenticated evidence.

The original QMP key sequence, pre-existing readiness evidence, deadlines,
and acceptance commands are unchanged. Diagnostic mode adds a finite-poll
devices-open gate before that same key command; it does not substitute a new
key sequence or readiness threshold. The orchestrated controller result is
recorded separately from the diagnostic checker result: diagnostic success
cannot rescue a failed controller. The controller result includes diagnostic
steps and is not a separately computed original-only acceptance result. Tracing
can perturb timing, and even a complete
trace-enabled pass is not an unchanged-timing cold-reliability sample or a
cause/fix claim. Keep every failed attempt.

The option fails before disk creation if the existing raw collector or Qt
observer is not already installed in the selected desktop root. After building
the small consumers with `scripts/alpine-build-tests.sh x86_64`, run
`scripts/alpine-desktop-install-tests.sh x86_64` to copy them, including the
raw collector, without packing a disk or starting a VM. This is not fixture
verification: inspect the resulting disk and provenance separately before any
boot. No Qt or other desktop library is built by the installer.

## Experimental frozen launcher (review required)

This is a draft host-side launcher, **not approval to boot**. Agree on a single
attempt and independently review the launcher, full fixture provenance and
security metadata first. A SHA-256 match alone does not validate the disk's
ownership, capabilities, executable modes, initramfs limitations, embedded
kernel command line, host QEMU firmware/libraries, or guest behavior. Hash
checks have time-of-check/time-of-use limitations; protect the fixture and
launcher from modification throughout. The frozen path attempts process-group cleanup and final hash checks on Python
exceptions and interruption, but its failure handling and host environment
need independent review. Do not use this path until those gates are satisfied. Record unresolved failures and skips, and never substitute inert
host controls for guest evidence.

The manifest has this exact JSON shape, with absolute paths and reviewed,
lowercase SHA-256 digests of the actual regular files:

```json
{
  "version": 1,
  "qemu": {"path": "/absolute/path/to/qemu-system-x86_64", "sha256": "<64 lowercase hex digits>"},
  "iso": {"path": "/absolute/path/to/9os.iso", "sha256": "<64 lowercase hex digits>"},
  "golden_disk": {"path": "/absolute/path/to/golden.raw", "sha256": "<64 lowercase hex digits>"}
}
```

After approval, copy the read-only golden disk to a separate disposable regular
file, retaining the original. Verify its digest matches before the attempt.
Set `M40_FROZEN_MANIFEST` to the manifest path, `M40_FROZEN_DISK` to the
copy, and `ALPINE_INPUT_CHAIN_DIAGNOSTICS=1`, then invoke
`scripts/alpine-kms-tests.sh x86_64`. It uses the frozen direct QEMU argv,
not `make` or `mkdisk.sh`. It rejects alternate architecture, memory, CPU
count, cmdline, QEMU_EXTRA, network forwarding, additional disks and changed 300s boot/780s per-command
deadlines in this mode. Host helpers run in a separate process group, with partial
output retained on timeout and group cleanup attempted on exit or interruption.
Descendants that deliberately create new sessions are not contained. Frozen mode requires a new, exclusively created host log. Config, manifest and
log refusals report a preflight failure and explicitly mark the parsed requested
steps skipped. If QEMU cannot start, the launcher reports that failure and skips
on stdout and in the fresh host log. A refusal before log creation has no log.
This does not guarantee a complete summary for arbitrary host exceptions.
Frozen mode refuses nondefault Qt trace, D-Bus validation and terminal-query
settings and requires Tesseract with English data. It uses a fresh private
output directory, never appends checker results to the controller log, and
prints the orchestrated-controller and checker return codes separately on
stdout. Capture stdout independently. Screenshots refuse existing output paths
and use private intermediate files. This freshness rule also applies outside
frozen mode: each wrapper run chooses fresh screenshot names rather than
silently deleting earlier screenshots. These pathname checks do not prevent
same-UID interference, bind firmware, host libraries or scripts, establish host
isolation, or prevent initramfs userspace from running before the root gate.
The direct frozen controller accepts caller-supplied host shell commands: it is
not a host-command allowlist or a security boundary. Inert test-only guard
rejections are not production restrictions. Frozen runs stop on a failed host or
guest step, then terminate without issuing another guest command; unattempted
steps are skipped. All modes stop on a failed pre-type screenshot. This cannot contain same-UID races or escaped descendants.
The cmdline argument is a guard, not injection: independently inspect
the ISO to verify its embedded `root=/dev/vda1` before launching. After the
shell starts, frozen mode also requires exactly one `/` mount from
`/dev/vda1` of type `ext2`, and checks the two corrected helper owners and
modes. It additionally requires a regular, non-symlink `/etc/shadow` with
mode 0640 and root:shadow (0:42), including after early init. If this preflight fails, all
requested diagnostic steps are skipped and the run fails. This post-boot
check does not replace fixture review or a complete security audit.

Capture complete host stdout/stderr, the attempt-specific log and screenshots,
checker result, pre/post hashes, QEMU version and execution environment. The
orchestrated controller and diagnostic checker results remain separate; neither
is an independently computed original-only acceptance result. After the
process and its children stop, retain the disposable disk and recheck the
manifest, executable, ISO and golden hashes. A changed artifact, missing stream,
failed command, timeout or skip remains visible and blocks acceptance. Do not
retry or change the fixture silently.

## Opt-in x86 disk group correction (review required)

For a **new disposable image only**, `mkdisk.sh` supports the explicit setting
`ALPINE_DESKTOP_OWNER_POLICY=x86_64` and requires
`ALPINE_DESKTOP_APK_CACHE` to point to an offline cache containing the pinned
x86 Alpine 3.22.6 mini and the two APKs named in
`scripts/alpine-desktop-owner-repair.py`. The helper verifies the exact desktop
lock, pinned bootstrap and trust keys, both APK checksums and signatures, signed
archive ownership/modes and installed file bytes, plus the pinned generated
shadow-file digest. It checks the disk's group
identities, file types, inode links, original owners and modes before changing
only the three file groups (DBus helper 81, unix_chkpwd 42, and the
known generated `/etc/shadow` fixture 42), and rechecks all three. The shadow
content must exactly match the pinned mini plus repository account overlay;
changed accounts are refused and need separate review.
Any refusal aborts packing before the partition is copied into the image.
Do not use a failed or partially created image; keep the error visible.

This narrowly corrects three observed discrepancies. It does not approve setuid
policy, check all overlay/generated-file metadata, implement file capabilities,
fix newc ownership/xattr limitations, or verify other architectures. The repair
has time-of-check races and assumes exclusive control of an unmounted disposable
image and its inputs. Independent review and full fixture verification remain
mandatory before any boot; this option is off by default.
