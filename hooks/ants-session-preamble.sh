#!/usr/bin/env bash
# ants-terminal SessionStart hook — compact orientation block.
# Spec: docs/specs/ANTS-1252.md § 1.1
# Cap: ≤ 500 bytes total stdout (INV-3).
#
# Cost class: per-session injection (text-size-in-tokens).
# Injection contract: stdout is prepended to the model's first turn,
# so we emit ONLY structured `[ants:*]` lines and nothing else.

set -u

# shellcheck source=hooks/_common.sh disable=SC1091
. "$(dirname "$0")/_common.sh"

ants_in_project_or_exit
cd "$ANTS_PROJECT_ROOT" 2>/dev/null || exit 0

# Hard time budget: do not block Claude > 150 ms cold. If `git` /
# `jq` are missing we degrade silently per INV-1.
command -v git >/dev/null 2>&1 || exit 0

branch="$(git symbolic-ref --short HEAD 2>/dev/null || echo '?')"
upstream_state="$(git rev-list --left-right --count "@{u}...HEAD" 2>/dev/null || echo '0	0')"
behind="$(printf '%s' "$upstream_state" | awk '{print $1}')"
ahead="$(printf '%s' "$upstream_state" | awk '{print $2}')"
last_log="$(git log -1 --format='%s' 2>/dev/null | head -c 80)"

# Resume snapshot (set by PreCompact) — read it iff the session is the
# same one. Capped to 240 bytes so the project header always fits in
# the 500 B INV-3 envelope.
resume_line=""
sid="$(printf '%s' "${CLAUDE_SESSION_ID:-}" | LC_ALL=C tr -cd 'A-Za-z0-9_-' | head -c 64)"
if [ -n "$sid" ]; then
    snap="${HOME}/.cache/ants-terminal/precompact_${sid}.json"
    if [ -r "$snap" ] && command -v jq >/dev/null 2>&1; then
        resume_line="$(jq -r '
            if (.todos | type) == "array" then
                "[ants:resume] " + (
                    .todos
                    | map(select(.status == "in_progress" or .status == "pending"))
                    | map(.content)
                    | join("; ")
                    | .[0:200]
                )
            else empty end
        ' "$snap" 2>/dev/null | head -c 240)"
    fi
fi

# Compose under the 500 B cap. `head -c` on the final pipeline is the
# enforcement, but each line is already pre-trimmed.
{
    printf '[ants:project] branch=%s ahead=%s behind=%s last="%s"\n' \
        "$branch" "${ahead:-0}" "${behind:-0}" "$last_log"
    if [ -f .ants-tests-summary ]; then
        head -1 .ants-tests-summary | sed 's/^/[ants:tests] /' | head -c 120
        printf '\n'
    fi
    [ -n "$resume_line" ] && printf '%s\n' "$resume_line"
} | head -c 500

exit 0
