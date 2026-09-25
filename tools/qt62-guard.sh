#!/usr/bin/env bash
# ANTS-4131 — the Qt 6.2 floor guard, as a unit with ONE owner.
#
# The project's minimum supported Qt is 6.2 (dependencies.md § 4). This dev box
# runs a much newer Qt, so a newer-than-floor API compiles here, passes the full
# suite here, and passes the pre-push hook here — and breaks only in CI's
# `qt62-baseline` job. That is not hypothetical: ANTS-4108 shipped
# `QRegularExpressionMatch::hasCaptured()` (Qt 6.3+) and broke CI on three
# consecutive pushes. Only a compile against a real Qt 6.2 can see it.
#
# So: compile the whole project — app AND every test TU, exactly as ci.yml's
# qt62-baseline job does — inside a podman ubuntu:22.04 container. No ctest;
# running the suite is build-test's remit. This is a COMPILE guard.
#
# Why this is a script and not a function inside tools/ci-parity.sh: two
# callers need it (that script's --qt62 gate, and tools/hooks/pre-push), and
# the image/volume/tag derivation must not exist in two places — a second copy
# is how the parity harness silently stops reproducing CI.
#
# ── What makes it cheap enough to run before a push ──────────────────────────
#
# Measured on this host, 2026-08-12:
#
#     apt layer (the dependency image) ..... 61 s, once
#     compile, empty build tree ............ 617 s
#     compile, warm tree, no change ........   5 s
#     compile, warm tree, one TU touched ...   7 s
#     compile, warm tree, floor violation ..   1 s  (fails, naming the symbol)
#
# The ROADMAP bullet assumed apt was most of the ~25 min and scoped the fix as
# caching it. It is not: apt is one minute of ten, and the compile is the rest.
# Both are cached here — the apt layer as an image, and the build tree as a
# podman volume — and it is the SECOND that turns the leg from unusable into a
# 7-second pre-push check.
#
# ── Keying ───────────────────────────────────────────────────────────────────
#
# The apt package set is EXTRACTED FROM ci.yml at run time, never mirrored
# here. A hand-copied list would be a third copy of one set (ci.yml,
# release.yml, this file) and the copy that drifts is the one nobody runs.
# Extraction makes the lockstep mechanical; if ci.yml stops parsing, this
# script REFUSES rather than guessing, because a guessed package set compiles
# something that is not the baseline and reports it as the baseline.
#
# Image and build volume are both keyed to a digest of that package list, so a
# change to ci.yml yields a fresh image AND a fresh tree — a stale pair can
# never answer for a changed one.
#
# Disk: ~1.2 GB image + ~3.5 GB build volume. Reclaim with --clean.
#
# ── Interruption (ANTS-5124) ─────────────────────────────────────────────────
#
# A killed push must not leave the compile running, nor leave a tree the next
# run trusts. The container is named and runs under --init, and a trap on
# INT, TERM and HUP stops it. `podman run` is started in the background and
# waited on, because bash runs a trap only after a foreground command returns.
# A marker is written as the compile starts and removed only when it finishes,
# so a run killed by a signal no trap sees leaves it too. --warm-only skips a
# marked tree; a normal run discards the volume and builds cold.
#
# ── Usage ────────────────────────────────────────────────────────────────────
#
#   tools/qt62-guard.sh              # run the guard; build the image if needed
#   tools/qt62-guard.sh --warm-only  # run ONLY if both caches exist; else skip 0
#   tools/qt62-guard.sh --clean      # drop every cached image + build volume
#   tools/qt62-guard.sh --print      # show the resolved image/volume/packages
#
# Any of the above takes `--job build-test` to guard ci.yml's build-test job
# instead: ubuntu:24.04, its GCC 13 and mold. This box's GCC 16 extracts fewer
# members from a static archive than GCC 13 + mold does, so an under-linked
# target links here and fails only there — bench_partition_walk broke three
# CI pushes that way. Same compile, same caches, own image and volume.
#
# Exit: 0 pass (or skipped under --warm-only), 1 compile failure or refusal,
# 130 interrupted.
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || {
    echo "qt62-guard: cannot cd to repo root" >&2; exit 1; }

