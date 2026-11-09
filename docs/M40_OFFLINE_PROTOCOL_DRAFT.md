# Draft: one offline M40 diagnostic attempt

**Not approval to boot or merge.** This is a proposed contract for discussion,
not a verified fixture, host, launcher, or acceptance result. Selecting the
current VM and no guest Internet does not by itself approve this protocol or
accept the remaining release risks.

## Configuration

The opt-in frozen wrapper option `M40_FROZEN_OFFLINE=1` passes
`--frozen-offline` to the controller. The frozen QEMU argv replaces the user
network backend and virtio NIC with `-nic none`, which requests no initial
network devices. `FROZEN_NETWORK_MODE=no-guest-nic` in stdout and the attempt
log records the selected initial configuration, not independent proof that the
guest cannot access a network. The old frozen mode is unchanged unless this
option is set. Direct QMP/monitor access and host commands can change or bypass
this configuration: protect the sockets and trust and review the whole launcher
and helper chain. Frozen mode also refuses workspace paths containing shell
metacharacters or spaces because host helper commands interpolate those paths.
This narrow check does not prevent same-UID races or make arbitrary host commands
safe. This option does not isolate the host.

Use a fresh network namespace with no external interfaces or routes, drop all
capabilities and set `no_new_privs`, clear environment and inherited file
descriptors, and mask unrelated files, process information and credentials.
Independently verify these properties *for the actual execution process* before
any boot, and keep the process under a separately reviewed supervisor. Its pre-exec gate must start the
blocked child in a dedicated cgroup before allowing it to execute, provide an
allowlisted environment and only intended file descriptors, mount only reviewed
inputs and writable outputs, and hide cgroup controls from the child. Afterward,
it must kill the entire cgroup and verify `cgroup.events` reports `populated 0`
before hashing; fail closed if containment or verification is unavailable. A
cgroup mechanism tested with a harmless child is not a review of this supervisor.
Namespace and capability controls do not establish independently isolated hardware, guard
against same-UID interference or kernel/emulator vulnerabilities, or prevent
access through inherited sockets, Unix sockets, or exposed files. The current
VM is not an independently isolated host. A host-only sandbox experiment is not
verification of this launch arrangement.

## Prerequisites, before requesting the attempt

- Review the exact committed launcher, all host scripts, QEMU, firmware and
  dependencies. Record their versions and hashes; keep inputs immutable. Verify
  a clean workspace and an isolated, credential-free host execution environment.
- Supply and inspect the exact ISO and golden image: provenance, embedded kernel
  command line, security metadata, executable modes, capabilities, ownership,
  initramfs behavior and relevant root contents. Confirm diagnostic helpers and
  OCR language data. A hash alone is not a security review. The current strict
  root proposal is **not** enabled in the existing frozen launcher or ISO.
- Create and independently verify the frozen manifest and a separate disposable
  writable disk copy. Protect the original and verify all pre-run hashes. Resolve
  the known fixture, process-containment and launcher blockers, or obtain the
  user's explicit informed risk decision where appropriate. Re-check exact files
  and isolation immediately before launch.

## Proposed single attempt, only after explicit agreement

Run exactly one `scripts/alpine-kms-tests.sh x86_64` invocation with
`M40_FROZEN_MANIFEST`, `M40_FROZEN_DISK`,
`ALPINE_INPUT_CHAIN_DIAGNOSTICS=1`, and `M40_FROZEN_OFFLINE=1`, under the
verified offline, reduced-privilege supervisor. The selected frozen configuration
retains 4 virtual CPUs, 3072 MiB, the 300-second boot timeout, the 780-second
per-command timeout, existing readiness checks, screenshots, key sequence and
fail/skip behavior. These are not a hard end-to-end wall-clock deadline; a
single-attempt ledger and complete cleanup are not currently enforced. The
proposed supervisor must atomically create and durably retain an exclusive
attempt ledger before the invocation; even a preflight refusal consumes that
attempt. A further invocation requires new user agreement. It must confirm all
descendants are stopped before post-run hashing; the existing process-group
cleanup cannot guarantee this.

`scripts/m40-attempt-ledger.py` is an isolated host-tested reservation primitive,
not a supervisor, launch gate, or approval. It reserves once per stable, trusted,
pre-provisioned directory, not once globally or per contract. It writes the
independently supplied approved-contract digest and syncs the file and directory
ancestry, but cannot validate agreement, prevent bypass or repeated execution,
or establish that storage is persistent or cannot roll back. The supervisor
must pin that directory and its ancestry, control all launch paths, bind the
contract and limit one successful reservation to one launch. A crash or I/O
error before confirmed durability requires external durable authorization records
and manual reconciliation; absence of a marker is not permission to retry. Same-
UID replacement, deletion, snapshots and volatile filesystems can defeat the
primitive. Complete containment, launch integration and recovery are not yet
implemented. Software emulation and removal of the guest NIC can change timing or behavior; the result
is not an unchanged-timing reliability sample.

Record complete host stdout/stderr, mode, controller and checker results, fresh
log, screenshots, skipped and failed steps, environment and pre/post hashes.
Retain the disposable disk; verify fixture and launcher hashes again after
processes stop. Missing evidence, failed commands, timeouts, changes or skips
remain failures or incompletes. Do not retry or silently change the fixture,
protocol, deadlines, or checks. No host-only or inert test is guest evidence.
