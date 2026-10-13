#!/bin/sh
# fetch OUT URL [URL...]: download the first URL that yields a valid archive.
# Retries each mirror, refuses HTML error pages / truncated files, caches in
# $TOOLS_DIR/dl when set. Sourced or executed.
set -e
OUT=$1; shift
valid_archive() {
    case "$OUT" in
        *.gz|*.tgz) gzip -t "$1" 2>/dev/null ;;
        *.bz2) bzip2 -t "$1" 2>/dev/null ;;
        *.xz) xz -t "$1" 2>/dev/null ;;
        *) test -s "$1" ;;
    esac
}
if [ -s "$OUT" ]; then
    if valid_archive "$OUT"; then exit 0; fi
    echo "fetch: discarding invalid cached archive $OUT" >&2
    rm -f "$OUT"
fi
mkdir -p "$(dirname "$OUT")"
trap 'rm -f "$OUT.part"' 0
trap 'exit 1' HUP INT TERM
for u in "$@"; do
    for try in 1 2 3; do
        rm -f "$OUT.part"
        if curl -fsSL --retry 3 --retry-all-errors --retry-max-time 120 --connect-timeout 20 --max-time 120 -o "$OUT.part" "$u"; then
            if ! valid_archive "$OUT.part"; then
                echo "fetch: invalid archive from $u" >&2; continue
            fi
            mv "$OUT.part" "$OUT"; exit 0
        fi
        echo "fetch: $u failed (try $try)" >&2; sleep $((try * 2))
    done
done
rm -f "$OUT.part"; echo "fetch: all mirrors failed for $OUT" >&2; exit 1
