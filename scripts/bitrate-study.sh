#!/bin/sh
# Bitrate study (docs/progress.md, "Bitrate report"). Encodes the PNG clips in
# out/bitrate/src (rendered with `08_text|06_look --grain G --frames DIR 1 FIRST LAST`)
# at CRF 18/21/24/27 in yuv420p and yuv444p (high444), with the same ffmpeg
# arguments as an_scene_export, and measures each against its source frames:
# size, Mbps, PSNR and SSIM (RGB, all frames), and grain retained on a paper-only
# patch (spike_grain_measure). Writes 4x crops of text and riso edges as
# montages: row 1 source + 420 at each CRF, row 2 source + 444 at each CRF.
#
#   scripts/bitrate-study.sh            -> out/bitrate/results.tsv, out/bitrate/crops/
#   GRAINS="0.06s" CRFS="18 21" RESULTS=static.tsv scripts/bitrate-study.sh
#       other clip sets (src/<clip>_g<GRAIN>) and CRFs; crops at the first two CRFs
set -eu
cd "$(dirname "$0")/.."
OUT=out/bitrate
SRC=$OUT/src
ENC=$OUT/enc
CROPS=$OUT/crops
mkdir -p "$ENC" "$CROPS"
MEASURE=./build/examples/spike_grain_measure
[ -x "$MEASURE" ] || { echo "build first (make)" >&2; exit 1; }
FPS=60
NFR=180
DEC="scale=in_color_matrix=bt709:in_range=tv"

# clip: name, first frame, file prefix, paper patch (w:h:x:y), crops (name w:h:x:y ...)
clip_info() {
    case $1 in
    text) echo "400 text 300:200:1520:800" ;;
    look) echo "90 look 240:120:20:20" ;;
    esac
}
clip_crops() {
    case $1 in
    text) echo "title_riso 160:100:330:100 code 240:60:490:455" ;;
    look) echo "rect_riso 160:100:260:160 halftone 120:80:470:190" ;;
    esac
}

GRAINS=${GRAINS:-0.03 0.045 0.06}
CRFS=${CRFS:-18 21 24 27}
RESULTS=$OUT/${RESULTS:-results.tsv}
printf 'clip\tgrain\tfmt\tcrf\tbytes\tMbps\tPSNR\tSSIM\tretained\tcorr\ttemporal\n' > $RESULTS
for clip in text look; do
    set -- $(clip_info $clip)
    first=$1 prefix=$2 patch=$3
    pw=${patch%%:*}; rest=${patch#*:}; ph=${rest%%:*}
    for g in $GRAINS; do
        dir=$SRC/${clip}_g$g
        in="-framerate $FPS -start_number $first -i $dir/${prefix}_%04d.png"
        # the source patch, grey
        ffmpeg -v error -y $in -frames:v $NFR -vf "format=rgb24,crop=$patch,format=gray" -f rawvideo $ENC/src.gray
        for fmt in 420 444; do
            if [ $fmt = 420 ]; then pf="-pix_fmt yuv420p"; else pf="-pix_fmt yuv444p -profile:v high444"; fi
            for crf in $CRFS; do
                f=$ENC/${clip}_g${g}_${fmt}_crf$crf.mp4
                ffmpeg -v error -y $in -frames:v $NFR -vf scale=out_color_matrix=bt709:out_range=tv,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv \
                    -c:v libx264 -preset medium -crf $crf $pf \
                    -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv \
                    -movflags +faststart "$f"
                bytes=$(stat -f %z "$f")
                mbps=$(echo "scale=2; $bytes * 8 / ($NFR / $FPS) / 1000000" | bc)
                cmp="[0:v]$DEC,format=gbrp[a];[1:v]format=gbrp[b];[a][b]"
                psnr=$(ffmpeg -v info -i "$f" $in -frames:v $NFR -lavfi "${cmp}psnr" -f null - 2>&1 | sed -n 's/.*PSNR.*average:\([0-9.inf]*\).*/\1/p' | tail -1)
                ssim=$(ffmpeg -v info -i "$f" $in -frames:v $NFR -lavfi "${cmp}ssim" -f null - 2>&1 | sed -n 's/.*SSIM.*All:\([0-9.]*\).*/\1/p' | tail -1)
                ffmpeg -v error -y -i "$f" -vf "$DEC,format=rgb24,crop=$patch,format=gray" -f rawvideo $ENC/dec.gray
                gm=$($MEASURE $pw $ph $ENC/src.gray $ENC/dec.gray)
                ret=$(echo "$gm" | awk '{print $2}'); cor=$(echo "$gm" | awk '{print $4}'); tmp=$(echo "$gm" | awk '{print $6}')
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $clip $g $fmt $crf $bytes $mbps "$psnr" "$ssim" $ret $cor $tmp >> $RESULTS
                echo "$clip g$g $fmt crf$crf: $mbps Mbps PSNR $psnr SSIM $ssim grain $gm"
            done
        done
        # crops: frame 120 of the clip, 4x nearest; montage rows: 420, 444
        n=$(printf %04d $((first + 120)))
        for c in $(clip_crops $clip | tr ' ' '\n' | paste - - | tr '\t' '='); do
            name=${c%%=*} box=${c#*=}
            cw=${box%%:*}; r2=${box#*:}; ch=${r2%%:*}
            up="crop=$box,scale=iw*4:ih*4:flags=neighbor"
            ffmpeg -v error -y -i $dir/${prefix}_$n.png -vf "format=rgb24,$up" $ENC/crop_src.png
            tw=$((cw * 4)); th=$((ch * 4))
            inputs=""; layout=""; k=0; row=0
            for fmt in 420 444; do
                col=0
                inputs="$inputs -i $ENC/crop_src.png"; layout="$layout|$((col * tw))_$((row * th))"; k=$((k + 1)); col=1
                for crf in $CRFS; do
                    ffmpeg -v error -y -i $ENC/${clip}_g${g}_${fmt}_crf$crf.mp4 \
                        -vf "select=eq(n\,120),$DEC,format=rgb24,$up" -frames:v 1 $ENC/crop_${fmt}_$crf.png
                    inputs="$inputs -i $ENC/crop_${fmt}_$crf.png"; layout="$layout|$((col * tw))_$((row * th))"
                    k=$((k + 1)); col=$((col + 1))
                done
                row=1
            done
            ffmpeg -v error -y $inputs -filter_complex "xstack=inputs=$k:layout=${layout#|}" $CROPS/${clip}_${name}_g$g.png
        done
    done
done
rm -f $ENC/src.gray $ENC/dec.gray $ENC/crop_*.png
echo "results: $RESULTS; crops: $CROPS/"
