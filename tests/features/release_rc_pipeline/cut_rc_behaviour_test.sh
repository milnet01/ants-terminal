#!/usr/bin/env bash
#
# Behavioural conformance for the ANTS-2164 cadence-hardening + ANTS-2165
# hotfix guards in packaging/cut-rc.sh. Drives the real script against
# throwaway git repos (QTemporaryDir-style) seeded with CMakeLists.txt +
# CHANGELOG.md + metainfo + debian/changelog + tags, asserting exit codes and
# the resulting file/tag state.
#
# Why a shell harness (not the C++ bundle): cut-rc.sh is bash, and the project
# already registers shell feature tests via add_test(COMMAND bash …)
# (tests/features/hook_pack/test_hooks.sh, claude_git_context_hook). Driving
# git + bash from C++ QProcess would only re-wrap what this does directly.
# The C++ test_release_rc_pipeline.cpp keeps the source-scrape structural
# invariants; this file is the behavioural layer.
#
# Each `git push`/`gh release create` is exercised against a per-repo bare
# origin + a no-op `gh` shim on PATH, with --skip-build (no cmake in a
# fixture). require_no_version_drift runs a per-repo drift stub.
#
# Exit 0 = every assertion held. Non-zero = a guard regressed.

set -u
# CUTRC overridable (env) so the suite can be pointed at a pre-fix copy to
# confirm the reproduce-before-fix property; defaults to the repo's script.
CUTRC="${CUTRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/packaging/cut-rc.sh}"
[ -f "$CUTRC" ] || { echo "cut-rc.sh not found at $CUTRC" >&2; exit 2; }

PASS=0; FAIL=0
ok()  { echo "  ok   — $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL — $1"; FAIL=$((FAIL + 1)); }
TODAY=$(date +%F)
PLACE="opens the 0.7.98 preview channel. No changes yet."
TMPROOT=$(mktemp -d)
trap 'rm -rf "$TMPROOT"' EXIT

seed_repo() {                       # $1 = repo dir
    local d=$1
    rm -rf "$d"; mkdir -p "$d/packaging/linux" "$d/packaging/debian" "$d/packaging/obs" "$d/.bin"
    # ANTS-3728 — the OBS recipe pins a tag; promote must bump it in lockstep.
    # Seeded one release BEHIND so a promote that fails to touch it is visible.
    printf '<services>\n  <service name="obs_scm">\n    <param name="revision">v0.7.97</param>\n  </service>\n</services>\n' \
        > "$d/packaging/obs/_service"
    git init -q -b main "$d"
    git -C "$d" config user.email t@example.com
    git -C "$d" config user.name  tester
    git init -q --bare "${d}-origin.git"
    git -C "$d" remote add origin "${d}-origin.git"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$d/.bin/gh"; chmod +x "$d/.bin/gh"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$d/packaging/check-version-drift.sh"
    chmod +x "$d/packaging/check-version-drift.sh"
    cp "$CUTRC" "$d/packaging/cut-rc.sh"; chmod +x "$d/packaging/cut-rc.sh"
}
write_cmake()  { printf 'project(ants-terminal VERSION %s LANGUAGES CXX)\n' "$2" > "$1/CMakeLists.txt"; }
write_metainfo() {                  # $1 dir $2 ver $3 body
    cat > "$1/packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml" <<EOF
<component>
  <releases>
    <release version="$2" date="2026-06-24">
      <description><p>$3</p></description>
    </release>
    <release version="0.7.97" date="2026-06-24">
      <description><p>Real notes.</p></description>
    </release>
  </releases>
</component>
EOF
}
write_debian() {                    # $1 dir $2 ver $3 body
    cat > "$1/packaging/debian/changelog" <<EOF
ants-terminal ($2-1) unstable; urgency=medium

  * $3

 -- Tester <t@example.com>  Wed, 24 Jun 2026 12:00:00 +0200

ants-terminal (0.7.97-1) unstable; urgency=medium

  * Real notes.

 -- Tester <t@example.com>  Wed, 24 Jun 2026 12:00:00 +0200
EOF
}
# $1 dir $2 unreleased-body(empty ok) $3 ver $4 ver-section-body
write_changelog() {
    local d=$1 unrel=$2 ver=$3 body=$4
    {
        echo "# Changelog"; echo
        echo "## [Unreleased]"; echo
        [ -n "$unrel" ] && printf '%s\n\n' "$unrel"
        echo "## [$ver] — unreleased (Patron RC preview)"; echo
        printf '%s\n\n' "$body"
        echo "## [0.7.97] — 2026-06-24"; echo
        echo "- Old stuff."; echo
    } > "$d/CHANGELOG.md"
}
commit_all() { git -C "$1" add -A; git -C "$1" commit -q -m "${2:-seed}"; }
run() {                             # $1 dir, rest = args → sets RC, OUT
    local d=$1; shift
    OUT=$(cd "$d" && PATH="$d/.bin:$PATH" bash packaging/cut-rc.sh "$@" 2>&1); RC=$?
}
has_tag() { git -C "$1" tag | grep -qx "$2"; }

