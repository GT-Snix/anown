#!/bin/sh
# Time a clean build (`make clean && make`) in a throwaway copy of the tree, so
# the real build/ (and anything else) is never touched. CLAUDE.md "Build": the
# clean build must stay under about 5 seconds.
#
#   scripts/time-clean-build.sh [--cold] [DIR]   DIR: tree to time (default: this repo)
#
# The copy keeps .vendor-cache/ (the cached sokol and stb objects, see the
# Makefile), as a clean build in the real tree would. --cold leaves it out: the
# first build after a vendor change.
set -eu
COLD=
if [ "${1:-}" = --cold ]; then COLD=--exclude=./.vendor-cache; shift; fi
SRC=$(cd "${1:-$(dirname "$0")/..}" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/anown-cleanbuild.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
# everything except outputs and the VCS; tools/ (sokol-shdc) is needed
(cd "$SRC" && tar cf - --exclude=./build --exclude=./out --exclude=./.git $COLD .) | (cd "$TMP" && tar xf -)
cd "$TMP"
echo "load: $(sysctl -n vm.loadavg)"
/usr/bin/time -p make >/dev/null 2>"$TMP/time.txt" || { cat "$TMP/time.txt" >&2; exit 1; }
grep '^real' "$TMP/time.txt"
