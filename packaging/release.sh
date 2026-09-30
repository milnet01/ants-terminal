#!/usr/bin/env bash
#
# release.sh — cut a public release (ANTS-5577).
#
# Every release is a full public release, cut when there is something worth
# shipping. There are no release candidates and no cadence: the tag is
# vX.Y.Z and nothing else.
#
# The base X.Y.Z in CMakeLists.txt is set by `cut-release --bump-only`, never
# by this script. Bump first, then run `release`.
#
# Subcommands:
#   status    Show the base version, the latest public tag and whether
#             `release` has anything to ship.
#   release   In this order:
#               1. check the tree (on main, clean, no version drift)
#               2. merge [Unreleased] into "## [X.Y.Z] - <today>", date the
#                  metainfo and debian entries, pin the OBS recipe to vX.Y.Z,
#                  and commit
#               3. build and test
#               4. push main
#               5. build that commit on every distro in the OBS staging
#                  project, which publishes nothing (ANTS-5305). Red stops
#                  here: no tag exists yet.
#               6. wait for GitHub's CI on that commit (ANTS-1978). Red stops
#                  here too.
#               7. tag vX.Y.Z on that commit and push the tag
#               8. submit to the real OBS project
#               9. wait for release.yml, which builds and signs the AppImage
#                  and creates the GitHub release with its files attached
#              10. wait for the OBS builds
#             Safe to re-run after any failure: each step finds what the last
#             run already did.
#
# Flags:
#   --push          Actually commit, tag, push and publish. Without it the
#                   script is a rehearsal: it runs every check, builds and
#                   tests, prints what the rest would do, and writes no file,
#                   commit or tag.
#   --skip-build    Skip the local build + test (use only when the caller has
#                   already built and tested this exact HEAD).
#   --skip-staging  Skip the OBS staging gate. A distro break is then found
#                   after publishing, which is what the gate exists to stop.
#
# Exit 0 = released and verified. 1 = refused or aborted before the tag.
# 2 = usage. 3 = the tag is out but something after it needs attention; the
# last lines say what. Never force-pushes.

set -euo pipefail

# ANTS-5573 — the OSC 133 tests read this key from the environment, so the
# suite fails when run from an Ants tab. Remove this line when that ships.
unset ANTS_OSC133_KEY

# Temp files made beside their target (TL-6) are removed on any exit, so an
# interrupted run does not leave CHANGELOG.md.XXXXXX in the working tree.
RELEASE_TMPS=()
trap 'rm -f -- "${RELEASE_TMPS[@]+"${RELEASE_TMPS[@]}"}"' EXIT

# ---- Setup -----------------------------------------------------------

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "release: not inside a git repository" >&2
    exit 1
}
cd "$repo_root"

usage() {
    echo "release: usage: release.sh {status|release} [--push] [--skip-build] [--skip-staging]" >&2
    exit 2
}

DO_PUSH=0
SKIP_BUILD=0
SKIP_STAGING=0
SUBCMD=""

for arg in "$@"; do
    case "$arg" in
        --push)         DO_PUSH=1 ;;
        --skip-build)   SKIP_BUILD=1 ;;
        --skip-staging) SKIP_STAGING=1 ;;
        status|release) [ -z "$SUBCMD" ] || usage; SUBCMD="$arg" ;;
        *)              echo "release: unknown argument '$arg'" >&2; usage ;;
    esac
done
[ -n "$SUBCMD" ] || usage

CHANGELOG_FILE="CHANGELOG.md"
METAINFO_FILE="packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml"
DEBIAN_CHANGELOG_FILE="packaging/debian/changelog"
OBS_SERVICE_FILE="packaging/obs/_service"
OBS_SUBMIT="packaging/obs/obs-submit.sh"
OBS_STATUS="packaging/obs/obs-status.sh"
OBS_STAGING_PROJECT="${OBS_STAGING_PROJECT:-home:milnet:ants-terminal-staging}"

# What the tag is out without. Reported at the end, with exit 3.
ATTENTION=()

# Base X.Y.Z from CMakeLists.txt (same regex the drift script trusts).
base_version() {
    grep -oE 'project\s*\([^)]*VERSION\s+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+$' | head -1
}

