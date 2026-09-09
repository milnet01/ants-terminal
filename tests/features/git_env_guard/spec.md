# Feature: git-fixture tests cannot reach the real repository

Test contract for ANTS-3841.

## Problem

Measured 2026-08-06 during ANTS-3833. With `GIT_DIR` exported, every
git-fixture test operates on whatever repository that variable names rather
than its own temp fixture. `git init`, `git commit`, `git checkout` and `git
branch` all land on the real repo. That session rewrote local `main` onto
fixture commits ("init", "old", "base"), created four stray branches and left
a worktree full of staged deletions. `origin` was untouched and everything was
recoverable from reflog, but nothing in the suite warned.

This is a SAFETY defect, not a tidiness one: a test run must not be able to
destroy the developer's history.

Verified 2026-09-09, because the mechanism decides the assertion: with
`GIT_DIR` set, `git -C <fixture> init` creates the repository at `GIT_DIR` and
leaves `<fixture>/.git` absent. The `-C` argument does not win.

Also verified, and it narrows the item: git does NOT export these to the hooks
that run this suite. A `pre-push` hook receives none of them, and `pre-commit`
receives only a relative `GIT_INDEX_FILE`. So the trigger is a session or
wrapper exporting the variable, which is what happened — not every push.

## Invariants under test

- **INV-1** — no redirecting git variable survives into a test process.
  `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE` and `GIT_COMMON_DIR` are unset
  by the time any `TEST` body runs, whatever the caller exported.
- **INV-2** — the behaviour that matters, not just the variable: `git -C
  <fixture> init` creates its repository inside the fixture. This is what INV-1
  exists to protect, and it fails independently if the scrub is removed.
- **INV-3** — the scrub is scoped to the variables that redirect WHICH
  repository git acts on. Identity and config variables are left alone: they
  cannot send a write elsewhere, and fixtures set their own identity.

## Why a second ctest entry

Under a normal run nothing exports these variables, so INV-1 and INV-2 pass
without exercising the scrub — the vacuous pass this suite must not rely on.
So the same test is registered a second time as `git_env_guard_polluted`, with
the four variables pointed at a decoy path through ctest's `ENVIRONMENT`
property. That entry is the one that goes red if the scrub is removed; the
ordinary entry only proves the scrub is harmless when there was nothing to
scrub.

`ENVIRONMENT_MODIFICATION` would express this more directly but needs CMake
3.22, and this project's floor is 3.20.

## Test shape

gtest in the `test_core` bundle. The scrub itself lives in
`tests/_support/git_env_guard.h` and is called from both `tests/bundle_main_
core.cpp` and `tests/bundle_main_gui.cpp` — the one place every C++ bundle
passes through, beside the existing `XDG_DATA_HOME` (ANTS-3856) and
`XDG_CONFIG_HOME` (ANTS-4898) sandboxes. INV-2 skips when git is absent, the
same posture the rest of the suite takes.

Shell tests are not covered by a bundle main and scrub their own environment;
`cut_rc_behaviour`, `claude_git_context_script` and `prepush_asan_gate` each
run git.

Label: `features;fast`.
