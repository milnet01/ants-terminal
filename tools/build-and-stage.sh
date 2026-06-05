#!/usr/bin/env bash
# ANTS-2025 — rebuild without corrupting a running instance.
#
# The terminal is launched from build/ants-terminal directly (launch.sh / the
# Plasma task-manager icon), and the binary resolves its assets relative to its
# own path (applicationDirPath()/../assets — main.cpp, titlebar.cpp) and the MCP
# project root the same way (remotecontrol.cpp), so it MUST keep running from
# build/. But `cmake --build build` relinks build/ants-terminal IN PLACE: the
# linker truncates and rewrites the file, and an instance still running from
# that inode later demand-pages a corrupted/shifted code page and SIGSEGVs
# (observed 2026-06-05, ~2.5 min after an in-place relink — impossible
# backtrace = text-segment corruption, not a logic bug).
#
# Fix: build in the isolated build-fast/ tree, then swap the result into
# build/ants-terminal with an atomic rename(2). build/ and build-fast/ live on
# the same filesystem (under the repo root), so the rename is atomic: any
# instance already running keeps its old inode, while the next launch picks up
# the fresh one. Result: always-latest, never-corrupting. Use this instead of
# `cmake --build build` whenever an instance might be running.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."

[ -f build-fast/build.ninja ] || cmake --preset=fast
cmake --build build-fast "$@"

tmp="build/.ants-terminal.staging.$$"
cp -f build-fast/ants-terminal "$tmp"
mv -f "$tmp" build/ants-terminal     # atomic rename: fresh inode, running PID safe
echo "Staged latest build → build/ants-terminal (atomic; safe while running)."