# ── INV-1 — new-rc refuses an empty / placeholder [Unreleased] ───────────────
D=$TMPROOT/inv1; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" 0.7.98 "_${PLACE}_"
write_metainfo "$D" 0.7.98 "$PLACE"; write_debian "$D" 0.7.98 "$PLACE"; commit_all "$D"
run "$D" new-rc --skip-build --force-non-wed
{ [ "$RC" -ne 0 ] && grep -q "INV-1" <<<"$OUT"; } && ok "INV-1 empty new-rc refused" || bad "INV-1 (rc=$RC): $OUT"
run "$D" new-rc --skip-build --force-non-wed --allow-empty-rc --push
{ [ "$RC" -eq 0 ] && has_tag "$D" v0.7.98-rc1; } && ok "INV-1 --allow-empty-rc override" || bad "INV-1 override (rc=$RC): $OUT"

# ── INV-4 — new-rc rolls [Unreleased] into the [ver] section ─────────────────
D=$TMPROOT/inv4; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "### Added
- Shiny new thing." 0.7.98 "_${PLACE}_"
write_metainfo "$D" 0.7.98 "$PLACE"; write_debian "$D" 0.7.98 "$PLACE"; commit_all "$D"
run "$D" new-rc --skip-build --force-non-wed --push
if [ "$RC" -eq 0 ]; then
    sec=$(awk '/^## \[0.7.98\]/{f=1;next}/^## \[/{f=0}f' "$D/CHANGELOG.md")
    grep -q "Shiny new thing" <<<"$sec" && ok "INV-4 rolled into [0.7.98]" || bad "INV-4 roll missing"
    unrel=$(awk '/^## \[Unreleased\]/{f=1;next}/^## \[/{f=0}f' "$D/CHANGELOG.md")
    grep -q "Shiny" <<<"$unrel" && bad "INV-4 [Unreleased] not emptied" || ok "INV-4 [Unreleased] emptied"
else bad "INV-4 new-rc (rc=$RC): $OUT"; fi

# ── INV-2 — promote refuses a placeholder release ───────────────────────────
D=$TMPROOT/inv2; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" 0.7.98 "_${PLACE}_"
write_metainfo "$D" 0.7.98 "$PLACE"; write_debian "$D" 0.7.98 "$PLACE"; commit_all "$D"
git -C "$D" tag -a v0.7.98-rc1 -m rc1
run "$D" promote --force-non-wed
{ [ "$RC" -ne 0 ] && grep -q "INV-2" <<<"$OUT"; } && ok "INV-2 placeholder promote refused" || bad "INV-2 (rc=$RC): $OUT"

