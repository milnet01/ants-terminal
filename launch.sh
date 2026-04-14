#!/bin/bash
# Self-locating wrapper — resolves the script's own directory so the
# launcher works regardless of where the user installed the project.
set -eu
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
exec "$SCRIPT_DIR/build/ants-terminal" "$@" 2>"${XDG_RUNTIME_DIR:-/tmp}/ants-terminal-$(id -u).log"
