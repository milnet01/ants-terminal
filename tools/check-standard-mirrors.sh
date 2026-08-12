#!/usr/bin/env bash
# ANTS-4133 — is each mirrored global standard still identical to its owner?
#
#   tools/check-standard-mirrors.sh           # check; exit 1 on drift
#   tools/check-standard-mirrors.sh --write   # re-copy each owner into its mirror
#
# WHY a mirror exists at all: this repo is PUBLIC, and the four delta standards
# in docs/standards/ point at `~/.claude/standards/<name>.md` — a path an
# outside contributor cannot open, so for them the rule was simply absent. Each
# delta therefore carries a verbatim copy of its owner below the delta content.
#
# WHY this script rather than `~/.claude/.githooks/check-copied-standards`: that
# one asks the OPPOSITE question. It flags any project file overlapping a global
# standard as a breach — it already reports docs/standards/security.md, the
# sanctioned mirror, as "COPY 99%" and exits 1. Here the copy is intended; what
# must not happen is the copy going stale, which is precisely what happened to
# the four unmarked, unchecked copies these files replaced on 2026-08-12.
#
# The mirrored half of a file is the region between
#     <!-- MIRROR BEGIN <owner-path> -->
#     <!-- MIRROR END -->
# and must equal the owner byte for byte, minus the owner's own leading
# `<!-- ants-*: N -->` marker line (the mirror carries its own on line 1).
#
# A checkout with no ~/.claude/standards — an outside contributor, CI — SKIPS
# loudly and passes. The mirror is what such a reader has *instead of* the
# owner; there is nothing to check it against.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "check-standard-mirrors: not a git checkout" >&2; exit 2; }
cd "$repo_root" || exit 2

mode="check"
case "${1:-}" in
    ""|--check) ;;
    --write) mode="write" ;;
    *) echo "usage: ${0##*/} [--check|--write]" >&2; exit 2 ;;
esac

BEGIN_RE='^<!-- MIRROR BEGIN .* -->$'
END_RE='^<!-- MIRROR END -->$'

# The owner minus its own leading marker comment, which the mirror replaces
# with one of its own.
owner_body() { sed '1{/^<!-- ants-.*-->$/d;}' "$1"; }

status=0
checked=0
skipped=0

for f in docs/standards/*.md; do
    [ -f "$f" ] || continue
    grep -qE "$BEGIN_RE" "$f" || continue

    begin=$(grep -nE "$BEGIN_RE" "$f" | head -1 | cut -d: -f1)
    end=$(grep -nE "$END_RE" "$f" | head -1 | cut -d: -f1)
    if [ -z "$end" ] || [ "$end" -le "$begin" ]; then
        echo "$f: MIRROR BEGIN at line $begin has no MIRROR END after it" >&2
        status=1; continue
    fi

    owner=$(sed -n "${begin}p" "$f" | sed 's/^<!-- MIRROR BEGIN //; s/ -->$//')
    owner="${owner/#\~/$HOME}"
    if [ ! -f "$owner" ]; then
        echo "$f: SKIP — owner not present at $owner (nothing to check against)"
        skipped=$((skipped + 1)); continue
    fi

    if diff -q <(owner_body "$owner") \
               <(sed -n "$((begin + 1)),$((end - 1))p" "$f") >/dev/null; then
        checked=$((checked + 1)); continue
    fi

    if [ "$mode" = "write" ]; then
        tmp="$f.mirror.$$"
        { head -n "$begin" "$f"; owner_body "$owner"; tail -n +"$end" "$f"; } > "$tmp" \
            && mv "$tmp" "$f" \
            && echo "$f: refreshed from $owner"
        checked=$((checked + 1))
    else
        echo "$f: DRIFTED from $owner" >&2
        diff <(owner_body "$owner") \
             <(sed -n "$((begin + 1)),$((end - 1))p" "$f") | head -20 >&2
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    cat >&2 <<'EOF'

A mirrored standard no longer matches its owner. The mirror is a copy, not a
second standard: fix the owner under ~/.claude/standards/, then run

    tools/check-standard-mirrors.sh --write

to re-copy it down. Do NOT edit the text between the MIRROR markers.
Bypass for one commit: git commit --no-verify (or ANTS_PRECOMMIT_NO_MIRRORS=1).
EOF
    exit 1
fi

echo "check-standard-mirrors: $checked in sync, $skipped skipped"
exit 0
