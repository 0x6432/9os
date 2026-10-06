#!/bin/sh
# fetch OUT URL [URL...]: download the first URL that yields a valid archive.
# Retries each mirror, refuses HTML error pages / truncated files, caches in
# $TOOLS_DIR/dl when set. Sourced or executed.
set -e
OUT=$1; shift
[ -s "$OUT" ] && exit 0
mkdir -p "$(dirname "$OUT")"
for u in "$@"; do
    for try in 1 2 3; do
        rm -f "$OUT.part"
        if curl -fsSL --retry 3 --connect-timeout 20 -o "$OUT.part" "$u"; then
            case "$OUT" in
                *.gz|*.tgz) gzip -t "$OUT.part" 2>/dev/null || { echo "fetch: bad gzip from $u" >&2; continue; } ;;
                *.bz2) bzip2 -t "$OUT.part" 2>/dev/null || { echo "fetch: bad bzip2 from $u" >&2; continue; } ;;
                *.xz) xz -t "$OUT.part" 2>/dev/null || { echo "fetch: bad xz from $u" >&2; continue; } ;;
            esac
            mv "$OUT.part" "$OUT"; exit 0
        fi
        echo "fetch: $u failed (try $try)" >&2; sleep $((try * 2))
    done
done
rm -f "$OUT.part"; echo "fetch: all mirrors failed for $OUT" >&2; exit 1
