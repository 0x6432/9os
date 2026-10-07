# 9os

A 64-bit hobby operating system in C23. Boots with [Limine](https://github.com/limine-bootloader/limine),
uses [uACPI](https://github.com/uACPI/uACPI) for ACPI and targets the Linux syscall ABI so that
static [musl](https://musl.libc.org) programs — BusyBox and Bash — run unmodified.

Targets: **x86_64** (primary), riscv64 and aarch64 (all boot-tested in CI).

## Building

Requirements: clang (≥15; C23 mode is used when available, `-std=c2x` otherwise), ld.lld,
xorriso, cpio, make, git, and QEMU for testing.

```sh
./scripts/fetch-deps.sh     # Limine binaries + uACPI
make iso                    # build/x86_64/9os.iso
make run                    # boot in QEMU (serial on stdio)
                             # log in as root (no password) or user / 9os
```

See [docs/PLAN.md](docs/PLAN.md) for the roadmap and [docs/HANDOFF.md](docs/HANDOFF.md) for the current state.

## Allocator and scheduler regression tests

Host tests compile the **actual kernel implementations** with undefined-behavior traps
(no userland or QEMU needed):

```sh
scripts/pmm-host-tests.sh     # page caches, pressure, coalescing and overlapping drains
scripts/slab-host-tests.sh    # object reuse, zeroing, real OOM and concurrent drains
scripts/sched-host-tests.sh   # RR/MLFQ local locks, switches, wake races and idle deadlines
```

After building userland, `scripts/ci-tests.sh ARCH` runs the full guest suite,
including `slabtest`, `idletest`, `pcputest`, affinity/nice/RT and busy-CPU balancing.
For focused single-CPU or alternate-policy coverage:

```sh
python3 scripts/qemu-test.py x86_64 --smp 1 slabtest idletest nicetest
python3 scripts/qemu-test.py riscv64 --smp 2 --sched mlfq slabtest idletest afftest balancetest
```

Pass `--sched mlfq` to the QEMU runner itself: it invokes Make and otherwise uses
the default RR build even if an MLFQ ISO was built previously. CI runs single-CPU
and MLFQ-focused tests on all three architectures before publishing the default RR ISOs.
