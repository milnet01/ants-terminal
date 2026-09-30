#!/usr/bin/env bash
#
# Behavioural conformance for tests/features/release_pipeline/spec.md
# (INV-1 to INV-16, INV-19 to INV-21): drives the real packaging/release.sh and
# packaging/release-notes.sh against throwaway git repos.
#
# Why this exists: ANTS-5577 retired the RC cadence for one `release` command.
# The retired roll was a no-op when the version section already had entries,
# and the retired promote published the release before its files were attached
# and tagged undated notes. Each case below locks one of those routes shut.
#
# Fixture: a repo per case with a bare origin, CMakeLists.txt, CHANGELOG.md,
# metainfo, debian/changelog and obs/_service. The stubs are `gh` (on PATH,
# outside the repo) and packaging/obs/obs-submit.sh, obs-status.sh and
# check-version-drift.sh (inside the repo, as the script finds them by relative
# path). The stubs log every call to ${dir}-log and take their exit codes from
# STUB_* environment variables. The gh stub tells the ci.yml run (STUB_CI_ID,
# STUB_CI_RC, STUB_CI_NONE, STUB_CI_EMPTY_FIRST) from the release.yml run
# (id 12345, STUB_RUN_RC, STUB_RUN_NONE, STUB_RUN_EMPTY_FIRST).
# STUB_CI_AFTER_DISPATCH: the ci.yml lookup returns an id only once the stub has
# seen `gh workflow run ci.yml`. Every gh call, ci.yml lookups and dispatches
# included, is in gh.log; the ci-* lines in obs.log carry the tags on origin at
# that moment, so order against the gate and the tag is readable. --skip-build always: a fixture has no cmake.
#
# Exit 0 = every assertion held. Non-zero = a guard regressed.

set -u

# ANTS-3841 — a shell test does not pass through a bundle main, so it scrubs
# the git environment itself; otherwise `git init` lands on the caller's repo.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RELEASE="${RELEASE:-$ROOT/packaging/release.sh}"
RELEASE_NOTES="${RELEASE_NOTES:-$ROOT/packaging/release-notes.sh}"
[ -f "$RELEASE" ] || { echo "release.sh not found at $RELEASE" >&2; exit 2; }

PASS=0; FAIL=0
ok()  { echo "  ok   — $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL — $1"; FAIL=$((FAIL + 1)); }
# expect <label> <expected> <actual>: prints both on a mismatch.
expect() { [ "$2" = "$3" ] && ok "$1" || bad "$1 — expected [$2], actual [$3]"; }

TODAY=$(date +%F)
TODAY_DEB=$(LC_ALL=C date -d "$TODAY" '+%a, %d %b %Y')
TODAY_RFC=$(LC_ALL=C date -d "$TODAY" -R)
STAGING_PROJECT="home:milnet:ants-terminal-staging"
THEME='**Theme:** A short plain-language summary of this release.'
TMPROOT=$(mktemp -d)
trap 'rm -rf "$TMPROOT"' EXIT

