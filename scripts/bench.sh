#!/bin/sh
# Frame benchmark: 5 runs of each spike_frame_bench scenario (0..4), then one
# summary line per scenario: median, p99 and frames over 12 ms per run (frames
# 30+, warm-up excluded), and the tessellation average.
#
#   make && scripts/bench.sh            all scenarios
#   scripts/bench.sh 0 3                only these
#
# The bench refuses to report while other work keeps more than 1.5 CPU cores busy
# (measured over 1 s before and after each run; exit code 3). This script stops
# at the first refusal and says so; it never reports a partial set as if it were
# clean.
set -u
cd "$(dirname "$0")/.." || exit 1
BENCH=./build/examples/spike_frame_bench
RUNS=5
[ -x "$BENCH" ] || { echo "bench.sh: $BENCH not built; run make first" >&2; exit 1; }
SCENARIOS=${*:-0 1 2 3 4}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/anown-bench.XXXXXX") || exit 1

for sc in $SCENARIOS; do
    i=1
    while [ "$i" -le "$RUNS" ]; do
        "$BENCH" "$sc" > "$OUT/s$sc-r$i.txt" 2> "$OUT/s$sc-r$i.err"
        rc=$?
        if [ "$rc" -eq 3 ]; then
            cat "$OUT/s$sc-r$i.err" >&2
            echo "bench.sh: stopped at scenario $sc, run $i: machine not quiet. No timings reported." >&2
            exit 3
        elif [ "$rc" -ne 0 ]; then
            cat "$OUT/s$sc-r$i.err" >&2
            echo "bench.sh: scenario $sc run $i failed (exit $rc)" >&2
            exit "$rc"
        fi
        i=$((i + 1))
    done
done

printf '%-9s %-28s %-34s %-34s %-22s %s\n' scenario name "median ms (5 runs)" "p99 ms (5 runs)" "frames > 12 ms" "tess avg ms"
for sc in $SCENARIOS; do
    name=$(sed -n 's/^200 strokes + 50 filled shapes, scenario [0-9]*: \(.*\), [0-9]* frames,.*/\1/p' "$OUT/s$sc-r1.txt")
    med=""; p99=""; over=""; tess=""
    i=1
    while [ "$i" -le "$RUNS" ]; do
        f="$OUT/s$sc-r$i.txt"
        med="$med $(sed -n 's/.*full frame.*median \([0-9.]*\) .*/\1/p' "$f")"
        p99="$p99 $(sed -n 's/.*full frame.* p99 \([0-9.]*\) .*/\1/p' "$f")"
        over="$over $(sed -n 's/.*; \([0-9]*\) frames over 12 ms.*/\1/p' "$f")"
        tess="$tess $(sed -n 's/.*tessellation *avg \([0-9.]*\) ms.*/\1/p' "$f")"
        i=$((i + 1))
    done
    printf '%-9s %-28s %-34s %-34s %-22s %s\n' "$sc" "$(echo "$name" | cut -c1-28)" "$med" "$p99" "$over" "$tess"
done
echo "raw output: $OUT"
