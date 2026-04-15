#!/bin/bash
# Self-locating wrapper — resolves the script's own directory so the
# launcher works regardless of where the user installed the project.
set -eu
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
LOG_FILE="${XDG_RUNTIME_DIR:-/tmp}/ants-terminal-$(id -u).log"
BIN="$SCRIPT_DIR/build/ants-terminal"

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
