#!/usr/bin/env bash
# ants-terminal PreCompact hook — todo snapshot for next-session resume.
# Spec: docs/specs/ANTS-1252.md § 1.5
# Side-effect-only (INV-10 → zero stdout). Exits 0 always.
#
# Reads transcript via $CLAUDE_TRANSCRIPT_PATH (or falls back to the
# stdin payload's `transcript_path`), extracts the most recent
# TodoWrite snapshot + in-progress spec path, writes to
# ~/.cache/ants-terminal/precompact_<sessionId>.json.
#
# INV-2 — sessionId regex-validated BEFORE any filesystem
# interpolation. INV-9 — only fires inside a recognised project.

set -u

# shellcheck source=hooks/_common.sh disable=SC1091
. "$(dirname "$0")/_common.sh"

ants_in_project_or_exit

command -v jq >/dev/null 2>&1 || exit 0

# Stdin carries the hook envelope; we want sessionId + transcript_path.
payload="$(cat 2>/dev/null || true)"
[ -z "$payload" ] && exit 0

raw_sid="$(printf '%s' "$payload" | jq -r '.session_id // .sessionId // empty' 2>/dev/null)"
sid="$(ants_validate_session_id "$raw_sid")"
[ -z "$sid" ] && exit 0

transcript="$(printf '%s' "$payload" | jq -r '.transcript_path // empty' 2>/dev/null)"
[ -z "$transcript" ] && exit 0
[ -r "$transcript" ] || exit 0

cache_dir="${HOME}/.cache/ants-terminal"
mkdir -p "$cache_dir" 2>/dev/null || exit 0

# `sid` already matches `^[a-zA-Z0-9_-]{1,64}$` (INV-2) so it's safe
# to interpolate into the path.
out="$cache_dir/precompact_${sid}.json"
tmp="$out.tmp"

# Walk the JSONL transcript; remember the last TodoWrite payload.
# Bounded by `tail -n 5000` so a runaway transcript can't make this
# hook block compaction (caller's perceptible latency budget: 30 ms).
last_todos="$(tail -n 5000 "$transcript" 2>/dev/null \
    | jq -c 'select(.message.content[]?.input.todos? != null)
             | .message.content[].input.todos' 2>/dev/null \
    | tail -n 1)"

[ -z "$last_todos" ] && exit 0

jq -nc --argjson todos "$last_todos" \
    '{ts: now | todate, todos: $todos}' > "$tmp" 2>/dev/null || exit 0
mv -f "$tmp" "$out" 2>/dev/null || rm -f "$tmp" 2>/dev/null

exit 0
