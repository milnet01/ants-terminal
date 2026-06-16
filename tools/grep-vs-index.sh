#!/usr/bin/env bash
# tools/grep-vs-index.sh — ANTS-2141 "are we greping or querying the index?"
# readout. Prints the grep-nudge tally (the "grep side") that
# hooks/ants-bash-veto.sh records, and reminds you to compare it against the
# "index side" — mcp__ants__token_usage's per-verb n_calls for
# workspace_search / find_definition / find_sources.
#
# The counter is best-effort and may contain malformed lines under concurrent
# Bash tool calls (accepted lossy, see the spec) — this reader skips them.
#
# Usage: tools/grep-vs-index.sh [path-to-count.jsonl]
#   Default: ~/.cache/ants-terminal/grep-nudge/count.jsonl

set -u

f="${1:-${HOME:-/tmp}/.cache/ants-terminal/grep-nudge/count.jsonl}"

if [ ! -f "$f" ]; then
    echo "grep-nudge tally: none yet ($f absent)."
    echo "Run some grep/find searches in an Ants project first, or check the path."
    exit 0
fi

# Count only well-formed lines (must contain "tool" and "warned").
total=0; warned=0
declare -A by_tool
while IFS= read -r line; do
    case "$line" in *'"tool"'*'"warned"'*) ;; *) continue ;; esac
    total=$((total + 1))
    case "$line" in *'"warned":true'*) warned=$((warned + 1)) ;; esac
    tool="$(printf '%s' "$line" | sed -n 's/.*"tool":"\([^"]*\)".*/\1/p')"
    [ -n "$tool" ] && by_tool["$tool"]=$(( ${by_tool["$tool"]:-0} + 1 ))
done < "$f"

echo "grep-nudge tally  ($f)"
echo "  total source-searches : $total"
echo "  nudges emitted        : $warned   (the rest were throttle-suppressed)"
if [ "$total" -gt 0 ]; then
    echo "  by tool:"
    for t in "${!by_tool[@]}"; do
        printf '    %-10s %d\n' "$t" "${by_tool[$t]}"
    done
fi
echo
echo "Index side: call mcp__ants__token_usage and compare these counts against"
echo "the n_calls for workspace_search / find_definition / find_sources."
echo "A healthy ratio trends toward the index verbs over raw grep/find."
