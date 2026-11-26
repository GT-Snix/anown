#!/bin/sh
# The whole 7-minute eigenvector video in one command: render every part of
# examples/12_eigen at 1920x1080, 60 fps (4:4:4 master intermediates), cut them
# together on the narration's timeline, lay the clock tag over the middle, put the
# recorded clips under it (0.8 s of silence under each stinger) and encode
# out/eigen/final/eigenvectors_7min.mp4 (H.264, share settings, BT.709).
#
# Timeline (out/eigen/seven/SCRIPT.md; clip lengths from the recordings):
#   cold open (8) | THE FAST VERSION | 1 2 0 3 4 | BY HAND | 9 | INTO 3D | 7 |
#   WHY CARE | 10 5 11 | THE FUN STUFF | 12 13 14 15 | RECAP | montage | outro (16)
# The recap is a montage cut from the rendered chapters, one shot per recap line.
#
#   scripts/assemble-eigen.sh          render everything, then assemble
#   SKIP_RENDER=1 scripts/assemble-eigen.sh    assemble from the parts already rendered
set -eu
cd "$(dirname "$0")/.."
FPS=60
OUT=out/eigen/final
PARTS=$OUT/parts
AUDIO=out/eigen/seven/audio/clips
BIN=./build/examples/12_eigen
mkdir -p "$PARTS"
make "$BIN" > /dev/null