# ── fixture helpers ──────────────────────────────────────────────────────────
seed_repo() {                       # $1 = repo dir
    local d=$1
    rm -rf "$d" "${d}-bin" "${d}-log" "${d}-hooks" "${d}-origin.git"
    mkdir -p "$d/packaging/linux" "$d/packaging/debian" "$d/packaging/obs" \
             "${d}-bin" "${d}-log"
    printf '<services>\n  <service name="obs_scm">\n    <param name="revision">v0.7.97</param>\n  </service>\n</services>\n' \
        > "$d/packaging/obs/_service"
    git init -q -b main "$d"
    git -C "$d" config user.email t@example.com
    git -C "$d" config user.name  tester
    git -C "$d" config core.hooksPath /dev/null    # no machine-wide hooks in a fixture
    git init -q --bare "${d}-origin.git"
    git -C "$d" remote add origin "${d}-origin.git"

    # gh: logs every call; exit codes and outputs come from STUB_* variables.
    cat > "${d}-bin/gh" <<'EOF'
#!/usr/bin/env bash
echo "gh $*" >> "$STUB_LOG_DIR/gh.log"
case "${1-} ${2-}" in
  "run list")
      # Two workflows, told apart by the --workflow value. ci.yml is GitHub's
      # CI run for the commit (id STUB_CI_ID); anything else is the release.yml
      # run (id 12345). Each has its own look counter. A CI lookup also writes
      # one line to obs.log so the order against the OBS calls and the tag on
      # origin can be read from one file.
      wf=release
      [[ "$*" == *"--workflow ci.yml"* ]] && wf=ci
      n=$(cat "$STUB_LOG_DIR/$wf.list.n" 2>/dev/null || echo 0); n=$((n + 1))
      echo "$n" > "$STUB_LOG_DIR/$wf.list.n"
      if [ "$wf" = ci ]; then
          printf 'ci-lookup look=%s origin_tags=[%s]\n' "$n" \
              "$(git -C "$STUB_ORIGIN" tag -l | tr '\n' ' ')" >> "$STUB_LOG_DIR/obs.log"
          [ -n "${STUB_CI_NONE:-}" ] && exit 0
          [ "$n" -le "${STUB_CI_EMPTY_FIRST:-0}" ] && exit 0
          # STUB_CI_AFTER_DISPATCH: no run exists until `workflow run ci.yml`
          # was called (ci.yml skips a push that touches only docs).
          if [ -n "${STUB_CI_AFTER_DISPATCH:-}" ] && [ ! -e "$STUB_LOG_DIR/ci.dispatched" ]; then exit 0; fi
          echo "${STUB_CI_ID:-777}"; exit 0
      fi
      [ -n "${STUB_RUN_NONE:-}" ] && exit 0
      [ "$n" -le "${STUB_RUN_EMPTY_FIRST:-0}" ] && exit 0
      echo 12345; exit 0;;
  "workflow run")
      printf 'ci-dispatch origin_tags=[%s]\n' \
          "$(git -C "$STUB_ORIGIN" tag -l | tr '\n' ' ')" >> "$STUB_LOG_DIR/obs.log"
      [[ "$*" == *ci.yml* ]] && touch "$STUB_LOG_DIR/ci.dispatched"
      exit "${STUB_DISPATCH_RC:-0}";;
  "run watch")
      # The exit code follows the run id: the CI run's id gets STUB_CI_RC,
      # every other id (the release.yml run) gets STUB_RUN_RC.
      if [ "${3-}" = "${STUB_CI_ID:-777}" ]; then
          printf 'ci-watch origin_tags=[%s]\n' \
              "$(git -C "$STUB_ORIGIN" tag -l | tr '\n' ' ')" >> "$STUB_LOG_DIR/obs.log"
          exit "${STUB_CI_RC:-0}"
      fi
      exit "${STUB_RUN_RC:-0}";;
  "release view")
      if [[ "$*" == *isDraft* ]]; then echo false; exit 0; fi
      if [[ "$*" == *assets* ]]; then
          v=${3#v}
          printf '%s\n' "Ants_Terminal-$v-x86_64.AppImage" \
              "Ants_Terminal-$v-x86_64.AppImage.zsync" \
              "Ants_Terminal-$v-x86_64.AppImage.manifest" \
              "Ants_Terminal-$v-x86_64.AppImage.manifest.sig" \
              "Ants_Terminal-x86_64.AppImage" \
              | grep -vFx -- "${STUB_ASSET_OMIT:-__none__}"
          exit 0
      fi
      exit 0;;
esac
exit 0
EOF
    cat > "$d/packaging/obs/obs-submit.sh" <<'EOF'
#!/usr/bin/env bash
printf 'submit args=[%s] proj=%s origin_main=%s origin_tags=[%s]\n' "$*" "${OBS_PROJECT-}" \
    "$(git -C "$STUB_ORIGIN" rev-parse main 2>/dev/null)" \
    "$(git -C "$STUB_ORIGIN" tag -l | tr '\n' ' ')" >> "$STUB_LOG_DIR/obs.log"
if [ "${1-}" = "--staging" ]; then exit "${STUB_STAGING_SUBMIT_RC:-0}"; fi
exit "${STUB_REAL_SUBMIT_RC:-0}"
EOF
    cat > "$d/packaging/obs/obs-status.sh" <<'EOF'
#!/usr/bin/env bash
printf 'status args=[%s] proj=%s origin_main=%s origin_tags=[%s]\n' "$*" "${OBS_PROJECT-}" \
    "$(git -C "$STUB_ORIGIN" rev-parse main 2>/dev/null)" \
    "$(git -C "$STUB_ORIGIN" tag -l | tr '\n' ' ')" >> "$STUB_LOG_DIR/obs.log"
if [[ "$*" == *--require-tests* ]]; then exit "${STUB_STAGING_STATUS_RC:-0}"; fi
exit "${STUB_REAL_STATUS_RC:-0}"
EOF
    printf '#!/usr/bin/env bash\nexit 0\n' > "$d/packaging/check-version-drift.sh"
    chmod +x "${d}-bin/gh" "$d/packaging/obs/obs-submit.sh" "$d/packaging/obs/obs-status.sh" \
             "$d/packaging/check-version-drift.sh"
    cp "$RELEASE" "$d/packaging/release.sh"; chmod +x "$d/packaging/release.sh"
    [ -f "$RELEASE_NOTES" ] && { cp "$RELEASE_NOTES" "$d/packaging/release-notes.sh"; chmod +x "$d/packaging/release-notes.sh"; }
    return 0
}
write_cmake()  { printf 'project(ants-terminal VERSION %s LANGUAGES CXX)\n' "$2" > "$1/CMakeLists.txt"; }
write_metainfo() {                  # $1 dir $2 ver $3 date-of-$2
    cat > "$1/packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml" <<EOF
<component>
  <releases>
    <release version="$2" date="$3">
      <description><p>Notes for $2.</p></description>
    </release>
    <release version="0.7.97" date="2026-06-24">
      <description><p>Real notes.</p></description>
    </release>
  </releases>
</component>
EOF
}
write_debian() {                    # $1 dir $2 ver $3 RFC-date-of-$2
    cat > "$1/packaging/debian/changelog" <<EOF
ants-terminal ($2-1) unstable; urgency=medium

  * Notes for $2.

 -- Tester <t@example.com>  $3

ants-terminal (0.7.97-1) unstable; urgency=medium

  * Real notes.

 -- Tester <t@example.com>  Wed, 24 Jun 2026 12:00:00 +0200
EOF
}
# $1 dir, $2 [Unreleased] body (may be empty), $3 full version heading line
# (empty = no version section), $4 version-section body.
write_changelog() {
    local d=$1 unrel=$2 vhdr=$3 vbody=$4
    {
        echo "# Changelog"; echo
        echo "## [Unreleased]"; echo
        [ -n "$unrel" ] && printf '%s\n\n' "$unrel"
        if [ -n "$vhdr" ]; then echo "$vhdr"; echo; printf '%s\n\n' "$vbody"; fi
        echo "## [0.7.97] — 2026-06-24"; echo
        echo "- Old stuff."; echo
    } > "$d/CHANGELOG.md"
}
commit_local() { git -C "$1" add -A; git -C "$1" commit -q -m "${2:-seed}"; }
commit_all()   { commit_local "$@"; git -C "$1" push -q origin main; }
# A release-ready repo: Theme + one Added entry in [Unreleased], no 0.7.98 section.
std_repo() {
    local d=$1
    seed_repo "$d"; write_cmake "$d" 0.7.98
    write_changelog "$d" "$THEME
### Added
- Shiny new thing." "" ""
    write_metainfo "$d" 0.7.98 2026-06-24; write_debian "$d" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"
    commit_all "$d"
}
run() {                             # $1 dir, rest = args → sets RC, OUT
    local d=$1; shift
    OUT=$(cd "$d" && env -u OBS_PROJECT -u OBS_API PATH="${d}-bin:$PATH" \
            STUB_LOG_DIR="${d}-log" STUB_ORIGIN="${d}-origin.git" \
            RELEASE_POLL_SECS=0 RELEASE_POLL_MAX="${RELEASE_POLL_MAX:-3}" \
            timeout 90 bash packaging/release.sh "$@" 2>&1); RC=$?
}
FULL=(release --push --skip-build)
NOSTAGE=(release --push --skip-build --skip-staging)

snap() {                            # everything a refusal or rehearsal must not change
    local d=$1
    echo "head=$(git -C "$d" rev-parse HEAD)" \
         "branch=$(git -C "$d" rev-parse --abbrev-ref HEAD)" \
         "tags=$(git -C "$d" tag -l | tr '\n' ,)" \
         "status=$(git -C "$d" status --porcelain | md5sum)" \
         "origin=$(git -C "${d}-origin.git" rev-parse main 2>/dev/null)" \
         "otags=$(git -C "${d}-origin.git" tag -l | tr '\n' ,)" \
         "files=$(cd "$d" && cat CHANGELOG.md CMakeLists.txt packaging/linux/*.xml packaging/debian/changelog packaging/obs/_service | md5sum)"
}
has_tag()  { git -C "$1" tag -l | grep -qx "$2"; }
has_otag() { git -C "${1}-origin.git" tag -l | grep -qx "$2"; }
section() {                         # $1 dir $2 version-or-Unreleased → section body
    awk -v h="## [$2]" 'index($0,h)==1{f=1;next} /^## \[/{f=0} f' "$1/CHANGELOG.md"
}
lineno() { grep -nF -m1 -- "$2" <<<"$1" | cut -d: -f1; }
before() {                          # $1 text $2 a $3 b → a appears above b
    local la lb; la=$(lineno "$1" "$2"); lb=$(lineno "$1" "$3")
    [ -n "$la" ] && [ -n "$lb" ] && [ "$la" -lt "$lb" ]
}
deb_trailer() { awk -v v="ants-terminal ($2-1)" 'index($0,v)==1{f=1} f&&/^ -- /{print;exit}' "$1/packaging/debian/changelog"; }
obs_log() { cat "${1}-log/obs.log" 2>/dev/null; }
gh_log()  { cat "${1}-log/gh.log" 2>/dev/null; }

