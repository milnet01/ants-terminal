#!/usr/bin/env bash
# Memory-bounded build wrapper — ANTS-1217 guardrail Layer 3.
#
# Wraps `cmake --build` in a systemd-user scope with MemoryMax + MemorySwapMax
# caps. If the build oversubscribes (e.g. a future regression reintroduces the
# Qt-bloated cc1plus over-parallelism that triggered the 2026-05-09 incident),
# the kernel kills the *build*, not the active session — losing only the
# current cc1plus rather than the editor / terminal / linker writing the
# main exe.
#
# This is the last-resort backstop. The in-tree CMake `JOB_POOLS` cap
# (Layer 1) and the `--preset=workstation` `-j6` preset (Layer 2) should
# make this script unnecessary. Run it when paranoid (kernel update, new
# Qt major release, upstream toolchain churn).
#
# Usage:
#     tools/safe-build.sh                   # builds ./build with default args
#     tools/safe-build.sh build-asan        # builds ./build-asan
#     tools/safe-build.sh build -j$(nproc)  # passes through extra flags
#
# Environment:
#     ANTS_SAFE_BUILD_MEM_MAX       — default 24G
#     ANTS_SAFE_BUILD_SWAP_MAX      — default 8G
#
# Requires: systemd-run with --user --scope support (systemd ≥ 232,
# every supported distro since 2017).

set -euo pipefail

if ! command -v systemd-run >/dev/null 2>&1; then
    echo "safe-build.sh: systemd-run not found; falling back to plain cmake --build" >&2
    exec cmake --build "${1:-build}" "${@:2}"
fi

mem_max="${ANTS_SAFE_BUILD_MEM_MAX:-24G}"
swap_max="${ANTS_SAFE_BUILD_SWAP_MAX:-8G}"

build_dir="${1:-build}"
shift || true

exec systemd-run --user --scope --quiet \
    -p "MemoryMax=${mem_max}" \
    -p "MemorySwapMax=${swap_max}" \
    -- cmake --build "${build_dir}" "$@"
