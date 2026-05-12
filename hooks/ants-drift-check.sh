#!/usr/bin/env bash
# ants-terminal Stop hook — drift-check launcher.
# Spec: docs/specs/ANTS-1252.md § 1.4
# Side-effect-only (INV-10 → zero stdout). Exits 0 always.
#
# Architecture:
# - flock on PID-file (INV-11) prevents overlapping runs across the
#   ~2-3 s window the Qt6 ants-helper takes to scan the build.
# - Backgrounds the actual check so the hook returns within 100 ms.
# - Logs failures to ~/.cache/ants-terminal/drift-check.log so silent
#   backgrounding doesn't hide breakage (cold-eyes L8).
# - Writes `.ants-audit-pending` ONLY inside ANTS_PROJECT_ROOT (INV-9).

set -u

# shellcheck source=hooks/_common.sh disable=SC1091
. "$(dirname "$0")/_common.sh"

ants_in_project_or_exit

# We require `flock` (util-linux) and the ants-helper binary on PATH.
# Either missing → silent no-op per INV-1.
command -v flock >/dev/null 2>&1 || exit 0
command -v ants-helper >/dev/null 2>&1 || exit 0

cache_dir="${HOME}/.cache/ants-terminal"
mkdir -p "$cache_dir" 2>/dev/null || exit 0

lockfile="$cache_dir/drift-check.lock"
logfile="$cache_dir/drift-check.log"
project_root="$ANTS_PROJECT_ROOT"
marker="$project_root/.ants-audit-pending"

# INV-9 belt-and-braces: re-check that marker would land inside a sane
# project root, not at "/", "/home", "/root", "/tmp", or $HOME. Even
# though _common.sh's gate already enforces this, the marker write is
# the destructive step — re-checking here closes the race where a
# malicious symlink moves $ANTS_PROJECT_ROOT between the gate and now.
case "$project_root" in
    /|/home|/root|/tmp|"${HOME:-/nonexistent}") exit 0 ;;
esac
[ -d "$project_root" ] || exit 0

# Background the heavy lifting so Stop returns within 100 ms.
(
    # INV-11 — flock with -n: if another drift-check is still running,
    # bail without contention. Lock-fd 9 is the canonical bash idiom.
    exec 9>"$lockfile"
    if ! flock -n 9; then exit 0; fi

    # ants-helper writes a JSON object to stdout, exit code 3 = drift.
    if out="$(ants-helper drift-check --repo-root "$project_root" 2>>"$logfile")"; then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" -eq 3 ]; then
        printf '%s\n' "$out" > "$marker.tmp" 2>>"$logfile" \
            && mv -f "$marker.tmp" "$marker" 2>>"$logfile"
    elif [ "$rc" -eq 0 ]; then
        # Clean — clear stale marker if any.
        rm -f "$marker" 2>>"$logfile"
    else
        printf '[%s] ants-helper rc=%s out=%s\n' \
            "$(date -Iseconds)" "$rc" "$out" >>"$logfile" 2>/dev/null
    fi
) >/dev/null 2>&1 &

exit 0
