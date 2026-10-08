# Build loops, presets and the local CI gate

## Faster loops

- `cmake --build build --target ants-terminal` skips the test binaries.
- `-DANTS_CCACHE=ON` (default). Keep the cache large: `ccache -M 20G` once;
  `ccache -s` to check.
- `-DANTS_USE_MOLD=ON` (default when `mold` is on PATH).
- `-DANTS_UNITY_BUILD=ON`: cold full builds only.
- `cmake --preset=fast`: isolated `build-fast/`, parallel test-bundle linking.

## CMake presets

Each: `cmake --preset=X && cmake --build --preset=X && ctest --preset=X`.

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`. |
| `workstation` | Release in `build-workstation/`, capped `-j3`. |
| `debug` | Debug + ASan/UBSan in `build-asan/`, serial tests. |
| `fast` | Release in `build-fast/` for hot iteration. |

## Backstop: `tools/safe-build.sh`

Wraps `cmake --build` in a memory-capped systemd scope. Use it after kernel
or Qt-major updates. **Cppcheck:** pass `--library=qt`, on Qt projects only.

## Local CI and the pre-push hook

- **`tools/ci-parity.sh --full` is this project's local CI.** It executes
  `.github/workflows/ci.yml`'s jobs through `tools/ci_workflow.py`. There is
  no second script. `--stress` adds CPU load.
- Hunt a flaky test with `ctest --test-dir build --repeat until-fail:5 -R <test>`.
- **Run `tools/setup-git-hooks.sh` once per clone.** It sets
  `core.hooksPath=tools/hooks`. How the push hook runs the gate is committed
  in `.ants/gate.conf`.
- `tools/hooks/pre-push` hands off to the machine-wide hook
  (`~/.claude/githooks/pre-push`). That hook runs the secret scan, works out
  what the push changes, and runs `tools/local-ci.sh` in the real checkout.
  It refuses a push from a tree with uncommitted or untracked files, or of a
  commit that is not HEAD.
- `tools/local-ci.sh` runs `ci.yml`'s `build-test` job in CI's own image
  (ubuntu:24.04: its GCC, mold and Qt 6.4) through
  `tools/qt62-guard.sh --job build-test --run-job`. A cold or stale image
  blocks the push: run that command (about 20 min, through `cc-job`), then
  push again. With no podman the job runs on this machine and the image leg is
  declared skipped. It also runs
  `build-asan` when `build-asan/` is warm, and the Qt 6.2 compile guard
  `tools/qt62-guard.sh --warm-only`. On a push with compilable source, a
  cold Qt 6.2 cache blocks the push: run `tools/qt62-guard.sh` (about
  11 min, through `cc-job`), then push again.
- This box's newer Qt can pass a test CI's Qt fails (ANTS-5479's button
  test). Reproduce a CI-only failure with the `--run-job` command above.
- A push that `ci.yml`'s `paths-ignore` treats as docs-only runs
  `tools/local-ci.sh --docs`: no build, no suite. It runs the document checks
  for what the push touches: the README claim check, `check-roadmap.sh`, the
  standards checks, and the tests that read `CLAUDE.md`'s text, against the
  existing `build/`.
- `tools/ci-parity.sh --full` runs every job, including those the hook
  leaves to GitHub.
- A tool the GitHub runner lacks cannot be caught locally.
  `tests/features/ci_workflow_deps` checks the recipes statically. **A new
  carrier that runs `ctest` must be added to that test.**
- Escape hatches: `SKIP_LOCAL_CI=1` (skips the gate, keeps the secret
  scan) and `git push --no-verify` both need the user (`commits.md` § 2.3);
  `ANTS_PREPUSH_NO_ASAN=1`,
  `ANTS_PREPUSH_NO_QT62=1`, `ANTS_PREPUSH_NO_UBUNTU24=1` (runs the job on
  this machine, with no ubuntu:24.04 leg at all). The ASan leg's
  contract: `tests/features/prepush_asan_gate/spec.md`.
