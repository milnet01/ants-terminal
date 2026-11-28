#!/bin/bash
# ANTS-4118 — behavioural test for tools/local-ci.sh's build-asan cost gate.
# See tests/features/prepush_asan_gate/spec.md.
#
# Drives the REAL gate in a throwaway git repo with ctest/cmake/ninja stubbed
# on PATH. The stub ninja prints ANTS_TEST_NINJA_EDGES dry-run lines, so "cold
# tree" vs "warm tree" is a single variable; the stub cmake logs its argv, so
# "never built that tree" is asserted, not inferred.

set -uo pipefail

# ANTS-3841 — a shell test does not pass through a bundle main, so it scrubs
# the git environment itself. With GIT_DIR exported, `git init` / `git commit`
# below land on the caller's real repository instead of this test's fixture.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR


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
# ANTS-5322 — the hook runs the pushed repo's OWN ci.yml through its own
# tools/ci_workflow.py, so the fixture carries both. The jobs call the stubbed
# cmake/ctest exactly as the real ci.yml does, which keeps "was the sanitizer
# tree built?" an assertion on cmake's argv.
command -v python3 >/dev/null 2>&1 && python3 -c 'import yaml' 2>/dev/null \
    || { echo "SKIP: python3 + PyYAML not available"; exit 77; }
repo="$tmp/repo"
mkdir -p "$repo/tools" "$repo/.github/workflows"
cp "$(dirname "$PREPUSH_HOOK")/ci_workflow.py" "$repo/tools/"
cat > "$repo/.github/workflows/ci.yml" <<'EOF'
on:
  push:
    paths-ignore: ['docs/**']
jobs:
  build-test:
    runs-on: ubuntu-24.04
    steps:
      - run: cmake --build build --parallel
      - working-directory: build
        run: ctest -j2 --output-on-failure --timeout 300
  build-asan:
    runs-on: ubuntu-24.04
    steps:
      - run: cmake --build build-asan --parallel
      - working-directory: build-asan
        run: ctest -j2 --output-on-failure --timeout 300 -LE perf
EOF
git init -q "$repo"
git -C "$repo" config user.email t@t; git -C "$repo" config user.name t
echo hi > "$repo/f.txt"
git -C "$repo" add f.txt
git -C "$repo" commit -qm init

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
        env -u ANTS_PREPUSH_NO_ASAN -u ANTS_PREPUSH_NO_QT62 -u ANTS_PUSH_CHANGED \
            -u ANTS_PREPUSH_ASAN_MAX_EDGES \
        PATH="$tmp/bin:$PATH" \
        CMAKE_CALL_LOG="$tmp/cmake-calls" \
        NINJA_CALL_LOG="$tmp/ninja-calls" \
        ANTS_TEST_NINJA_EDGES="$edges" \
        ANTS_TEST_NINJA_MODE="$mode" \
        ANTS_TEST_NINJA_REGEN_DONE="$tmp/regen-done" \
        bash "$PREPUSH_HOOK" </dev/null 2>&1)
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
# ANTS-6656 — a pending regen in the marked tree: nothing measures it.
run_hook 3 regen
check "the regen was NOT run in a marked tree (ANTS-6656)" \
      "$(grep -q 'build.ninja' <<<"$ninja_calls" && echo 1 || echo 0)"
check "and the marker skip still fires" \
      "$(grep -q -- '--clean-first' <<<"$out" && echo 0 || echo 1)"
rm -f "$repo/build-asan/.ants-prepush-interrupted"

echo "INV-5 — the Release leg is unaffected and the hatch still short-circuits"
out=$(cd "$repo" && PATH="$tmp/bin:$PATH" CMAKE_CALL_LOG="$tmp/cmake-calls" \
      NINJA_CALL_LOG="$tmp/ninja-calls" \
      ANTS_PREPUSH_NO_ASAN=1 env -u ANTS_PUSH_CHANGED bash "$PREPUSH_HOOK" \
      </dev/null 2>&1)
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

echo "INV-10 — the Release leg runs ci.yml's build-test job, and its failure blocks (ANTS-5322)"
run_hook 0
check "build-test's own build step ran (cmake --build build --parallel)" \
      "$(grep -q -- '--build build --parallel' <<<"$calls" && echo 0 || echo 1)"
