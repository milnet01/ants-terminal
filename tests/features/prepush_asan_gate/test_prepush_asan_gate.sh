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
echo "ninja $*" >> "$NINJA_CALL_LOG"
# Only the dry run is stubbed. MODE reproduces the two readings observed on
# this repo's own build-asan on 2026-08-12: a pending CMake regen (which hides
# every real edge behind it) and a deps file damaged by a killed build.
case "${ANTS_TEST_NINJA_MODE:-normal}" in
  regen|regen_fails)
           # The regen is a STATE, not a constant reading: `ninja -C <dir>
           # build.ninja` is the regen edge itself, and after it runs the dry
           # run reports the real edges it was hiding. A stateless stub would
           # report the regen forever and could not tell the two arms apart.
           if [[ "$*" == *build.ninja* ]]; then
               [[ "$ANTS_TEST_NINJA_MODE" == regen_fails ]] && exit 1
               touch "$ANTS_TEST_NINJA_REGEN_DONE"
               exit 0
           fi
           if [[ ! -f "$ANTS_TEST_NINJA_REGEN_DONE" ]]; then
               echo "[0/1] Re-running CMake..."; exit 0
           fi
           ;;
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
    rm -f "$tmp/cmake-calls" "$tmp/ninja-calls" "$tmp/regen-done"
    touch "$tmp/cmake-calls" "$tmp/ninja-calls"
    # ANTS-4883 — scrub the hook's OWN tunables. The hook runs as a child of
    # whoever set them, so a caller who exported the documented escape hatch
    # to skip the slow leg for one push failed this suite instead. Each case
    # sets only what it is exercising; INV-9 asserts the scrub holds.
    out=$(cd "$repo" && \
        env -u ANTS_PREPUSH_NO_ASAN -u ANTS_PREPUSH_NO_QT62 \
            -u ANTS_PREPUSH_ASAN_MAX_EDGES \
        PATH="$tmp/bin:$PATH" \
        CMAKE_CALL_LOG="$tmp/cmake-calls" \
        NINJA_CALL_LOG="$tmp/ninja-calls" \
        ANTS_TEST_NINJA_EDGES="$edges" \
        ANTS_TEST_NINJA_MODE="$mode" \
        ANTS_TEST_NINJA_REGEN_DONE="$tmp/regen-done" \
        bash "$PREPUSH_HOOK" origin git@example:x <<<"refs/heads/main $sha refs/heads/main 0000000000000000000000000000000000000000" 2>&1)
    rc=$?
    calls=$(cat "$tmp/cmake-calls")
    ninja_calls=$(cat "$tmp/ninja-calls")
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
      NINJA_CALL_LOG="$tmp/ninja-calls" \
      ANTS_PREPUSH_NO_ASAN=1 bash "$PREPUSH_HOOK" origin git@example:x \
      <<<"refs/heads/main $sha refs/heads/main 0000000000000000000000000000000000000000" 2>&1)
rc=$?
check "exit 0 with the hatch set" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "hatch branch still reports the skip" \
      "$(grep -q 'ANTS_PREPUSH_NO_ASAN set' <<<"$out" && echo 0 || echo 1)"

echo "INV-6 — a pending CMake regen is measured, not skipped (ANTS-4536)"
run_hook 3 regen
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "the regen edge itself was run (asserted on ninja's argv)" \
      "$(grep -q 'build.ninja' <<<"$ninja_calls" && echo 0 || echo 1)"
check "the leg RUNS on the warm count the regen revealed" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 0 || echo 1)"

echo "INV-6b — the count the regen reveals is gated like any other"
run_hook 200 regen
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "a cold tree behind the regen is still refused" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
check "output names the count the regen revealed" \
      "$(grep -q '200' <<<"$out" && echo 0 || echo 1)"

echo "INV-6c — a regen that FAILS is genuinely unmeasurable and skips"
run_hook 3 regen_fails
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "cmake --build was NOT run behind an unresolved regen" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
check "output says the pending work could not be measured" \
      "$(grep -qi 'measure' <<<"$out" && echo 0 || echo 1)"

echo "INV-8 — the interrupt-marker skip states the marker's age (ANTS-4943)"
touch -d '3 days ago' "$repo/build-asan/.ants-prepush-interrupted"
run_hook 3
check "the age is reported, so a stale skip is not invisible" \
      "$(grep -q '3 days ago' <<<"$out" && echo 0 || echo 1)"
touch "$repo/build-asan/.ants-prepush-interrupted"
run_hook 3
check "a marker written this session does not read as days old" \
      "$(grep -q 'days ago' <<<"$out" && echo 1 || echo 0)"
check "and it still reports an age rather than nothing" \
      "$(grep -qE '[0-9]+ (hour|hours|minute|minutes|day|days) ago' <<<"$out" \
         && echo 0 || echo 1)"
rm -f "$repo/build-asan/.ants-prepush-interrupted"

echo "INV-9 — an ambient escape hatch does not decide this suite (ANTS-4883)"
export ANTS_PREPUSH_NO_ASAN=1
run_hook 3
unset ANTS_PREPUSH_NO_ASAN
check "the leg still runs with the hatch exported by the CALLER" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 0 || echo 1)"
check "and the hatch's own skip message is absent" \
      "$(grep -q 'ANTS_PREPUSH_NO_ASAN set' <<<"$out" && echo 1 || echo 0)"

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
