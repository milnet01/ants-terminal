#!/bin/bash
# Self-locating wrapper. Copies the freshest built binary OUT of the
# project tree to a stable home-drive location and launches it from
# there. Two wins (ANTS-2174, an ANTS-2025 follow-up):
#   * The running process is a home-drive copy that shares no inode with
#     any build output, so an in-place relink of build/ (or build-fast/)
#     while Ants is open can never corrupt the live code pages — the
#     ANTS-2025 SIGSEGV class is gone. Rebuild build/ freely while
#     running.
#   * The project tree is never written to at launch (no build/ promote
#     step), so launching leaves `git status` clean.
#
# The binary is self-contained at any path: the only path-relative load
# is the app-icon fallback (applicationDirPath()/../assets), which never
# fires because the icon is installed in the hicolor theme; the MCP
# project root is resolved per-call from caller_cwd / the focused tab's
# cwd, not from the binary's location (see remotecontrol.cpp:8129); all
# user state lives under XDG paths. So running from ~/.local/share is
# fully equivalent to running from build/.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
LOG_FILE="${XDG_RUNTIME_DIR:-/tmp}/ants-terminal-$(id -u).log"

# Where the live binary runs from — a bin/ subdir of the app's XDG data
# dir, on the system drive and fully outside the project tree. Kept
# separate from the sibling data dirs (sessions/, logs/, recordings/).
RUN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/bin"
RUN_BIN="$RUN_DIR/ants-terminal"

# Candidate build outputs in the project tree (READ-only here). Pick the
# newest of build/ and build-fast/ — whichever the last build wrote.
SRC=""
for cand in "$SCRIPT_DIR/build-fast/ants-terminal" "$SCRIPT_DIR/build/ants-terminal"; do
    if [ -x "$cand" ]; then
        if [ -z "$SRC" ] || [ "$cand" -nt "$SRC" ]; then SRC="$cand"; fi
    fi
done

# Promote the freshest build into the run location atomically, only when
# it is newer than what is already there (or nothing is there yet). A
# still-running instance keeps its old inode; this launch gets the new
# file via the rename.
if [ -n "$SRC" ] && { [ ! -e "$RUN_BIN" ] || [ "$SRC" -nt "$RUN_BIN" ]; }; then
    mkdir -p "$RUN_DIR"
    tmp="$RUN_DIR/.ants-terminal.staging.$$"
    if cp -f "$SRC" "$tmp" && mv -f "$tmp" "$RUN_BIN"; then
        : # promoted the fresher build into the home-drive run location
    else
        rm -f "$tmp" 2>/dev/null || true  # fall back to whatever is there
    fi
fi

# Diagnose common launch failures to stderr before exec — a user who
# double-clicked the .desktop entry never sees the logfile.
if [ ! -x "$RUN_BIN" ]; then
    printf 'ants-terminal: no runnable binary at %s\n' "$RUN_BIN" >&2
    printf 'ants-terminal: build it first (cmake --build build), then relaunch.\n' >&2
    printf 'ants-terminal: stderr log path: %s\n' "$LOG_FILE" >&2
    exit 127
fi

exec "$RUN_BIN" "$@" 2>"$LOG_FILE"