sed -i 's/ctest -j2 --output-on-failure --timeout 300$/false/' "$repo/.github/workflows/ci.yml"
run_hook 0
check "a failing build-test step blocks the push (exit non-zero)" \
      "$([[ $rc -ne 0 ]] && echo 0 || echo 1)"
check "the block names ci.yml's build-test job" \
      "$(grep -q "build-test job FAILED" <<<"$out" && echo 0 || echo 1)"
check "the sanitizer leg did not run after the block" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
git -C "$repo" checkout -q -- .github/workflows/ci.yml 2>/dev/null \
    || sed -i 's/^        run: false$/        run: ctest -j2 --output-on-failure --timeout 300/' \
           "$repo/.github/workflows/ci.yml"

echo "INV-11 — a tree another builder holds is skipped, not built (asan-nightly)"
# One process holds the lock, so killing it releases the lock (a `flock cmd`
# holder leaves its child holding it after the kill).
bash -c 'exec 8>"$1"; flock 8; exec sleep 60' _ "$repo/build-asan/.ants-build.lock" &
holder=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    flock -n "$repo/build-asan/.ants-build.lock" true 2>/dev/null || break
    sleep 0.1
done
# A pending regen: the gate would run CMake in the tree to measure it.
run_hook 3 regen
kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null
check "exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "cmake --build was NOT run while the tree was held" \
      "$(grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0)"
check "ninja was NOT run in the held tree, not even the regen (ANTS-6631)" \
      "$([[ -z "$ninja_calls" ]] && echo 0 || echo 1)"
check "the skip names the busy tree" \
      "$(grep -q 'being' <<<"$out" && echo 0 || echo 1)"

echo "INV-12 — tools/asan-nightly.sh builds the tree, healing a marked one"
nightly="$(dirname "$PREPUSH_HOOK")/asan-nightly.sh"
run_nightly() {
    : > "$tmp/cmake-calls"
    out=$(PATH="$tmp/bin:$PATH" CMAKE_CALL_LOG="$tmp/cmake-calls" \
          ANTS_ASAN_DIR="$repo/build-asan" bash "$nightly" </dev/null 2>&1)
    rc=$?
    calls=$(cat "$tmp/cmake-calls")
}
run_nightly
check "a warm tree: exit 0" "$([[ $rc -eq 0 ]] && echo 0 || echo 1)"
check "a warm tree is built incrementally" \
      "$(grep -qx "cmake --build $repo/build-asan" <<<"$calls" && echo 0 || echo 1)"
touch "$repo/build-asan/.ants-prepush-interrupted"
run_nightly
check "a marked tree is rebuilt with --clean-first" \
      "$(grep -q -- '--clean-first' <<<"$calls" && echo 0 || echo 1)"
check "the marker is cleared after a good build" \
      "$([[ ! -e "$repo/build-asan/.ants-prepush-interrupted" ]] && echo 0 || echo 1)"
# One process holds the lock, so killing it releases the lock (a `flock cmd`
# holder leaves its child holding it after the kill).
bash -c 'exec 8>"$1"; flock 8; exec sleep 60' _ "$repo/build-asan/.ants-build.lock" &
holder=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    flock -n "$repo/build-asan/.ants-build.lock" true 2>/dev/null || break
    sleep 0.1
done
run_nightly
kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null
check "a held tree: exit 0 and nothing built" \
      "$([[ $rc -eq 0 && -z "$calls" ]] && echo 0 || echo 1)"

echo "INV-13 — a tree the last green sanitized run already covers is not run again (ANTS-6839)"
# The cases above ran with the fixture's files untracked. These need a clean
# tree at a commit, which is what the real gate runs in.
asan_ran()     { grep -q -- '--build build-asan' <<<"$calls" && echo 0 || echo 1; }
asan_not_run() { grep -q -- '--build build-asan' <<<"$calls" && echo 1 || echo 0; }
commit_file() {  # commit_file <path> <content>
    mkdir -p "$repo/$(dirname "$1")"
    echo "$2" > "$repo/$1"
    git -C "$repo" add -A
    git -C "$repo" commit -qm "$1"
}
stamp="$repo/build-asan/.ants-asan-passed"
printf '#!/bin/bash\n' > "$repo/build-asan/test_bundle"
chmod +x "$repo/build-asan/test_bundle"
rm -f "$stamp"
commit_file .gitignore $'build/\nbuild-asan/'
run_hook 0
check "the first run of an up-to-date tree runs the leg" "$(asan_ran)"
check "and records the commit that passed" \
      "$([[ "$(head -1 "$stamp" 2>/dev/null)" == "$(git -C "$repo" rev-parse HEAD)" ]] && echo 0 || echo 1)"
