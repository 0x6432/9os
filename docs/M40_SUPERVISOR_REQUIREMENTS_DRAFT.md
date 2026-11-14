# M40 offline supervisor — draft requirements

**Design only. The supervisor below is not implemented or approved.** The
reservation primitive is not a launch gate. No guest boot, CI run or merge is
permitted by this document.

## Authorization and reservation

An independent, durable authorization record must identify the exact approved
contract and its one permitted invocation. The supervisor must pin one stable,
pre-provisioned ledger identity on reviewed persistent storage, outside the
child's mount view, with exclusive control of its directory and ancestors. It
must verify the contract digest and independently confirm the user's agreement.
It then reserves before any substantive preflight or launch; a preflight refusal
consumes the attempt. Any failure, missing or partial record, crash, uncertain
storage durability or rollback requires manual reconciliation against the
external authorization record, not an automatic retry. Creating a new directory
or bypassing the reservation is not a retry mechanism.

The existing `m40-attempt-ledger.py` only reserves within a chosen directory and
cannot verify authorization, stop another caller, enforce one launch after
success, prevent directory replacement or prove persistent storage. It is a
building block, not this supervisor.

## Child-before-exec and containment

A trusted parent stays outside a new dedicated cgroup. A single-threaded trusted
bootstrap child waits on a private pipe before executing or forking anything
untrusted. EOF, an invalid token, or any setup error makes it exit without exec.
The parent attaches that blocked child to the cgroup and verifies the same live
process and its membership, using a pidfd and identity checks where supported.
Only then may a reviewed bootstrap set up isolation, independently expose its
state for checking, and await a separate release. Do not start an untrusted
process first and move it afterward.

Before release, independently verify the **actual** child has a separate
network namespace with no external route or interfaces, the reviewed mount and
IPC view, an allowlisted environment and file descriptors, no unrelated host
sockets, devices, credentials or broad host process information, and no way to
write cgroup controls. Use a separate execution UID where feasible; clear
supplementary groups, all capability sets and bounding set, and set
`no_new_privs`. Review restrictions on privilege regain, namespace changes,
tracing and cgroup migration. Read-only binds do not protect against a host
writer changing the underlying source. Prevent same-UID and socket interference
and independently verify the whole arrangement immediately before release.

Execute only the agreed, committed frozen wrapper and explicit offline settings,
with fixed reviewed paths and environment. The existing direct controller
accepts arbitrary `host:` shell commands, so it is not an allowlist or a security
boundary. `-nic none` disables the requested initial guest NIC; it does not
establish the integrity of QEMU, its monitor or the host.

## Cleanup and evidence

A separate trusted custodian must handle supervisor death and surviving attempt
cgroups. On completion, error or interruption, kill the dedicated cgroup, reap
the direct child, and require `cgroup.events` to report `populated 0`. Missing
controls, failure to kill, uninterruptible tasks or ambiguous state are
incomplete and require quarantine. Do not assert cleanup or final hashes until
containment is verified. A cgroup test with an inert child does not prove a
complete supervisor or guard against host/kernel failure.

Capture stdout/stderr outside child-writable evidence storage, separately from
the controller and checker results, with reviewed storage limits; overflow,
write, flush or sync errors fail visibly. Protect sockets and outputs. After
verified cleanup, retain and hash the disposable disk and all evidence, and
recheck reviewed input and dependency hashes. Guest output and screenshots are
not authenticated evidence against a hostile guest or emulator. The current
controller's internal post-run hashes and checker run occur before an external
supervisor can verify cgroup emptiness; treat them as provisional, and perform
final checks after containment. Changing that behavior needs separate review.

Keep the existing 300-second boot and 780-second per-command timeouts and every
readiness, fail and skip condition. They are not an end-to-end hard deadline;
any new global timeout or resource limit that could alter the attempt requires
prior user agreement. No automatic retry.

## Required inert controls before use

Test simultaneous reservation, all write/sync errors, partial records and
process death; test no exec on parent death, EOF, attach failure and every
verification failure. Use harmless children that fork, create new sessions,
retain pipes and attempt migration, and verify cgroup-wide cleanup and recovery
after supervisor death. Independently inspect actual child UID, groups,
capabilities, namespaces, mounts, file descriptors, environment, routes and
socket access. Exercise output exhaustion and missing evidence. These are
host-only controls, not guest, isolation or security signoff.

This VM remains the chosen host but is not independently isolated. Kernel,
emulator, privileged-host, same-UID, hardware and rollback threats remain. A
verified supervisor, exact host, launcher, fixture and agreed diagnostic protocol
are still prerequisites to any real boot.
