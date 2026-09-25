#!/usr/bin/env bash
# ANTS-2134 — reproduce CI locally before a push.
# ANTS-5322 — by EXECUTING .github/workflows/ci.yml, not by copying it.
#
# Until ANTS-5322 this script re-stated ci.yml's steps in shell (ANTS-4392
# recorded it as a hand-maintained parallel implementation), so the two could
# drift and a drifted copy returns green for a pipeline that will fail —
# local-gate.md § 3. Now it only decides WHICH jobs run and WHERE:
#
#   * on this machine, through tools/ci_workflow.py, which runs each job's own
#     `run:` steps with its env, working-directory and if: — build-test,
#     build-asan and cppcheck. They build in build/ and build-asan/, the same
#     trees ci.yml names, so a run starts warm (ANTS-5193).
#   * in a podman container, through tools/qt62-guard.sh, for the two things
#     this openSUSE box cannot be: qt62-baseline's Ubuntu 22.04 / Qt 6.2, and
#     build-test's Ubuntu 24.04 / GCC 13 / mold toolchain. The container's
#     package list is extracted from ci.yml too.
#
# Every job in ci.yml must be claimed below. A job added to ci.yml and not
# here fails the run, rather than leaving a green local run that never saw it.
#
# Usage:
#   tools/ci-parity.sh              # build-test (build, suite, shellcheck, packaging lints)
#   tools/ci-parity.sh --lints      #  + cppcheck
#   tools/ci-parity.sh --asan       #  + build-asan (sanitized build, suite, smoke)
#   tools/ci-parity.sh --qt62       #  + qt62-baseline in an ubuntu:22.04 container
#   tools/ci-parity.sh --ubuntu24   #  + build-test's toolchain in an ubuntu:24.04 container
#   tools/ci-parity.sh --full       #  every job above: all of ci.yml
#   tools/ci-parity.sh --qt62-clean # drop the cached container images + volumes
#   tools/ci-parity.sh --stress     # add background CPU load (needs stress-ng)
#   CI_PARITY_LOCALE=... overrides the runner's C.UTF-8 locale.
#
# Jobs accumulate: every requested job runs and all failures are listed at the
# end. A container leg with no podman is SKIPPED loudly, never reported green.
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.." || { echo "ci-parity: cannot cd to repo root" >&2; exit 1; }

host_jobs=(build-test build-asan cppcheck)
container_jobs=(qt62-baseline)

stress=0 do_lints=0 do_asan=0 do_qt62=0 do_qt62_clean=0 do_ubuntu24=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --stress)     stress=1 ;;
        --lints)      do_lints=1 ;;
        --asan)       do_asan=1 ;;
        --qt62)       do_qt62=1 ;;
        --qt62-clean) do_qt62_clean=1 ;;
        --ubuntu24)   do_ubuntu24=1 ;;
        --full)       do_lints=1; do_asan=1; do_qt62=1; do_ubuntu24=1 ;;
        *) echo "ci-parity: unknown argument '$1' (see the header for usage)" >&2
           exit 2 ;;
    esac
    shift
done

# Every ci.yml job is claimed, or the run stops here.
if ! ci_jobs="$(python3 tools/ci_workflow.py jobs)"; then
    echo "ci-parity: cannot read .github/workflows/ci.yml" >&2; exit 1
fi
for j in $ci_jobs; do
    if [[ " ${host_jobs[*]} ${container_jobs[*]} " != *" $j "* ]]; then
        echo "ci-parity: ci.yml has a job '$j' this script does not run." >&2
        echo "           Add it to host_jobs or container_jobs in tools/ci-parity.sh." >&2
        exit 1
    fi
done

if [[ "$do_qt62_clean" == 1 ]]; then
    bash tools/qt62-guard.sh --clean
    bash tools/qt62-guard.sh --job build-test --clean
    [[ "$do_qt62" == 1 || "$do_ubuntu24" == 1 ]] || exit 0
fi

declare -a FAILED=() SKIPPED=()

if [[ "$stress" == 1 ]]; then
    command -v stress-ng >/dev/null 2>&1 || {
        echo "ci-parity: --stress needs stress-ng (zypper install stress-ng)" >&2; exit 1; }
    # Sustained load so timing races surface like they do on a busy runner.
    stress-ng --cpu 2 --timeout 3600 &
    stress_pid=$!
    trap 'kill "$stress_pid" 2>/dev/null || true' EXIT
fi

host_job() {
    echo "══ ci.yml job: $1 (this machine)"
    python3 tools/ci_workflow.py run "$1" || FAILED+=("$1")
}

container_leg() {
    local label="$1"; shift
    echo "══ $label (podman)"
    if ! command -v podman >/dev/null 2>&1; then
        echo "   ⊘ SKIPPED — podman not installed (CI still runs this)"
        SKIPPED+=("$label (podman missing)")
    elif ! bash tools/qt62-guard.sh "$@"; then
        FAILED+=("$label")
    fi
}

host_job build-test
[[ "$do_lints" == 1 ]] && host_job cppcheck
[[ "$do_asan" == 1 ]] && host_job build-asan
[[ "$do_qt62" == 1 ]] && container_leg "qt62-baseline: ubuntu:22.04 / Qt 6.2 build"
[[ "$do_ubuntu24" == 1 ]] && container_leg "build-test toolchain: ubuntu:24.04 / GCC 13 / mold build" --job build-test

echo
echo "══ ci-parity summary ══"
if [[ ${#SKIPPED[@]} -gt 0 ]]; then
    echo "SKIPPED (local parity INCOMPLETE for these; CI still runs them):"
    printf '  ⊘ %s\n' "${SKIPPED[@]}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "FAILED:"
    printf '  ✗ %s\n' "${FAILED[@]}"
    exit 1
fi
echo "ci-parity: every requested ci.yml job passed."
