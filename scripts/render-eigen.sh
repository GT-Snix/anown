#!/bin/sh
# Render the chapters of examples/12_eigen at 1920x1080, 60 fps, share mode, to
# out/eigen/final/chN.mp4, in one command. Each chapter is its own scene and
# starts from the state it declares, so the seams between chapters are cuts.
#
# Script order (out/eigen/seven/SCRIPT.md): the fast version is chapters 1 2 0 3 4,
# then "by hand" (not built yet), chapter 7 (into 3D), and chapter 5 inside
# "eigen-coordinates". Chapter 6 (the recap) is retired: done in the editor.
#
# Prints every chapter's rendered length against its DUR (chapter 7: its recorded
# clip) and the total, and compares the seams that are adjacent in the script: the
# last frame of one chapter against the first of the next, PNG bytes. A pair that
# differs is a cut, not a failure.
#
#   scripts/render-eigen.sh            render all chapters
#   scripts/render-eigen.sh 3 4        render only these (the table still lists all)
set -u
cd "$(dirname "$0")/.." || exit 1
FPS=60
OUT=out/eigen/final
SEAMS=$OUT/seams
BIN=./build/examples/12_eigen
ORDER="1 2 0 3 4 7 5"                  # the script's order
PAIRS="1:2 2:0 0:3 3:4"                # adjacent in the script (the fast version)
RENDER=${*:-$ORDER}

make "$BIN" > /dev/null || exit 1
mkdir -p "$OUT" "$SEAMS"

# the DUR table from the source (chapters 0..6), and chapter 7's length
DUR=$(sed -n 's/^static const float DUR\[NCH\] = { \(.*\) };.*/\1/p' examples/12_eigen/main.c | tr -d ' f')
CH7=$(sed -n 's/^static const float CH7_LEN = \([0-9.]*\)f;.*/\1/p' examples/12_eigen/main.c)
planned() { [ "$1" -eq 7 ] && echo "$CH7" || echo "$DUR" | cut -d, -f$(($1 + 1)); }

for n in $RENDER; do
    [ "$n" -eq 6 ] && { echo "chapter 6: retired (the recap is done in the editor)"; continue; }
    printf 'chapter %s: rendering... ' "$n"
    start=$(date +%s)
    rm -f "$SEAMS"/ch${n}_*.png
    if ! "$BIN" --chapter "$n" --fps "$FPS" --seams "$SEAMS" --export "$OUT/ch$n.mp4" > "$OUT/ch$n.log" 2>&1; then
        echo "failed (see $OUT/ch$n.log)"
        exit 1
    fi
    echo "$(($(date +%s) - start)) s"
done

echo
printf '%-8s %-10s %-10s %-12s %-12s %s\n' chapter "DUR (s)" frames rendered "vs DUR" ""
total_frames=0
total_dur=0
for n in $ORDER; do
    [ -f "$OUT/ch$n.mp4" ] || { printf '%-8s %-10s (not rendered)\n' "$n" "$(planned "$n")"; continue; }
    f=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT/ch$n.mp4")
    d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT/ch$n.mp4")
    p=$(planned "$n")
    case " $RENDER " in *" $n "*) note="re-rendered" ;; *) note="kept" ;; esac
    printf '%-8s %-10s %-10s %-12s %-12s %s\n' "$n" "$p" "$f" "$(printf '%.3f s' "$d")" "$(printf '%+.3f s' "$(echo "$d - $p" | bc -l)")" "$note"
    total_frames=$((total_frames + f))
    total_dur=$(echo "$total_dur + $d" | bc -l)
done
printf '%-8s %-10s %-10s %-12s\n' total "" "$total_frames" "$(printf '%.3f s' "$total_dur")"

echo
echo "seams adjacent in the script (last frame of A == first frame of B, PNG bytes):"
for pr in $PAIRS; do
    a=${pr%:*}
    b=${pr#*:}
    la=$(ls "$SEAMS"/ch${a}_last_*.png 2>/dev/null | head -1)
    fb=$(ls "$SEAMS"/ch${b}_first_*.png 2>/dev/null | head -1)
    if [ -z "$la" ] || [ -z "$fb" ]; then
        echo "  $a -> $b: not rendered"
    elif cmp -s "$la" "$fb"; then
        echo "  $a -> $b: identical (chained)"
    else
        echo "  $a -> $b: a cut (not chained)"
    fi
done
echo "  4 -> 7, 7 -> 5: new sections between them in the script (by hand, eigen-coordinates)"
