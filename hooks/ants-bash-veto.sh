#!/usr/bin/env bash
# ants-terminal PreToolUse(Bash) hook — token-saving veto.
# Spec: docs/specs/ANTS-1252.md § 1.2
# Decision: emit JSON `{"decision":"block","reason":"..."}` to stdout
# only when redirecting to an MCP tool would save >300 tokens.
# INV-4: reason ≤ 200 bytes. INV-7: jq -r only. INV-12: bypass token
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

reason=""

# Heuristics — in priority order, first match wins. Each reason ≤ 200 B.
case "$cmd" in
    *"grep -r "*"src/"* | *"grep -R "*"src/"* | \
    *"find src/"*-name* | *"find src "*-name* )
        reason='use mcp__ants__workspace_search (ripgrep wrapper, ~80% fewer tokens than streaming grep output back through the model)'
        ;;
    "git status"|"git status "*|*"git diff --stat"*|"git log --oneline"*|"git log -n"*"--oneline"*)
        reason='use mcp__ants__get_git_status / mcp__ants__roadmap_query — paginated git facts cost ~30 tokens vs raw stdout'
        ;;
    *"cat ROADMAP.md"*"grep"*|*"grep "*"ROADMAP.md"*)
        reason='use mcp__ants__roadmap_query (status=active returns ~10x fewer tokens than full ROADMAP)'
        ;;
esac

[ -z "$reason" ] && exit 0

# Cap to 200 B (INV-4). printf truncation via `cut -c` is locale-safe.
reason="$(printf '%s' "$reason" | LC_ALL=C cut -c1-200)"

# Emit decision-block JSON. jq -nc keeps the payload compact and
# escapes the reason properly without us hand-rolling JSON.
jq -nc --arg r "$reason" '{decision:"block", reason:$r}'

exit 0
