#!/usr/bin/env bash
# ANTS-4762 — does docs/standards/README.md still name every standard beside it?
#
#   tools/check-standards-index.sh    # exit 1 on an unindexed standard
#
# WHY. That README calls itself the index, and it had drifted into omitting
# several files. Four of those were named in no other document either — not in
# it, not in CLAUDE.md — so a session adding a QMenu read CLAUDE.md, followed
# the standards it names, and never learned menus.md existed. It could not tell
# it had breached a rule it had no route to.
#
# A hand-maintained table cannot notice a new file landing beside it, which is
# why this is a check rather than a habit. It needs no ~/.claude and no network,
# so it runs for an outside contributor exactly as it runs here.
#
# Bypass for one commit: git commit --no-verify.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "check-standards-index: not a git checkout" >&2; exit 2; }
cd "$repo_root" || exit 2

index="docs/standards/README.md"
[ -r "$index" ] || {
    echo "check-standards-index: ⊘ SKIPPED — no $index to check."; exit 0; }

missing=()
counted=0
for f in docs/standards/*.md; do
    [ -f "$f" ] || continue
    base="${f##*/}"
    [ "$base" = "README.md" ] && continue
    counted=$((counted + 1))
    grep -qF "$base" "$index" || missing+=("$base")
done

if [ ${#missing[@]} -eq 0 ]; then
    echo "check-standards-index: $counted standards, all named in $index"
    exit 0
fi

echo "check-standards-index: $index does not name:" >&2
printf '    %s\n' "${missing[@]}" >&2
cat >&2 <<'EOF'

That file calls itself the index, so a standard missing from it is one a
session has no route to and cannot tell it has breached. Add a row for each
to the table it belongs in, describing what the standard covers.
Bypass for one commit: git commit --no-verify.
EOF
exit 1
