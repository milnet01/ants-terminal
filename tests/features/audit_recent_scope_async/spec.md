# audit_recent_scope_async — the audit dialog reads git history off the GUI thread

**Bundle:** `test_dialogs` · **Suite:** `AuditRecentScopeAsync` · **Label:** `features;fast`

## Problem

`AuditDialog::computeRecentChangeSets` ran `git log`, `git rev-parse` and
`git diff` with blocking waits of up to 5, 2 and 8 seconds on the GUI thread.
`runAudit` called it when the Recent files scope or the Since baseline pill was
on, and `onSinceBaselineToggled` called it on every toggle. A large or slow
repository froze the whole window, and every terminal in it, for that long.

## Surface

- `AuditDialog::readRecentChangeSets(projectPath, commits, includeLines)` is a
  static function that reads the sets and touches no dialog state.
- `requestRecentChangeSets(includeLines, then)` runs it on a worker thread,
  applies the result on the GUI thread, then calls every waiting continuation.
  While it runs, `m_recentScopeError` is set, so the recent filters stand down
  as they do when git fails. A request that needs no line data joins one in
  flight; one that needs lines while a files-only request runs starts a new
  request, and only the latest result is applied.
- `runAudit` starts its checks from the continuation. `onSinceBaselineToggled`
  re-renders from it. A run cancelled or superseded meanwhile is dropped by
  `m_runGeneration`.

## Invariants

- **INV-1 — changed files and lines.** In a repository whose last commit
  changed line 2 of `a.txt`, `readRecentChangeSets(dir, 1, true)` returns
  `files` = {`a.txt`}, `lines[a.txt]` = {2} and an empty `error`.
- **INV-2 — a history shorter than the window.** With `commits` = 10 on a
  two-commit repository, every line of `a.txt` counts as changed and `error`
  is empty.
- **INV-3 — files only.** With `includeLines` false, `files` holds `a.txt` and
  `lines` is empty.
- **INV-4 — not a repository.** On a directory that is not a git repository,
  `error` is not empty.
- **INV-5 — no git on the GUI thread.** In the AuditDialog sources,
  `computeRecentChangeSets` no longer exists; `runAudit` and
  `onSinceBaselineToggled` call `requestRecentChangeSets`; and
  `readRecentChangeSets` is called from inside the `QThread::create` in
  `requestRecentChangeSets`.

## Test surface

INV-1 to INV-4 build scratch repositories with `git`, with the global and
system git config pointed at `/dev/null`. They skip when `git` is absent.
INV-5 reads the source text.

## Regression history

- **ANTS-5084:** two blocking git runs, up to 13 s, sat on the GUI thread in
  `runAudit` and the pill toggle.
