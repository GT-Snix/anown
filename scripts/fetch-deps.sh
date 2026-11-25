#!/bin/sh
# Fetch pinned dependencies. To upgrade: set the SHAs to upstream HEAD
# (https://github.com/floooh/sokol/commits/master and
#  https://github.com/floooh/sokol-tools-bin/commits/master),
# rerun, rebuild, run every example, note the bump in docs/progress.md.
set -eu

SOKOL_SHA=09558a10085705e664eed01bdaf0a6cdadd960dd       # 2026-11-21
SOKOL_TOOLS_SHA=d4c808b8314d7f67af9382b25a1d828b79dd062e # 2026-11-23
STB_SHA=6b5799bae8a1fe3792691ef4b72ae26434c3e799         # 2026-11-19 (HEAD on 2026-11-25; truetype v1.28, image_write v1.17+)

cd "$(dirname "$0")/.."
mkdir -p vendor/sokol vendor/stb tools

for f in sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h LICENSE; do
    curl -fsSL -o "vendor/sokol/$f" \
        "https://raw.githubusercontent.com/floooh/sokol/$SOKOL_SHA/$f"
done

for f in stb_image_write.h stb_truetype.h LICENSE; do
    curl -fsSL -o "vendor/stb/$f" \
        "https://raw.githubusercontent.com/nothings/stb/$STB_SHA/$f"
done

curl -fsSL -o tools/sokol-shdc \
    "https://raw.githubusercontent.com/floooh/sokol-tools-bin/$SOKOL_TOOLS_SHA/bin/osx_arm64/sokol-shdc"
chmod +x tools/sokol-shdc

echo "sokol $SOKOL_SHA, sokol-tools-bin $SOKOL_TOOLS_SHA, stb $STB_SHA"
