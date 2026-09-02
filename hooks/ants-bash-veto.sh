#!/usr/bin/env bash
# ants-terminal PreToolUse(Bash) hook — token-saving veto + soft-warn.
# Spec: docs/specs/ANTS-1252.md § 1.2; soft-warn: docs/specs/ANTS-2141.md.
# Two routing classes:
#  - SOFT-WARN (ANTS-2141): a raw source search (grep -r / rg / project find)
#    → non-blocking PreToolUse `additionalContext` nudge toward the MCP index
#    verbs; the command still runs. Throttled per session, tallied always.
#  - BLOCK: git status / log → get_git_status; a --stat diff → git_state
#    op:diff (a diff needs CHANGED LINES, which get_git_status has no field
#    for); ROADMAP|grep → roadmap_query. Emit
#    `{"decision":"block","reason":"..."}`.
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
    # shellcheck disable=SC2016  # backticks are literal markdown, not a subshell
    ctx='Ants tip: that was a raw source search. Prefer mcp__ants__workspace_search (project-wide code search) / find_definition / find_sources — indexed, far fewer tokens than streaming grep/find output back. Append `# ants-bypass` to silence.'
    jq -nc --arg c "$ctx" \
        '{hookSpecificOutput:{hookEventName:"PreToolUse", additionalContext:$c}}'
    exit 0
fi

# ANTS-2023 — soft-warn a source read-dump (cat/head/tail/bat over a code file).
# Same non-blocking shape + shared throttle/counter as the grep/find nudge above;
# only the advice text + the tally-bucket fallback differ. Fail-open. See
# docs/specs/ANTS-2023.md §2.1-§2.3.
if ants_is_source_read "$cmd"; then
    nudge_tool="${ANTS_NUDGE_TOOL:-cat}"
    if ants_grep_nudge_throttled; then
        ants_grep_nudge_record "$nudge_tool" false   # counted, nudge suppressed
        exit 0
    fi
    ants_grep_nudge_record "$nudge_tool" true
    # shellcheck disable=SC2016  # backticks are literal markdown, not a subshell
    ctx='Ants tip: that dumped a whole source file. Prefer mcp__ants__file_outline (symbols/structure) or mcp__ants__read_region (a line range) — far fewer tokens than cat-ing the entire file back. Append `# ants-bypass` to silence.'
    jq -nc --arg c "$ctx" \
        '{hookSpecificOutput:{hookEventName:"PreToolUse", additionalContext:$c}}'
    exit 0
fi

# ANTS-2169 — the ROADMAP block below matches the literal filename anywhere in
# the command, so it wrongly fires when ROADMAP.md is a grep EXCLUSION
# (grep -v ROADMAP.md, --exclude=ROADMAP.md) rather than a file grep READS.
# roadmap_query is the cheaper path for `grep <pat> ROADMAP.md` (a read);
# filtering the file out of a pipe / file-list is a legitimate use the veto
# must not block. Detect the exclusion shape (grep … -v … ROADMAP.md, in that
# order) and mark it a non-read so the block below skips it.
roadmap_read=1
case "$cmd" in
    *"grep"*"-v"*"ROADMAP.md"*|*"--exclude"*"ROADMAP.md"*) roadmap_read=0 ;;
esac

# ANTS-4517 — same false-positive shape, on the git branch. The diff pattern
# below matched the phrase ANYWHERE in the command, so WRITING about a diff —
# an echo, a heredoc, a sed replacement, this file's own comments — was
# vetoed while running no git at all. It blocked the fix to itself twice.
#
# A real invocation sits at a COMMAND POSITION. Anchoring to the start of the
# whole string is the obvious over-correction: a pipeline's or a compound's
# later stage runs git exactly as its first does. So split on the separators
# and test each segment's head.
git_diff_stat=0
_scan=$(printf '%s' "$cmd" | tr '|;&()' '\n\n\n\n\n')
while IFS= read -r _seg; do
    _seg=${_seg#"${_seg%%[![:space:]]*}"}
    case "$_seg" in
        "git diff --stat"*) git_diff_stat=1 ;;
    esac
done <<SCAN
$_scan
SCAN

# Block branches — precise, low false-positive routing kept as hard vetoes.
reason=""
case "$cmd" in
    # A diff asks for CHANGED LINES. get_git_status cannot answer that at all
    # and roadmap_query is not a git verb, so the old shared reason sent the
    # caller to a dead end and then to `# ants-bypass` — the raw command the
    # veto exists to prevent. git_state op:diff is the verb that answers it
    # (ANTS-2074 working-tree, ANTS-3377 hunks); ANTS-2169 Part 2 identified it
    # but the message kept naming the wrong two.
    *"git diff --stat"*)
        [ "$git_diff_stat" -eq 1 ] && \
            reason='use mcp__ants__git_state op:"diff" — per-file added/removed counts (hunks:true for @@ headers); get_git_status is status-only and cannot answer a diff'
        ;;
    "git status"|"git status "*|"git log --oneline"*|"git log -n"*"--oneline"*)
        reason='use mcp__ants__get_git_status (status+branch+ahead/behind) or mcp__ants__git_state op:"log" — paginated git facts cost ~30 tokens vs raw stdout'
        ;;
    *"cat ROADMAP.md"*"grep"*|*"grep "*"ROADMAP.md"*)
        [ "$roadmap_read" -eq 1 ] && \
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
