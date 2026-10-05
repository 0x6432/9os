# 9os

A 64-bit hobby operating system in C23. Boots with [Limine](https://github.com/limine-bootloader/limine),
uses [uACPI](https://github.com/uACPI/uACPI) for ACPI and targets the Linux syscall ABI so that
static [musl](https://musl.libc.org) programs — BusyBox and Bash — run unmodified.

Targets: **x86_64** (primary), riscv64 and aarch64 (planned).

## Building

Requirements: clang (≥15; C23 mode is used when available, `-std=c2x` otherwise), ld.lld,
xorriso, cpio, make, git, and QEMU for testing.

```sh
./scripts/fetch-deps.sh     # Limine binaries + uACPI
make iso                    # build/x86_64/9os.iso
make run                    # boot in QEMU (serial on stdio)
```

See [docs/PLAN.md](docs/PLAN.md) for the roadmap and [docs/HANDOFF.md](docs/HANDOFF.md) for the current state.

## Scheduler regression tests

`scripts/sched-host-tests.sh` checks the actual RR/MLFQ queue code on the host with
undefined-behavior traps (no userland or QEMU needed). After building the userland,
`python3 scripts/qemu-test.py x86_64 --smp 2 balancetest` checks busy-CPU balancing and
sibling-thread affinity in the guest. `scripts/ci-tests.sh ARCH` runs the full boot suite.