# ── INV-1 — refuses off main and with a dirty tree ───────────────────────────
D=$TMPROOT/inv1a; std_repo "$D"; git -C "$D" checkout -q -b side
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && grep -qi 'main' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-1 off main refused, nothing changed" || bad "INV-1 off main (rc=$RC): $OUT"
D=$TMPROOT/inv1b; std_repo "$D"; echo junk > "$D/untracked.txt"
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && grep -qiE 'dirty|uncommitted|clean' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-1 untracked file refused, nothing changed" || bad "INV-1 untracked (rc=$RC): $OUT"
D=$TMPROOT/inv1c; std_repo "$D"; echo "extra" >> "$D/CHANGELOG.md"
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && grep -qiE 'dirty|uncommitted|clean' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-1 modified tracked file refused, nothing changed" || bad "INV-1 modified (rc=$RC): $OUT"

# ── INV-2 — a vX.Y.Z tag at another commit means already public ──────────────
D=$TMPROOT/inv2; std_repo "$D"
git -C "$D" tag -a v0.7.98 -m public
echo more > "$D/more.txt"; commit_all "$D" "after the public tag"
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && grep -qi 'bump' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-2 already-public version refused, says bump" || bad "INV-2 (rc=$RC): $OUT"

# ── INV-3 — nothing to release ───────────────────────────────────────────────
D=$TMPROOT/inv3a; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "$THEME" "" ""           # Theme only: no bullet, no category
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-3 empty [Unreleased], no version section: refused" || bad "INV-3a (rc=$RC): $OUT"
D=$TMPROOT/inv3b; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" "## [0.7.98] — unreleased" "$THEME

_Placeholder, no changes yet._"
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-3 empty [Unreleased], placeholder version section: refused" || bad "INV-3b (rc=$RC): $OUT"

# ── INV-4 — no Theme line means refuse, and write nothing ────────────────────
D=$TMPROOT/inv4a; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "### Added
- Shiny new thing." "" ""
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
S0=$(snap "$D"); run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && grep -qi 'theme' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-4 missing Theme refused, nothing written" || bad "INV-4 no theme (rc=$RC): $OUT"
# Theme already in [0.7.98], entries in [Unreleased]: accepted, Theme kept.
D=$TMPROOT/inv4b; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "### Added
- Shiny new thing." "## [0.7.98] — unreleased" "$THEME

### Fixed
- Old fix."
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
run "$D" "${NOSTAGE[@]}"
sec=$(section "$D" 0.7.98)
{ [ "$RC" -eq 0 ] && grep -qF '**Theme:**' <<<"$sec" && grep -qF 'Shiny new thing' <<<"$sec"; } \
    && ok "INV-4 Theme in the version section accepted and kept" || bad "INV-4 theme in section (rc=$RC): $OUT"
# Theme at the top of [Unreleased]: accepted and moved into the section.
D=$TMPROOT/inv4c; std_repo "$D"; run "$D" "${NOSTAGE[@]}"
sec=$(section "$D" 0.7.98)
{ [ "$RC" -eq 0 ] && grep -qF '**Theme:**' <<<"$sec"; } \
    && ok "INV-4 Theme from [Unreleased] ends up in the version section" || bad "INV-4 theme moved (rc=$RC): $OUT"

# ── INV-5 — merge, including a version section that already has entries ─────
D=$TMPROOT/inv5a; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "$THEME
### Added
- new-add
### Fixed
- new-fix" "## [0.7.98] — unreleased (Patron RC preview)" "### Added
- old-add

### Changed
- old-change

### Fixed
- old-fix"
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
run "$D" "${NOSTAGE[@]}"
if [ "$RC" -eq 0 ]; then
    sec=$(section "$D" 0.7.98); unrel=$(section "$D" Unreleased)
    expect "INV-5 exactly one [0.7.98] heading" 1 "$(grep -c '^## \[0\.7\.98\]' "$D/CHANGELOG.md")"
    expect "INV-5 [Unreleased] heading remains" 1 "$(grep -c '^## \[Unreleased\]' "$D/CHANGELOG.md")"
    { ! grep -qE '^- |^### ' <<<"$unrel"; } && ok "INV-5 [Unreleased] holds no entries" || bad "INV-5 [Unreleased] still has entries: $unrel"
    all=1; for b in new-add new-fix old-add old-change old-fix; do grep -qF -- "- $b" <<<"$sec" || { all=0; bad "INV-5 bullet lost: $b"; }; done
    [ "$all" = 1 ] && ok "INV-5 old and new bullets all under [0.7.98]"
    expect "INV-5 one ### Added" 1 "$(grep -c '^### Added$' <<<"$sec")"
    expect "INV-5 one ### Changed" 1 "$(grep -c '^### Changed$' <<<"$sec")"
    expect "INV-5 one ### Fixed" 1 "$(grep -c '^### Fixed$' <<<"$sec")"
    { before "$sec" "- new-add" "- old-add" && before "$sec" "- new-fix" "- old-fix"; } \
        && ok "INV-5 new bullets precede old within a category" || bad "INV-5 bullet order: $sec"
    { before "$sec" "### Added" "### Changed" && before "$sec" "### Changed" "### Fixed"; } \
        && ok "INV-5 category order Added, Changed, Fixed" || bad "INV-5 category order: $sec"
else bad "INV-5 merge run (rc=$RC): $OUT"; fi
D=$TMPROOT/inv5b; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "$THEME
### Security
- sec-1

### Removed
- rem-1

### Deprecated
- dep-1

### Changed
- chg-1

### Added
- add-1

### Fixed
- fix-1" "" ""
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
run "$D" "${NOSTAGE[@]}"
sec=$(section "$D" 0.7.98); okorder=1; prev=""
for c in Added Changed Deprecated Removed Fixed Security; do
    [ -n "$prev" ] && { before "$sec" "### $prev" "### $c" || okorder=0; }; prev=$c
