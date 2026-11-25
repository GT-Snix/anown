#!/bin/sh
# Reference frames: render PNGs from every example that can write them into DIR
# and print a SHA-256 manifest (DIR/manifest.txt). Run before and after a change
# that must not alter output (a dependency bump, a refactor), then diff the two
# manifests: identical bytes, or the change isn't a no-op.
#
#   scripts/ref-frames.sh out/ref/before
#   scripts/ref-frames.sh out/ref/after
#   diff out/ref/before/manifest.txt out/ref/after/manifest.txt
set -eu
cd "$(dirname "$0")/.."
DIR=${1:?usage: scripts/ref-frames.sh DIR}
B=./build/examples
mkdir -p "$DIR"
for f in 0 30 90 150; do "$B/03_strokes" --png "$f" "$DIR/strokes_$f.png" > /dev/null; done
"$B/04_stroke_stress" "$DIR/stroke_stress.png" > /dev/null
"$B/05_style" --frames "$DIR" 15 > /dev/null
"$B/06_look" --frames "$DIR" 15 > /dev/null
"$B/07_palettes" "$DIR" > /dev/null
[ -d out/test ] && for p in out/test/*.png; do cp "$p" "$DIR/test_$(basename "$p")"; done
(cd "$DIR" && shasum -a 256 *.png > manifest.txt)
echo "$(wc -l < "$DIR/manifest.txt" | tr -d ' ') PNGs, manifest $DIR/manifest.txt"
