#!/usr/bin/env bash
# ANTS-2134 — reproduce CI test conditions locally.
# ANTS-3410 — extended into a full pre-push CI mirror (--lints / --asan / --full).
#
# ANTS-4392 — WHAT THIS IS, precisely: a hand-maintained PARALLEL
# IMPLEMENTATION of .github/workflows/ci.yml. It does not parse, read or
# execute that file — there is no workflow parser and no `act` here — it
# re-states the jobs in shell. So the two CAN DRIFT, and the class of drift
# this cannot catch by construction is anything declared in ci.yml that the
# script never knew to assume: a runner package, an env var, an action
# version, a `paths-ignore` rule.
#
# ANTS-4391 is what that costs. `ripgrep` was declared by no job in ci.yml and
# CI was red for five commits, invisible to every local run — rg exists on the
# dev box, so no parallel implementation could ever have noticed. The repair
# was a STATIC guard comparing source against the workflow
# (tests/features/ci_workflow_deps), which is the shape that closes this hole:
# check the two agree, rather than pretending one runs the other.
#
# Driving the real workflow with `act` was considered and rejected: it pulls
# container images and is slow enough that nobody would run it before a push,
# and a gate nobody runs catches nothing.
#
# Tests pass on the dev box but fail on the GitHub runner because the two
# environments differ in ways that change test outcomes:
#   * Locale — CI runs under C.UTF-8 (POSIX collation); a dev box usually
#     runs a UTF-8 locale (Unicode collation). This silently broke a
#     QCollator numeric sort (ANTS-2120).
#   * Load / timing — the runner is a throttled 4-vCPU host under queue
#     load, so timing races fire there but not on an idle multi-core box
#     (ANTS-2130: a mid-execve /proc window).
#
# Default mode runs the suite the way CI's build-test job does. The opt-in
# --lints / --asan / --full phases add the remaining reproducible CI gates,
# so a broken push (like ANTS-3391's 14 B wire-budget overrun, which stayed
# CI-red across three commits) is caught locally in one command. Build +
# test happen in isolated build-ci-parity*/ trees so the live build/ binary
# is never relinked underneath a running instance (ANTS-2025).
#
# Usage:
#   tools/ci-parity.sh                 # build-test: Release build + ctest under C.UTF-8
#   tools/ci-parity.sh --lints         #  + cppcheck / appstream / desktop / groff /
#                                      #    version-drift / bash+zsh+fish completions
#   tools/ci-parity.sh --asan          #  + build-asan: Debug ASan/UBSan build, sanitized
#                                      #    ctest, --version/--help smoke
#   tools/ci-parity.sh --qt62          #  + qt62-baseline compile guard in a podman
#                                      #    ubuntu:22.04 / Qt 6.2 container (needs podman)
#   tools/ci-parity.sh --qt62-clean    # drop the cached qt62 image + build volume
#   tools/ci-parity.sh --full          #  --lints + --asan + --qt62 (every CI gate)
#   tools/ci-parity.sh --repeat 5      # rerun each test up to 5x, fail on first flake
#   tools/ci-parity.sh --stress        # add background CPU load (needs stress-ng)
#   tools/ci-parity.sh -R SomeTest     # extra args pass through to ctest
#
# Gates accumulate: every requested gate runs and all failures are listed in
# the final summary (the run still exits non-zero). A gate whose tool is
# absent is SKIPPED with a loud warning and listed in the summary, so a local
# "green" never silently masks a gate CI will still run.
#
# The qt62-baseline job (ubuntu-22.04 / Qt 6.2 compile guard) needs a distro +
# Qt older than an openSUSE dev box provides, so --qt62 runs it inside a podman
# ubuntu:22.04 container (source bind-mounted read-only, build tree in a podman
# volume so the host repo stays clean). It is the only gate that needs a
# container, and SKIPs loudly when podman is absent; --full includes it.
# ANTS-4131 caches both its apt layer and its build tree — see the section
# comment above qt62_ci_packages() for the keying and the disk budget.
#
# NB: no `set -e` — gates must accumulate rather than abort on the first
# failure. Critical build failures are handled with explicit `|| exit`.
set -uo pipefail
# Guarded cd: with no `set -e`, a failed cd would otherwise run every gate
# against the wrong tree (shellcheck SC2164).
cd "$(dirname "$(readlink -f "$0")")/.." || { echo "ci-parity: cannot cd to repo root" >&2; exit 1; }