done
{ [ "$RC" -eq 0 ] && [ "$okorder" = 1 ]; } \
    && ok "INV-5 six categories ordered Added..Security" || bad "INV-5 six-category order (rc=$RC): $sec"

# ── INV-6 — dating and stamping ──────────────────────────────────────────────
D=$TMPROOT/inv6a; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" "## [0.7.98] — unreleased (Patron RC preview)" "$THEME

### Fixed
- Real fix."
write_metainfo "$D" 0.7.98 2026-06-24; write_debian "$D" 0.7.98 "Wed, 24 Jun 2026 12:00:00 +0200"; commit_all "$D"
CMAKE0=$(cat "$D/CMakeLists.txt"); run "$D" "${NOSTAGE[@]}"
if [ "$RC" -eq 0 ]; then
    grep -Fxq "## [0.7.98] - $TODAY" "$D/CHANGELOG.md" && ok "INV-6 heading rewritten to ASCII '- date'" || bad "INV-6 heading: $(grep '^## \[0.7.98' "$D/CHANGELOG.md")"
    grep -Fxq '## [0.7.97] — 2026-06-24' "$D/CHANGELOG.md" && ok "INV-6 other versions' headings untouched" || bad "INV-6 0.7.97 heading changed"
    grep -qF "<release version=\"0.7.98\" date=\"$TODAY\"" "$D/packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml" \
        && ok "INV-6 metainfo dated" || bad "INV-6 metainfo"
    grep -qF '<release version="0.7.97" date="2026-06-24"' "$D/packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml" \
        && ok "INV-6 metainfo 0.7.97 untouched" || bad "INV-6 metainfo 0.7.97 changed"
    grep -qF "$TODAY_DEB" <<<"$(deb_trailer "$D" 0.7.98)" && ok "INV-6 debian trailer dated" || bad "INV-6 debian trailer: $(deb_trailer "$D" 0.7.98) (want $TODAY_DEB)"
    grep -qF '24 Jun 2026' <<<"$(deb_trailer "$D" 0.7.97)" && ok "INV-6 debian 0.7.97 trailer untouched" || bad "INV-6 debian 0.7.97 trailer changed"
    grep -qF '<param name="revision">v0.7.98</param>' "$D/packaging/obs/_service" \
        && ok "INV-6 obs _service pinned to v0.7.98" || bad "INV-6 _service: $(cat "$D/packaging/obs/_service")"
    expect "INV-6 CMakeLists.txt version untouched" "$CMAKE0" "$(cat "$D/CMakeLists.txt")"
else bad "INV-6 run (rc=$RC): $OUT"; fi
D=$TMPROOT/inv6b; std_repo "$D"; run "$D" "${NOSTAGE[@]}"     # no section yet: heading created dated
{ [ "$RC" -eq 0 ] && grep -Fxq "## [0.7.98] - $TODAY" "$D/CHANGELOG.md"; } \
    && ok "INV-6 new heading created as '- date'" || bad "INV-6 fresh heading (rc=$RC): $(grep '^## \[' "$D/CHANGELOG.md")"

# ── INV-7/8/9/10 — full flow on a good tree ──────────────────────────────────
D=$TMPROOT/full; std_repo "$D"; BASE=$(git -C "$D" rev-parse HEAD)
run "$D" "${FULL[@]}"
if [ "$RC" -eq 0 ]; then
    HEAD1=$(git -C "$D" rev-parse HEAD)
    expect "INV-7 exactly one new commit" 1 "$(git -C "$D" rev-list --count "$BASE..HEAD")"
    files=$(git -C "$D" diff --name-only "$BASE" HEAD | sort | tr '\n' ' ')
    exp_files="CHANGELOG.md packaging/debian/changelog packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml packaging/obs/_service "
    expect "INV-7 commit touches only the four carriers (no version edit)" "$exp_files" "$files"
    expect "INV-7 main pushed to origin" "$HEAD1" "$(git -C "${D}-origin.git" rev-parse main)"
    sub=$(obs_log "$D" | grep 'submit args=\[--staging ' || true)
    expect "INV-7 one staging submit call" 1 "$(grep -c . <<<"$sub")"
    gate_sha=$(sed -n 's/.*args=\[--staging \([0-9a-f]*\) .*/\1/p' <<<"$sub")
    origin_then=$(sed -n 's/.* origin_main=\([0-9a-f]*\) .*/\1/p' <<<"$sub")
    expect "INV-7 gate sha is a full sha" 40 "${#gate_sha}"
    expect "INV-7 gate sha equals origin/main at that moment" "$origin_then" "$gate_sha"
    expect "INV-7 gate sha is the release commit" "$HEAD1" "$gate_sha"
    grep -qF 'args=[--staging '"$gate_sha"' 0.7.98]' <<<"$sub" && ok "INV-8 staging submit args '--staging <sha> 0.7.98'" || bad "INV-8 staging args: $sub"
    stat=$(obs_log "$D" | grep 'status args=\[--require-tests\]' || true)
    [ -n "$stat" ] && ok "INV-8 staging status run with --require-tests" || bad "INV-8 no 'obs-status.sh --require-tests' call: $(obs_log "$D")"
    stg=$(printf '%s\n%s\n' "$sub" "$stat")
    { ! grep -v "proj=$STAGING_PROJECT " <<<"$stg" | grep -q .; } \
        && ok "INV-8 both gate calls carry OBS_PROJECT=$STAGING_PROJECT" || bad "INV-8 gate project: $stg"
    { ! grep -q 'v0.7.98' <<<"$stg"; } && ok "INV-8 gate ran before any tag on origin" || bad "INV-8 tag existed at gate time: $stg"
    expect "INV-9 tag is annotated" tag "$(git -C "$D" cat-file -t v0.7.98 2>/dev/null)"
    expect "INV-9 tag at the gate-tested commit" "$gate_sha" "$(git -C "$D" rev-parse 'v0.7.98^{commit}' 2>/dev/null)"
    expect "INV-9 tag pushed to origin at the same commit" "$gate_sha" "$(git -C "${D}-origin.git" rev-parse 'v0.7.98^{commit}' 2>/dev/null)"
    expect "INV-9 no rc tag locally" "" "$(git -C "$D" tag -l '*rc*')"
    expect "INV-9 no rc tag on origin" "" "$(git -C "${D}-origin.git" tag -l '*rc*')"
    real_sub=$(obs_log "$D" | grep 'submit args=\[\] ' || true)
    real_stat=$(obs_log "$D" | grep 'status args=\[\] ' || true)
    { [ -n "$real_sub" ] && [ -n "$real_stat" ]; } && ok "INV-9 real-project submit and status ran with no arguments" || bad "INV-9 real calls missing: $(obs_log "$D")"
    { grep -q 'v0.7.98' <<<"$real_sub" && ! grep -q "proj=$STAGING_PROJECT " <<<"$real_sub"; } \
        && ok "INV-9 real submit ran after the tag push, not against staging" || bad "INV-9 real submit: $real_sub"
    gl=$(gh_log "$D")
    { ! grep -q 'release create' <<<"$gl"; } && ok "INV-10 no gh release create" || bad "INV-10 gh release create called: $gl"
    grep -E 'run list ' <<<"$gl" | grep -F -- '--workflow release.yml' | grep -qF 'v0.7.98' && ok "INV-10 looks up the workflow run" || bad "INV-10 run list: $gl"
    grep -qF 'run watch 12345 --exit-status' <<<"$gl" && ok "INV-10 waits on the run with --exit-status" || bad "INV-10 run watch: $gl"
    grep -q 'release view v0.7.98 --json isDraft' <<<"$gl" && grep -q 'release view v0.7.98 --json assets' <<<"$gl" \
        && ok "INV-10 verifies the release (draft state and assets)" || bad "INV-10 release view: $gl"
