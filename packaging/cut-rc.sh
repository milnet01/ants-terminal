#!/usr/bin/env bash
#
# cut-rc.sh — ANTS-1318 frozen-RC pipeline tooling (project-local).
#
# Why project-local and not the global /release skill: the /release
# skill lives at ~/.claude/skills/release/SKILL.md and is shared by
# every project on this machine. The frozen-RC cadence (weekly Wed
# cut, Patron 7-day window, rc-channel AppImage split) is specific to
# Ants Terminal, so its orchestration lives here. /release still cuts
# normal public releases; this script layers the RC mechanics on top.
#
# The `-rcN` suffix lives ONLY at the git tag, the GitHub-release
# title, and the AppImage filename (ANTS-1318 INV-3 / INV-9). The
# base X.Y.Z in CMakeLists.txt is set by /bump, never by this script.
#
# Subcommands:
#   status                 Show base version, latest RC, in-flight state
#                          (incl. the in-flight RC's age in days — ANTS-2164).
#   new-rc                 Cut vX.Y.Z-rc1 (or rcN+1) from main HEAD. Refuses an
#                          empty RC and auto-rolls [Unreleased] (ANTS-2164).
#   respin <sha> [sha...]  Cherry-pick fix(es) onto the latest RC tag,
#                          re-tag rc(N+1) (ANTS-1318 §2.3).
#   promote                Tag public vX.Y.Z at the latest RC's commit
#                          (INV-2 zero-diff check), prerelease=false. Refuses an
#                          empty/placeholder or stale RC and auto-date-stamps the
#                          CHANGELOG/metainfo/debian carriers (ANTS-2164).
#   cycle                  The guarded Wednesday cadence (ANTS-2164): promote the
#                          in-flight RC, then cut the next one. Each phase
#                          self-skips when there is nothing to do, hard-refuses a
#                          broken/empty/stale RC. The N→N+1 bump between phases is
#                          a separate assistant '/bump' step (this script never
#                          edits version files — ANTS-1318 INV-9).
#   hotfix <sha> [sha...]  Out-of-cadence emergency release (ANTS-2165), two
#   hotfix --continue      phases around a '/bump': phase 1 branches _hotfix off
#                          the latest public tag + cherry-picks the fix(es) and
#                          stops for the bump; '--continue' builds, tags the next
#                          public patch vH, publishes, records [H] on main, prunes.
#
# Flags:
#   --push        Actually push tags + create GitHub releases. Without
#                 it, the script does everything locally (build, test,
#                 annotated tag) and PRINTS the push/publish commands
#                 it would run — a safe rehearsal.
#   --skip-build  Skip the build+test gate (use only when the caller
#                 already built+tested the exact HEAD being tagged).
#   --force-non-wed  Allow cutting on a non-Wednesday (INV-1 is a soft
#                 cadence invariant; default warns and proceeds).
#   --continue       (hotfix only) run phase 2 of an in-progress hotfix.
#   --allow-empty-rc Override the ANTS-2164 INV-1 empty-RC refusal in new-rc.
#   --promote-empty  Override the ANTS-2164 INV-2 empty/placeholder refusal in
#                 promote.
#   --force-stale    Override the ANTS-2164 INV-8 stale-RC (≥15-day) refusal.
#
# Exit non-zero on any guard failure. Never force-pushes.

set -euo pipefail

# ---- Setup -----------------------------------------------------------

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "cut-rc: not inside a git repository" >&2
    exit 1
}
cd "$repo_root"

DO_PUSH=0
SKIP_BUILD=0
FORCE_NON_WED=0
CONTINUE=0          # hotfix phase 2 (ANTS-2165)
ALLOW_EMPTY_RC=0    # override new-rc INV-1 empty-RC refusal (ANTS-2164)
PROMOTE_EMPTY=0     # override promote INV-2 empty/placeholder refusal (ANTS-2164)
FORCE_STALE=0       # override promote/cycle INV-8 stale-RC refusal (ANTS-2164)
SUBCMD=""
# Positional fix SHAs for respin / hotfix (both take "<sha> [sha...]").
declare -a RESPIN_SHAS=()

for arg in "$@"; do
    case "$arg" in
        --push)           DO_PUSH=1 ;;
        --skip-build)     SKIP_BUILD=1 ;;
        --force-non-wed)  FORCE_NON_WED=1 ;;
        --continue)       CONTINUE=1 ;;
        --allow-empty-rc) ALLOW_EMPTY_RC=1 ;;
        --promote-empty)  PROMOTE_EMPTY=1 ;;
        --force-stale)    FORCE_STALE=1 ;;
        status|new-rc|respin|promote|cycle|hotfix)
            if [ -z "$SUBCMD" ]; then SUBCMD="$arg"; else RESPIN_SHAS+=("$arg"); fi ;;
        *)                RESPIN_SHAS+=("$arg") ;;
    esac
done

[ -n "$SUBCMD" ] || { echo "cut-rc: usage: cut-rc.sh {status|new-rc|respin <sha…>|promote|cycle|hotfix <sha…>|hotfix --continue} [--push] [--skip-build] [--force-non-wed] [--allow-empty-rc] [--promote-empty] [--force-stale]" >&2; exit 2; }

# Base X.Y.Z from CMakeLists.txt (same regex the drift script trusts).
base_version() {
    grep -oE 'project\s*\([^)]*VERSION\s+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+$' | head -1
}

