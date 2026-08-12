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
# ── Usage ────────────────────────────────────────────────────────────────────
#
#   tools/qt62-guard.sh              # run the guard; build the image if needed
#   tools/qt62-guard.sh --warm-only  # run ONLY if both caches exist; else skip 0
#   tools/qt62-guard.sh --clean      # drop every cached image + build volume
#   tools/qt62-guard.sh --print      # show the resolved image/volume/packages
#
# Exit: 0 pass (or skipped under --warm-only), 1 compile failure or refusal.
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || {
    echo "qt62-guard: cannot cd to repo root" >&2; exit 1; }

qt62_base="docker.io/library/ubuntu:22.04"

# `git` + `ca-certificates` are NOT in ci.yml's list and are NOT a divergence
# from it: the GitHub ubuntu-22.04 runner ships them pre-installed, but the
# bare ubuntu:22.04 image does not — and the tests' GTest fallback
# (CMakeLists.txt, FetchContent GIT_REPOSITORY googletest) git-clones over
# HTTPS when no system GTest is found. The runner has none either, so CI takes
# the same FetchContent path. These reproduce the runner ENV.
qt62_extra_pkgs="git ca-certificates"

mode="run"
case "${1:-}" in
    --warm-only) mode="warm-only" ;;
    --clean)     mode="clean" ;;
    --print)     mode="print" ;;
    "")          ;;
    *) echo "qt62-guard: unknown argument '$1' (see the header for usage)" >&2
       exit 1 ;;
esac

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
        | grep '^localhost/ants-qt62-baseline:' | xargs -r podman image rm -f
    podman volume ls --format '{{.Name}}' \
        | grep '^ants-qt62-build-' | xargs -r podman volume rm -f
    echo "qt62-guard: cache cleared."
    exit 0
fi

# --- extract ci.yml's package set -------------------------------------------
# Prints one package per line; non-zero (with a diagnosis) if ci.yml no longer
# parses. Refusing is deliberate — see the header.
qt62_ci_packages() {
    local pkgs
    pkgs="$(awk '
        /^      - name: Install Qt6 \+ build deps \(release baseline\)$/ { inblk=1; next }
        inblk && /^      - name:/ { exit }
        inblk { print }
    ' .github/workflows/ci.yml \
      | sed -e 's/#.*$//' \
            -e '/apt-get update/d' \
            -e 's/.*--no-install-recommends//' \
            -e '/run: |/d' \
            -e 's/\\[[:space:]]*$//' \
      | tr -s '[:space:]' '\n' | sed '/^$/d' | sort -u)"

    # The sentinel package must be present and the set plausibly whole.
    if ! grep -qx 'qt6-base-dev' <<<"$pkgs" || (( $(wc -l <<<"$pkgs") < 10 )); then
        echo "qt62-guard: cannot parse ci.yml's qt62-baseline install step." >&2
        echo "            Expected the 'Install Qt6 + build deps (release baseline)'" >&2
        echo "            step to be an apt-get install list; got:" >&2
        while IFS= read -r l; do echo "              $l" >&2; done <<<"$pkgs"
        echo "            Update qt62_ci_packages() in tools/qt62-guard.sh." >&2
        return 1
    fi
    printf '%s\n' "$pkgs"
}

qt62_pkgs=""; qt62_tag=""; qt62_image=""; qt62_volume=""
qt62_resolve() {
    local list
    list="$(qt62_ci_packages)" || return 1
    qt62_pkgs="$(printf '%s\n%s\n' "$list" "$(tr ' ' '\n' <<<"$qt62_extra_pkgs")" | sort -u)"
    qt62_tag="$(printf '%s\n%s\n' "$qt62_base" "$qt62_pkgs" | sha256sum | cut -c1-12)"
    qt62_image="localhost/ants-qt62-baseline:$qt62_tag"
    qt62_volume="ants-qt62-build-$qt62_tag"
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
    echo "qt62-guard: ⊘ podman not installed — the Qt 6.2 floor guard did NOT run."
    echo "            CI's qt62-baseline job still covers this. Install podman to"
    echo "            cover it locally."
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
        echo "qt62-guard: ⊘ Qt 6.2 floor guard SKIPPED — no cached $missing for the"
        echo "            current ci.yml package set. Building it is a one-off ~11 min"
        echo "            (61 s image + ~10 min first compile), too long to sit inside"
        echo "            a push. Warm it once, then this check costs ~7 s:"
        echo "              tools/qt62-guard.sh"
        echo "            CI's qt62-baseline job still covers this push."
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
echo "qt62-guard: compiling against Qt 6.2 ($qt62_base, tag $qt62_tag)…"
podman run --rm --security-opt label=disable \
    -v "$PWD:/src:ro" -v "$qt62_volume:/build" -w /src "$qt62_image" \
    bash -euo pipefail -c '
        cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build /build --parallel
    '
rc=$?

if (( rc != 0 )); then
    echo >&2
    echo "qt62-guard: ✗ FAILED to compile on the Qt 6.2 floor." >&2
    echo "            This is the class CI catches as qt62-baseline and nothing" >&2
    echo "            local can see — an API newer than the floor (dependencies.md" >&2
    echo "            § 4). Check the error above for the offending symbol." >&2
    exit 1
fi
echo "qt62-guard: ✓ compiles on the Qt 6.2 floor."
exit 0