else bad "INV-7..10 full run (rc=$RC): $OUT"; fi
# The run appears late: polling finds it.
D=$TMPROOT/poll; std_repo "$D"
STUB_RUN_EMPTY_FIRST=2 RELEASE_POLL_MAX=5 run "$D" "${FULL[@]}"
{ [ "$RC" -eq 0 ] && grep -qF 'run watch 12345 --exit-status' <<<"$(gh_log "$D")"; } \
    && ok "INV-10 polls until the workflow run appears" || bad "INV-10 late run (rc=$RC): $OUT"

# ── INV-8 — red staging gate, skip, missing script ──────────────────────────
D=$TMPROOT/inv8a; std_repo "$D"
STUB_STAGING_STATUS_RC=1 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && ! has_tag "$D" v0.7.98 && ! has_otag "$D" v0.7.98 && grep -qi 'staging' <<<"$OUT"; } \
    && ok "INV-8 red staging status: non-zero, no tag, names staging" || bad "INV-8 red status (rc=$RC): $OUT"
{ ! obs_log "$D" | grep -q 'args=\[\] '; } && ok "INV-8 real OBS never touched after a red gate" || bad "INV-8 real OBS called: $(obs_log "$D")"
{ ! gh_log "$D" | grep -qF -- 'ci.yml'; } && ok "INV-21 no CI lookup after a red staging gate" || bad "INV-21 CI looked up after a red gate: $(gh_log "$D")"
D=$TMPROOT/inv8b; std_repo "$D"
STUB_STAGING_SUBMIT_RC=1 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && ! has_tag "$D" v0.7.98 && ! has_otag "$D" v0.7.98 && grep -qi 'staging' <<<"$OUT"; } \
    && ok "INV-8 red staging submit: non-zero, no tag, names staging" || bad "INV-8 red submit (rc=$RC): $OUT"
D=$TMPROOT/inv8c; std_repo "$D"
STUB_STAGING_SUBMIT_RC=1 STUB_STAGING_STATUS_RC=1 run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -eq 0 ] && grep -qi 'warn' <<<"$OUT" && ! obs_log "$D" | grep -qE -- '--staging|--require-tests' && has_otag "$D" v0.7.98; } \
    && ok "INV-8 --skip-staging: gate not called, warning printed, release proceeds" || bad "INV-8 skip (rc=$RC): $OUT | $(obs_log "$D")"
D=$TMPROOT/inv8d; std_repo "$D"; chmod -x "$D/packaging/obs/obs-status.sh"; commit_all "$D" "status not executable"
run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && ! has_tag "$D" v0.7.98 && ! has_otag "$D" v0.7.98; } \
    && ok "INV-8 non-executable gate script refuses, no tag" || bad "INV-8 not executable (rc=$RC): $OUT"
D=$TMPROOT/inv8e; std_repo "$D"; git -C "$D" rm -q packaging/obs/obs-submit.sh; commit_all "$D" "submit missing"
run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && ! has_tag "$D" v0.7.98 && ! has_otag "$D" v0.7.98; } \
    && ok "INV-8 missing gate script refuses, no tag" || bad "INV-8 missing (rc=$RC): $OUT"

# ── INV-11 — re-run after a red gate merges the fix entry ───────────────────
D=$TMPROOT/inv11; std_repo "$D"
STUB_STAGING_STATUS_RC=1 run "$D" "${FULL[@]}"
[ "$RC" -ne 0 ] || bad "INV-11 setup: gate was meant to be red (rc=$RC)"
echo fix > "$D/fix.txt"
awk '{print} /^## \[Unreleased\]$/{print ""; print "### Added"; print "- second-thing"}' "$D/CHANGELOG.md" > "$D/CHANGELOG.new" \
    && mv "$D/CHANGELOG.new" "$D/CHANGELOG.md"
commit_local "$D" "fix after red gate"
run "$D" "${FULL[@]}"
if [ "$RC" -eq 0 ]; then
    sec=$(section "$D" 0.7.98)
    expect "INV-11 one [0.7.98] heading" 1 "$(grep -c '^## \[0\.7\.98\]' "$D/CHANGELOG.md")"
    expect "INV-11 one ### Added" 1 "$(grep -c '^### Added$' <<<"$sec")"
    { grep -qF -- '- Shiny new thing.' <<<"$sec" && before "$sec" "- second-thing" "- Shiny new thing."; } \
        && ok "INV-11 new entry merged beside the earlier one, new first" || bad "INV-11 merge: $sec"
    expect "INV-11 tag at the new HEAD" "$(git -C "$D" rev-parse HEAD)" "$(git -C "$D" rev-parse 'v0.7.98^{commit}' 2>/dev/null)"
    expect "INV-11 origin tag at the new HEAD" "$(git -C "$D" rev-parse HEAD)" "$(git -C "${D}-origin.git" rev-parse 'v0.7.98^{commit}' 2>/dev/null)"
else bad "INV-11 second run (rc=$RC): $OUT"; fi

# ── INV-12 — resume: tag at HEAD, not pushed ────────────────────────────────
D=$TMPROOT/inv12; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" "## [0.7.98] - $TODAY" "$THEME

### Added
- Shiny new thing."
write_metainfo "$D" 0.7.98 "$TODAY"; write_debian "$D" 0.7.98 "$TODAY_RFC"
sed -i 's/v0.7.97/v0.7.98/' "$D/packaging/obs/_service"
commit_all "$D" "release 0.7.98"; git -C "$D" tag -a v0.7.98 -m "0.7.98"
H0=$(git -C "$D" rev-parse HEAD)
run "$D" "${FULL[@]}"
{ [ "$RC" -eq 0 ] && [ "$(git -C "$D" rev-parse HEAD)" = "$H0" ] \
    && [ "$(git -C "${D}-origin.git" rev-parse 'v0.7.98^{commit}' 2>/dev/null)" = "$H0" ]; } \
    && ok "INV-12 resume: no refusal, no new commit, tag pushed" || bad "INV-12 (rc=$RC): $OUT"