render() { # name chapter [title]
    [ -n "${SKIP_RENDER:-}" ] && [ -f "$PARTS/$1.mp4" ] && return 0
    printf '  %-10s chapter %-2s ' "$1" "$2"
    start=$(date +%s)
    if [ $# -ge 3 ]; then
        "$BIN" --chapter "$2" --fps $FPS --master 1 --title "$3" --export "$PARTS/$1.mp4" > "$PARTS/$1.log" 2>&1
    else
        "$BIN" --chapter "$2" --fps $FPS --master 1 --export "$PARTS/$1.mp4" > "$PARTS/$1.log" 2>&1
    fi
    if grep -q 'already passed\|limit' "$PARTS/$1.log"; then echo "(cue warnings: see $PARTS/$1.log)"; fi
    echo "$(($(date +%s) - start)) s"
}

echo "rendering parts:"
render cold 8
render st_fast 17 "THE FAST VERSION"
render c1 1
render c2 2
render c0 0
render c3 3
render c4 4
render st_hand 17 "BY HAND"
render hand 9
render st_3d 17 "INTO 3D"
render c7 7
render st_why 17 "WHY CARE"
render ca 10
render c5 5
render cb 11
render st_fun 17 "THE FUN STUFF"
render fib 12
render google 13
render music 14
render data 15
render st_recap 17 "RECAP"
render outro 16
render clock 19

echo "assembling:"
python3 - "$OUT" "$PARTS" "$AUDIO" <<'PY'
import subprocess, sys
out, parts, audio = sys.argv[1:4]
FPS = 60
clip = [24.422857, 84.532653, 65.825692, 54.477755, 52.827143, 101.382630, 27.861633, 13.762653]
ST = 0.8
# (part, start in the part (s), length (s))
recap = [("c0", 7.6, 5.4),      # "Let's recap. An eigenvector is a direction a transformation doesn't turn."
         ("c3", 15.3, 3.4),     # "The eigenvalue is how much it stretches along it."
         ("hand", 16.1, 2.9),   # "You find them by setting a determinant to zero."
         ("c7", 9.4, 3.2),      # "In 3D, they're the axis of every spin."
         ("c5", 5.0, 2.6),      # "Repeat a transformation, and the biggest one wins."
         ("fib", 15.0, 1.1),    # "Golden ratio,"
         ("google", 9.0, 0.9),  # "Google,"
         ("music", 6.0, 0.8),   # "guitar strings"
         ("data", 14.0, 2.5),   # "and your data, all hiding in the same idea."
         ("c7", 45.8, 0.0)]     # "Find the direction that doesn't turn, and the rest is just stretching."
recap[-1] = ("c7", 45.8, clip[6] - sum(d for _, _, d in recap[:-1]))
segs = [("cold", 0, clip[0]), ("st_fast", 0, ST),
        ("c1", 0, 9.9), ("c2", 0, 8.1), ("c0", 0, 14.0), ("c3", 0, 31.2), ("c4", 0, clip[1] - 63.2),
        ("st_hand", 0, ST), ("hand", 0, clip[2]),
        ("st_3d", 0, ST), ("c7", 0, clip[3]),
        ("st_why", 0, ST), ("ca", 0, 13.9), ("c5", 0, 17.0), ("cb", 0, clip[4] - 30.9),
        ("st_fun", 0, ST), ("fib", 0, 38.6), ("google", 0, 22.6), ("music", 0, 11.6), ("data", 0, clip[5] - 72.8),
        ("st_recap", 0, ST)] + recap + [("outro", 0, clip[7])]
# frame-exact cuts: each segment covers [round(T0 fps), round(T1 fps)) of the timeline
cmd = ["ffmpeg", "-y", "-loglevel", "error", "-stats"]
filt, labels, t, total_frames = [], [], 0.0, 0
for i, (name, s0, d) in enumerate(segs):
    f0, f1 = round(t * FPS), round((t + d) * FPS)
    n, a = f1 - f0, round(s0 * FPS)
    cmd += ["-i", f"{parts}/{name}.mp4"]
    filt.append(f"[{i}:v]trim=start_frame={a}:end_frame={a + n},setpts=PTS-STARTPTS,fps={FPS}[v{i}]")
    labels.append(f"[v{i}]")
    t += d
    total_frames += n
nv = len(segs)
# the clock tag from the fast version's start to the outro's
starts = [0.0]
for k in range(1, 8): starts.append(starts[-1] + clip[k - 1] + (ST if k <= 6 else 0.0))
c0, c1 = starts[1], starts[7]
cmd += ["-i", f"{parts}/clock.mp4"]
ic = nv
filt.append("".join(labels) + f"concat=n={nv}:v=1:a=0[main]")
filt.append(f"[{ic}:v]setpts=PTS-STARTPTS+{c0:.6f}/TB[ck]")
filt.append(f"[main][ck]overlay=x=1600:y=950:enable='between(t,{c0:.6f},{c1 - 1.0 / FPS:.6f})':eof_action=pass,"
            "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,"
            "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv[v]")
# audio: the clips in order, 0.8 s of silence before sections 1..6
alabels, ia = [], ic + 1
for k in range(8):
    if 1 <= k <= 6:
        cmd += ["-f", "lavfi", "-t", f"{ST}", "-i", "anullsrc=r=44100:cl=mono"]
        alabels.append(f"[{ia}:a]"); ia += 1
    cmd += ["-i", f"{audio}/{k}.eigen.mp3"]
    filt.append(f"[{ia}:a]aformat=sample_rates=44100:channel_layouts=mono,atrim=end={clip[k]:.6f}[a{k}]")
    alabels.append(f"[a{k}]"); ia += 1
filt.append("".join(alabels) + f"concat=n={len(alabels)}:v=0:a=1[a]")
cmd += ["-filter_complex", ";".join(filt), "-map", "[v]", "-map", "[a]",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-profile:v", "high", "-pix_fmt", "yuv420p",
        "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv",
        "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", f"{out}/eigenvectors_7min.mp4"]
print(f"  {nv} segments, {total_frames} frames ({total_frames / FPS:.3f} s); clock {c0:.3f} .. {c1:.3f} s; "
      f"audio {sum(clip) + 6 * ST:.3f} s")
subprocess.run(cmd, check=True)
PY
f="$OUT/eigenvectors_7min.mp4"
echo "wrote $f"
ffprobe -v error -show_entries stream=codec_type,codec_name,width,height,r_frame_rate,duration -of compact "$f"
