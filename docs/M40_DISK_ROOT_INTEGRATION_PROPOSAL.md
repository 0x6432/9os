# M40 strict disk-root integration proposal (unimplemented)

Status: host-only prototype, not an ISO builder, launcher, fixture, runtime verification, or boot approval. No existing build or workflow calls the prototype. A `READY` file alone is never evidence of success; verify the artifacts. The prototype assumes a trusted private parent: it does not confine symlinked ancestors, defend against same-user races, or provide a durable transactional marker.

## Goal and limits

Investigate an opt-in disk-root-only image with a deterministic empty initramfs and exactly one hard-coded `root=/dev/vda1 root_required=1` command line, without weakening the existing newc metadata refusal or changing legacy defaults. This can avoid trying to store full Alpine-root capability xattrs in newc; it does not verify disk metadata, file-capability execution, or even the chosen disk. Root-required is not disk authentication, containment, or a guarantee that virtual mounts transfer correctly. Existing harness compatibility must be reviewed rather than assumed.

## Requirements before implementation is treated as usable

- Separate strict and legacy output trees. Reject conflicting GNU make overrides, including output variables, cmdline and rootfs-related variables; account for command line, environment, MAKEFLAGS and recursive make behavior. Reject unsupported architecture and tool configuration rather than silently reuse output. Avoid parser-time side effects when rejecting overrides.
- A strict invocation must invalidate any previously published strict ISO before a failed build can leave it looking current; preserve evidence deliberately, use a private same-filesystem staging area and serialize concurrent builds. Clearly define what is safe to remove and avoid deleting user-supplied paths. Never infer success from timestamps or a marker alone.
- Verify exact consumed archive and Limine configuration, then inspect the final packaged ISO after all post-processing and before atomic publication. Require exactly one cmdline and the intended archive/config bytes. Validate using a reviewed trust model; path ownership, symlink ancestry, hard links and races need explicit controls. An atomic name switch alone is not proof of durability.
- Independently compare unchanged legacy defaults and output behavior. Do not route existing workflows, harnesses, or real boots to this profile until the integration is separately reviewed.

## Required host-only tests

- Pre-seed newer fake ISO, archive and config, corrupt inputs or outputs, switch between strict and legacy modes, inject failures at each build/publish phase, and confirm no stale strict ISO is presented as valid.
- Exercise every relevant variable override and invocation channel, including nonempty/empty cmdline and inherited flags. Test unexpected config tokens, multiple cmdline entries, symlinks, hard links and permission changes within the chosen trust model.
- Independently extract final simulated/real packaged bytes, check exact archive and config after post-processing, and demonstrate legacy behavior unchanged. Mock dangerous or external operations; do not dispatch CI or run a guest.

Actual fixture/host/firmware/launcher/dependency verification, capability handling, runtime containment and one-attempt diagnostic protocol remain separate release work. No boot without explicit agreement on the exact verified setup and protocol; retain 300-second boot and 780-second command deadlines and all failure/skip evidence, with no automatic retry.

## Extracted-tree check (host-only)

`scripts/disk-root-extracted-check.py` checks two exact paths in an already-extracted private tree against independently specified expected bytes/hashes. It rejects unsafe leaf/root links, hard links, special files, unexpected owner or writable permissions, and mismatched bytes. Its host tests use fixed fixtures plus modified, truncated, newer stale, linked, and permission-altered inputs.

It does not run an ISO extractor or establish provenance, ISO record uniqueness, extractor behavior, kernel integrity or final ISO contents. It ignores unrelated extracted files. It assumes a stable private tree and trusted ancestors; checks are raceable, and an ancestor of the supplied tree may be a symlink. It is not yet used by Make or any workflow. It cannot authorize a build or boot and is only a component for a future reviewed transaction.