grep -qF 'run watch 12345 --exit-status' <<<"$(gh_log "$D")" \
    && ok "INV-12 resume carries on to the workflow wait" || bad "INV-12 did not carry on: $(gh_log "$D")"

# ── INV-13 / ANTS-4865 — a refused commit aborts by name ────────────────────
D=$TMPROOT/inv13; std_repo "$D"
mkdir -p "${D}-hooks"
printf '#!/usr/bin/env bash\necho "pre-commit: mirror drift — refusing" >&2\nexit 1\n' > "${D}-hooks/pre-commit"
chmod +x "${D}-hooks/pre-commit"; git -C "$D" config core.hooksPath "${D}-hooks"
O0=$(git -C "${D}-origin.git" rev-parse main)
run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && grep -q 'ANTS-4865' <<<"$OUT" && ! has_tag "$D" v0.7.98 && ! has_otag "$D" v0.7.98; } \
    && ok "INV-13 refused commit aborts with ANTS-4865 diagnostic, no tag" || bad "INV-13 abort (rc=$RC): $OUT"
git -C "$D" diff --cached --quiet && bad "INV-13 written files were discarded, not left staged" || ok "INV-13 written files left staged"
expect "INV-13 origin main unchanged" "$O0" "$(git -C "${D}-origin.git" rev-parse main)"

# ── INV-14 — failures after the tag push keep the tag and say what to fix ───
D=$TMPROOT/inv14a; std_repo "$D"
STUB_REAL_SUBMIT_RC=1 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qi 'obs' <<<"$OUT"; } \
    && ok "INV-14 failing real obs-submit: tag kept, non-zero, names OBS" || bad "INV-14 submit (rc=$RC): $OUT"
D=$TMPROOT/inv14b; std_repo "$D"
STUB_REAL_STATUS_RC=1 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qi 'obs' <<<"$OUT"; } \
    && ok "INV-14 failing real obs-status: tag kept, non-zero, names OBS" || bad "INV-14 status (rc=$RC): $OUT"
D=$TMPROOT/inv14c; std_repo "$D"
STUB_RUN_RC=1 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qiE 'workflow|release\.yml' <<<"$OUT"; } \
    && ok "INV-14 failed workflow run: tag kept, non-zero, names the workflow" || bad "INV-14 run failed (rc=$RC): $OUT"
D=$TMPROOT/inv14d; std_repo "$D"
STUB_RUN_NONE=1 RELEASE_POLL_MAX=2 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qiE 'workflow|release\.yml' <<<"$OUT"; } \
    && ok "INV-14 workflow run never appears: tag kept, non-zero, names the workflow" || bad "INV-14 no run (rc=$RC): $OUT"
D=$TMPROOT/inv14e; std_repo "$D"
STUB_ASSET_OMIT="Ants_Terminal-0.7.98-x86_64.AppImage.manifest.sig" run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qi 'release' <<<"$OUT"; } \
    && ok "INV-14 release missing .manifest.sig: tag kept, non-zero, names the release" || bad "INV-14 asset sig (rc=$RC): $OUT"
D=$TMPROOT/inv14f; std_repo "$D"
STUB_ASSET_OMIT="Ants_Terminal-x86_64.AppImage" run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qi 'release' <<<"$OUT"; } \
    && ok "INV-14 release missing the stable-name AppImage: tag kept, non-zero, names the release" || bad "INV-14 asset stable (rc=$RC): $OUT"

# ── INV-15 — rehearsal mutates nothing; status is read-only ─────────────────
D=$TMPROOT/inv15; std_repo "$D"
S0=$(snap "$D"); run "$D" release --skip-build --skip-staging
{ [ "$RC" -eq 0 ] && grep -qF '[rehearsal]' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ] && [ -z "$(git -C "$D" status --porcelain)" ]; } \
    && ok "INV-15 rehearsal: prints [rehearsal], changes nothing" || bad "INV-15 rehearsal (rc=$RC): $OUT"
run "$D" release --skip-build
{ [ "$RC" -eq 0 ] && grep -qF '[rehearsal]' <<<"$OUT" && [ "$(snap "$D")" = "$S0" ]; } \
    && ok "INV-15 rehearsal with staging enabled changes nothing" || bad "INV-15 rehearsal staging (rc=$RC): $OUT"
run "$D" status
{ [ "$RC" -eq 0 ] && [ "$(snap "$D")" = "$S0" ]; } && ok "INV-15 status is read-only" || bad "INV-15 status (rc=$RC): $OUT"

# ── INV-16 — release-notes.sh ────────────────────────────────────────────────
NOTES_D=$TMPROOT/notes; mkdir -p "$NOTES_D"
notes() { OUT=$(cd "$NOTES_D" && bash "$RELEASE_NOTES" "$1" 2>&1); RC=$?; }
if [ -f "$RELEASE_NOTES" ]; then
    cat > "$NOTES_D/CHANGELOG.md" <<'EOF'
# Changelog

## [Unreleased]

## [0.7.98] - 2026-09-30

**Theme:** One release, plain words.

A second lead paragraph.

### Added
- entry-a-should-not-appear

### Fixed
- entry-b-should-not-appear

## [0.7.97] — 2026-06-24

**Theme:** Older lead.

### Fixed
- old-entry

## [0.7.96] - 2026-06-01

Lead with no categories at all.

## [0.7.95] - 2026-05-01

**Theme:** Never shown for 0.7.96.
EOF
    notes 0.7.98
    LINK='https://github.com/milnet01/ants-terminal/blob/v0.7.98/CHANGELOG.md'
    first=$(grep -m1 . <<<"$OUT")
    { [ "$RC" -eq 0 ] && [ "$first" = "One release, plain words." ]; } && ok "INV-16 lead starts with the Theme text, label removed" || bad "INV-16 first line (rc=$RC): [$first]"
    { grep -qF 'A second lead paragraph.' <<<"$OUT" && ! grep -qF 'Theme:' <<<"$OUT"; } && ok "INV-16 whole lead kept, no Theme label" || bad "INV-16 lead: $OUT"
    { ! grep -qE 'entry-[ab]-should-not-appear|^### |^- ' <<<"$OUT"; } && ok "INV-16 no category entries" || bad "INV-16 entries leaked: $OUT"
    { ! grep -qF 'Older lead' <<<"$OUT"; } && ok "INV-16 no text from other sections" || bad "INV-16 other section leaked: $OUT"
    grep -qF "$LINK" <<<"$OUT" && ok "INV-16 links the tagged CHANGELOG" || bad "INV-16 link missing: $OUT"
    notes 0.7.97
    { [ "$RC" -eq 0 ] && grep -qF 'Older lead.' <<<"$OUT" && ! grep -qF 'Theme:' <<<"$OUT" && grep -qF 'blob/v0.7.97/CHANGELOG.md' <<<"$OUT"; } \
        && ok "INV-16 accepts an em dash date heading" || bad "INV-16 em dash (rc=$RC): $OUT"
    notes 0.7.96
    { [ "$RC" -eq 0 ] && grep -qF 'Lead with no categories at all.' <<<"$OUT" && ! grep -qF 'Never shown' <<<"$OUT"; } \
        && ok "INV-16 a section with no ### stops at the next ## heading" || bad "INV-16 no categories (rc=$RC): $OUT"
    notes 9.9.9
    [ "$RC" -ne 0 ] && ok "INV-16 unknown version exits non-zero" || bad "INV-16 unknown version exited 0: $OUT"