mode="run"
job="qt62-baseline"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --warm-only) mode="warm-only" ;;
        --clean)     mode="clean" ;;
        --print)     mode="print" ;;
        --job)       job="${2:-}"; shift ;;
        *) echo "qt62-guard: unknown argument '$1' (see the header for usage)" >&2
           exit 1 ;;
    esac
    shift
done

# Per job: the container base, the ci.yml install step the packages come from,
# and the name prefix its image and build volume are cached under.
case "$job" in
    qt62-baseline)
        qt62_base="docker.io/library/ubuntu:22.04"
        qt62_step="Install Qt6 + build deps (release baseline)"
        qt62_prefix="ants-qt62"
        qt62_what="the Qt 6.2 floor"
        qt62_cmd="tools/qt62-guard.sh" ;;
    build-test)
        qt62_base="docker.io/library/ubuntu:24.04"
        qt62_step="Install Qt6 + build + packaging deps (cached)"
        qt62_prefix="ants-ubuntu24"
        qt62_what="CI's build-test toolchain (ubuntu 24.04, GCC 13, mold)"
        qt62_cmd="tools/qt62-guard.sh --job build-test" ;;
    *) echo "qt62-guard: unknown --job '$job' (qt62-baseline or build-test)" >&2
       exit 1 ;;
esac

# `git` + `ca-certificates` are NOT in ci.yml's list and are NOT a divergence
# from it: the GitHub ubuntu-22.04 runner ships them pre-installed, but the
# bare ubuntu:22.04 image does not — and the tests' GTest fallback
# (CMakeLists.txt, FetchContent GIT_REPOSITORY googletest) git-clones over
# HTTPS when no system GTest is found. The runner has none either, so CI takes
# the same FetchContent path. These reproduce the runner ENV.
qt62_extra_pkgs="git ca-certificates"

# Host-side, beside nothing else: the markers must outlive a killed run.
qt62_state_dir="${XDG_CACHE_HOME:-$HOME/.cache}/ants-terminal/qt62-guard"

need_podman() {
    command -v podman >/dev/null 2>&1
}

# --- clean ------------------------------------------------------------------
if [[ "$mode" == "clean" ]]; then
    if ! need_podman; then
        echo "qt62-guard: podman not installed — nothing to clear."
        exit 0
    fi
    echo "qt62-guard: removing cached images + build volumes"
    podman image ls --format '{{.Repository}}:{{.Tag}}' \
        | grep "^localhost/$qt62_prefix-baseline:" | xargs -r podman image rm -f
    podman volume ls --format '{{.Name}}' \
        | grep "^$qt62_prefix-build-" | xargs -r podman volume rm -f
    # This job's markers only: the other job's volume may be marked interrupted.
    rm -f "$qt62_state_dir/$qt62_prefix-build-"*.interrupted
    echo "qt62-guard: cache cleared."
    exit 0
fi

# --- extract ci.yml's package set -------------------------------------------
# Prints one package per line; non-zero (with a diagnosis) if ci.yml no longer
# parses. Refusing is deliberate — see the header.
qt62_ci_packages() {
    local pkgs
    # qt62-baseline installs with `apt-get install` in a run: block; build-test
    # passes a `packages: >-` list to cache-apt-pkgs-action. Both parse here.
    pkgs="$(awk -v step="      - name: $qt62_step" '
        $0 == step { inblk=1; next }
        inblk && /^      - name:/ { exit }
        inblk { print }
    ' .github/workflows/ci.yml \
      | sed -e 's/#.*$//' \
            -e '/apt-get update/d' \
            -e 's/.*--no-install-recommends//' \
            -e '/run: |/d' \
            -e '/uses:/d' -e '/with:/d' -e '/version:/d' \
            -e 's/packages: >-//' \
            -e 's/\\[[:space:]]*$//' \
      | tr -s '[:space:]' '\n' | sed '/^$/d' | sort -u)"

    # The sentinel package must be present and the set plausibly whole.
    if ! grep -qx 'qt6-base-dev' <<<"$pkgs" || (( $(wc -l <<<"$pkgs") < 10 )); then
        echo "qt62-guard: cannot parse ci.yml's $job install step." >&2
        echo "            Expected the '$qt62_step'" >&2
        echo "            step to be a package list; got:" >&2
        while IFS= read -r l; do echo "              $l" >&2; done <<<"$pkgs"
        echo "            Update qt62_ci_packages() in tools/qt62-guard.sh." >&2
        return 1
    fi
    printf '%s\n' "$pkgs"
}

