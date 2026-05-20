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
#   status                 Show base version, latest RC, in-flight state.
#   new-rc                 Cut vX.Y.Z-rc1 (or rcN+1) from main HEAD.
#   respin <sha> [sha...]  Cherry-pick fix(es) onto the latest RC tag,
#                          re-tag rc(N+1) (ANTS-1318 §2.3).
#   promote                Tag public vX.Y.Z at the latest RC's commit
#                          (INV-2 zero-diff check), prerelease=false.
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
SUBCMD=""
declare -a RESPIN_SHAS=()

for arg in "$@"; do
    case "$arg" in
        --push)          DO_PUSH=1 ;;
        --skip-build)    SKIP_BUILD=1 ;;
        --force-non-wed) FORCE_NON_WED=1 ;;
        status|new-rc|respin|promote)
            if [ -z "$SUBCMD" ]; then SUBCMD="$arg"; else RESPIN_SHAS+=("$arg"); fi ;;
        *)               RESPIN_SHAS+=("$arg") ;;
    esac
done

[ -n "$SUBCMD" ] || { echo "cut-rc: usage: cut-rc.sh {status|new-rc|respin <sha…>|promote} [--push] [--skip-build] [--force-non-wed]" >&2; exit 2; }

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

changelog_excerpt() {
    # Body of the first "## [X.Y.Z]" section (not [Unreleased]).
    awk '
        /^## \[[0-9]+\.[0-9]+\.[0-9]+\]/ { if (seen) exit; seen=1; next }
        seen { print }
    ' CHANGELOG.md
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
        echo "In-flight RC:                 v${inflight}-rc${n}  (no public v${inflight} yet)"
    else
        echo "In-flight RC:                 none"
    fi
}

cmd_new_rc() {
    require_clean_main
    wednesday_guard
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

    n=$(( $(latest_rc_n "$base") + 1 ))
    tag="v${base}-rc${n}"
    git rev-parse -q --verify "refs/tags/${tag}" >/dev/null \
        && { echo "cut-rc: tag ${tag} already exists" >&2; exit 1; }

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

    local notes; notes=$(changelog_excerpt)
    if [ "$DO_PUSH" = 1 ]; then
        echo "+ gh release create ${tag} --prerelease --title \"${base} RC${n} — Patron preview\""
        gh release create "${tag}" --prerelease \
            --title "${base} RC${n} — Patron preview" \
            --notes "${notes:-See CHANGELOG.md for highlights.}"
    else
        echo "  [rehearsal] would run: gh release create ${tag} --prerelease --title \"${base} RC${n} — Patron preview\" --notes <CHANGELOG [${base}] body>"
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

cmd_promote() {
    require_clean_main
    wednesday_guard
    local inflight n rctag pub
    inflight=$(inflight_base)
    [ -n "$inflight" ] || { echo "cut-rc: no RC in flight to promote" >&2; exit 1; }
    n=$(latest_rc_n "$inflight")
    rctag="v${inflight}-rc${n}"
    pub="v${inflight}"
    # INV-2: the public tag is the RC's exact bits.
    if ! git diff --quiet "${rctag}"; then
        echo "cut-rc: working tree differs from ${rctag}; promote tags the RC's commit." >&2
    fi
    echo "cut-rc: promoting ${rctag} → ${pub} (public, prerelease=false)"
    if [ "$DO_PUSH" = 1 ]; then
        git tag -a "${pub}" -m "${inflight}" "${rctag}^{commit}"
    else
        echo "  [rehearsal] would create annotated tag ${pub} at ${rctag}"
    fi
    confirm_or_print git push origin "${pub}"
    if [ "$DO_PUSH" = 1 ]; then
        gh release create "${pub}" \
            --title "${inflight}" \
            --notes "$(changelog_excerpt)"
    else
        echo "  [rehearsal] would run: gh release create ${pub} (public, prerelease=false) …"
    fi
    echo "cut-rc: ${pub} promoted. Now bump to the next patch and run 'new-rc'"
    echo "        to cut the following week's RC (ANTS-1318 §2.4 dual-cut)."
}

case "$SUBCMD" in
    status)  cmd_status ;;
    new-rc)  cmd_new_rc ;;
    respin)  cmd_respin ;;
    promote) cmd_promote ;;
esac
