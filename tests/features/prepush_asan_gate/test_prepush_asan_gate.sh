#!/bin/bash
# ANTS-4118 — behavioural test for tools/hooks/pre-push's build-asan cost gate.
# See tests/features/prepush_asan_gate/spec.md.
#
# Drives the REAL hook in a throwaway git repo with ctest/cmake/ninja stubbed
# on PATH. The stub ninja prints ANTS_TEST_NINJA_EDGES dry-run lines, so "cold
# tree" vs "warm tree" is a single variable; the stub cmake logs its argv, so
# "never built that tree" is asserted, not inferred.

set -uo pipefail

: "${PREPUSH_HOOK:?PREPUSH_HOOK env var must be set by CMake}"
command -v git >/dev/null 2>&1 || { echo "SKIP: git not available"; exit 0; }

failures=0
check() {  # check <description> <condition-result>
    if [[ "$2" == "0" ]]; then
        echo "  ok   — $1"
    else
        echo "  FAIL — $1"
        failures=$((failures + 1))
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- stubs -----------------------------------------------------------------
mkdir -p "$tmp/bin"
cat > "$tmp/bin/ctest" <<'EOF'
#!/bin/bash
exit 0
EOF
cat > "$tmp/bin/cmake" <<'EOF'
#!/bin/bash
echo "cmake $*" >> "$CMAKE_CALL_LOG"
exit 0
EOF
cat > "$tmp/bin/ninja" <<'EOF'
#!/bin/bash
# Only the dry run is stubbed. MODE reproduces the two readings observed on
# this repo's own build-asan on 2026-08-12: a pending CMake regen (which hides
# every real edge behind it) and a deps file damaged by a killed build.
case "${ANTS_TEST_NINJA_MODE:-normal}" in
  regen)   echo "[0/1] Re-running CMake..."; exit 0 ;;
  damaged) echo "ninja: warning: premature end of file; recovering" >&2
           echo "[1/2] Building CXX object src/thing_1.cpp.o"
           echo "[2/2] Linking CXX executable thing"
           exit 0 ;;
esac
n="${ANTS_TEST_NINJA_EDGES:-0}"
if [[ "$n" -eq 0 ]]; then
    echo "ninja: no work to do."
    exit 0
fi
for ((i = 1; i <= n; i++)); do
    echo "[$i/$n] Building CXX object src/thing_$i.cpp.o"
done
exit 0
EOF
chmod +x "$tmp/bin/ctest" "$tmp/bin/cmake" "$tmp/bin/ninja"

# --- throwaway repo with a Release build tree and a warm-looking ASan tree ---
repo="$tmp/repo"
mkdir -p "$repo"
git init -q "$repo"
git -C "$repo" config user.email t@t; git -C "$repo" config user.name t
echo hi > "$repo/f.txt"
git -C "$repo" add f.txt
git -C "$repo" commit -qm init
sha=$(git -C "$repo" rev-parse HEAD)

mkdir -p "$repo/build" "$repo/build-asan"
touch "$repo/build/CTestTestfile.cmake"
echo 'ANTS_SANITIZERS:BOOL=ON' > "$repo/build-asan/CMakeCache.txt"
touch "$repo/build-asan/build.ninja"

# Run the hook. $1 = pending edge count; remaining args are extra env
# assignments. Sets $out (combined output), $rc, and $CMAKE_CALL_LOG contents.
run_hook() {
    local edges="$1"
    local mode="${2:-normal}"
    rm -f "$tmp/cmake-calls"
    touch "$tmp/cmake-calls"
    out=$(cd "$repo" && \
        PATH="$tmp/bin:$PATH" \
        CMAKE_CALL_LOG="$tmp/cmake-calls" \
        ANTS_TEST_NINJA_EDGES="$edges" \
        ANTS_TEST_NINJA_MODE="$mode" \
        bash "$PREPUSH_HOOK" origin git@example:x <<<"refs/heads/main $sha refs/heads/main 0000000000000000000000000000000000000000" 2>&1)
    rc=$?
    calls=$(cat "$tmp/cmake-calls")
}

echo "INV-1 — a cold sanitizer tree is refused, not built"
run_hook 200
check "exit 0 (the push still proceeds; CI is the backstop)" \
      "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "output names the pending-edge count" \
      "$(grep -q '200' <<<"$out" && echo 0 || echo 1)"
check "cmake --build was NOT run on the sanitizer tree" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
check "output tells the caller how to run it deliberately" \
      "$(grep -qi 'ci-parity\|ANTS_PREPUSH_ASAN_MAX_EDGES' <<<"$out" && echo 0 || echo 1)"

echo "INV-2 — a warm sanitizer tree still runs the leg"
run_hook 3
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "cmake --build WAS run on the sanitizer tree" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 0 || echo 1)"

echo "INV-3 — the escape hatch is announced in the branch that runs the leg"
check "ANTS_PREPUSH_NO_ASAN named before the build" \
      "$(grep -q 'ANTS_PREPUSH_NO_ASAN' <<<"$out" && echo 0 || echo 1)"

echo "INV-4 — an interrupt marker skips the leg and offers a heal command"
touch "$repo/build-asan/.ants-prepush-interrupted"
run_hook 3
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "cmake --build was NOT run over the suspect tree" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
check "message carries a clean-first heal command" \
      "$(grep -q -- '--clean-first' <<<"$out" && echo 0 || echo 1)"
check "message names the marker so it can be cleared" \
      "$(grep -q '.ants-prepush-interrupted' <<<"$out" && echo 0 || echo 1)"
rm -f "$repo/build-asan/.ants-prepush-interrupted"

echo "INV-5 — the Release leg is unaffected and the hatch still short-circuits"
out=$(cd "$repo" && PATH="$tmp/bin:$PATH" CMAKE_CALL_LOG="$tmp/cmake-calls" \
      ANTS_PREPUSH_NO_ASAN=1 bash "$PREPUSH_HOOK" origin git@example:x \
      <<<"refs/heads/main $sha refs/heads/main 0000000000000000000000000000000000000000" 2>&1)
rc=$?
check "exit 0 with the hatch set" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "hatch branch still reports the skip" \
      "$(grep -q 'ANTS_PREPUSH_NO_ASAN set' <<<"$out" && echo 0 || echo 1)"

echo "INV-6 — a pending CMake regen is not a warm reading"
run_hook 1 regen
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "cmake --build was NOT run behind the regen edge" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
check "output says the pending work could not be measured" \
      "$(grep -qi 'measure' <<<"$out" && echo 0 || echo 1)"

echo "INV-7 — a truncated deps log is reported, never gated on"
run_hook 2 damaged
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "the leg STILL RUNS (a gate on this would never clear)" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 0 || echo 1)"
check "the warning is surfaced to the caller" \
      "$(grep -qi 'deps log' <<<"$out" && echo 0 || echo 1)"

if [[ $failures -gt 0 ]]; then
    echo "FAILED: $failures assertion(s)"
    exit 1
fi
echo "PASS"
exit 0
