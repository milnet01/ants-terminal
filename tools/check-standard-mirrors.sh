#!/usr/bin/env bash
# ANTS-4133 — is each mirrored global standard still identical to its owner?
#
#   tools/check-standard-mirrors.sh           # check; exit 1 on drift or dead link
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
# with one of its own, and minus the link syntax on any target this repo
# cannot carry.
#
# ANTS-4875 — a link out of a mirrored half that nothing mirrors is copied
# down as PLAIN TEXT, not as a link: a skeleton, or a target outside
# standards/ such as the owner's review history or a foundation document.
# It cannot resolve for the public reader the mirror exists to serve, and a
# link that 404s costs more than a named path — it also hides a real broken
# link among doc_integrity's reports. The link TEXT is kept, because at every
# such site it already names the path.
#
# This lives in owner_body, so the drift check and --write derive the mirror
# by the same rule: the copy stays byte-derivable from its owner, and no
# mirror is ever hand-edited. `languages/` is untouched — those files are in
# the mirror set and their links must resolve.
owner_body() {
    sed -E -e '1{/^<!-- ants-.*-->$/d;}' \
        -e 's/\[([^][]*)\]\((\.\.\/|skeletons\/)[A-Za-z0-9_.\/-]*\.md[^)]*\)/\1/g' "$1"
}

status=0
checked=0
skipped=0

# ANTS-4764 — mirrors live in subdirectories too (docs/standards/languages/),
# so the walk cannot be a flat glob.
mapfile -t STANDARD_FILES < <(find docs/standards -type f -name '*.md' | sort)

for f in "${STANDARD_FILES[@]}"; do
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

# ANTS-4761 - a link out of a mirrored half must resolve inside this folder.
# The mirror is verbatim, so a dead link here cannot be fixed by rewriting it:
# that would fail the drift check above. The only fix is to mirror the missing
# sibling, which is the rule docs/standards/README.md states.
#
# The set is a top-level sibling plus languages/, which ANTS-4764 added because
# the mirrored coding.md names those files as owning this project's C++, Qt and
# Python spellings. Out of the set by rule: a skeleton, which is a template to
# copy rather than a rule to conform to; a path leaving standards/ altogether;
# an illustrative path inside a quoted example; and README.md, which resolves to
# this index rather than the global one it means (ANTS-4138).
#
# ANTS-4825 — a target out of that set is skipped BY RULE, and the gate names
# it and says why instead of leaving it to be inferred from the path shape. It
# could not resolve: the owner is a foundation document or a skeleton that
# nothing mirrors, so a reader on GitHub got a 404, and a bare "links resolve"
# over a tree carrying one is what made this gate and doc_integrity look like
# they disagreed when each was answering its own question correctly.
#
# ANTS-4875 de-links those on the way down (see owner_body), so they now arrive
# as plain text and no longer reach this scan. What is left here is the net for
# a link shape the de-link does not match — it copies nothing and edits no
# mirror, and a non-zero count means owner_body needs widening.
dead_links=0
unmirrorable=0
unmirrorable_list=""
for f in "${STANDARD_FILES[@]}"; do
    [ -f "$f" ] || continue
    begin=$(grep -nE "$BEGIN_RE" "$f" | head -1 | cut -d: -f1)
    [ -n "$begin" ] || continue
    end=$(grep -nE "$END_RE" "$f" | head -1 | cut -d: -f1)
    { [ -n "$end" ] && [ "$end" -gt "$begin" ]; } || continue

    dir="${f%/*}"
    while read -r target; do
        [ -n "$target" ] || continue
        # Relative to the mirror's own directory, so a subdirectory mirror
        # resolves its siblings correctly.
        [ -e "$dir/$target" ] && continue
        # Classify by what is IN the mirror set, not by listing exclusions: a
        # top-level sibling, and languages/. Anything else carrying a directory
        # component is out of it by rule — a skeleton, a path leaving
        # standards/, or an illustrative path inside a quoted example.
        case "$target" in
            README.md) continue ;;   # resolves to this index (ANTS-4138)
            languages/*) ;;          # carried since ANTS-4764 — must resolve
            ../*)
                # Leaves docs/standards/ altogether. The owner is a foundation
                # document rather than a standard, so nothing mirrors it and
                # this link does not resolve for a public reader.
                unmirrorable=$((unmirrorable + 1))
                unmirrorable_list="${unmirrorable_list}
  $f -> $target (outside standards/; owner is not a standard, so nothing mirrors it)"
                continue ;;
            skeletons/*)
                # A skeleton is a template to copy, not a rule to conform to.
                unmirrorable=$((unmirrorable + 1))
                unmirrorable_list="${unmirrorable_list}
  $f -> $target (a skeleton is a template to copy, not a rule to conform to)"
                continue ;;
            */*) continue ;;
        esac
        echo "$f: DEAD LINK to '$target' - not mirrored into docs/standards/" >&2
        dead_links=$((dead_links + 1))
    done <<EOF
$(sed -n "$((begin + 1)),$((end - 1))p" "$f" \
    | grep -oE '\]\([A-Za-z0-9_./-]+\.md[^)]*\)' \
    | sed 's/^](//; s/[)#].*//' | sort -u)
EOF
done

if [ "$status" -ne 0 ]; then
    cat >&2 <<'EOF'

A mirrored standard no longer matches its owner. The mirror is a copy, not a
second standard: fix the owner under ~/.claude/standards/, then run

    tools/check-standard-mirrors.sh --write

to re-copy it down. Do NOT edit the text between the MIRROR markers.
Bypass for one commit: git commit --no-verify (or ANTS_PRECOMMIT_NO_MIRRORS=1).
EOF
fi

if [ "$dead_links" -ne 0 ]; then
    cat >&2 <<'XEOF'

A mirrored standard links to a global sibling this repo does not carry, so the
link dead-ends for the public reader the mirror exists to serve. Do NOT edit
the link: the mirror is verbatim and the drift gate refuses a copy that
differs from its owner. Mirror the missing sibling instead - add
docs/standards/<name>.md carrying the MIRROR markers, then run --write.
Bypass for one commit: git commit --no-verify (or ANTS_PRECOMMIT_NO_MIRRORS=1).
XEOF
fi

if [ "$status" -ne 0 ] || [ "$dead_links" -ne 0 ]; then
    exit 1
fi

if [ "$unmirrorable" -ne 0 ]; then
    echo "check-standard-mirrors: $checked in sync, $skipped skipped, mirrorable links resolve"
    echo "check-standard-mirrors: $unmirrorable link(s) cannot be mirrored and are skipped by rule."
    echo "  They do NOT resolve for a reader on GitHub — doc_integrity reports them, correctly:$unmirrorable_list"
else
    echo "check-standard-mirrors: $checked in sync, $skipped skipped, links resolve"
fi
exit 0
