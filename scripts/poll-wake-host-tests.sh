#!/bin/sh
# Run the exact small guest consumer against Linux; timings are informational.
set -eu
cd "$(dirname "$0")/.."
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT HUP INT TERM
reject_summary() {
    # A bare "! grep" is exempt from set -e; explicitly return failure instead.
    if grep -q '^pollwakebench: transfers=' "$1"; then return 1; fi
}
printf '%s\n' 'pollwakebench: transfers=1000' > "$D/bad-summary"
if reject_summary "$D/bad-summary"; then
    echo 'poll-wake-host: invalid-output guard accepted a result summary' >&2
    exit 1
fi
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror -pthread userland/tests/pollwakebench.c -o "$D/test"
for mode in default --inotify --unix --unix-inotify --epoll --epoll-inotify --epoll-unix --epoll-unix-inotify; do
    api=poll; transport=eventfd; watches=0
    case "$mode" in *epoll*) api=epoll_pwait;; esac
    case "$mode" in *unix*) transport=unix-stream;; esac
    case "$mode" in *inotify*) watches=24;; esac
    if [ "$mode" = default ]; then "$D/test" > "$D/result"
    else "$D/test" "$mode" > "$D/result"; fi
    cat "$D/result"
    grep -Eq "^pollwakebench: transfers=1000 idle_waiters=24 wall_ms=[0-9]+ producer_switches=[0-9]+ waiter_switches=[0-9]+ failures=0 inotify_waiters=$watches transport=$transport waiter_api=$api$" "$D/result"
    test "$(wc -l < "$D/result")" = 1
done
for arg in --bad --unix-extra --epoll-extra --epoll-unix-extra ''; do
    set +e
    "$D/test" "$arg" > "$D/invalid.out" 2>&1
    rc=$?
    set -e
    test "$rc" = 2
    reject_summary "$D/invalid.out"
done
set +e
"$D/test" --epoll --unix > "$D/invalid.out" 2>&1
rc=$?
set -e
test "$rc" = 2
reject_summary "$D/invalid.out"
set +e
"$D/test" --unix --inotify > "$D/invalid.out" 2>&1
rc=$?
set -e
test "$rc" = 2
reject_summary "$D/invalid.out"
# Independently wrong delivery tags and event counts must fail, not silently
# fall back to poll or count an unrelated wake as successful delivery.
python3 - "$D" <<'PY'
import pathlib, sys
source = pathlib.Path("userland/tests/pollwakebench.c").read_text()
for name, old, new in [
    ("tag", "event.data.u64 != 1", "event.data.u64 != 2"),
    ("count", "r != 1 || event.events", "r != 2 || event.events"),
]:
    assert source.count(old) == 1
    pathlib.Path(sys.argv[1], name + ".c").write_text(source.replace(old, new))
PY
for mutant in tag count; do
    ${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror -pthread "$D/$mutant.c" -o "$D/$mutant"
    set +e
    "$D/$mutant" --epoll-unix-inotify > "$D/$mutant.out" 2>&1
    rc=$?
    set -e
    test "$rc" = 1
    grep -q 'failures=24 .*waiter_api=epoll_pwait$' "$D/$mutant.out"
done
echo 'poll-wake-host: PASS eight exact modes, seven invalid CLI controls and two delivery mutants; no timing thresholds'
${CC:-cc} -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror -pthread userland/tests/unixpolltest.c -o "$D/unixpolltest"
"$D/unixpolltest"
set +e
"$D/unixpolltest" bad > "$D/invalid-unix.out" 2>&1
rc=$?
set -e
test "$rc" = 2
echo 'unixpoll-host: PASS exact native endpoint controls and CLI rejection; no GUI proof'