# ── INV-3 + INV-6 — promote stamps the carriers & tags the frozen RC commit ──
D=$TMPROOT/inv36; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" 0.7.98 "### Fixed
- Real fix that shipped."
write_metainfo "$D" 0.7.98 "Real preview notes."; write_debian "$D" 0.7.98 "Real preview notes."
commit_all "$D"; git -C "$D" tag -a v0.7.98-rc1 -m rc1
RC_COMMIT=$(git -C "$D" rev-parse v0.7.98-rc1^{commit})
echo extra > "$D/extra.txt"; commit_all "$D" "post-rc work"   # main ahead of RC
run "$D" promote --push --force-non-wed
if [ "$RC" -eq 0 ]; then
    has_tag "$D" v0.7.98 && ok "INV-6 public tag created" || bad "INV-6 no public tag"
    [ "$(git -C "$D" rev-parse v0.7.98^{commit})" = "$RC_COMMIT" ] \
        && ok "INV-6 public tags frozen RC commit (main ahead)" || bad "INV-6 wrong commit"
    grep -q "## \[0.7.98\] — ${TODAY}" "$D/CHANGELOG.md" && ok "INV-3 CHANGELOG stamped" || bad "INV-3 CHANGELOG"
    grep -q "version=\"0.7.98\" date=\"${TODAY}\"" "$D/packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml" \
        && ok "INV-3 metainfo stamped" || bad "INV-3 metainfo"
    RFCDAY=$(date -R -d "$TODAY" | sed 's/[+-][0-9]*$//')
    awk '/^ants-terminal \(0.7.98-/{f=1} f&&/^ -- /{print;exit}' "$D/packaging/debian/changelog" \
        | grep -q "$RFCDAY" && ok "INV-3 debian trailer stamped" || bad "INV-3 debian"
    # ANTS-3728 — a stale pin makes OBS rebuild the PREVIOUS release on a tag
    # push, which goes green and is wrong; assert promote moved it off v0.7.97.
    grep -q '<param name="revision">v0.7.98</param>' "$D/packaging/obs/_service" \
        && ok "INV-3 obs _service pinned to the published tag" || bad "INV-3 obs _service pin"
else bad "INV-3/6 promote (rc=$RC): $OUT"; fi

# ── INV-8 — RC age: 14 days promotes, ≥15 refuses ───────────────────────────
mk_aged_rc() {                      # $1 dir $2 days-old
    local d=$1 days=$2
    seed_repo "$d"; write_cmake "$d" 0.7.98
    write_changelog "$d" "" 0.7.98 "### Fixed
- Real fix."
    write_metainfo "$d" 0.7.98 "Real."; write_debian "$d" 0.7.98 "Real."; commit_all "$d"
    GIT_COMMITTER_DATE="$(date -d "${days} days ago" -R)" \
        git -C "$d" tag -a v0.7.98-rc1 -m rc1
}
D=$TMPROOT/inv8stale; mk_aged_rc "$D" 20
run "$D" promote --force-non-wed
{ [ "$RC" -ne 0 ] && grep -q "INV-8" <<<"$OUT"; } && ok "INV-8 stale (20d) refused" || bad "INV-8 stale (rc=$RC): $OUT"
D=$TMPROOT/inv8fresh; mk_aged_rc "$D" 14
run "$D" promote --push --force-non-wed
{ [ "$RC" -eq 0 ] && ! grep -q "INV-8" <<<"$OUT"; } && ok "INV-8 14-day boundary promotes" || bad "INV-8 14d (rc=$RC): $OUT"
D=$TMPROOT/inv8edge; mk_aged_rc "$D" 15
run "$D" promote --force-non-wed
{ [ "$RC" -ne 0 ] && grep -q "INV-8" <<<"$OUT"; } && ok "INV-8 15-day boundary refused" || bad "INV-8 15d (rc=$RC): $OUT"
D=$TMPROOT/inv8force; mk_aged_rc "$D" 20
run "$D" promote --push --force-non-wed --force-stale
{ [ "$RC" -eq 0 ]; } && ok "INV-8 --force-stale override" || bad "INV-8 force (rc=$RC): $OUT"

# ── INV-9 — new-rc refuses a base that is already public ─────────────────────
D=$TMPROOT/inv9; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "### Added
- thing." 0.7.98 "_${PLACE}_"
write_metainfo "$D" 0.7.98 "$PLACE"; write_debian "$D" 0.7.98 "$PLACE"; commit_all "$D"
git -C "$D" tag -a v0.7.98 -m pub
run "$D" new-rc --skip-build --force-non-wed
{ [ "$RC" -ne 0 ] && grep -q "INV-9" <<<"$OUT"; } && ok "INV-9 public-base new-rc refused" || bad "INV-9 (rc=$RC): $OUT"

# ── INV-7 — cycle self-skips (nothing to do) and runs both phases when ready ─
D=$TMPROOT/inv7skip; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" 0.7.98 "_${PLACE}_"
write_metainfo "$D" 0.7.98 "$PLACE"; write_debian "$D" 0.7.98 "$PLACE"; commit_all "$D"
run "$D" cycle --force-non-wed
{ [ "$RC" -eq 0 ] && grep -q "skip promote" <<<"$OUT" && grep -q "skip new-rc" <<<"$OUT"; } \
    && ok "INV-7 cycle self-skips cleanly" || bad "INV-7 skip (rc=$RC): $OUT"
