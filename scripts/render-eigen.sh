#!/bin/sh
# Render every chapter of examples/12_eigen (0..7) at 1920x1080, 60 fps, share
# mode, to out/eigen/final/chN.mp4, in one command. Prints each chapter's frame
# count and duration next to the DUR table (chapter 7: its recorded clip), the
# total, and checks the 2D seams: a chapter's first frame is the previous
# chapter's last (docs/eigen_script.md), byte for byte, rendered in separate runs.
# 6 -> 7 is a cut into the 3D scene (no shared frame), so it isn't compared.
#
#   scripts/render-eigen.sh            all chapters
#   scripts/render-eigen.sh 3 4        only these (seams checked where both sides exist)
set -u
cd "$(dirname "$0")/.." || exit 1
FPS=60
OUT=out/eigen/final
SEAMS=$OUT/seams
BIN=./build/examples/12_eigen
CHAPTERS=${*:-0 1 2 3 4 5 6 7}

make "$BIN" > /dev/null || exit 1
mkdir -p "$OUT" "$SEAMS"

# the DUR table from the source (chapters 0..6), and chapter 7's length
DUR=$(sed -n 's/^static const float DUR\[NCH\] = { \(.*\) };.*/\1/p' examples/12_eigen/main.c | tr -d ' ')
CH7=$(sed -n 's/^static const float CH7_LEN = \([0-9.]*\)f;.*/\1/p' examples/12_eigen/main.c)
planned() { [ "$1" -eq 7 ] && echo "$CH7" || echo "$DUR" | cut -d, -f$(($1 + 1)); }

for n in $CHAPTERS; do
    printf 'chapter %s: rendering... ' "$n"
    start=$(date +%s)
    if ! "$BIN" --chapter "$n" --fps "$FPS" --seams "$SEAMS" --export "$OUT/ch$n.mp4" > "$OUT/ch$n.log" 2>&1; then
        echo "failed (see $OUT/ch$n.log)"
        exit 1
    fi
    echo "$(($(date +%s) - start)) s"
done

echo
printf '%-8s %-10s %-10s %-12s %s\n' chapter "DUR (s)" frames "duration" "frames in the timeline"
total_frames=0
total_dur=0
for n in $CHAPTERS; do
    f=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT/ch$n.mp4")
    d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT/ch$n.mp4")
    range=$(sed -n 's/^chapter [0-9]* range: frames \(.*\)/\1/p' "$OUT/ch$n.log")
    printf '%-8s %-10s %-10s %-12s %s\n' "$n" "$(planned "$n")" "$f" "$(printf '%.3f s' "$d")" "$range"
    total_frames=$((total_frames + f))
    total_dur=$(echo "$total_dur + $d" | bc -l)
done
printf '%-8s %-10s %-10s %-12s\n' total "" "$total_frames" "$(printf '%.3f s' "$total_dur")"

echo
echo "seams (first frame of N == last frame of N-1, PNG bytes):"
bad=0
for n in $CHAPTERS; do
    [ "$n" -eq 0 ] || [ "$n" -eq 7 ] && continue
    p=$((n - 1))
    a=$(ls "$SEAMS"/ch${p}_last_*.png 2>/dev/null | head -1)
    b=$(ls "$SEAMS"/ch${n}_first_*.png 2>/dev/null | head -1)
    [ -n "$a" ] && [ -n "$b" ] || continue
    fa=$(basename "$a" .png | sed 's/.*_//')
    fb=$(basename "$b" .png | sed 's/.*_//')
    if [ "$fa" = "$fb" ] && cmp -s "$a" "$b"; then
        echo "  $p -> $n: frame $fa, identical"
    else
        echo "  $p -> $n: DIFFER (last of $p is frame $fa, first of $n is frame $fb)"
        bad=$((bad + 1))
    fi
done
echo "  6 -> 7: a cut into the 3D scene (starts on frame 0, a boil step); not compared"
[ "$bad" -eq 0 ] || exit 2
