#!/usr/bin/env bash
# ANTS-4618 — does the global roadmap-format standard still carry every section
# this project's copy has?
#
#   tools/check-roadmap-format-containment.sh   # exit 1 on a missing section
#
# WHY. This project is upstream of ~/.claude/standards/roadmap-format.md
# (CFG-0069): the parser, store and migration live here, so the global copy is
# corrected to match this one, never the reverse. The two bodies legitimately
# differ in extent — the global copy split its CHANGELOG half into
# changelog-format.md — so a mirror check is wrong here and
# tools/check-standard-mirrors.sh skips this file (it carries no MIRROR BEGIN).
# That left drift invisible. This is the weaker check decided on 2026-09-07:
# every section heading (## to ####) of this copy must appear, verbatim, in
# the global roadmap-format.md or changelog-format.md. Extra global sections,
# and differing prose under a shared heading, are not checked.
#
# Headings inside fenced code blocks are examples, not sections, and are
# skipped on both sides.
#
# A checkout with no global standards tree — an outside contributor, CI —
# SKIPS. ANTS_GLOBAL_STANDARDS points at another tree (tests use it).
#
# Bypass for one commit: git commit --no-verify, or ANTS_PRECOMMIT_NO_MIRRORS=1
# from the pre-commit hook, which covers every check reading the owner tree.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "check-roadmap-format-containment: not a git checkout" >&2; exit 2; }
cd "$repo_root" || exit 2

local_copy="docs/standards/roadmap-format.md"
global_dir="${ANTS_GLOBAL_STANDARDS:-$HOME/.claude/standards}"
global_roadmap="$global_dir/roadmap-format.md"
global_changelog="$global_dir/changelog-format.md"

[ -r "$local_copy" ] || {
    echo "check-roadmap-format-containment: ⊘ SKIPPED — no $local_copy."; exit 0; }
[ -r "$global_roadmap" ] || {
    echo "check-roadmap-format-containment: ⊘ SKIPPED — no global copy at $global_roadmap."
    exit 0; }

# Section headings outside fenced code blocks, one per line.
headings() {
    awk '/^(```|~~~)/ { fence = !fence; next }
         !fence && /^#{2,4} / { print }' "$@"
}

global_sources=("$global_roadmap")
[ -r "$global_changelog" ] && global_sources+=("$global_changelog")

global_set="$(headings "${global_sources[@]}")"
missing=()
counted=0
while IFS= read -r h; do
    [ -n "$h" ] || continue
    counted=$((counted + 1))
    grep -qxF -- "$h" <<<"$global_set" || missing+=("$h")
done < <(headings "$local_copy")

if [ ${#missing[@]} -eq 0 ]; then
    echo "check-roadmap-format-containment: $counted sections, all in the global copy"
    exit 0
fi

echo "check-roadmap-format-containment: the global copy lacks these sections of $local_copy:" >&2
printf '    %s\n' "${missing[@]}" >&2
cat >&2 <<EOF

This project owns roadmap-format.md (CFG-0069), so the global copy at
$global_dir is the one to correct: add or retitle the section there, from a
~/.claude session. A section renamed here on purpose needs the same rename
there. Bypass for one commit: git commit --no-verify.
EOF
exit 1
