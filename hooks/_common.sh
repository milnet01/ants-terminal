# shellcheck shell=bash
# ants-terminal hook pack — shared helpers (sourced, not executed).
# See docs/specs/ANTS-1252.md.
#
# Two surfaces:
#   ants_in_project_or_exit    — INV-9 per-project gate (sentinel-only).
#   ants_validate_session_id   — INV-2 sessionId regex validator.

# ANTS-1252-INV-9 — per-project gate. Walk up from $PWD looking for
# `.ants-project`, bail at $HOME (or `/`). NO `git rev-parse` fork:
# stat-only ascent stays under 1 ms typically. Reject sane-toplevel
# roots belt-and-braces (`/`, `/home`, `/root`, `/tmp`, `$HOME`).
ants_in_project_or_exit() {
    local dir="${PWD}"
    local home="${HOME:-/nonexistent}"
    local hops=0
    while [ -n "$dir" ] && [ "$dir" != "/" ] && [ "$dir" != "$home" ]; do
        case "$dir" in
            /|/home|/root|/tmp) exit 0 ;;
        esac
        if [ -e "$dir/.ants-project" ]; then
            ANTS_PROJECT_ROOT="$dir"
            export ANTS_PROJECT_ROOT
            return 0
        fi
        # Cap ascent; prevents pathological symlink loops.
        hops=$((hops + 1))
        if [ "$hops" -gt 32 ]; then exit 0; fi
        dir="$(dirname "$dir")"
    done
    exit 0
}

# ANTS-1252-INV-2 — sessionId validator. Reject anything outside
# `^[a-zA-Z0-9_-]{1,64}$` BEFORE any filesystem interpolation.
# Caller passes "$1" (the candidate id); we echo it back on success
# or `exit 0` silently on rejection so PreCompact / drift-check can't
# be coerced into path traversal via a hostile transcript.
ants_validate_session_id() {
    local sid="$1"
    case "$sid" in
        "" ) exit 0 ;;
    esac
    # POSIX-ish bracket pattern; bash glob is good enough here. Length
    # bound enforced by the second test.
    if ! printf '%s' "$sid" | LC_ALL=C grep -Eq '^[a-zA-Z0-9_-]{1,64}$'; then
        exit 0
    fi
    printf '%s' "$sid"
}
