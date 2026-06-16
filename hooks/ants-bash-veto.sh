#!/usr/bin/env bash
# ants-terminal PreToolUse(Bash) hook — token-saving veto + soft-warn.
# Spec: docs/specs/ANTS-1252.md § 1.2; soft-warn: docs/specs/ANTS-2141.md.
# Two routing classes:
#  - SOFT-WARN (ANTS-2141): a raw source search (grep -r / rg / project find)
#    → non-blocking PreToolUse `additionalContext` nudge toward the MCP index
#    verbs; the command still runs. Throttled per session, tallied always.
#  - BLOCK (unchanged): git status / log / diff → get_git_status; ROADMAP|grep
#    → roadmap_query. Emit `{"decision":"block","reason":"..."}`.
# INV-4: block reason ≤ 200 B. INV-7: jq -r only. INV-12: bypass token
# never appears in the reason string.

set -u

# shellcheck source=hooks/_common.sh disable=SC1091
. "$(dirname "$0")/_common.sh"

ants_in_project_or_exit

command -v jq >/dev/null 2>&1 || exit 0

# INV-7 — parse via jq -r, never regex on raw JSON.
cmd="$(jq -r '.tool_input.command // empty' 2>/dev/null)"
[ -z "$cmd" ] && exit 0

# Override: trailing `# ants-bypass` comment. We intentionally do NOT
# echo this token into the reason text (INV-12) — readers learn about
# the override from hooks/README.md. Strip the comment before any
# pattern match so a Claude that learned the bypass gets through
# without the veto looking at "grep" inside the bypass marker.
case "$cmd" in
    *"# ants-bypass"*) exit 0 ;;
esac

# ANTS-2141 — soft-warn a raw source search (grep -r / rg / project find).
# NON-blocking: emit a PreToolUse `additionalContext` reminder and let the
# command run. Throttled per session (PPID); every eligible match is tallied
# even when the nudge is suppressed. Fail-open (a broken cache never silences
# the warn). See docs/specs/ANTS-2141.md §2.1-§2.4.
if ants_is_source_search "$cmd"; then
    nudge_tool="${ANTS_NUDGE_TOOL:-grep}"
    if ants_grep_nudge_throttled; then
        ants_grep_nudge_record "$nudge_tool" false   # counted, nudge suppressed
        exit 0
    fi
    ants_grep_nudge_record "$nudge_tool" true
    ctx='Ants tip: that was a raw source search. Prefer mcp__ants__workspace_search (project-wide code search) / find_definition / find_sources — indexed, far fewer tokens than streaming grep/find output back. Append `# ants-bypass` to silence.'
    jq -nc --arg c "$ctx" \
        '{hookSpecificOutput:{hookEventName:"PreToolUse", additionalContext:$c}}'
    exit 0
fi

# Block branches — precise, low false-positive routing kept as hard vetoes.
reason=""
case "$cmd" in
    "git status"|"git status "*|*"git diff --stat"*|"git log --oneline"*|"git log -n"*"--oneline"*)
        reason='use mcp__ants__get_git_status / mcp__ants__roadmap_query — paginated git facts cost ~30 tokens vs raw stdout'
        ;;
    *"cat ROADMAP.md"*"grep"*|*"grep "*"ROADMAP.md"*)
        reason='use mcp__ants__roadmap_query (status=active returns ~10x fewer tokens than full ROADMAP)'
        ;;
esac

[ -z "$reason" ] && exit 0

# Cap to 200 B (ANTS-1252 INV-4). printf truncation via `cut -c` is locale-safe.
reason="$(printf '%s' "$reason" | LC_ALL=C cut -c1-200)"

# Emit decision-block JSON. jq -nc keeps the payload compact and
# escapes the reason properly without us hand-rolling JSON.
jq -nc --arg r "$reason" '{decision:"block", reason:$r}'

exit 0
