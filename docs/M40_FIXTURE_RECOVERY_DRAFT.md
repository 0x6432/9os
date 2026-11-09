# M40 fixture recovery — host-only status

**No fixture is approved for boot.** The candidate files identified in the
[prior review packet](https://github.com/0x6432/9os/pull/57#issuecomment-6741568192)
were built from `bb03832b58db35eb1f28018dac6a61ecad3d3515`, not this draft's
source revision. The packet contains hashes and inspection logs, not the ISO,
disk or QEMU executable themselves.

## Historical recovery targets

| File | Historical SHA-256 |
| --- | --- |
| Frozen manifest | `9e53145789ac54232eca2905560d1e605d592925028d45d216c4c042b17ae8be` |
| QEMU executable | `a9aca6627cbb9ff3ae73fd2de485f9584ec5dc185338403ac17c9c7b5de55865` |
| ISO | `61c462977a0d58def9b165540eacf6b3b93efba16bdb65e744eb27645b9e13d8` |
| Golden raw disk | `dfbfc729249e159f2bd2207d9ca931a254d6665a6e0912b7491e2fd2057bb74f` |

These hashes identify prior bytes; they do not authenticate provenance, verify
metadata, or make the files safe. Paths, manifest, firmware, QEMU libraries and
launcher must also be reviewed. Rebuilding or changing any input makes a new
candidate; identical source and package locks do not guarantee identical bytes.
The earlier disk was 4 GiB; the ordinary current wrapper's 3584 MiB packing
option is not an exact substitute.

A bounded check of the accessible workspace on 2026-11-09 found none of the
prior ISO, raw disk, manifest, APK cache, unpacked desktop root or local tool
cache. The frozen wrapper also checks for host-side desktop and input-chain
helpers and the Qt observer in the unpacked desktop root. The installed QEMU
11.0.0 hash was `606dce839738b12831c6486c9888d356e214eece549e76a0760d56ce06226abd`;
it is not the prior reviewed QEMU binary. Environment state can change; repeat
checks against the actual host, not this observation.

## Shortest recovery or rebuild path

1. Recover the former `/data/m40-shadow`, `/data/9os` and `/data/tools` inputs
   or a trusted copy: manifest, ISO, raw disk, QEMU and dependencies, APK cache,
   unpacked roots and inspection logs. Verify exact bytes and provenance, then
   re-review all current host, launcher, fixture and security properties. The
   former manifest's absolute paths and hash binding must still be valid; changes
   require a new manifest and review.
2. If recovery is impossible, prepare a *new* candidate in a disposable build
   workspace from the pinned mini rootfs, signed package locks, compiler,
   Limine and uACPI inputs. The prior investigation counted 595 distinct APK
   URLs across the two locks. Obtain and verify the exact offline inputs and
   signing-key provenance before assembly. Restore the base and desktop roots,
   build and install the diagnostic consumers, build the base-Alpine ISO and
   create a new desktop disk. The narrow x86 owner repair and all host tools,
   modes, xattrs and build assumptions require review. Do not invoke the
   repository Makefile as a read-only inspection: it writes even during parsing.
3. Independently inspect the embedded command line, ISO/initramfs, disk
   partitions and ext2, security metadata and capabilities, helper bytes,
   provenance, firmware and libraries. Hash all inputs, create a fresh manifest
   and a separately verified disposable disk copy. A matching hash is necessary,
   not sufficient. A rebuilt candidate must be reviewed from scratch.

The previous repair addressed only three group metadata discrepancies. The
previous initramfs flattened archive ownership to `0:0` and lost capability
xattrs. With the existing `root=/dev/vda1` contract, failed disk mounting can
fall back to initramfs userspace before the post-shell root check. The proposed
`root_required=1` safeguard is not integrated in that frozen ISO or contract;
it would require a new ISO, launcher contract and review. Even a successful
root mount may retain initramfs-derived paths. These are outstanding risks,
not solved by offline QEMU or a recreated disk.

Do not boot, run CI, retry, or merge on the strength of these notes. The exact
host, fixture, launcher and one-attempt protocol remain subject to verification
and explicit agreement. See `M40_OFFLINE_PROTOCOL_DRAFT.md` and the prior
[review evidence](https://github.com/0x6432/9os/pull/58#issuecomment-6743466461).

## Subsequent local candidate and archive-packing draft

An unapproved candidate was later rebuilt locally; exact hashes and narrow
checks are recorded in [PR #61](https://github.com/0x6432/9os/pull/61#issuecomment-6745276795).
Its original newc archive contained 365 regular multi-link records, including
254 zero-payload timezone entries. The kernel loader does not reconstruct newc
hardlinks, so these may unpack as empty files. This is an offline inference,
not a guest result.

The proposed `initramfs-pack.sh` stages independent file copies before packing.
This preserves each file's bytes in the archive but intentionally does not
implement hardlink identity; newc still cannot carry capability xattrs. Host
controls and an exact archive comparison are not a substitute for review,
actual kernel extraction, guest behavior, or fixture security inspection.
Rebuilding changes the ISO and requires a new manifest and exact-input review.