else bad "INV-16 packaging/release-notes.sh not found at $RELEASE_NOTES"; fi

# ── INV-19 — only status/release and the three flags ────────────────────────
D=$TMPROOT/inv19; std_repo "$D"; S0=$(snap "$D")
for args in "new-rc" "respin" "promote" "cycle" "hotfix" "release --bogus" "status --force-non-wed" "release --push --allow-empty-rc"; do
    ERR=$(cd "$D" && PATH="${D}-bin:$PATH" STUB_LOG_DIR="${D}-log" STUB_ORIGIN="${D}-origin.git" \
            timeout 60 bash packaging/release.sh $args 2>&1 >/dev/null); RC=$?
    { [ "$RC" -eq 2 ] && grep -qi 'usage' <<<"$ERR" && [ "$(snap "$D")" = "$S0" ]; } \
        && ok "INV-19 '$args' → exit 2, usage on stderr" || bad "INV-19 '$args' (rc=$RC): $ERR"
done

# ── INV-20 — a version-drift failure refuses ────────────────────────────────
D=$TMPROOT/inv20; std_repo "$D"
printf '#!/usr/bin/env bash\necho "drift: carriers disagree" >&2\nexit 1\n' > "$D/packaging/check-version-drift.sh"
commit_all "$D" "drift stub fails"; S0=$(snap "$D")
run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -ne 0 ] && [ "$(snap "$D")" = "$S0" ]; } && ok "INV-20 version drift refuses, nothing changed" || bad "INV-20 (rc=$RC): $OUT"

# ── INV-21 — CI gate: read GitHub's verdict on the tested commit before tagging
ci_lists()   { gh_log "$1" | grep -cF -- 'run list --workflow ci.yml' || true; }
ci_watches() { gh_log "$1" | grep -cF -- 'run watch 777 ' || true; }
ci_disp()    { gh_log "$1" | grep -cF -- 'workflow run' || true; }
oline()      { grep -n -m1 -- "$2" "${1}-log/obs.log" 2>/dev/null | cut -d: -f1; }
names_ci()   { grep -qiE '\bci\b|ci\.yml' <<<"$1"; }
# refused_by_ci <label> <dir> <rc> <out>: the shape point 4 of INV-21 requires.
refused_by_ci() {
    local l=$1 d=$2 rc=$3 out=$4
    { [ "$rc" -ne 0 ] && ! has_tag "$d" v0.7.98 && ! has_otag "$d" v0.7.98 && names_ci "$out"; } \
        && ok "$l: non-zero, no tag locally or on origin, names CI" || bad "$l (rc=$rc, tag local=$(has_tag "$d" v0.7.98 && echo y || echo n) origin=$(has_otag "$d" v0.7.98 && echo y || echo n)): $out"
    { ! obs_log "$d" | grep -q 'args=\[\] '; } && ok "$l: real OBS never touched" || bad "$l: real OBS called: $(obs_log "$d")"
    { ! gh_log "$d" | grep -qF -- '--workflow release.yml'; } && ok "$l: release.yml never looked up" || bad "$l: release.yml looked up: $(gh_log "$d")"
    expect "$l: main keeps the pushed release commit" "$(git -C "$d" rev-parse HEAD)" "$(git -C "${d}-origin.git" rev-parse main)"
}

# Green CI on the first look: the release carries on; lookup is for the tested
# commit, after the gate, before the tag; no CI run is started.
D=$TMPROOT/inv21ok; std_repo "$D"; BASE=$(git -C "$D" rev-parse HEAD)
run "$D" "${FULL[@]}"
if [ "$RC" -eq 0 ]; then
    HEAD1=$(git -C "$D" rev-parse HEAD); gl=$(gh_log "$D")
    [ "$HEAD1" != "$BASE" ] || bad "INV-21 setup: no release commit was made"
    grep -qF -- "run list --workflow ci.yml --commit $HEAD1 --limit 1" <<<"$gl" \
        && ok "INV-21 looks up the ci.yml run for the full sha of the tested commit" || bad "INV-21 lookup args (want --commit $HEAD1): $gl"
    grep -qF -- 'run watch 777 --exit-status' <<<"$gl" && ok "INV-21 waits on the CI run with --exit-status" || bad "INV-21 no CI watch: $gl"
    expect "INV-21 no CI run started when one existed at the first look" 0 "$(ci_disp "$D")"
    expect "INV-21 one CI lookup when the run exists at once" 1 "$(ci_lists "$D")"
    st=$(oline "$D" 'status args=\[--require-tests\]'); cl=$(oline "$D" 'ci-lookup'); cw=$(oline "$D" 'ci-watch'); rs=$(oline "$D" 'submit args=\[\] ')
    { [ -n "$st" ] && [ -n "$cl" ] && [ -n "$cw" ] && [ -n "$rs" ] && [ "$st" -lt "$cl" ] && [ "$cl" -lt "$cw" ] && [ "$cw" -lt "$rs" ]; } \
        && ok "INV-21 order: staging gate, CI lookup, CI watch, real OBS submit" || bad "INV-21 order (status=$st lookup=$cl watch=$cw real-submit=$rs): $(obs_log "$D")"
    { [ -n "$cl" ] && ! grep -E '^ci-' "${D}-log/obs.log" | grep -q 'origin_tags=\[[^]]*v0\.7\.98'; } \
        && ok "INV-21 no tag on origin during the CI lookup and watch" || bad "INV-21 tag existed during the CI check: $(obs_log "$D")"
    { has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && grep -qF -- 'run watch 12345 --exit-status' <<<"$gl"; } \
        && ok "INV-21 green CI: tag pushed and the release.yml wait still runs" || bad "INV-21 green CI did not carry on: $gl"
