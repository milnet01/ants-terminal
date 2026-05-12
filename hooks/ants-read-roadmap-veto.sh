#!/usr/bin/env bash
# ants-terminal PreToolUse(Read) hook — block full reads of ROADMAP.md.
# Spec: docs/specs/ANTS-1252.md § 1.3
# Triggers: tool=Read AND target == project ROADMAP.md AND size > 50 KiB
#           AND no `offset`/`limit` (partial reads are always allowed).
# INV-7: jq -r only. INV-12 has no analogue here (no in-band override).

set -u

# shellcheck source=hooks/_common.sh disable=SC1091
. "$(dirname "$0")/_common.sh"

ants_in_project_or_exit

command -v jq >/dev/null 2>&1 || exit 0

# INV-7 — jq -r per field, never raw-regex. Buffer stdin once: each
# `jq` consumes its FD, so three sequential reads would race.
payload="$(cat 2>/dev/null || true)"
[ -z "$payload" ] && exit 0
target="$(printf '%s' "$payload" | jq -r '.tool_input.file_path // empty' 2>/dev/null)"
offset="$(printf '%s' "$payload" | jq -r '.tool_input.offset // empty' 2>/dev/null)"
limit="$(printf '%s' "$payload" | jq -r '.tool_input.limit // empty' 2>/dev/null)"

[ -z "$target" ] && exit 0

# Partial-slice reads always allowed (per spec § 1.3 escape path).
if [ -n "$offset" ] || [ -n "$limit" ]; then
    exit 0
fi

# Resolve target relative to project root. The Read tool is documented
# to receive absolute paths, but we treat the bare filename as a hit
# too in case the contract loosens.
roadmap="$ANTS_PROJECT_ROOT/ROADMAP.md"
case "$target" in
    "$roadmap"|"ROADMAP.md") : ;;
    *) exit 0 ;;
esac

[ -r "$roadmap" ] || exit 0

# Size gate: only veto large ROADMAPs. `wc -c` is portable and
# byte-exact; 51200 = 50 KiB.
size="$(wc -c <"$roadmap" 2>/dev/null || echo 0)"
if [ "${size:-0}" -lt 51200 ]; then
    exit 0
fi

reason='use mcp__ants__roadmap_query (status=active saves ~10x tokens; status=all returns the full set at ~12 K vs ~123 K). Read with offset/limit also bypasses this veto.'
reason="$(printf '%s' "$reason" | LC_ALL=C cut -c1-200)"

jq -nc --arg r "$reason" '{decision:"block", reason:$r}'

exit 0
