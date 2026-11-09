#!/bin/sh
# Runs real production kmain with inert host stubs; never starts a guest.
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d /tmp/9os-root-dispatch-XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/include/arch"
# Replace only the privileged STI in the test header; do not patch production.
python3 - "$tmp" <<'PY'
import pathlib, sys
p = pathlib.Path('kernel/arch/x86_64/include/arch/cpu.h').read_text()
old = 'static inline void arch_irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }'
assert p.count(old) == 1
p = p.replace(old, 'static inline void arch_irq_enable(void) { }')
pathlib.Path(sys.argv[1], 'include/arch/cpu.h').write_text(p)
PY
build() {
    clang -std=c2x -ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter \
        -I"$tmp/include" -Ikernel/include -Ithird_party/limine -Ikernel/arch/x86_64/include \
        -D__9OS_ARCH_x86_64__ -c "$1" -o "$tmp/main.o"
    clang -std=c2x -Wall -Wextra -Werror -Ikernel/include -Ithird_party/limine \
        -Ikernel/arch/x86_64/include tests/root-policy-host/dispatch.c \
        tests/root-policy-host/stubs-x86_64.S "$tmp/main.o" -o "$tmp/dispatch"
}
build kernel/core/main.c
for case in strict-ok strict-negative strict-positive invalid-flag missing-root duplicate-root duplicate-flag legacy-ok legacy-fail no-root legacy-substring legacy-duplicate legacy-empty legacy-tab legacy-truncated; do
    timeout 5 "$tmp/dispatch" "$case"
done
echo 'production kmain root dispatch host controls PASS; no guest evidence'
# Prove controls detect a negative strict result incorrectly falling through.
python3 - "$tmp" <<'PY'
import pathlib, sys
p = pathlib.Path('kernel/core/main.c').read_text()
old = 'if (strict_root < 0) {'
assert p.count(old) == 1
p = p.replace(old, 'if (strict_root < 0) strict_root = 0;\n    if (strict_root < 0) {')
pathlib.Path(sys.argv[1], 'mutant.c').write_text(p)
PY
build "$tmp/mutant.c"
for case in strict-negative invalid-flag; do
    set +e
    timeout 5 "$tmp/dispatch" "$case" >"$tmp/mutation.out" 2>&1
    status=$?
    set -e
    case "$case" in
        strict-negative) expected='strict-negative FAIL stop=1 loaded=1 calls=2 panic=0 init=1 pressure=1';;
        invalid-flag) expected='invalid-flag FAIL stop=1 loaded=1 calls=1 panic=0 init=1 pressure=1';;
    esac
    if [ "$status" -ne 1 ] || ! grep -Eq "^${expected} error=" "$tmp/mutation.out"; then
        echo "mutation did not produce the expected assertion failure: $case (status $status)" >&2
        cat "$tmp/mutation.out" >&2
        exit 1
    fi
done
echo 'dispatch fallthrough mutation rejected PASS'
