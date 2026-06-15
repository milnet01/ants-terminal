#!/usr/bin/env bash
# ANTS-2134 — reproduce CI test conditions locally.
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
# This harness runs the suite the way CI does. Build + test happen in an
# isolated build-ci-parity/ tree so the live build/ binary is never
# relinked underneath a running instance (ANTS-2025).
#
# Usage:
#   tools/ci-parity.sh                 # configure+build, run ctest under C.UTF-8
#   tools/ci-parity.sh --repeat 5      # rerun each test up to 5x, fail on first flake
#   tools/ci-parity.sh --stress        # add background CPU load (needs stress-ng)
#   tools/ci-parity.sh -R SomeTest     # extra args pass through to ctest
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."

build_dir="build-ci-parity"
locale="${CI_PARITY_LOCALE:-C.UTF-8}"
repeat=0
stress=0
ctest_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repeat) repeat="$2"; shift 2 ;;
        --stress) stress=1; shift ;;
        *) ctest_args+=("$1"); shift ;;
    esac
done

# Mirror CI's configure: Release + Ninja (ccache if available, like CI).
[ -f "$build_dir/build.ninja" ] || \
    cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir"

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

echo "ci-parity: LC_ALL=$locale ctest ${ctest_args[*]:-} (in $build_dir)"
LC_ALL="$locale" ctest --test-dir "$build_dir" --output-on-failure "${ctest_args[@]}"
echo "ci-parity: suite complete."
