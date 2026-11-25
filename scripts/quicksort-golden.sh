#!/bin/sh
# Compare every 10th quicksort frame with a recorded manifest (default: the
# golden set from commit ad288d0, out/golden/quicksort_ad288d0.sha256).
# Prints each frame that differs; exit 0 if none do.
#
#   scripts/quicksort-golden.sh [MANIFEST]
set -eu
cd "$(dirname "$0")/.."
MAN=${1:-out/golden/quicksort_ad288d0.sha256}
DIR=$(mktemp -d "${TMPDIR:-/tmp}/anown-qs.XXXXXX")
./build/examples/10_quicksort --frames "$DIR" --every 10 > /dev/null
(cd "$DIR" && shasum -a 256 *.png) > "$DIR/now.sha256"
if diff -q "$MAN" "$DIR/now.sha256" > /dev/null; then
    echo "all $(wc -l < "$MAN" | tr -d ' ') frames identical to $MAN"
    rm -rf "$DIR"
    exit 0
fi
diff "$MAN" "$DIR/now.sha256" | grep '^>' | awk '{print $3}' > "$DIR/differ.txt" || true
echo "$(wc -l < "$DIR/differ.txt" | tr -d ' ') of $(wc -l < "$MAN" | tr -d ' ') frames differ (frames kept in $DIR):"
tr '\n' ' ' < "$DIR/differ.txt"; echo
exit 1