# both-ready: in-flight RC at 0.7.98 (real notes), base pre-bumped to 0.7.99,
# [Unreleased] has content → promote 0.7.98 then cut 0.7.99-rc1.
D=$TMPROOT/inv7both; seed_repo "$D"; write_cmake "$D" 0.7.99
write_changelog "$D" "### Added
- next-cycle work." 0.7.98 "### Fixed
- shipped fix."
write_metainfo "$D" 0.7.98 "Real."; write_debian "$D" 0.7.98 "Real."; commit_all "$D"
git -C "$D" tag -a v0.7.98-rc1 -m rc1
run "$D" cycle --push --skip-build --force-non-wed
if [ "$RC" -eq 0 ]; then
    has_tag "$D" v0.7.98     && ok "INV-7 cycle promoted v0.7.98"   || bad "INV-7 promote tag"
    has_tag "$D" v0.7.99-rc1 && ok "INV-7 cycle cut v0.7.99-rc1"    || bad "INV-7 new-rc tag"
else bad "INV-7 both-ready (rc=$RC): $OUT"; fi

# ── ANTS-2165 INV-3 — hotfix refuses an off-main fix SHA ─────────────────────
D=$TMPROOT/hf_offmain; seed_repo "$D"; write_cmake "$D" 0.7.97
write_changelog "$D" "" 0.7.98 "_${PLACE}_"; write_metainfo "$D" 0.7.97 x; write_debian "$D" 0.7.97 x
commit_all "$D"; git -C "$D" tag -a v0.7.97 -m pub
git -C "$D" checkout -q -b side; echo y > "$D/y.txt"; commit_all "$D" offmain
OFFSHA=$(git -C "$D" rev-parse HEAD); git -C "$D" checkout -q main
run "$D" hotfix "$OFFSHA"
{ [ "$RC" -ne 0 ] && grep -q "fix_not_on_main" <<<"$OUT"; } && ok "2165 INV-3 off-main sha refused" || bad "2165 INV-3 (rc=$RC): $OUT"

# ── ANTS-2165 rc_base_mismatch — in-flight RC base must equal H ──────────────
D=$TMPROOT/hf_mismatch; seed_repo "$D"; write_cmake "$D" 0.7.99
write_changelog "$D" "" 0.7.99 "real" ; write_metainfo "$D" 0.7.99 x; write_debian "$D" 0.7.99 x
commit_all "$D"; git -C "$D" tag -a v0.7.97 -m pub
echo fix > "$D/fix.txt"; commit_all "$D" "the fix"; FIXSHA=$(git -C "$D" rev-parse HEAD)
git -C "$D" tag -a v0.7.99-rc1 -m rc1     # RC base 0.7.99 != H(0.7.98)
run "$D" hotfix "$FIXSHA"
{ [ "$RC" -ne 0 ] && grep -q "rc_base_mismatch" <<<"$OUT"; } && ok "2165 rc_base_mismatch refused" || bad "2165 mismatch (rc=$RC): $OUT"

# ── ANTS-2165 hotfix_branch_exists — abandoned _hotfix refused ───────────────
D=$TMPROOT/hf_branch; seed_repo "$D"; write_cmake "$D" 0.7.97
write_changelog "$D" "" 0.7.98 "_${PLACE}_"; write_metainfo "$D" 0.7.97 x; write_debian "$D" 0.7.97 x
commit_all "$D"; git -C "$D" tag -a v0.7.97 -m pub
echo fix > "$D/fix.txt"; commit_all "$D" "the fix"; FIXSHA=$(git -C "$D" rev-parse HEAD)
git -C "$D" branch _hotfix            # leftover from an interrupted run
run "$D" hotfix "$FIXSHA"
{ [ "$RC" -ne 0 ] && grep -q "hotfix_branch_exists" <<<"$OUT"; } && ok "2165 hotfix_branch_exists refused" || bad "2165 branch (rc=$RC): $OUT"