passed_short="$(git -C "$repo" rev-parse --short HEAD)"
run_hook 0
check "the same tree again: exit 0 and the leg is skipped" \
      "$([[ $rc -eq 0 ]] && asan_not_run)"
check "the skip names the commit that passed" \
      "$(grep -q "SKIPPED.*$passed_short" <<<"$(tr '\n' ' ' <<<"$out")" && echo 0 || echo 1)"
commit_file docs/note.md "a note"
run_hook 0
check "a document changed since the pass: still skipped" "$(asan_not_run)"
commit_file tests/data.json "{}"
run_hook 0
check "a test fixture changed since the pass: the leg runs" "$(asan_ran)"
commit_file newfolder/thing.txt "x"
run_hook 0
check "a path in a folder the gate does not know: the leg runs" "$(asan_ran)"
commit_file packaging/shell-integration/x.zsh "x"
run_hook 0
check "a shell-integration script changed: the leg runs" "$(asan_ran)"
commit_file docs/helper.h "// x"
run_hook 0
check "a compilable file inside a document folder: the leg runs" "$(asan_ran)"
run_hook 0
check "control: with nothing changed the leg is skipped again" "$(asan_not_run)"
run_hook 3
check "pending build steps: the leg runs" "$(asan_ran)"
echo '# rebuilt' >> "$repo/build-asan/test_bundle"
run_hook 0
check "a program rebuilt since the pass: the leg runs" "$(asan_ran)"
sed -i '1s/.*/1234567890abcdef1234567890abcdef12345678/' "$stamp"
run_hook 0
check "a recorded commit git does not have: the leg runs" "$(asan_ran)"
echo dirty >> "$repo/f.txt"
run_hook 0
check "uncommitted changes: the leg runs" "$(asan_ran)"
sed -i '1s/.*/1234567890abcdef1234567890abcdef12345678/' "$stamp"
run_hook 0
check "and a green run over a dirty tree records no pass" \
      "$([[ "$(head -1 "$stamp")" == 1234567890abcdef1234567890abcdef12345678 ]] && echo 0 || echo 1)"
git -C "$repo" checkout -q -- f.txt
run_hook 0
run_hook 0
check "control: the clean tree, passed once more, is skipped" "$(asan_not_run)"
echo '# a comment' >> "$repo/.github/workflows/ci.yml"
git -C "$repo" add -A; git -C "$repo" commit -qm ci
run_hook 0
check "ci.yml changed since the pass: the leg runs" "$(asan_ran)"

echo "INV-14 — a Qt 6.2 guard that could not run is not reported as a floor violation (ANTS-6631)"
# A stub guard and podman: every guard call passes except --warm-only, which
# exits with STUB_QT62_RC.
printf '#!/bin/bash\n[[ " $* " == *" --warm-only "* ]] && exit "${STUB_QT62_RC:-0}"\nexit 0\n' \
    > "$repo/tools/qt62-guard.sh"
printf '#!/bin/bash\nexit 0\n' > "$tmp/bin/podman"
chmod +x "$repo/tools/qt62-guard.sh" "$tmp/bin/podman"
export STUB_QT62_RC=4
run_hook 0
check "the guard's own failure blocks the push" "$([[ $rc -ne 0 ]] && echo 0 || echo 1)"
check "and says the guard could not run" \
      "$(grep -q 'guard could not run' <<<"$out" && echo 0 || echo 1)"
check "and does not call it a floor violation" \
      "$(grep -q 'FLOOR violation' <<<"$out" && echo 1 || echo 0)"
export STUB_QT62_RC=1
run_hook 0
check "control: a compile failure is still a floor violation" \
      "$([[ $rc -ne 0 ]] && grep -q 'FLOOR violation' <<<"$out" && echo 0 || echo 1)"
unset STUB_QT62_RC
rm -f "$repo/tools/qt62-guard.sh" "$tmp/bin/podman"

if [[ $failures -gt 0 ]]; then
    echo "FAILED: $failures assertion(s)"
    exit 1
fi
echo "PASS"
exit 0