else bad "INV-21 green run (rc=$RC): $OUT"; fi

# Red CI run: no tag, real OBS untouched, output names CI.
D=$TMPROOT/inv21red; std_repo "$D"
STUB_CI_RC=1 run "$D" "${FULL[@]}"
refused_by_ci "INV-21 red CI run" "$D" "$RC" "$OUT"
expect "INV-21 red CI run: watched the CI run" 1 "$(ci_watches "$D")"

# Only the release.yml run is red: CI is green, so the tag goes out and is kept (INV-14).
D=$TMPROOT/inv21rel; std_repo "$D"
STUB_RUN_RC=1 run "$D" "${FULL[@]}"
{ [ "$RC" -ne 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98 && [ "$(ci_watches "$D")" = 1 ]; } \
    && ok "INV-21 a red release.yml run is not read as a red CI run" || bad "INV-21 release.yml red (rc=$RC): $OUT"

# No CI run ever appears, even after one is started: looks twice MAX times,
# starts one run, gives up with no tag.
D=$TMPROOT/inv21none; std_repo "$D"
STUB_CI_NONE=1 RELEASE_POLL_MAX=2 run "$D" "${FULL[@]}"
refused_by_ci "INV-21 no CI run ever appears" "$D" "$RC" "$OUT"
expect "INV-21 no CI run: looked RELEASE_POLL_MAX times, dispatched, looked RELEASE_POLL_MAX times" 4 "$(ci_lists "$D")"
expect "INV-21 no CI run: exactly one 'gh workflow run ci.yml --ref main'" 1 "$(gh_log "$D" | grep -cFx -- 'gh workflow run ci.yml --ref main' || true)"
expect "INV-21 no CI run: nothing to watch" 0 "$(ci_watches "$D")"
dl=$(oline "$D" 'ci-dispatch')
{ [ -n "$dl" ] && ! grep -E '^ci-' "${D}-log/obs.log" | grep -q 'origin_tags=\[[^]]*v0\.7\.98'; } \
    && ok "INV-21 no CI run: dispatch happened before any tag existed" || bad "INV-21 dispatch order: $(obs_log "$D")"

# The commit had no CI run (docs-only push): the script starts one, accepts it, tags.
D=$TMPROOT/inv21disp; std_repo "$D"
STUB_CI_AFTER_DISPATCH=1 RELEASE_POLL_MAX=2 run "$D" "${FULL[@]}"
{ [ "$RC" -eq 0 ] && has_tag "$D" v0.7.98 && has_otag "$D" v0.7.98; } \
    && ok "INV-21 run appears only after 'workflow run ci.yml': accepted, release goes on" || bad "INV-21 dispatch path (rc=$RC): $OUT"
expect "INV-21 dispatch path: exactly one 'gh workflow run ci.yml --ref main'" 1 "$(gh_log "$D" | grep -cFx -- 'gh workflow run ci.yml --ref main' || true)"
expect "INV-21 dispatch path: the started run is watched" 1 "$(ci_watches "$D")"
# ...and if that started run is red, no tag.
D=$TMPROOT/inv21dispred; std_repo "$D"
STUB_CI_AFTER_DISPATCH=1 STUB_CI_RC=1 RELEASE_POLL_MAX=2 run "$D" "${FULL[@]}"
refused_by_ci "INV-21 started CI run is red" "$D" "$RC" "$OUT"

# A run that shows up on a later look (no dispatch needed) is waited for.
D=$TMPROOT/inv21late; std_repo "$D"
STUB_CI_EMPTY_FIRST=2 RELEASE_POLL_MAX=5 run "$D" "${FULL[@]}"
{ [ "$RC" -eq 0 ] && has_otag "$D" v0.7.98 && [ "$(ci_watches "$D")" = 1 ] && [ "$(ci_disp "$D")" = 0 ]; } \
    && ok "INV-21 CI run found on a later look: watched, none started" || bad "INV-21 late CI run (rc=$RC): $OUT | $(gh_log "$D")"

# --skip-staging does not skip the CI check.
D=$TMPROOT/inv21skipred; std_repo "$D"
STUB_CI_RC=1 run "$D" "${NOSTAGE[@]}"
refused_by_ci "INV-21 --skip-staging with red CI" "$D" "$RC" "$OUT"
{ ! obs_log "$D" | grep -qE -- '--staging|--require-tests'; } && ok "INV-21 --skip-staging: gate scripts still not called" || bad "INV-21 gate called under --skip-staging: $(obs_log "$D")"
D=$TMPROOT/inv21skipok; std_repo "$D"
run "$D" "${NOSTAGE[@]}"
{ [ "$RC" -eq 0 ] && [ "$(ci_watches "$D")" = 1 ] && has_otag "$D" v0.7.98; } \
    && ok "INV-21 --skip-staging with green CI: CI still watched, release goes on" || bad "INV-21 skip-staging green (rc=$RC): $OUT | $(gh_log "$D")"

# Rehearsal: gh not called at all, a [rehearsal] line names the CI check.
D=$TMPROOT/inv21reh; std_repo "$D"
for reh in "release --skip-build --skip-staging" "release --skip-build"; do
    rm -f "${D}-log/gh.log"; run "$D" $reh
    { [ "$RC" -eq 0 ] && [ -z "$(gh_log "$D")" ] && grep -E '\[rehearsal\]' <<<"$OUT" | grep -qiE '\bci\b|ci\.yml'; } \
        && ok "INV-21 rehearsal '$reh': gh never called, [rehearsal] line names CI" || bad "INV-21 rehearsal '$reh' (rc=$RC, gh log=[$(gh_log "$D")]): $OUT"
done

# Resume (tag at HEAD, not on origin): CI is not asked about, even if it is red.
D=$TMPROOT/inv21res; seed_repo "$D"; write_cmake "$D" 0.7.98
write_changelog "$D" "" "## [0.7.98] - $TODAY" "$THEME

### Added
- Shiny new thing."
write_metainfo "$D" 0.7.98 "$TODAY"; write_debian "$D" 0.7.98 "$TODAY_RFC"
sed -i 's/v0.7.97/v0.7.98/' "$D/packaging/obs/_service"
commit_all "$D" "release 0.7.98"; git -C "$D" tag -a v0.7.98 -m "0.7.98"
STUB_CI_RC=1 STUB_CI_NONE=1 run "$D" "${FULL[@]}"
{ [ "$RC" -eq 0 ] && has_otag "$D" v0.7.98 && ! gh_log "$D" | grep -qE 'ci\.yml|workflow run'; } \
    && ok "INV-21 resumed run does not ask about CI" || bad "INV-21 resume (rc=$RC): $OUT | $(gh_log "$D")"

echo
echo "release_pipeline behavioural: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
