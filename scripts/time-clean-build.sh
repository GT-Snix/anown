#!/bin/sh
# Time a clean build (`make clean && make`) in a throwaway copy of the tree, so
# the real build/ (and anything else) is never touched. CLAUDE.md "Build": the
# clean build must stay under about 5 seconds.
#
#   scripts/time-clean-build.sh [DIR]    DIR: tree to time (default: this repo)
set -eu
SRC=$(cd "${1:-$(dirname "$0")/..}" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/anown-cleanbuild.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
# everything except outputs and the VCS; tools/ (sokol-shdc) is needed
(cd "$SRC" && tar cf - --exclude=./build --exclude=./out --exclude=./.git .) | (cd "$TMP" && tar xf -)
cd "$TMP"
echo "load: $(sysctl -n vm.loadavg)"
/usr/bin/time -p make >/dev/null 2>"$TMP/time.txt" || { cat "$TMP/time.txt" >&2; exit 1; }
grep '^real' "$TMP/time.txt"
