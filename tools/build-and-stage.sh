#!/usr/bin/env bash
# ANTS-2025 — build in build-fast/, then stage the result into
# build/ants-terminal without relinking it in place.
#
# launch.sh runs a copy under ${XDG_DATA_HOME:-~/.local/share}/ants-terminal/bin/
# (ANTS-2174), so a terminal it started shares no inode with build output and
# `cmake --build build` cannot corrupt it. An instance started from
# build/ants-terminal directly still can: an in-place relink truncates and
# rewrites the file under it, and it later SIGSEGVs on a corrupted code page
# (observed 2026-06-05).
#
# This script builds in build-fast/ and swaps the result into
# build/ants-terminal with an atomic rename(2). Both trees are on one
# filesystem, so an instance running from build/ keeps its old inode, and
# build/ants-terminal is still the latest build.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."

[ -f build-fast/build.ninja ] || cmake --preset=fast
cmake --build build-fast "$@"

tmp="build/.ants-terminal.staging.$$"
cp -f build-fast/ants-terminal "$tmp"
mv -f "$tmp" build/ants-terminal     # atomic rename: fresh inode, running PID safe
echo "Staged latest build → build/ants-terminal (atomic; safe while running)."