# Highest rcN for a given base (0 if none).
latest_rc_n() {
    local base=$1 n max=0
    while IFS= read -r t; do
        [ -n "$t" ] || continue
        n=${t##*-rc}
        [ "$n" -gt "$max" ] 2>/dev/null && max=$n
    done < <(git tag -l "v${base}-rc[0-9]*")
    echo "$max"
}

# The single in-flight RC base, if any: an rc tag whose public vX.Y.Z
# does NOT yet exist. Echoes "base" or "" (none).
inflight_base() {
    local t base
    while IFS= read -r t; do
        [ -n "$t" ] || continue
        base=${t#v}; base=${base%-rc*}
        if ! git rev-parse -q --verify "refs/tags/v${base}" >/dev/null; then
            echo "$base"; return 0
        fi
    done < <(git tag -l 'v*-rc[0-9]*' | sort -V)
    echo ""
}

confirm_or_print() {
    # $@ = command to run. Runs it under --push; otherwise prints it.
    if [ "$DO_PUSH" = 1 ]; then
        echo "+ $*"
        "$@"
    else
        echo "  [rehearsal] would run: $*"
    fi
}

build_and_test() {
    [ "$SKIP_BUILD" = 1 ] && { echo "cut-rc: --skip-build set, skipping build+test gate"; return 0; }
    echo "cut-rc: building (cmake --build build)…"
    cmake --build build >/dev/null 2>&1 || { echo "cut-rc: build FAILED — aborting" >&2; exit 1; }
    echo "cut-rc: running feature tests (ctest -L features)…"
    ctest --test-dir build -L features --output-on-failure >/dev/null 2>&1 \
        || { echo "cut-rc: tests FAILED — aborting" >&2; exit 1; }
    echo "cut-rc: build + tests green."
}

wednesday_guard() {
    if [ "$(date +%u)" != 3 ] && [ "$FORCE_NON_WED" != 1 ]; then
        echo "cut-rc: WARNING — today is $(date +%A), not Wednesday." >&2
        echo "        The cadence cuts on Wednesdays (INV-1, soft). Proceeding;" >&2
        echo "        pass --force-non-wed to silence, or Ctrl-C to abort." >&2
    fi
}

require_clean_main() {
    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)
    [ "$branch" = main ] || { echo "cut-rc: must be on 'main' (on '$branch')" >&2; exit 1; }
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "cut-rc: working tree is dirty — commit or stash first" >&2
        exit 1
    fi
}

release_notes() {
    # Concise GitHub-release body for tag $1: the **Theme:** paragraph
    # of the first "## [X.Y.Z]" CHANGELOG section + a link to the full
    # notes at the tag. The full section can run to hundreds of KB
    # (a whole milestone), which blows past both argv limits and
    # GitHub's release-body cap — so we summarise + link, never inline
    # the whole section.
    local tag=$1
    awk '
        /^## \[[0-9]+\.[0-9]+\.[0-9]+\]/ { if (seen) exit; seen=1; next }
        seen && /^\*\*Theme:\*\*/ { intheme=1 }
        seen && intheme && /^$/   { exit }
        seen && intheme           { print }
    ' CHANGELOG.md
    printf '\nFull release notes: CHANGELOG.md at this tag —\n'
    printf 'https://github.com/milnet01/ants-terminal/blob/%s/CHANGELOG.md\n' "$tag"
}

# ---- ANTS-2164 cadence-hardening helpers -----------------------------

CHANGELOG_FILE="CHANGELOG.md"
METAINFO_FILE="packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml"
DEBIAN_CHANGELOG_FILE="packaging/debian/changelog"
OBS_SERVICE_FILE="packaging/obs/_service"

# True (exit 0) if the "## [Unreleased]" section has ≥1 real entry — a
# changelog bullet (^\s*[-*]) or a "### Category" header. The italic
# channel-opener prose and the "**Theme:**" line do NOT count. This is the
# authoritative "is there a release's worth of work to preview?" signal
# (ANTS-2164 §2.1).
unreleased_has_content() {
    awk '
        /^## \[Unreleased\]/ { inseg=1; next }
        inseg && /^## \[/ { exit }
        inseg && /^[ \t]*[-*][ \t]/ { found=1 }
        inseg && /^### (Added|Changed|Deprecated|Removed|Fixed|Security)/ { found=1 }
        END { exit (found ? 0 : 1) }
    ' "$CHANGELOG_FILE"
}

# True (exit 0) if the CHANGELOG "## [X.Y.Z]" section is still the
# channel-opener placeholder. Isolates that version's slice FIRST (up to the
# next "## ["), then matches the single-line anchor within it (ANTS-2164
# §2.1) — never a bare repo-wide match.
changelog_section_is_placeholder() {
    awk -v ver="$1" '
        $0 ~ ("^## \\[" ver "\\]") { inseg=1; next }
        inseg && /^## \[/ { exit }
        inseg && /opens the .* preview channel/ { found=1; exit }
        END { exit (found ? 0 : 1) }
    ' "$CHANGELOG_FILE"
}

# True (exit 0) if the metainfo <release version="X.Y.Z"> body OR the debian
# (X.Y.Z-1) block is still the channel-opener placeholder. Each isolates the
# version's element/block before matching the anchor (ANTS-2164 §2.1). The
# CHANGELOG is checked separately by changelog_section_is_placeholder.
release_body_is_placeholder() {
    awk -v ver="$1" '
        $0 ~ ("<release version=\"" ver "\"") { inseg=1 }
        inseg && /opens the .* preview channel/ { found=1 }
        inseg && /<\/release>/ { inseg=0 }
        END { exit (found ? 0 : 1) }
    ' "$METAINFO_FILE" && return 0
    awk -v ver="$1" '
        $0 ~ ("^ants-terminal \\(" ver "-") { inseg=1; next }
        inseg && /^ants-terminal \(/ { exit }
        inseg && /opens the .* preview channel/ { found=1; exit }
        END { exit (found ? 0 : 1) }
    ' "$DEBIAN_CHANGELOG_FILE" && return 0
    return 1
}

# Roll the NON-EMPTY "## [Unreleased]" body into a "## [X.Y.Z] — unreleased
# (Patron RC preview)" section, leaving "## [Unreleased]" with an empty body
# (never collapsing the two headings). Creates the [X.Y.Z] section if absent;
# REPLACES a channel-opener-placeholder body; no-ops when [X.Y.Z] already has
# real content (idempotent). Pure awk + atomic rename (ANTS-2164 §2.1).
roll_unreleased() {
    local v=$1 tmp
    tmp=$(mktemp)
    # A && B || C is intended here and is not a mis-written if-then-else: C is
    # cleanup-on-any-failure, wanted whether awk failed or the rename did.
    # Neither arm can leave a half-written file — awk writes to "$tmp" and the
    # original is only ever replaced by an atomic mv (ANTS-4763).
    # shellcheck disable=SC2015
    awk -v ver="$v" -v verraw="$v" '
        { lines[NR]=$0 }
        END {
            n=NR; unrel=0; target=0
            for (i=1;i<=n;i++) {
                if (lines[i] ~ /^## \[Unreleased\]/) unrel=i
                else if (target==0 && lines[i] ~ ("^## \\[" ver "\\]")) target=i
            }
            if (unrel==0) { for(i=1;i<=n;i++) print lines[i]; exit 0 }
            ue=n+1
            for (i=unrel+1;i<=n;i++) if (lines[i] ~ /^## \[/) { ue=i; break }
            ubn=0
            for (i=unrel+1;i<ue;i++) ub[++ubn]=lines[i]
            while (ubn>0 && ub[1] ~ /^[ \t]*$/) { for(j=1;j<ubn;j++) ub[j]=ub[j+1]; ubn-- }
            while (ubn>0 && ub[ubn] ~ /^[ \t]*$/) ubn--
            hasc=0
            for (i=1;i<=ubn;i++)
                if (ub[i] ~ /^[ \t]*[-*][ \t]/ || ub[i] ~ /^### (Added|Changed|Deprecated|Removed|Fixed|Security)/) hasc=1
            if (!hasc) { for(i=1;i<=n;i++) print lines[i]; exit 0 }
            if (target>0) {
                te=n+1
                for (i=target+1;i<=n;i++) if (lines[i] ~ /^## \[/) { te=i; break }
                treal=0
                for (i=target+1;i<te;i++)
                    if (lines[i] ~ /^[ \t]*[-*][ \t]/ || lines[i] ~ /^### (Added|Changed|Deprecated|Removed|Fixed|Security)/) treal=1
                if (treal) { for(i=1;i<=n;i++) print lines[i]; exit 0 }
            }
            for (i=1;i<=n;i++) {
                if (i>unrel && i<ue) continue
                if (i==unrel) {
                    print lines[i]; print ""
                    if (target==0) {
                        print "## [" verraw "] — unreleased (Patron RC preview)"
                        for (k=1;k<=ubn;k++) print ub[k]
                        print ""
                    }
                    continue
                }
                if (target>0 && i==target) {
                    print lines[i]
                    for (k=1;k<=ubn;k++) print ub[k]
                    print ""
                    continue
                }
                if (target>0 && i>target && i<te) continue
                print lines[i]
            }
        }
    ' "$CHANGELOG_FILE" > "$tmp" && mv "$tmp" "$CHANGELOG_FILE" \
        || { rm -f "$tmp"; echo "cut-rc: roll_unreleased failed" >&2; exit 1; }
}

# Rewrite <file> via awk program "$2..." to a temp + atomic rename. The awk
# program prints the whole transformed file and exits 0 only when it made the
# substitution (else exit 3 = target not found → hard error). Under --push it
# replaces the file; otherwise prints a one-line rehearsal notice and mutates
# nothing (ANTS-2164 INV-3). Used by stamp_release_date.
apply_rewrite() {
    local file=$1; shift
    local tmp; tmp=$(mktemp)
    if awk "$@" "$file" > "$tmp"; then
        if [ "$DO_PUSH" = 1 ]; then
            mv "$tmp" "$file"
        else
            echo "  [rehearsal] would stamp ${file}"
            rm -f "$tmp"
        fi
    else
        rm -f "$tmp"
        echo "cut-rc: stamp_release_date: target not found in ${file}" >&2
        exit 1
    fi
}

# Stamp the public-ship date in all three release-note carriers (ANTS-2164
# §2.1 / INV-3): the CHANGELOG "## [X.Y.Z] — …" heading, the metainfo
# <release version="X.Y.Z" date="…"> attribute, and the debian (X.Y.Z-1)
# block's RFC-2822 trailer (reformatted via `date -R -d`). awk + atomic
# rename, never an in-place stream edit (keeps the Inv4BaseReadOnly scrape
# green — ANTS-2164 §8).
stamp_release_date() {
    local v=$1 iso=$2 rfc
    rfc=$(date -R -d "$iso") || { echo "cut-rc: stamp_release_date: bad date '$iso'" >&2; exit 1; }

    # awk program, single-quoted deliberately: $1 and $0 below are awk's own
    # fields, not shell variables. Letting the shell expand them would empty
    # the program, so the quoting is the point rather than an oversight.
    # shellcheck disable=SC2016
    apply_rewrite "$CHANGELOG_FILE" -v ver="$v" -v verraw="$v" -v iso="$iso" '
        !done && $0 ~ ("^## \\[" ver "\\]") { print "## [" verraw "] — " iso; done=1; next }
        { print }
        END { exit (done ? 0 : 3) }
    '
    # awk program, single-quoted deliberately: $1 and $0 below are awk's own
    # fields, not shell variables. Letting the shell expand them would empty
    # the program, so the quoting is the point rather than an oversight.
    # shellcheck disable=SC2016
    apply_rewrite "$METAINFO_FILE" -v ver="$v" -v iso="$iso" '
        !done && $0 ~ ("<release version=\"" ver "\"") {
            sub(/date="[0-9-]+"/, "date=\"" iso "\""); done=1
        }
        { print }
        END { exit (done ? 0 : 3) }
    '
    # awk program, single-quoted deliberately: $1 and $0 below are awk's own
    # fields, not shell variables. Letting the shell expand them would empty
    # the program, so the quoting is the point rather than an oversight.
    # shellcheck disable=SC2016
    apply_rewrite "$DEBIAN_CHANGELOG_FILE" -v ver="$v" -v rfc="$rfc" '
        $0 ~ ("^ants-terminal \\(" ver "-") { inblk=1 }
        inblk && /^ants-terminal \(/ && seenhdr { inblk=0 }
        $0 ~ ("^ants-terminal \\(" ver "-") { seenhdr=1 }
        inblk && !done && /^ -- / { sub(/  [A-Z][a-z][a-z], .*$/, "  " rfc); done=1 }
        { print }
        END { exit (done ? 0 : 3) }
    '
}

# Days since the in-flight RC TAG was created (creatordate, NOT commit date —
# a respin re-tags an older commit). Empty creatordate (lightweight tag,
# should not occur — new-rc makes annotated tags) → echo 9999 so the stale
# check fails CLOSED (ANTS-2164 §2.1).
rc_age_days() {
    local tag=$1 created now
    created=$(git for-each-ref --format='%(creatordate:unix)' "refs/tags/${tag}" 2>/dev/null)
    [ -n "$created" ] || { echo 9999; return 0; }
    now=$(date +%s)
    echo $(( (now - created) / 86400 ))
}

# Hard version-drift gate (ANTS-2164 INV-5): abort non-zero on any drift.
require_no_version_drift() {
    if ! bash packaging/check-version-drift.sh; then
        echo "cut-rc: version drift detected — fix before releasing (ANTS-2164 INV-5)" >&2
        exit 1
    fi
}

# ANTS-4716 — publish the pinned revision to OBS. pin_obs_service_revision()
# above updates packaging/obs/_service IN GIT; obs_scm only re-clones when the
# services are TRIGGERED, so without this the repositories keep building the
# source archive they already hold and the correct git-side pin is exactly what
# makes that invisible.
#
# This has now bitten twice. ANTS-4587 found all four repositories two releases
# behind and repaired the instance by hand; the fix that shipped was the daily
# audit (ANTS-4588), which detects and cannot act. The very next release missed
# it again. Detection standing in for automation is the whole defect.
#
# Runs AFTER the tag is pushed, not with the stamp commit: obs-submit.sh refuses
# when _service pins a tag that does not exist yet, and OBS needs it on the
# remote to clone.
#
# A failure here does NOT fail the promote. By this point the tag is pushed and
# the release is published, so aborting would leave a state nobody can read.
# It reports loudly instead and names the one command to run.
submit_to_obs() {
    local script="packaging/obs/obs-submit.sh"
    if [ ! -x "$script" ]; then
        echo "cut-rc: ⊘ OBS submit SKIPPED — $script not executable." >&2
        echo "cut-rc:   Package repositories will keep serving the previous release." >&2
        return 0
    fi
    if ! command -v osc >/dev/null 2>&1; then
        echo "cut-rc: ⊘ OBS submit SKIPPED — osc not installed." >&2
        echo "cut-rc:   Run '$script' from a machine that has it." >&2
        return 0
    fi
    echo "cut-rc: submitting the pinned revision to OBS…"
    if "$script"; then
        echo "cut-rc: OBS submit committed; watch it with packaging/obs/obs-status.sh"
    else
        echo "" >&2
        echo "cut-rc: ✗ OBS SUBMIT FAILED — the release is tagged and published," >&2
        echo "cut-rc:   but package repositories still serve the PREVIOUS version." >&2
        echo "cut-rc:   This does not undo the release. Fix and run:" >&2
        echo "cut-rc:       $script" >&2
        echo "" >&2
    fi
}

# Shipped-coverage report (ANTS-4714): which items the roadmap store says
# shipped since the last public tag are cited by no CHANGELOG bullet. This is
# the CONVERSE of the release skill's own gate, which only checks that ids the
# CHANGELOG *claims* are really shipped — that direction cannot see work that
# shipped and was never written down. Since ANTS-4759 the same script also
# reports [Unreleased] bullets whose bold summary is a verbatim copy of the
# item's roadmap headline — for a defect item that headline states the
# problem, so the copy puts the bug in the release notes where the fix
# belongs.
#
# ADVISORY, not a hard gate, and the reason is deliberate rather than timid.
# Whether an uncovered item is release-note-worthy or deliberately internal is
# a judgement, and the check landed against a 19-item backlog. A hard gate on
# day one blocks every RC until someone triages that backlog, which is how a
# new gate gets switched off instead of used. It prints the list where the
# operator is already watching; the REQUIRED step is the bump-time run that
# .claude/bump.json owns, where there is still an open [Unreleased] to add to.
report_shipped_coverage() {
    [ -x tools/check-shipped-coverage.sh ] || return 0
    if ! tools/check-shipped-coverage.sh; then
        echo "cut-rc: ^ the coverage gate above found something (ANTS-4714/4759)." >&2
        echo "cut-rc:   Not blocking. Shipped work in no CHANGELOG bullet: add the" >&2
        echo "cut-rc:   release-note-worthy ones with changelog_log, and say in the" >&2
        echo "cut-rc:   release report which were left out on purpose. A bullet whose" >&2
        echo "cut-rc:   summary repeats its roadmap headline: reword it to say what" >&2
        echo "cut-rc:   shipped, since a defect headline states the problem." >&2
    fi
}

# X.Y.Z → X.Y.(Z+1) / X.Y.(Z-1) — the patch-field arithmetic the hotfix
# (ANTS-2165) and its guards use.
patch_plus_one() {
    local major minor patch; IFS=. read -r major minor patch <<< "$1"
    echo "${major}.${minor}.$((patch + 1))"
}
patch_minus_one() {
    local major minor patch; IFS=. read -r major minor patch <<< "$1"
    echo "${major}.${minor}.$((patch - 1))"
}

# Latest public vX.Y.Z tag (RC tags excluded), same derivation as cmd_status.
latest_public_tag() {
    git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | grep -vE -- '-rc' | sort -V | tail -1
}

# True (exit 0) if <sha> is already an ancestor of main (ANTS-2165 §2.2).
fix_is_on_main() {
    git merge-base --is-ancestor "$1" main 2>/dev/null
}

# ---- Subcommands -----------------------------------------------------

cmd_status() {
    local base inflight n
    base=$(base_version)
    inflight=$(inflight_base)
    echo "Base version (CMakeLists.txt): ${base}"
    echo "Latest public tag:            $(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | grep -vE -- '-rc' | sort -V | tail -1)"
    echo "Latest RC tag:                $(git tag -l 'v*-rc[0-9]*' | sort -V | tail -1)"
    if [ -n "$inflight" ]; then
        n=$(latest_rc_n "$inflight")
        local age; age=$(rc_age_days "v${inflight}-rc${n}")
        echo "In-flight RC:                 v${inflight}-rc${n}  (no public v${inflight} yet)"
        echo "In-flight RC age:             ${age} day(s)  (promote window: ≤14, ANTS-2164 INV-8)"
    else
        echo "In-flight RC:                 none"
    fi
}

cmd_new_rc() {
    require_clean_main
    wednesday_guard
    require_no_version_drift          # ANTS-2164 INV-5 (hard gate)
    report_shipped_coverage           # ANTS-4714 (advisory — see the function)
    local base inflight n tag
    base=$(base_version)
    [ -n "$base" ] || { echo "cut-rc: could not read base version from CMakeLists.txt" >&2; exit 1; }

    # INV-6 / §4.4 — at most one RC in flight. If an RC exists for a
    # DIFFERENT base with no public tag yet, refuse: promote or delete
    # it first.
    inflight=$(inflight_base)
    if [ -n "$inflight" ] && [ "$inflight" != "$base" ]; then
        echo "cut-rc: v${inflight}-rc* is in flight (no public v${inflight} yet)." >&2
        echo "        Cut public v${inflight} first ('promote'), or delete that RC," >&2
        echo "        before cutting an RC for ${base} (ANTS-1318 §4.4)." >&2
        exit 1
    fi

    # ANTS-2164 INV-9 — refuse if the base is already a public release (the
    # inter-phase /bump was missed; cutting an RC for a released version would
    # violate ANTS-1318 INV-6). Evaluated before the rc-tag computation.
    if git rev-parse -q --verify "refs/tags/v${base}" >/dev/null; then
        echo "cut-rc: base ${base} is already public (tag v${base} exists)." >&2
        echo "        Run '/bump' to the next patch before cutting its RC" >&2
        echo "        (ANTS-2164 INV-9)." >&2
        exit 1
    fi

    # ANTS-2164 INV-1 — refuse an empty RC: the "## [Unreleased]" section must
    # have ≥1 real entry to preview. This is the guard that stops a degenerate
    # same-tip channel-opener cut.
    if ! unreleased_has_content; then
        if [ "$ALLOW_EMPTY_RC" = 1 ]; then
            echo "cut-rc: [Unreleased] is empty but --allow-empty-rc set — proceeding." >&2
        else
            echo "cut-rc: nothing new to preview — '## [Unreleased]' has no entries." >&2
            echo "        Skip the RC this week, or pass --allow-empty-rc to override" >&2
            echo "        (ANTS-2164 INV-1)." >&2
            exit 1
        fi
    fi

    n=$(( $(latest_rc_n "$base") + 1 ))
    tag="v${base}-rc${n}"
    git rev-parse -q --verify "refs/tags/${tag}" >/dev/null \
        && { echo "cut-rc: tag ${tag} already exists" >&2; exit 1; }

    # ANTS-2164 INV-4 — roll the [Unreleased] body into "## [${base}] —
    # unreleased (Patron RC preview)" and commit, so the RC tag freezes a
    # dated-on-promote section (idempotent; no-op if already rolled).
    if [ "$DO_PUSH" = 1 ]; then
        roll_unreleased "$base"
        if ! git diff --quiet "$CHANGELOG_FILE"; then
            git add "$CHANGELOG_FILE"
            git commit -q -m "chore: roll [Unreleased] into [${base}] for RC${n}"
        fi
    else
        echo "  [rehearsal] would roll [Unreleased] → [${base}] in ${CHANGELOG_FILE} and commit"
    fi

    echo "cut-rc: cutting ${tag} from $(git rev-parse --short HEAD) on main"
    build_and_test
    if [ "$DO_PUSH" = 1 ]; then
        git tag -a "${tag}" -m "${base} RC${n} — Patron preview"
        echo "cut-rc: created local annotated tag ${tag}"
    else
        echo "  [rehearsal] would create annotated tag ${tag}"
    fi

    # main must be on origin so the tag's commit is reachable remotely.
    confirm_or_print git push origin main
    confirm_or_print git push origin "${tag}"

    if [ "$DO_PUSH" = 1 ]; then
        local notes_file; notes_file=$(mktemp)
        release_notes "${tag}" > "$notes_file"
        [ -s "$notes_file" ] || echo "See CHANGELOG.md for highlights." > "$notes_file"
        echo "+ gh release create ${tag} --prerelease --title \"${base} RC${n} — Patron preview\" --notes-file <theme + CHANGELOG link>"
        gh release create "${tag}" --prerelease \
            --title "${base} RC${n} — Patron preview" \
            --notes-file "$notes_file"
        rm -f "$notes_file"
    else
        echo "  [rehearsal] would run: gh release create ${tag} --prerelease --title \"${base} RC${n} — Patron preview\" --notes-file <theme + CHANGELOG link>"
    fi

    echo
    echo "RC freeze window: today → $(date -d '+7 days' +%Y-%m-%d) (public ship target)."
    echo "Patron-notification template: see docs/specs/ANTS-1318.md §7."
}

cmd_respin() {
    require_clean_main
    [ "${#RESPIN_SHAS[@]}" -gt 0 ] || { echo "cut-rc: respin needs one or more fix SHAs (already on main)" >&2; exit 1; }
    local inflight n from to sha
    inflight=$(inflight_base)
    [ -n "$inflight" ] || { echo "cut-rc: no RC in flight to respin" >&2; exit 1; }
    n=$(latest_rc_n "$inflight")
    from="v${inflight}-rc${n}"
    to="v${inflight}-rc$((n+1))"
    echo "cut-rc: respinning ${from} → ${to} with: ${RESPIN_SHAS[*]}"
    git checkout -q -b _rc-respin "${from}"
    for sha in "${RESPIN_SHAS[@]}"; do
        git cherry-pick "$sha" || {
            echo "cut-rc: cherry-pick of ${sha} conflicted — resolve by hand or abort" >&2
            echo "        (git cherry-pick --abort; git checkout main; git branch -D _rc-respin)" >&2
            exit 1
        }
    done
    build_and_test
    if [ "$DO_PUSH" = 1 ]; then
        git tag -a "${to}" -m "${inflight} RC$((n+1)) — Patron preview (respin)"
    else
        echo "  [rehearsal] would create annotated tag ${to}"
    fi
    confirm_or_print git push origin "${to}"
    if [ "$DO_PUSH" = 1 ]; then
        gh release create "${to}" --prerelease \
            --title "${inflight} RC$((n+1)) — Patron preview" \
            --notes "Respin of ${from} with cherry-picked fixes: ${RESPIN_SHAS[*]}"
    else
        echo "  [rehearsal] would run: gh release create ${to} --prerelease …"
    fi
    git checkout -q main
    git branch -D _rc-respin >/dev/null 2>&1 || true
    echo "cut-rc: respin complete; temp branch pruned."
}

# ANTS-3728 — keep packaging/obs/_service's pinned <revision> in lockstep with
# the tag being published.
#
# OBS's trigger_services re-runs the recipe OBS ALREADY HAS. So when a tag push
# fires the webhook and _service still names the previous tag, OBS happily
# rebuilds the PREVIOUS release: green, published, and the wrong version. That
# failure is near-invisible because nothing errors. Bumping the pin here, in the
# same commit that stamps the other release carriers, means the recipe cannot
# disagree with the tag it shipped alongside.
#
# Absent file is not an error: older checkouts and the test fixtures predate
# packaging/obs/, and promote must stay usable there.
pin_obs_service_revision() {
    local tag=$1
    [ -f "$OBS_SERVICE_FILE" ] || return 0
    apply_rewrite "$OBS_SERVICE_FILE" -v tag="$tag" '
        !done && /<param name="revision">/ {
            sub(/<param name="revision">[^<]*<\/param>/,
                "<param name=\"revision\">" tag "</param>")
            done = 1
        }
        { print }
        END { if (!done) exit 3 }
    '
}

cmd_promote() {
    require_clean_main
    wednesday_guard
    require_no_version_drift          # ANTS-2164 INV-5 (hard gate)
    local inflight n rctag pub age today
    inflight=$(inflight_base)
    [ -n "$inflight" ] || { echo "cut-rc: no RC in flight to promote" >&2; exit 1; }
    n=$(latest_rc_n "$inflight")
    rctag="v${inflight}-rc${n}"
    pub="v${inflight}"

    # ANTS-2164 INV-2 — refuse an empty/placeholder release: the CHANGELOG
    # section OR the metainfo/debian body must not still be the channel-opener
    # placeholder. (A *new* guard on this diff-check site; ANTS-1318 INV-2's
    # bit-identity is preserved below by the ^{commit} tag.)
    if changelog_section_is_placeholder "$inflight" || release_body_is_placeholder "$inflight"; then
        if [ "$PROMOTE_EMPTY" = 1 ]; then
            echo "cut-rc: ${inflight} release notes are still a placeholder but --promote-empty set — proceeding." >&2
        else
            echo "cut-rc: ${inflight} release notes are still the channel-opener placeholder." >&2
            echo "        Draft the [${inflight}] CHANGELOG/metainfo/debian notes first," >&2
            echo "        or pass --promote-empty to override (ANTS-2164 INV-2)." >&2
            exit 1
        fi
    fi

    # ANTS-2164 INV-8 — refuse a stale RC (≥15 days; day 14 is the last day an
    # RC may still promote, ANTS-1318 INV-4's 14-day window is inclusive).
    age=$(rc_age_days "$rctag")
    if [ "$age" -ge 15 ]; then
        if [ "$FORCE_STALE" = 1 ]; then
            echo "cut-rc: ${rctag} is ${age} days old but --force-stale set — proceeding." >&2
        else
            echo "cut-rc: ${rctag} is ${age} days old (>14-day window)." >&2
            echo "        Cut a fresh RC, or pass --force-stale to override" >&2
            echo "        (ANTS-2164 INV-8)." >&2
            exit 1
        fi
    fi

    # INV-2 (ANTS-1318): the public tag is the RC's exact bits.
    if ! git diff --quiet "${rctag}"; then
        echo "cut-rc: working tree differs from ${rctag}; promote tags the RC's commit." >&2
    fi

    # ANTS-2164 INV-3 — date-stamp the three carriers on main HEAD and commit
    # (stamp self-gates write-vs-rehearsal; the commit+push are --push-gated).
    today=$(date +%F)
    stamp_release_date "$inflight" "$today"
    pin_obs_service_revision "$pub"
    if [ "$DO_PUSH" = 1 ]; then
        git add "$CHANGELOG_FILE" "$METAINFO_FILE" "$DEBIAN_CHANGELOG_FILE"
        [ -f "$OBS_SERVICE_FILE" ] && git add "$OBS_SERVICE_FILE"
        git diff --cached --quiet || git commit -q -m "chore: stamp ${inflight} release date ${today}"
        confirm_or_print git push origin main
    fi

    echo "cut-rc: promoting ${rctag} → ${pub} (public, prerelease=false)"
    if [ "$DO_PUSH" = 1 ]; then
        git tag -a "${pub}" -m "${inflight}" "${rctag}^{commit}"
    else
        echo "  [rehearsal] would create annotated tag ${pub} at ${rctag}"
    fi
    confirm_or_print git push origin "${pub}"
    if [ "$DO_PUSH" = 1 ]; then
        local notes_file; notes_file=$(mktemp)
        release_notes "${pub}" > "$notes_file"
        [ -s "$notes_file" ] || echo "See CHANGELOG.md for highlights." > "$notes_file"
        gh release create "${pub}" \
            --title "${inflight}" \
            --notes-file "$notes_file"
        rm -f "$notes_file"
    else
        echo "  [rehearsal] would run: gh release create ${pub} (public, prerelease=false) --notes-file <theme + CHANGELOG link>"
    fi
    if [ "$DO_PUSH" = 1 ]; then
        submit_to_obs                 # ANTS-4716 — after the tag push, on purpose
    else
        echo "  [rehearsal] would run: packaging/obs/obs-submit.sh (ANTS-4716)"
    fi
    echo "cut-rc: ${pub} promoted. Now bump to the next patch and run 'new-rc'"
    echo "        to cut the following week's RC (ANTS-1318 §2.4 dual-cut)."
}

# ANTS-2164 §2.4 — the guarded Wednesday cadence: promote the in-flight RC,
# then cut the next one. Each phase self-skips when there is nothing to do, and
# hard-refuses (via the sub-command's own guards, which `exit` non-zero — INV-7)
# a broken/empty/stale RC. The N→N+1 bump between phases is a separate
# assistant '/bump' step (this script never edits version files); a missed bump
# is caught by new-rc's INV-9.
cmd_cycle() {
    require_clean_main
    wednesday_guard
    FORCE_NON_WED=1                   # warned once; silence the phases
    local inflight
    inflight=$(inflight_base)
    if [ -n "$inflight" ]; then
        echo "cut-rc: cycle phase 1 — promote in-flight v${inflight}"
        cmd_promote
    else
        echo "cut-rc: cycle phase 1 — no in-flight RC (skip promote)"
    fi
    echo
    if unreleased_has_content; then
        echo "cut-rc: cycle phase 2 — cut next RC from main"
        cmd_new_rc
    else
        echo "cut-rc: cycle phase 2 — no [Unreleased] content (skip new-rc)"
    fi
    echo "cut-rc: cycle complete."
}

# Insert the extracted "## [H]" block (file $3) into main's CHANGELOG
# immediately before the "## [N]" heading, so order reads [H] > [N]
# (ANTS-2165 INV-4). awk + atomic rename.
record_hotfix_on_main() {
    local N=$2 block=$3 tmp
    tmp=$(mktemp)
    # A && B || C is intended here and is not a mis-written if-then-else: C is
    # cleanup-on-any-failure, wanted whether awk failed or the rename did.
    # Neither arm can leave a half-written file — awk writes to "$tmp" and the
    # original is only ever replaced by an atomic mv (ANTS-4763).
    # shellcheck disable=SC2015
    awk -v nver="$N" -v blockf="$block" '
        !done && $0 ~ ("^## \\[" nver "\\]") {
            while ((getline line < blockf) > 0) print line
            done=1
        }
        { print }
        END { if (!done) exit 3 }
    ' "$CHANGELOG_FILE" > "$tmp" \
        && mv "$tmp" "$CHANGELOG_FILE" \
        || { rm -f "$tmp"; echo "cut-rc: record_hotfix_on_main failed — [${N}] anchor not found, or the CHANGELOG could not be replaced" >&2; exit 1; }
}

# ANTS-2165 — out-of-cadence emergency release, two phases around a '/bump'.
cmd_hotfix() {
    if [ "$CONTINUE" = 1 ]; then cmd_hotfix_continue; return; fi

    # Phase 1: guard + branch off the latest public tag + cherry-pick, STOP.
    require_clean_main
    require_no_version_drift          # main's own packaging consistent (not an H check)
    [ "${#RESPIN_SHAS[@]}" -gt 0 ] \
        || { echo "cut-rc: hotfix needs one or more fix SHAs (already on main)" >&2; exit 1; }
    local sha pub_tag N H inflight
    for sha in "${RESPIN_SHAS[@]}"; do
        fix_is_on_main "$sha" \
            || { echo "cut-rc: ${sha} is not on main (fix_not_on_main) — land the fix on trunk first (ANTS-2165 INV-3)" >&2; exit 1; }
    done
    pub_tag=$(latest_public_tag)
    [ -n "$pub_tag" ] || { echo "cut-rc: no public release tag to base a hotfix on" >&2; exit 1; }
    N=${pub_tag#v}
    H=$(patch_plus_one "$N")
    inflight=$(inflight_base)
    if [ -n "$inflight" ] && [ "$inflight" != "$H" ]; then
        echo "cut-rc: in-flight RC base ${inflight} != next patch ${H} (rc_base_mismatch)." >&2
        echo "        The roll-up arithmetic assumes the RC is the next patch after public." >&2
        exit 1
    fi
    if git rev-parse -q --verify refs/heads/_hotfix >/dev/null; then
        echo "cut-rc: a '_hotfix' branch already exists (hotfix_branch_exists)." >&2
        echo "        Resume with 'cut-rc.sh hotfix --continue', or discard it with" >&2
        echo "        'git branch -D _hotfix' before starting over." >&2
        exit 1
    fi
    echo "cut-rc: hotfix — branching _hotfix off ${pub_tag}; target public v${H}"
    git checkout -q -b _hotfix "$pub_tag"
    for sha in "${RESPIN_SHAS[@]}"; do
        git cherry-pick "$sha" || {
            echo "cut-rc: cherry-pick of ${sha} conflicted — aborting hotfix (ANTS-2165 INV-6)" >&2
            git cherry-pick --abort 2>/dev/null || true
            git checkout -q main
            git branch -D _hotfix >/dev/null 2>&1 || true
            exit 1
        }
    done
    echo
    echo "cut-rc: _hotfix ready with the fix cherry-picked. NEXT:"
    echo "  1. /bump _hotfix to ${H}"
    echo "  2. write its '## [${H}]' CHANGELOG/metainfo/debian notes (the fix only)"
    echo "  3. run: cut-rc.sh hotfix --continue [--push] [--skip-build]"
}

cmd_hotfix_continue() {
    local branch H pub_tag N
    branch=$(git rev-parse --abbrev-ref HEAD)
    [ "$branch" = _hotfix ] \
        || { echo "cut-rc: hotfix --continue must run on the _hotfix branch (on '$branch')" >&2; exit 1; }
    if git rev-parse -q --verify CHERRY_PICK_HEAD >/dev/null 2>&1; then
        echo "cut-rc: an in-progress cherry-pick is present on _hotfix — resolve/abort first" >&2
        exit 1
    fi
    require_no_version_drift          # against the _hotfix tree (CMakeLists == H)
    H=$(base_version)
    [ -n "$H" ] || { echo "cut-rc: could not read hotfix version from CMakeLists.txt" >&2; exit 1; }
    git rev-parse -q --verify "refs/tags/v${H}" >/dev/null \
        && { echo "cut-rc: v${H} already exists as a public tag" >&2; exit 1; }
    N=$(patch_minus_one "$H")
    pub_tag=$(latest_public_tag)
    [ "$pub_tag" = "v${N}" ] || {
        echo "cut-rc: latest public tag is ${pub_tag}, expected v${N} — a release landed" >&2
        echo "        between hotfix phases; re-run 'cut-rc.sh hotfix' from scratch." >&2
        exit 1
    }
    echo "cut-rc: hotfix — building + tagging public v${H} at _hotfix HEAD"
    build_and_test

    # Extract the [H] section from this (_hotfix) tree for recording on main.
    local hblock; hblock=$(mktemp)
    awk -v ver="$H" '
        $0 ~ ("^## \\[" ver "\\]") { inseg=1; print; next }
        inseg && /^## \[/ { exit }
        inseg { print }
    ' "$CHANGELOG_FILE" > "$hblock"

    if [ "$DO_PUSH" = 1 ]; then
        git tag -a "v${H}" -m "${H} — hotfix"
    else
        echo "  [rehearsal] would create annotated tag v${H} at _hotfix HEAD"
    fi
    confirm_or_print git push origin "v${H}"
    if [ "$DO_PUSH" = 1 ]; then
        local notes_file; notes_file=$(mktemp)
        release_notes "v${H}" > "$notes_file"
        [ -s "$notes_file" ] || echo "Hotfix ${H}." > "$notes_file"
        gh release create "v${H}" --title "${H} — hotfix" --notes-file "$notes_file"
        rm -f "$notes_file"
    else
        echo "  [rehearsal] would run: gh release create v${H} (public, prerelease=false)"
    fi

    # ANTS-2165 INV-4 — record [H] on main below the rolled section so main
    # reads [H+1] > [H] > [N] (or [H] > [N] when no RC was in flight).
    git checkout -q main
    if [ "$DO_PUSH" = 1 ]; then
        record_hotfix_on_main "$H" "$N" "$hblock"
        if ! git diff --quiet "$CHANGELOG_FILE"; then
            git add "$CHANGELOG_FILE"
            git commit -q -m "chore: record [${H}] hotfix in CHANGELOG (ANTS-2165 INV-4)"
            confirm_or_print git push origin main
        fi
        git branch -D _hotfix >/dev/null 2>&1 || true
    else
        echo "  [rehearsal] would record [${H}] in ${CHANGELOG_FILE} on main and prune _hotfix"
    fi
    rm -f "$hblock"
    echo "cut-rc: hotfix v${H} published; back on main."
}

case "$SUBCMD" in
    status)  cmd_status ;;
    new-rc)  cmd_new_rc ;;
    respin)  cmd_respin ;;
    promote) cmd_promote ;;
    cycle)   cmd_cycle ;;
    hotfix)  cmd_hotfix ;;
esac
