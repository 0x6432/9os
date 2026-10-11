#!/bin/sh
# check: C unit-test framework (libinput's test suite)
. "$(dirname "$0")/common.sh"
V=0.15.2
T=$(fetch https://github.com/libcheck/check/releases/download/$V/check-$V.tar.gz)
rm -rf "$PORTS_BUILD/check-$V"; tar xzf "$T" -C "$PORTS_BUILD"
autotools_port "$PORTS_BUILD/check-$V" --disable-static --disable-subunit --disable-build-docs --disable-timeout-tests
cleanup_la
echo "check $V installed"
