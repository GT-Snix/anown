#!/bin/sh
# Golden frames for an example: every 10th frame's PNG hashed (sha256).
#   scripts/example-golden.sh record EXAMPLE PREFIX MANIFEST   write MANIFEST
#   scripts/example-golden.sh check  EXAMPLE PREFIX MANIFEST   compare; list frames that differ
# e.g. scripts/example-golden.sh check 11_bst bst out/golden/bst_5d0ab00.sha256
# Exit 0 if identical (check) or recorded (record).
set -eu
cd "$(dirname "$0")/.."
MODE=$1 EX=$2 PREFIX=$3 MAN=$4
DIR=$(mktemp -d "${TMPDIR:-/tmp}/anown-golden.XXXXXX")
"./build/examples/$EX" --frames "$DIR" --every 10 > /dev/null
(cd "$DIR" && shasum -a 256 "$PREFIX"_*.png) > "$DIR/now.sha256"
if [ "$MODE" = record ]; then
    mkdir -p "$(dirname "$MAN")"
    cp "$DIR/now.sha256" "$MAN"
    echo "recorded $(wc -l < "$MAN" | tr -d ' ') frames of $EX in $MAN"
    rm -rf "$DIR"
    exit 0
fi
if diff -q "$MAN" "$DIR/now.sha256" > /dev/null; then
    echo "all $(wc -l < "$MAN" | tr -d ' ') frames of $EX identical to $MAN"
    rm -rf "$DIR"
    exit 0
fi
diff "$MAN" "$DIR/now.sha256" | grep '^>' | awk '{print $3}' > "$DIR/differ.txt" || true
echo "$(wc -l < "$DIR/differ.txt" | tr -d ' ') of $(wc -l < "$MAN" | tr -d ' ') frames of $EX differ (frames kept in $DIR):"
tr '\n' ' ' < "$DIR/differ.txt"; echo
exit 1