# Latest public vX.Y.Z tag. Tags from the retired RC flow are not releases.
latest_public_tag() {
    git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1
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
    [ "$SKIP_BUILD" = 1 ] && { echo "release: --skip-build set, skipping build+test gate"; return 0; }
    echo "release: building (cmake --build build)…"
    cmake --build build >/dev/null 2>&1 || { echo "release: build FAILED — aborting" >&2; exit 1; }
    echo "release: running feature tests (ctest -L features)…"
    ctest --test-dir build -L features --output-on-failure >/dev/null 2>&1 \
        || { echo "release: tests FAILED — aborting" >&2; exit 1; }
    echo "release: build + tests green."
}

require_clean_main() {
    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)
    [ "$branch" = main ] || { echo "release: must be on 'main' (on '$branch')" >&2; exit 1; }
    # Untracked files count: the pre-push hook refuses a push from a tree that
    # has any, and that refusal would land after the release commit is made.
    if [ -n "$(git status --porcelain)" ]; then
        echo "release: working tree is dirty — commit, stash or remove what 'git status' lists" >&2
        exit 1
    fi
}

# Commit "$1" (message), aborting loudly if the commit is REFUSED — most often
# by a pre-commit hook such as the standards-mirror gate. `set -e` already
# stops the run, but silently: the only text on screen is the hook's own,
# which says nothing about the release, and the non-zero status is lost the
# moment the run is piped (the project's own token-frugal `| tail` idiom). So
# the next session cannot tell a queued release from a failed one. "$2" states
# what did NOT happen. The staged files are deliberately left in place so the
# operator can see what was written (ANTS-4865).
commit_or_abort() {
    local msg=$1 consequence=$2
    git commit -q -m "$msg" && return 0
    echo "release: ABORTED — 'git commit' was refused (ANTS-4865)." >&2
    echo "         ${consequence}" >&2
    echo "         The staged files are left as written; inspect with 'git diff --cached'." >&2
    echo "         Clear the refusal printed above, then re-run the same command." >&2
    exit 1
}

# ---- CHANGELOG helpers -----------------------------------------------

# True (exit 0) if the "## [Unreleased]" section has ≥1 real entry — a
# changelog bullet (^\s*[-*]) or a "### Category" header. Prose such as the
# "**Theme:**" line does NOT count.
unreleased_has_content() {
    awk '
        /^## \[Unreleased\]/ { inseg=1; next }
        inseg && /^## \[/ { exit }
        inseg && /^[ \t]*[-*][ \t]/ { found=1 }
        inseg && /^### (Added|Changed|Deprecated|Removed|Fixed|Security)/ { found=1 }
        END { exit (found ? 0 : 1) }
    ' "$CHANGELOG_FILE"
}

# True (exit 0) if "## [X.Y.Z]" already carries ≥1 real entry — the same test.
# An interrupted or gate-stopped run leaves exactly this state: the merge
# commit landed and no tag was created.
section_has_content() {
    awk -v ver="$1" '
        index($0, "## [" ver "]") == 1 { inseg=1; next }
        inseg && /^## \[/ { exit }
        inseg && /^[ \t]*[-*][ \t]/ { found=1 }
        inseg && /^### (Added|Changed|Deprecated|Removed|Fixed|Security)/ { found=1 }
        END { exit (found ? 0 : 1) }
    ' "$CHANGELOG_FILE"
}