qt62_pkgs=""; qt62_tag=""; qt62_image=""; qt62_volume=""
qt62_container=""; qt62_interrupted_marker=""
qt62_resolve() {
    local list
    list="$(qt62_ci_packages)" || return 1
    qt62_pkgs="$(printf '%s\n%s\n' "$list" "$(tr ' ' '\n' <<<"$qt62_extra_pkgs")" | sort -u)"
    qt62_tag="$(printf '%s\n%s\n' "$qt62_base" "$qt62_pkgs" | sha256sum | cut -c1-12)"
    qt62_image="localhost/$qt62_prefix-baseline:$qt62_tag"
    qt62_volume="$qt62_prefix-build-$qt62_tag"
    qt62_container="$qt62_prefix-guard-$qt62_tag"
    qt62_interrupted_marker="$qt62_state_dir/$qt62_volume.interrupted"
}

qt62_resolve || exit 1

if [[ "$mode" == "print" ]]; then
    echo "base:    $qt62_base"
    echo "tag:     $qt62_tag"
    echo "image:   $qt62_image"
    echo "volume:  $qt62_volume"
    echo "packages ($(wc -l <<<"$qt62_pkgs"), extracted from ci.yml):"
    while IFS= read -r l; do echo "  $l"; done <<<"$qt62_pkgs"
    exit 0
fi

if ! need_podman; then
    echo "qt62-guard: ⊘ podman not installed — the guard for $qt62_what did NOT run."
    echo "            CI's $job job still covers this. Install podman to"
    echo "            cover it locally."
    [[ "$mode" == "warm-only" ]] && exit 0
    exit 1
fi

# --- an earlier compile still running (ANTS-5124) ----------------------------
# Its run died without stopping it. Two compiles must not share one tree.
if [[ "$(podman container inspect -f '{{.State.Running}}' "$qt62_container" \
         2>/dev/null)" == "true" ]]; then
    echo "qt62-guard: ⊘ an earlier $job compile is still running ($qt62_container)."
    echo "            Wait for it (podman wait $qt62_container), or stop it"
    echo "            (podman stop $qt62_container); the next run then builds cold."
    [[ "$mode" == "warm-only" ]] && exit 0
    exit 1
fi

# --- warm-only precondition -------------------------------------------------
# The hook must never pay a 10-minute cold build: that is how a hook gets
# bypassed, and a caller timeout killing it mid-ninja leaves a tree this
# project treats as untrustworthy. Both caches present == warm by construction,
# because the run that created them also populated the tree.
if [[ "$mode" == "warm-only" ]]; then
    missing=""
    podman image exists "$qt62_image"   || missing="image"
    podman volume exists "$qt62_volume" || missing="${missing:+$missing and }build tree"
    if [[ -n "$missing" ]]; then
        echo "qt62-guard: ⊘ $job guard SKIPPED — no cached $missing for the"
        echo "            current ci.yml package set. Building it is a one-off ~11 min"
        echo "            (61 s image + ~10 min first compile), too long to sit inside"
        echo "            a push. Warm it once, then this check costs ~7 s:"
        echo "              $qt62_cmd"
        echo "            CI's $job job still covers this push."
        exit 0
    fi
    # ANTS-5124 — an incremental result over a killed compile is a false pass.
    if [[ -e "$qt62_interrupted_marker" ]]; then
        echo "qt62-guard: ⊘ $job guard SKIPPED — the cached build tree was"
        echo "            interrupted mid-compile, so it cannot be trusted. Rebuild"
        echo "            it once, cold:"
        echo "              $qt62_cmd"
        echo "            CI's $job job still covers this push."
        exit 0
    fi
