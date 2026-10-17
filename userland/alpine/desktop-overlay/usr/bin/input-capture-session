#!/bin/sh
# Diagnostic only: publish completion status only after the read-only collector
# exits. An empty, pre-created status file must never signal completion.
set -eu
[ "$#" -eq 3 ] || exit 2
log=$1
status=$2
seconds=$3
[ ! -e "$status" ] && [ ! -e "$status.tmp" ] || exit 2
rc=0
inputeventcapture "$seconds" >"$log" 2>&1 || rc=$?
printf '%s\n' "$rc" >"$status.tmp"
mv "$status.tmp" "$status"
exit "$rc"