build_dir="build-ci-parity"
asan_dir="build-ci-parity-asan"
locale="${CI_PARITY_LOCALE:-C.UTF-8}"
repeat=0
stress=0
do_lints=0
do_asan=0
do_qt62=0
do_qt62_clean=0
ctest_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repeat) repeat="$2"; shift 2 ;;
        --stress) stress=1; shift ;;
        --lints)  do_lints=1; shift ;;
        --asan)   do_asan=1; shift ;;
        --qt62)   do_qt62=1; shift ;;
        --qt62-clean) do_qt62_clean=1; shift ;;
        --full)   do_lints=1; do_asan=1; do_qt62=1; shift ;;
        *) ctest_args+=("$1"); shift ;;
    esac
done

declare -a FAILED=()
declare -a SKIPPED=()

# --- --qt62-clean: reclaim the ANTS-4131 caches (runs before any gate) -------
# Alone it cleans and exits; with --qt62 it forces the next run to rebuild the
# image and the build tree from cold.
if [[ "$do_qt62_clean" == 1 ]]; then
    bash tools/qt62-guard.sh --clean
    [[ "$do_qt62" == 1 ]] || exit 0
fi

# gate <label> <cmd...> — run a CI gate; record (never abort) on failure.
gate() {
    local label="$1"; shift
    echo "── gate: $label"
    if "$@"; then echo "   ✓ $label"; else echo "   ✗ $label (exit $?)"; FAILED+=("$label"); fi
}

# maybe_gate <tool> <label> <cmd...> — gate if <tool> is installed, else SKIP.
maybe_gate() {
    local tool="$1" label="$2"; shift 2
    if command -v "$tool" >/dev/null 2>&1; then
        gate "$label" "$@"
    else
        echo "── gate: $label"
        echo "   ⊘ SKIPPED — $tool not installed (CI still runs this)"
        SKIPPED+=("$label ($tool missing)")
    fi
}

# --- build-test: Release build + full ctest under the CI locale (core) ------
if [[ ! -f "$build_dir/build.ninja" ]]; then
    cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        || { echo "ci-parity: release configure failed" >&2; exit 1; }
fi
cmake --build "$build_dir" || { echo "ci-parity: release build failed" >&2; exit 1; }

stress_pid=""
if [[ "$stress" == 1 ]]; then
    if ! command -v stress-ng >/dev/null 2>&1; then
        echo "ci-parity: --stress needs stress-ng (zypper install stress-ng)" >&2
        exit 1
    fi
    # Sustained load so timing races surface like they do on a busy runner.
    stress-ng --cpu 2 --timeout 3600 &
    stress_pid=$!
    trap '[[ -n "$stress_pid" ]] && kill "$stress_pid" 2>/dev/null || true' EXIT
fi

if [[ "$repeat" -gt 0 ]]; then
    ctest_args=("--repeat" "until-fail:$repeat" "${ctest_args[@]}")
fi

release_ctest() {
    LC_ALL="$locale" ctest --test-dir "$build_dir" --output-on-failure "${ctest_args[@]}"
}
echo "ci-parity: LC_ALL=$locale ctest ${ctest_args[*]:-} (in $build_dir)"
gate "build-test: ctest (LC_ALL=$locale)" release_ctest

# --- build-test: packaging + lint gates (--lints) ---------------------------
man_lint() { groff -man -Tutf8 -wall packaging/linux/ants-terminal.1 >/dev/null; }