# ── ANTS-2165 INV-6 — cherry-pick conflict aborts cleanly ───────────────────
D=$TMPROOT/hf_conflict; seed_repo "$D"; write_cmake "$D" 0.7.97
write_changelog "$D" "" 0.7.98 "_${PLACE}_"; write_metainfo "$D" 0.7.97 x; write_debian "$D" 0.7.97 x
echo base > "$D/conflict.txt"; commit_all "$D"; git -C "$D" tag -a v0.7.97 -m pub
echo X > "$D/conflict.txt"; commit_all "$D" diverge
echo Y > "$D/conflict.txt"; commit_all "$D" "the fix"; FIXSHA=$(git -C "$D" rev-parse HEAD)
run "$D" hotfix "$FIXSHA"
if [ "$RC" -ne 0 ] && grep -q "INV-6" <<<"$OUT"; then
    onmain=$(git -C "$D" rev-parse --abbrev-ref HEAD)
    nobranch=$(git -C "$D" branch --list _hotfix)
    clean=$(git -C "$D" status --porcelain)
    { [ "$onmain" = main ] && [ -z "$nobranch" ] && [ -z "$clean" ]; } \
        && ok "2165 INV-6 conflict aborts clean (on main, no _hotfix)" || bad "2165 INV-6 dirty after abort"
else bad "2165 INV-6 conflict (rc=$RC): $OUT"; fi

# ── ANTS-2165 INV-1/2/4 — full hotfix (no RC): minimal tree, [H]>[N] on main ─
D=$TMPROOT/hf_full; seed_repo "$D"; write_cmake "$D" 0.7.97
{ echo "# Changelog"; echo; echo "## [Unreleased]"; echo; echo "## [0.7.97] — 2026-06-24"; echo; echo "- Old."; echo; } > "$D/CHANGELOG.md"
write_metainfo "$D" 0.7.97 x; write_debian "$D" 0.7.97 x; commit_all "$D" base
git -C "$D" tag -a v0.7.97 -m pub
echo fixline > "$D/fix.txt"; commit_all "$D" "the fix"; FIXSHA=$(git -C "$D" rev-parse HEAD)
echo feat > "$D/feat.txt"; commit_all "$D" "a feature"     # must NOT reach vH
run "$D" hotfix "$FIXSHA"
if [ "$RC" -eq 0 ] && git -C "$D" rev-parse --abbrev-ref HEAD | grep -q _hotfix; then
    ok "2165 hotfix phase1 on _hotfix"
    # assistant /bump _hotfix → 0.7.98 + write its [0.7.98] notes
    write_cmake "$D" 0.7.98
    { echo "# Changelog"; echo; echo "## [0.7.98] — unreleased"; echo; echo "### Fixed"; echo "- The urgent fix."; echo; echo "## [0.7.97] — 2026-06-24"; echo; echo "- Old."; echo; } > "$D/CHANGELOG.md"
    write_metainfo "$D" 0.7.98 "Hotfix notes."; write_debian "$D" 0.7.98 "Hotfix notes."
    git -C "$D" add -A; git -C "$D" commit -q -m "bump 0.7.98 hotfix"
    run "$D" hotfix --continue --push --skip-build
    if [ "$RC" -eq 0 ]; then
        has_tag "$D" v0.7.98 && ok "2165 vH public tag" || bad "2165 no vH tag: $OUT"
        git -C "$D" ls-tree -r --name-only v0.7.98 | grep -qx feat.txt \
            && bad "2165 INV-2 feature leaked into vH" || ok "2165 INV-2 minimal tree (no feature)"
        git -C "$D" ls-tree -r --name-only v0.7.98 | grep -qx fix.txt \
            && ok "2165 INV-2 fix present in vH" || bad "2165 INV-2 fix missing"
        git -C "$D" rev-parse --abbrev-ref HEAD | grep -q main && ok "2165 back on main" || bad "2165 not on main"
        ndup=$(grep -c '^## \[0.7.98\]' "$D/CHANGELOG.md")
        order=$(grep -E '^## \[0\.7\.9[78]\]' "$D/CHANGELOG.md" | tr '\n' '>')
        { [ "$ndup" = 1 ] && grep -q '0.7.98.*>.*0.7.97' <<<"$order"; } \
            && ok "2165 INV-4 main [0.7.98]>[0.7.97], single section" || bad "2165 INV-4 (dup=$ndup): $order"
    else bad "2165 hotfix --continue (rc=$RC): $OUT"; fi
else bad "2165 hotfix phase1 (rc=$RC): $OUT"; fi

echo
echo "release_rc_pipeline behavioural: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