# True (exit 0) if "## [X.Y.Z]" in file "$1" opens with a "**Theme:**" line,
# before its first category. release-notes.sh turns that lead into the GitHub
# release text, which is also what the project website shows, so a release
# without one publishes a bare link.
section_has_theme() {
    awk -v ver="$2" '
        index($0, "## [" ver "]") == 1 { inseg=1; next }
        inseg && (/^## \[/ || /^### /) { exit }
        inseg && /^\*\*Theme:\*\*/ { found=1; exit }
        END { exit (found ? 0 : 1) }
    ' "$1"
}

# Print CHANGELOG.md with the "## [Unreleased]" body merged into "## [X.Y.Z]",
# leaving "## [Unreleased]" empty. Creates the section when absent. When it
# already has entries — a re-run after the staging gate stopped a release —
# the two are merged category by category, new bullets first, categories in
# Keep a Changelog order (changelog-format.md § 4.1). Prints the file
# unchanged when [Unreleased] is blank. The heading is dated afterwards by
# stamp_release_date.
merge_unreleased() {
    awk -v ver="$1" '
        function trimmed(s) {
            sub(/^([ \t]*\n)+/, "", s)
            sub(/(\n[ \t]*)+$/, "", s)
            return (s ~ /^[ \t]*$/) ? "" : s
        }
        # Lines [from,to] into buf[who, category]; "" is the lead before the
        # first category.
        function parse(from, to, who,    i, cur, key) {
            cur = ""
            for (i = from; i <= to; i++) {
                if (L[i] ~ /^### /) {
                    cur = substr(L[i], 5); sub(/[ \t]+$/, "", cur)
                    if ((who, cur) in seen) buf[who, cur] = buf[who, cur] "\n"
                    else { seen[who, cur] = 1; order[who, ++ncat[who]] = cur }
                    continue
                }
                key = who SUBSEP cur
                buf[key] = (key in buf) ? buf[key] "\n" L[i] : L[i]
            }
        }
        function emit_cat(c,    a, b) {
            if (c in emitted) return
            emitted[c] = 1
            if (!(("T", c) in seen) && !(("U", c) in seen)) return
            a = trimmed(buf["U", c]); b = trimmed(buf["T", c])
            print "### " c; print ""
            if (a != "") { print a; print "" }
            if (b != "") { print b; print "" }
        }
        function emit_section(head,    i, a, b, canon, nc) {
            print head; print ""
            b = trimmed(buf["T", ""]); a = trimmed(buf["U", ""])
            if (b != "") { print b; print "" }
            if (a != "") { print a; print "" }
            nc = split("Added Changed Deprecated Removed Fixed Security", canon, " ")
            for (i = 1; i <= nc; i++) emit_cat(canon[i])
            for (i = 1; i <= ncat["T"]; i++) emit_cat(order["T", i])
            for (i = 1; i <= ncat["U"]; i++) emit_cat(order["U", i])
        }
        { L[NR] = $0 }
        END {
            n = NR; u = 0; t = 0
            for (i = 1; i <= n; i++) {
                if (!u && L[i] ~ /^## \[Unreleased\]/) u = i
                else if (!t && index(L[i], "## [" ver "]") == 1) t = i
            }
            ue = n + 1
            if (u) for (i = u + 1; i <= n; i++) if (L[i] ~ /^## \[/) { ue = i; break }
            if (u) parse(u + 1, ue - 1, "U")
            if (!u || (ncat["U"] == 0 && trimmed(buf["U", ""]) == "")) {
                for (i = 1; i <= n; i++) print L[i]
                exit 0
            }
            te = 0
            if (t) {
                te = n + 1
                for (i = t + 1; i <= n; i++) if (L[i] ~ /^## \[/) { te = i; break }
                parse(t + 1, te - 1, "T")
            }
            for (i = 1; i <= n; i++) {
                if (i == u) {
                    print L[i]; print ""
                    if (!t) emit_section("## [" ver "]")
                    continue
                }
                if (i > u && i < ue) continue
                if (t && i == t) { emit_section(L[i]); continue }
                if (t && i > t && i < te) continue
                print L[i]
            }
        }
    ' "$CHANGELOG_FILE"
}

# Rewrite <file> via awk program "$2..." to a temp + atomic rename. The awk
# program prints the whole transformed file and exits 0 only when it made the
# substitution (else exit 3 = target not found → hard error).
apply_rewrite() {
    local file=$1; shift
    local tmp; tmp=$(mktemp "${file}.XXXXXX")   # same filesystem (TL-6)
    RELEASE_TMPS+=("$tmp")
    if awk "$@" "$file" > "$tmp"; then
        chmod --reference="$file" "$tmp"
        mv "$tmp" "$file"
    else
        rm -f "$tmp"
        echo "release: target not found in ${file}" >&2
        exit 1
    fi
}

# Stamp the release date in all three release-note carriers: the CHANGELOG
# "## [X.Y.Z] - <date>" heading (ASCII hyphen, changelog-format.md § 4.1),
# the metainfo <release version="X.Y.Z" date="…"> attribute, and the debian
# (X.Y.Z-1) block's RFC-2822 trailer (reformatted via `date -R -d`). awk +
# atomic rename, never an in-place stream edit.
stamp_release_date() {
    local v=$1 iso=$2 rfc
    rfc=$(date -R -d "$iso") || { echo "release: bad date '$iso'" >&2; exit 1; }

    # awk programs, single-quoted deliberately: $0 below is awk's own field,
    # not a shell variable. Letting the shell expand it would empty the
    # program, so the quoting is the point rather than an oversight.
    # shellcheck disable=SC2016
    apply_rewrite "$CHANGELOG_FILE" -v ver="$v" -v iso="$iso" '
        !done && index($0, "## [" ver "]") == 1 { print "## [" ver "] - " iso; done=1; next }
        { print }
        END { exit (done ? 0 : 3) }
    '
    # shellcheck disable=SC2016
    apply_rewrite "$METAINFO_FILE" -v ver="$v" -v iso="$iso" '
        !done && $0 ~ ("<release version=\"" ver "\"") {
            sub(/date="[0-9-]+"/, "date=\"" iso "\""); done=1
        }
        { print }
        END { exit (done ? 0 : 3) }
    '
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

# ANTS-3728 — keep packaging/obs/_service's pinned <revision> in lockstep with
# the tag being published.
#
# OBS's trigger_services re-runs the recipe OBS ALREADY HAS. So when a tag push
# fires the webhook and _service still names the previous tag, OBS happily
# rebuilds the PREVIOUS release: green, published, and the wrong version. That
# failure is near-invisible because nothing errors. Bumping the pin here, in the
# release commit itself, means the recipe cannot disagree with the tag it
# shipped alongside.
#
# Absent file is not an error: the test fixtures may omit packaging/obs/.
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

# ---- Gates -----------------------------------------------------------

# Hard version-drift gate: abort non-zero on any drift.
require_no_version_drift() {
    if ! bash packaging/check-version-drift.sh; then
        echo "release: version drift detected — fix before releasing" >&2
        exit 1
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
# a judgement. It prints the list where the operator is already watching; the
# REQUIRED step is the bump-time run that .claude/bump.json owns.
report_shipped_coverage() {
    [ -x tools/check-shipped-coverage.sh ] || return 0
    if ! tools/check-shipped-coverage.sh; then
        echo "release: ^ the coverage gate above found something (ANTS-4714/4759)." >&2
        echo "release:   Not blocking. Shipped work in no CHANGELOG bullet: add the" >&2
        echo "release:   release-note-worthy ones with changelog_log, and say in the" >&2
        echo "release:   release report which were left out on purpose. A bullet whose" >&2
        echo "release:   summary repeats its roadmap headline: reword it to say what" >&2
        echo "release:   shipped, since a defect headline states the problem." >&2
    fi
}

# Both release-note carriers the bump writes must already name this version,
# checked before anything is written so a miss cannot leave a half-stamped tree.
require_release_carriers() {
    local v=$1
    grep -q "<release version=\"${v}\"" "$METAINFO_FILE" \
        || { echo "release: ${METAINFO_FILE} has no <release version=\"${v}\"> entry — run the bump first" >&2; exit 1; }
    grep -q "^ants-terminal (${v}-" "$DEBIAN_CHANGELOG_FILE" \
        || { echo "release: ${DEBIAN_CHANGELOG_FILE} has no (${v}-1) entry — run the bump first" >&2; exit 1; }
}

# ANTS-5305 — build the commit about to be tagged on every distro, in the
# staging project, before the tag exists. The staging project publishes
# nothing. While the only distro signal came from the published tag, the fix
# for any break it found was always one release late.
staging_gate() {
    local sha=$1 ver=$2
    if [ "$SKIP_STAGING" = 1 ]; then
        echo "release: WARNING — --skip-staging set: the distro builds are NOT tested" >&2
        echo "         before the tag. A break will be found after publishing." >&2
        return 0
    fi
    if [ "$DO_PUSH" != 1 ]; then
        echo "  [rehearsal] would build ${sha} as ${ver} in ${OBS_STAGING_PROJECT} and wait for every distro"
        return 0
    fi
    echo "release: staging gate — building ${sha} as ${ver} in ${OBS_STAGING_PROJECT}"
    echo "         (publishes nothing; about half an hour per distro, in parallel)…"
    if ! OBS_PROJECT="$OBS_STAGING_PROJECT" "$OBS_SUBMIT" --staging "$sha" "$ver" \
            || ! OBS_PROJECT="$OBS_STAGING_PROJECT" "$OBS_STATUS" --require-tests; then
        echo "" >&2
        echo "release: ✗ STAGING GATE FAILED — no tag was created and nothing was published." >&2
        echo "         main carries the release commit. Fix the break, commit it, and" >&2
        echo "         re-run 'release.sh release --push'." >&2
        exit 1
    fi
    echo "release: staging gate green on every distro."
}

# ANTS-1978 — GitHub's CI verdict on the commit, read before the tag. The
# local build uses this machine's Qt, which is far newer than the baseline
# ci.yml builds against, and the AppImage is built on the release runner: a
# commit can pass everything here and still fail there. main was pushed before
# the staging gate, so CI has normally finished by the time this asks.
#
# ci.yml skips a push that touches only the changelog or docs, which a release
# commit can be (a re-run the same day changes CHANGELOG.md alone). When no run
# exists for the commit, one is started by hand and waited for.
ci_gate() {
    local sha=$1 id="" i round
    local poll=${RELEASE_POLL_SECS:-30} max=${RELEASE_POLL_MAX:-20}
    if [ "$DO_PUSH" != 1 ]; then
        echo "  [rehearsal] would wait for GitHub CI (ci.yml) on ${sha} and tag only if it passed"
        return 0
    fi
    for round in 1 2; do
        i=0
        while [ "$i" -lt "$max" ]; do
            i=$((i + 1))
            id=$(gh run list --workflow ci.yml --commit "$sha" --limit 1 \
                    --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null) || id=""
            [ -n "$id" ] && break 2
            sleep "$poll"
        done
        if [ "$round" = 1 ]; then
            echo "release: no CI run exists for ${sha} — starting one (gh workflow run ci.yml)…"
            gh workflow run ci.yml --ref main >/dev/null 2>&1 || true
        fi
    done
    if [ -z "$id" ]; then
        echo "release: ✗ no GitHub CI run appeared for ${sha} — no tag was created." >&2
        echo "         Start one with 'gh workflow run ci.yml --ref main', then re-run." >&2
        exit 1
    fi
    echo "release: waiting for GitHub CI run ${id} on ${sha}…"
    if ! gh run watch "$id" --exit-status >/dev/null 2>&1; then
        echo "release: ✗ GitHub CI FAILED on ${sha} (run ${id}) — no tag was created." >&2
        echo "         Read it with 'gh run view ${id} --log-failed', fix it, commit," >&2
        echo "         and re-run 'release.sh release --push'." >&2
        exit 1
    fi
    echo "release: GitHub CI green on ${sha}."
}

# ---- After the tag ---------------------------------------------------

# ANTS-4716 — publish the pinned revision to OBS. The release commit updates
# packaging/obs/_service IN GIT; obs_scm only re-clones when the recipe is
# committed to OBS, so without this the repositories keep building the source
# archive they already hold.
#
# Runs AFTER the tag is pushed: obs-submit.sh refuses when _service pins a tag
# that is not on the remote, and OBS needs it there to clone.
#
# A failure here does NOT undo the release. By this point the tag is pushed,
# so it is reported and the run ends non-zero.
submit_to_obs() {
    echo "release: submitting the pinned revision to OBS…"
    if ! "$OBS_SUBMIT"; then
        ATTENTION+=("OBS submit failed — package repositories still serve the PREVIOUS version. Run: ${OBS_SUBMIT}")
        return 0
    fi
    OBS_SUBMITTED=1
}

wait_for_obs() {
    [ "${OBS_SUBMITTED:-0}" = 1 ] || return 0
    echo "release: waiting for the OBS package builds…"
    "$OBS_STATUS" \
        || ATTENTION+=("an OBS package build did not finish green. Re-check with: ${OBS_STATUS}")
}

# release.yml builds and signs the AppImage on the tag push and creates the
# GitHub release with its files already attached, so there is never a public
# release with nothing to download. This script therefore creates no release
# itself: it waits for that run and checks what it published.
wait_for_github_release() {
    local tag=$1 ver=$2 id="" i=0 names want missing=0
    local poll=${RELEASE_POLL_SECS:-30} max=${RELEASE_POLL_MAX:-20}
    while [ "$i" -lt "$max" ]; do
        i=$((i + 1))
        # Not `--branch "$tag"`: that filter matches branches only, and
        # returns nothing for a run a tag push started (measured 2026-09-30).
        id=$(gh run list --workflow release.yml --event push --limit 20 \
                --json databaseId,headBranch \
                --jq "map(select(.headBranch == \"${tag}\")) | .[0].databaseId // empty" \
                2>/dev/null) || id=""
        [ -n "$id" ] && break
        sleep "$poll"
    done
    if [ -z "$id" ]; then
        ATTENTION+=("no release.yml run appeared for ${tag}. Start one with: gh workflow run release.yml -f tag=${tag}")
        return 0
    fi
    echo "release: waiting for release.yml run ${id} (builds and signs the AppImage, then publishes)…"
    if ! gh run watch "$id" --exit-status >/dev/null 2>&1; then
        ATTENTION+=("release.yml run ${id} failed, so ${tag} has no GitHub release. Read it with: gh run view ${id} --log-failed")
        return 0
    fi
    if [ "$(gh release view "$tag" --json isDraft --jq .isDraft 2>/dev/null)" != false ]; then
        ATTENTION+=("the GitHub release for ${tag} is missing or still a draft. Check: gh release view ${tag}")
        return 0
    fi
    names=$(gh release view "$tag" --json assets --jq '.assets[].name' 2>/dev/null) || names=""
    for want in "Ants_Terminal-${ver}-x86_64.AppImage" \
                "Ants_Terminal-${ver}-x86_64.AppImage.zsync" \
                "Ants_Terminal-${ver}-x86_64.AppImage.manifest" \
                "Ants_Terminal-${ver}-x86_64.AppImage.manifest.sig" \
                "Ants_Terminal-x86_64.AppImage"; do
        printf '%s\n' "$names" | grep -Fxq "$want" || missing=1
    done
    if [ "$missing" = 1 ]; then
        ATTENTION+=("the GitHub release for ${tag} is missing a file. Check: gh release view ${tag}")
        return 0
    fi
    echo "release: GitHub release ${tag} is public with every file attached."
}

# ---- Subcommands -----------------------------------------------------

cmd_status() {
    local base
    base=$(base_version)
    echo "Base version (CMakeLists.txt): ${base}"
    echo "Latest public tag:            $(latest_public_tag)"
    if git rev-parse -q --verify "refs/tags/v${base}" >/dev/null; then
        echo "State:                        v${base} is already public — bump before the next release"
    elif unreleased_has_content || section_has_content "$base"; then
        echo "State:                        ${base} has notes and is not yet released"
    else
        echo "State:                        nothing to release — no entries for ${base}"
    fi
}

# Step 2 of the header: write the release notes' final form and commit it.
# Every refusal here happens before the first write.
prepare_release_commit() {
    local base=$1 tag=$2 today=$3 merged
    if ! unreleased_has_content && ! section_has_content "$base"; then
        echo "release: nothing to release — neither '## [Unreleased]' nor" >&2
        echo "         '## [${base}]' has any entries." >&2
        exit 1
    fi
    require_release_carriers "$base"

    # Beside the target, not in /tmp: /tmp is tmpfs here, where mv is a copy
    # plus unlink and the rename below would not be atomic (audit TL-6).
    merged=$(mktemp "${CHANGELOG_FILE}.XXXXXX")
    RELEASE_TMPS+=("$merged")
    merge_unreleased "$base" > "$merged" \
        || { echo "release: could not merge [Unreleased] into [${base}]" >&2; exit 1; }
    if ! section_has_theme "$merged" "$base"; then
        rm -f "$merged"
        echo "release: '## [${base}]' would have no '**Theme:**' line." >&2
        echo "         Write one at the top of '## [Unreleased]': a plain-language" >&2
        echo "         summary of what is new. It becomes the GitHub release text," >&2
        echo "         which the project website shows." >&2
        exit 1
    fi

    if [ "$DO_PUSH" != 1 ]; then
        rm -f "$merged"
        echo "  [rehearsal] would merge [Unreleased] into '## [${base}] - ${today}', date the"
        echo "  [rehearsal]   metainfo and debian entries, pin ${OBS_SERVICE_FILE} to ${tag}, and commit"
        return 0
    fi

    chmod --reference="$CHANGELOG_FILE" "$merged"
    mv "$merged" "$CHANGELOG_FILE"
    stamp_release_date "$base" "$today"
    pin_obs_service_revision "$tag"
    git add "$CHANGELOG_FILE" "$METAINFO_FILE" "$DEBIAN_CHANGELOG_FILE"
    [ -f "$OBS_SERVICE_FILE" ] && git add "$OBS_SERVICE_FILE"
    git diff --cached --quiet || commit_or_abort \
        "chore: release ${base} (notes dated ${today})" \
        "No tag was created and nothing was pushed."
}

cmd_release() {
    require_clean_main
    require_no_version_drift
    report_shipped_coverage
    local base tag today sha resuming=0
    base=$(base_version)
    [ -n "$base" ] || { echo "release: could not read base version from CMakeLists.txt" >&2; exit 1; }
    tag="v${base}"
    today=$(date +%F)

    # The tag already exists. At HEAD it is OUR interrupted run: carry on from
    # the push. Anywhere else this version is already public.
    # Quoted for SC1083: the braces are git's peel syntax, meant literally.
    if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
        if [ "$(git rev-parse "refs/tags/${tag}^{commit}")" = "$(git rev-parse HEAD)" ]; then
            resuming=1
            echo "release: ${tag} is already tagged at HEAD — resuming from the push."
        else
            echo "release: ${base} is already public (tag ${tag} exists)." >&2
            echo "         Bump to the next version first ('cut-release --bump-only')." >&2
            exit 1
        fi
    fi

    if [ "$SKIP_STAGING" != 1 ] && [ "$resuming" = 0 ] \
            && { [ ! -x "$OBS_SUBMIT" ] || [ ! -x "$OBS_STATUS" ]; }; then
        echo "release: the staging gate cannot run — ${OBS_SUBMIT} or" >&2
        echo "         ${OBS_STATUS} is missing or not executable." >&2
        exit 1
    fi

    if [ "$resuming" = 0 ]; then
        prepare_release_commit "$base" "$tag" "$today"
        build_and_test
    fi
    sha=$(git rev-parse HEAD)

    # main must be on origin before the gate: obs_scm clones from GitHub.
    confirm_or_print git push origin main

    if [ "$resuming" = 0 ]; then
        staging_gate "$sha" "$base"
        ci_gate "$sha"
        if [ "$DO_PUSH" = 1 ]; then
            # The gates take the better part of an hour; tag what they tested.
            if [ "$(git rev-parse HEAD)" != "$sha" ] || ! git diff --quiet || ! git diff --cached --quiet; then
                echo "release: the tree changed while the staging gate ran — no tag was created." >&2
                echo "         Re-run so the gate tests what will be tagged." >&2
                exit 1
            fi
            git tag -a "${tag}" -m "${base}" "$sha"
            echo "release: created annotated tag ${tag} at ${sha}"
        else
            echo "  [rehearsal] would create annotated tag ${tag} at ${sha}"
        fi
    fi
    confirm_or_print git push origin "${tag}"

    if [ "$DO_PUSH" != 1 ]; then
        echo "  [rehearsal] would run: ${OBS_SUBMIT}"
        echo "  [rehearsal] would wait for release.yml to publish ${tag} with its files, then for the OBS builds"
        return 0
    fi

    submit_to_obs          # starts the OBS builds; they run while GitHub builds
    wait_for_github_release "$tag" "$base"
    wait_for_obs

    echo
    if [ "${#ATTENTION[@]}" -gt 0 ]; then
        echo "release: ${tag} is tagged and pushed, but needs attention:" >&2
        local item
        for item in "${ATTENTION[@]}"; do echo "  - ${item}" >&2; done
        exit 3
    fi
    echo "release: ${tag} released — GitHub release public with its files, OBS builds green."
}

case "$SUBCMD" in
    status)  cmd_status ;;
    release) cmd_release ;;
esac