fi

# --- build the dependency image on first use --------------------------------
qt62_ensure_image() {
    if podman image exists "$qt62_image"; then return 0; fi
    echo "qt62-guard: building $qt62_image (one-off; ~61 s)…"
    local ctx rc
    ctx="$(mktemp -d)" || return 1
    {
        echo "FROM $qt62_base"
        echo "ENV DEBIAN_FRONTEND=noninteractive"
        echo "RUN apt-get update -qq \\"
        echo " && apt-get install -y --no-install-recommends \\"
        # Deliberate word-splitting: one package per continuation line.
        # shellcheck disable=SC2086
        printf '      %s \\\n' $qt62_pkgs
        echo " && rm -rf /var/lib/apt/lists/*"
    } > "$ctx/Containerfile"
    podman build --security-opt label=disable -t "$qt62_image" "$ctx"
    rc=$?
    rm -rf "$ctx"
    return $rc
}

qt62_ensure_image || {
    echo "qt62-guard: image build failed" >&2; exit 1; }

# --- the compile guard ------------------------------------------------------
# Source is bind-mounted READ-ONLY, so a stray write fails loudly and the host
# repo and its build*/ trees are never touched. The build tree lives in the
# named volume, which is what makes a re-run incremental.
if [[ -e "$qt62_interrupted_marker" ]]; then
    echo "qt62-guard: the cached build tree was interrupted mid-compile;"
    echo "            discarding it and building cold."
    if podman volume exists "$qt62_volume"; then
        podman volume rm -f "$qt62_volume" >/dev/null || {
            echo "qt62-guard: cannot remove $qt62_volume" >&2; exit 1; }
    fi
    rm -f "$qt62_interrupted_marker"
fi

trap 'echo "qt62-guard: interrupted — stopping $qt62_container; the build tree stays marked interrupted." >&2
      podman stop -t 5 "$qt62_container" >/dev/null 2>&1
      exit 130' INT TERM HUP
mkdir -p "$qt62_state_dir" && : > "$qt62_interrupted_marker" || {
    echo "qt62-guard: cannot write $qt62_interrupted_marker" >&2; exit 1; }
# ANTS-5192 — a compile cache that outlives the build volume, so a cold tree
# (new package set, or a tree discarded after an interruption) still compiles
# from cache. Inside the checkout, so on the same drive as the source rather
# than under $HOME; gitignored. ccache is in both jobs' ci.yml package lists.
qt62_ccache="$PWD/.ccache-guard/$qt62_prefix"
mkdir -p "$qt62_ccache" || {
    echo "qt62-guard: cannot create $qt62_ccache" >&2; exit 1; }
echo "qt62-guard: compiling for $qt62_what ($qt62_base, tag $qt62_tag)…"
podman run --rm --security-opt label=disable --init --name "$qt62_container" \
    -v "$PWD:/src:ro" -v "$qt62_volume:/build" -v "$qt62_ccache:/ccache" \
    -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE=2G -e CCACHE_COMPRESS=1 \
    -w /src "$qt62_image" \
    bash -euo pipefail -c '
        cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        cmake --build /build --parallel
    ' &
wait $!
rc=$?
trap - INT TERM HUP
rm -f "$qt62_interrupted_marker"

if (( rc != 0 )); then
    echo >&2
    echo "qt62-guard: ✗ FAILED to build for $qt62_what." >&2
    echo "            This is the class CI catches as $job and nothing local" >&2
    echo "            can see. Check the error above for the offending symbol:" >&2
    echo "            an API newer than the Qt floor (dependencies.md § 4), or a" >&2
    echo "            target that only links under this box's newer compiler." >&2
    exit 1
fi
echo "qt62-guard: ✓ compiles for $qt62_what."
exit 0
