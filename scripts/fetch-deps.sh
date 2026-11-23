#!/bin/sh
# Fetch pinned dependencies. To upgrade: set the SHAs to upstream HEAD
# (https://github.com/floooh/sokol/commits/master and
#  https://github.com/floooh/sokol-tools-bin/commits/master),
# rerun, rebuild, run every example, note the bump in docs/progress.md.
set -eu

SOKOL_SHA=09558a10085705e664eed01bdaf0a6cdadd960dd       # 2026-11-21
SOKOL_TOOLS_SHA=d4c808b8314d7f67af9382b25a1d828b79dd062e # 2026-11-23

cd "$(dirname "$0")/.."
mkdir -p vendor/sokol tools

for f in sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h LICENSE; do
    curl -fsSL -o "vendor/sokol/$f" \
        "https://raw.githubusercontent.com/floooh/sokol/$SOKOL_SHA/$f"
done

curl -fsSL -o tools/sokol-shdc \
    "https://raw.githubusercontent.com/floooh/sokol-tools-bin/$SOKOL_TOOLS_SHA/bin/osx_arm64/sokol-shdc"
chmod +x tools/sokol-shdc

echo "sokol $SOKOL_SHA, sokol-tools-bin $SOKOL_TOOLS_SHA"