if [[ "$do_lints" == 1 ]]; then
    echo "ci-parity: build-test lint gates"
    # cppcheck is informational in CI (--error-exitcode=0 → never blocks);
    # mirror that exactly so its findings print but do not fail the run.
    # Every tool-dependent gate goes through maybe_gate, per this script's own
    # contract at the top: an absent tool is SKIPPED loudly and listed in the
    # summary as incomplete parity. Using plain `gate` here reported a missing
    # tool as a FAILURE (exit 127), which reads as "the check found something"
    # rather than "the check never ran" — the two need to stay distinguishable.
    # Scope + syntaxError suppression mirror ci.yml's step (ANTS-4788).
    maybe_gate cppcheck "cppcheck (informational)" \
        cppcheck --enable=all --std=c++20 --library=qt \
                 --suppress=missingIncludeSystem \
                 --suppress=unusedFunction \
                 --suppress=unknownMacro \
                 --suppress=normalCheckLevelMaxBranches \
                 --suppress=syntaxError:tests/* \
                 --error-exitcode=0 \
                 -I src src/ tests/
    maybe_gate appstreamcli "appstream metainfo" \
        appstreamcli validate --explain packaging/linux/za.co.antsprojectshub.AntsTerminal.metainfo.xml
    maybe_gate desktop-file-validate "desktop entry" \
        desktop-file-validate packaging/linux/za.co.antsprojectshub.AntsTerminal.desktop
    maybe_gate groff "man page (groff -wall)" man_lint
    gate "packaging version drift" bash packaging/check-version-drift.sh
    gate "completion: bash" bash -n packaging/completions/ants-terminal.bash
    maybe_gate zsh  "completion: zsh"  zsh  -n packaging/completions/_ants-terminal
    maybe_gate fish "completion: fish" fish --no-execute packaging/completions/ants-terminal.fish
fi

# --- build-asan: Debug + ASan/UBSan build, sanitized ctest, smoke (--asan) ---
asan_ctest() {
    # ANTS-4533 — mirrors ci.yml's build-asan ctest exactly. -j2 because the
    # sanitized suite is per-process-startup bound (measured 922s serial for
    # 3650 tests on CI's 4-vCPU runner), --timeout 300 so a hung test is one
    # failed test rather than the whole budget. Unlike ci.yml there is no
    # outer `timeout` wrapper: a killed ninja corrupts .ninja_deps, and a
    # local gate reports its own result without a cancelled/failed ambiguity.
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    LC_ALL="$locale" ctest --test-dir "$asan_dir" -j2 --output-on-failure \
                           --timeout 300
}
asan_smoke() {
    local pre=(QT_QPA_PLATFORM=offscreen
               ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
               UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1)
    env "${pre[@]}" "$asan_dir/ants-terminal" --version \
        && env "${pre[@]}" "$asan_dir/ants-terminal" --help
}

if [[ "$do_asan" == 1 ]]; then
    echo "ci-parity: build-asan (Debug + ANTS_SANITIZERS=ON, in $asan_dir)"
    if [[ ! -f "$asan_dir/build.ninja" ]]; then
        cmake -S . -B "$asan_dir" -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug -DANTS_SANITIZERS=ON \
            || FAILED+=("build-asan: configure")
    fi
    if [[ -f "$asan_dir/build.ninja" ]] && cmake --build "$asan_dir"; then
        gate "build-asan: ctest (sanitized)" asan_ctest
        gate "build-asan: binary smoke (--version/--help)" asan_smoke
    else
        FAILED+=("build-asan: build")
    fi
fi

# --- qt62-baseline: Qt 6.2 compile guard (ANTS-4131) -------------------------
# Delegated to tools/qt62-guard.sh, which owns the container base, the ci.yml
# package extraction, the cache keying and the compile itself. It is a separate
# script because tools/hooks/pre-push runs the same guard, and the image/volume
# derivation must not exist in two places — a second copy is how the parity
# harness silently stops reproducing CI.
if [[ "$do_qt62" == 1 ]]; then
    maybe_gate podman "qt62-baseline: Qt 6.2 compile guard (container)" \
        bash tools/qt62-guard.sh
fi


# --- summary ----------------------------------------------------------------
echo
echo "══ ci-parity summary ══"
if [[ ${#SKIPPED[@]} -gt 0 ]]; then
    echo "SKIPPED (tool absent — local parity INCOMPLETE for these; CI still runs them):"
    printf '  ⊘ %s\n' "${SKIPPED[@]}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "FAILED:"
    printf '  ✗ %s\n' "${FAILED[@]}"
    exit 1
fi
echo "ci-parity: all reproduced CI gates passed."
