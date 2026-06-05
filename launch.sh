#!/bin/bash
# Self-locating wrapper — resolves the script's own directory so the
# launcher works regardless of where the user installed the project.
set -eu
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
LOG_FILE="${XDG_RUNTIME_DIR:-/tmp}/ants-terminal-$(id -u).log"
BIN="$SCRIPT_DIR/build/ants-terminal"
FAST_BIN="$SCRIPT_DIR/build-fast/ants-terminal"

# ANTS-2025 — always launch the freshest binary, safely. In-session rebuilds go
# to the isolated build-fast/ tree, never an in-place relink of build/: the
# linker truncates+rewrites build/ants-terminal, and an instance still running
# from that inode later demand-pages a corrupted code page and SIGSEGVs. If
# build-fast/ holds a newer build, promote it into build/ via an atomic
# rename(2) — a still-running instance keeps its old inode while this launch
# picks up the new one. The binary must run from build/ (it resolves its assets
# and the MCP project root relative to its own path), so we swap the file in
# place rather than run from build-fast/.
if [ -x "$FAST_BIN" ] && [ "$FAST_BIN" -nt "$BIN" ]; then
    tmp="$SCRIPT_DIR/build/.ants-terminal.staging.$$"
    if cp -f "$FAST_BIN" "$tmp" && mv -f "$tmp" "$BIN"; then
        : # promoted the fresher build-fast binary into build/
    else
        rm -f "$tmp" 2>/dev/null || true  # fall back to whatever build/ has
    fi
fi

# Diagnose common launch failures to stderr before exec — a user who
# double-clicked the .desktop entry never sees the logfile. A failed
# exec would otherwise disappear silently.
if [ ! -x "$BIN" ]; then
    printf 'ants-terminal: binary not found or not executable: %s\n' "$BIN" >&2
    printf 'ants-terminal: run `cmake --build build` to build it first.\n' >&2
    printf 'ants-terminal: stderr log path: %s\n' "$LOG_FILE" >&2
    exit 127
fi

exec "$BIN" "$@" 2>"$LOG_FILE"
