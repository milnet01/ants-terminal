# git_optional_locks — feature-conformance contract (ANTS-4999)

## Problem

A plain `git status` refreshes the index and can hold `.git/index.lock`
while it does. Ants runs read-only git calls on a timer and on demand. A
user's `git add` or `git commit` that lands in that window fails with
"index.lock: File exists".

`GIT_OPTIONAL_LOCKS=0` is git's documented switch for polling tools. Git
then skips optional locks and still takes every lock a write needs.

## Invariants

- **INV-1** — `GitWrap::readOnlyEnvironment()` returns the process
  environment with `GIT_OPTIONAL_LOCKS=0`, even when the parent
  environment does not set it.
- **INV-2** — `GitWrap::run` hands that environment to git. The test
  checks it through a one-shot shell alias that prints the variable as
  git's child sees it.
- **INV-3** — every read-only git runner that does not go through
  `GitWrap::run` uses `GitWrap::readOnlyEnvironment()` too:
  `MainWindow::refreshReviewButton`, the `get_git_status` provider,
  RemoteControl's shared `runGit`, and the three git runners in
  `auditscope.cpp`. Checked by source scrape, comments stripped.
- **INV-4** — the Claude git-context hook script runs its `git status`
  with `GIT_OPTIONAL_LOCKS=0`. Checked by source scrape. The
  `claude_git_context_hook` behavioural test still runs the script.

## Not covered

- The diff viewer sets the switch itself (ANTS-3509, `tree_watcher`
  INV-6).
- A hook script already installed keeps its old text until the user runs
  the installer again from Settings.
