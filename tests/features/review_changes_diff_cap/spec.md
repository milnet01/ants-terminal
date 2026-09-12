# Review Changes diff: size cap, off-thread render, generation guard (ANTS-5059); probes destroyed while running (ANTS-5128)

## Problem

`diffviewer::show()` (`src/diffviewer.cpp`) reads the whole `git diff
--stat --patch HEAD` output into `ProbeState::diff` with no size limit.
`finalize()` then splits it, builds one styled `<span>` per line, and
calls `viewerGuard->setHtml(html)` — all synchronously on the GUI
thread — and repeats this on every live-refresh burst (the
`DirTreeWatcher` fires on any tracked-file content edit, not just a
commit). The byte-identical skip (`if (*lastHtml == html) return;`)
only runs *after* the HTML has already been built, so it saves the
`setHtml()` call but not the multi-MiB string-building pass.

A rewrite of a large text file — a lockfile, a minified bundle, a
regenerated `ROADMAP.md` — produces tens of thousands of diff lines and
a multi-second GUI-thread freeze. Because the dialog is live (it
re-probes on every git-directory change), that freeze recurs on every
burst while an agent or editor keeps touching the file. New
(untracked) files already get a 200 KB read cap with a "(truncated at
200 KB)" notice (`diffviewer.cpp`'s New-files section); the tracked-file
`Diff` section has no equivalent.

Related, same dialog: `runProbes()` builds a fresh `ProbeState` per
refresh, but nothing stops an **older, slower** round's `finalize()`
from completing and rendering *after* a **newer** round has already
started (and possibly already rendered) — a stale diff can silently
overwrite a fresher view. And every `QProcess` `runAsync()` spawns is
`new QProcess(parent)`, where `parent` is the caller's widget
(`MainWindow`, in production) rather than the dialog itself, so closing
the dialog does not terminate its in-flight probes.

### ANTS-5128 — closing the dialog destroys its probes while they are still running

Once the probes above were reparented to the dialog itself, closing the
dialog while a probe is still in flight deletes that still-running
`QProcess` as a child of the `QDialog` being destroyed.
`~QProcess` kills that child and reaps it, and the reap emits
`finished` from inside the destructor, into `runAsync()`'s handler,
which calls `finalize()`, which
dereferences a `QPointer<QDialog> dlgGuard` that by this point in
`~QDialog`'s call chain has already been degraded to `QWidget` —
`QPointer<QDialog>::data()`'s downcast is undefined behaviour on an
object no longer of that type. Caught by UBSan in CI (run 34650028988,
job "Build and test (ASan/UBSan)"): a `runtime error: downcast of
address … which does not point to an object of type 'QDialog'`, five
`QProcess: Destroyed while process ("git") is still running` warnings,
inside `~QProcess::waitForFinished()` called from
`QObjectPrivate::deleteChildren()` called from `~QWidget`/`~QDialog`.
Locally the git probes finish (milliseconds, temp-repo diffs) before a
user has time to close the dialog, so this was invisible outside a
loaded CI runner.

## Fix (intended; not implemented as of this test's authoring)

1. Cut the diff text at a line boundary once it exceeds a byte cap on
   the order of 1 MiB, and say so in the render (wording containing
   "truncated").
2. Keep the byte-identical skip, but return before building any HTML at
   all when the five raw probe outputs already equal the last round's.
3. A generation/round counter: a round that is no longer the newest
   does not render, even if its `finalize()` runs to completion.
4. Parent the probe `QProcess` objects to the dialog, not to the
   caller's widget, so closing the dialog kills them.
5. (ANTS-5128) A small `QObject` host, child of the dialog, owning the
   probe `QProcess`es; its destructor disconnects and kills them before
   `~QObject` deletes them, so no probe is ever destroyed while
   running and no `finished`/`errorOccurred` handler runs during
   teardown. It covers every destruction path, not the Close button
   alone: the parent window, and application exit.

## Invariants

Three checks pin the ANTS-5059 defect (RED — expected to fail against
pre-fix code); one pins behaviour that must not regress once that fix
lands (GUARD — expected to pass now and after the fix). A fifth check,
added for ANTS-5128, pinned a defect introduced by the ANTS-5059 fix
itself (RED when authored; fixed 2026-09-12).

- **INV-1** (RED, behavioural — `LargeDiffIsCappedAndTruncated`): in a
  real git repository, rewriting a *tracked* file to ~100,000 distinct
  lines (several MiB of uncommitted diff) and opening the dialog must
  produce a `QTextBrowser` whose plain text is bounded (well under the
  raw diff size) and mentions truncation. Today the whole diff renders
  verbatim and nothing says it was cut.
- **INV-2** (RED, behavioural — `ProbeProcessesParentedToDialog`):
  immediately after `diffviewer::show(callerWidget, …)` returns (before
  any event-loop turn), the returned dialog must have at least one
  `QProcess` descendant. Today `runAsync()` parents every probe
  `QProcess` to `callerWidget`, so the dialog has none and closing it
  cannot kill them.

  Checked as "dialog has ≥1 `QProcess` child", not "caller has zero" —
  `show()` also spawns a `rev-parse` / `ls-files` pair for the
  live-refresh watcher via `reseed()`/`enumerate()`, and the fix note
  above only commits to reparenting "the probe `QProcess`es", not every
  process the dialog ever spawns. Asserting the caller ends up with
  zero `QProcess` children would bind this test to an implementation
  choice the roadmap item doesn't make; asserting the dialog gets at
  least one is the part every reading of the fix agrees on, and it is
  false today.
- **INV-3** (RED, source-scrape — `GenerationCheckGatesStaleRender`):
  `src/diffviewer.cpp`, comments stripped, body of `QDialog *show(QWidget
  *parent,` inside `namespace diffviewer`, must contain (a) a `++` bump
  on a counter whose name contains "generation", "epoch", "sequence", or
  "seq" (case-insensitive — spelling-tolerant, since the fix doesn't
  exist yet to pin an exact name), and (b) an `if (...that
  counter... != ...) return;` (or `==`) guard appearing *before*
  `viewerGuard->setHtml(` in the body. Today neither exists — no round
  is ever refused.

  The needle choice follows this codebase's own existing convention for
  the identical shape: `src/llmclient.cpp`'s
  `++m_sendGeneration` / `const quint64 gen = m_sendGeneration;` /
  `if (gen != m_sendGeneration) return;` in `emitDeferredError()`, and
  `src/config.h`'s `autoProfileRulesGeneration()`. "generation" is the
  most likely spelling; "epoch"/"sequence"/"seq" are accepted so a
  differently-named but equivalent counter still passes.
- **INV-4** (GUARD, behavioural —
  `SmallDiffRendersFullyNoTruncationNotice`): an ordinary small diff (a
  three-line file, one line changed) must still render its actual
  content in full and must NOT carry a truncation notice. This holds
  today and must keep holding — the cap in INV-1 must not fire on
  diffs nowhere near it.
- **INV-5** (RED, behavioural — `ClosingDoesNotDestroyRunningProbes`,
  ANTS-5128): closing the Review Changes dialog must cancel its probe
  `QProcess`es before they are destroyed, so none is ever destroyed
  while still running. Exercised with a slow `git` shim placed first on
  `PATH` before calling `diffviewer::show()` (the fixture repo is built
  with the real `git`, before `PATH` is touched), so the probes are
  still running at the instant `close()` is called. The check reads
  Qt's own warnings over the close: Qt reports every `QProcess`
  destroyed in a running state, and the test fails if any appears.

  The observable is that warning rather than the UBSan error, because
  the error exists only in the sanitizer build and this bundle also
  runs in Release. The warning reports the precondition the UBSan error
  needs: the CI stack trace runs `~QDialog` → `~QWidget` →
  `deleteChildren()` → `~QProcess` → `finished` → the probe handler's
  `QPointer<QDialog>` downcast, and that chain begins with a running
  probe being destroyed. Remove the precondition and the chain cannot
  start.

  The probes must have reached `QProcess::Running`, not merely
  `Starting`, before `close()`. `~QProcess` emits nothing for a process
  that never reached `Running`, so a close during `Starting` exercises
  none of this and the test passes while the defect is live. The test
  pumps until every probe is `Running` and fails the fixture otherwise.
  Measured 2026-09-12.

## Rationale

The freeze is user-visible and recurring, not one-off: because the
dialog re-probes on every tracked-directory change (`DirTreeWatcher`),
a single large-file rewrite (an agent regenerating `ROADMAP.md`, a
lockfile update) produces the same multi-second GUI-thread stall on
every burst until the user closes the dialog or the file stops
changing. New-files already earned a 200 KB cap for exactly this
reason (`diffviewer.cpp`, `kCap = 200 * 1024`); the tracked-file `Diff`
section is the larger, more common case (any tracked file, not just an
untracked one) and has no equivalent.

## Scope

### In scope
- The `Diff` section's byte cap and its truncation wording.
- The stale-round/generation guard in `finalize()`.
- `QProcess` parentage of the async git probes.
- (ANTS-5128) Whether closing the dialog destroys a probe `QProcess`
  that is still running.

### Out of scope
- The byte-identical-skip *timing* fix (returning before the HTML build
  rather than after) — not independently observable from outside
  `finalize()` without instrumenting it, and the existing
  `review_changes_scroll_preserve` test already pins the skip's
  existence.
- Cancelling an in-flight `QProcess` when a newer round starts (as
  opposed to refusing to *render* a stale round that already
  completed) — the roadmap item's fix note commits to a generation
  guard on render, not to process cancellation, and the two are
  observably different only under process-level instrumentation this
  test does not attempt.
- Diff colourization / anchor-linking correctness — covered by
  `review_changes_branches` and `review_changes_scroll_preserve`.
- The New-files 200 KB untracked-file cap — already correct and
  unrelated to this defect (INV-4 confirms an ordinary tracked-file
  diff still renders in full, but does not re-test the New-files path).
- (ANTS-5128) Graceful vs. forceful process termination (`terminate()`
  vs. `kill()`), and whatever grace period the fix chooses — INV-5 pins
  only that no probe is destroyed while running.
- (ANTS-5128) The grandchild a killed shim leaves behind. Killing the
  probe kills the process Qt started, not its own children, so a
  `git` that had spawned a helper leaves it orphaned. The same was
  true before this fix.
- (ANTS-5128) The app-exit teardown path specifically. The fix covers
  every destruction path because the host cancels the probes in its own
  destructor, but this test only exercises an explicit `close()`;
  app-exit teardown has no harness here.

## Regression history

- **Reported:** ANTS-5059 (roadmap item, 2026-09-11) — no size cap on
  the tracked-file diff, GUI-thread HTML build, no stale-round
  cancellation.
- **Fixed:** 2026-09-11 (ANTS-5059, items 1-4 under "Fix" above). The
  three RED invariants were authored against the still-live defect and
  pass against the fix; INV-4 passes throughout.
- **Reported:** ANTS-5128 (2026-09-12; CI run 34650028988, job "Build
  and test (ASan/UBSan)") — the ANTS-5059 fix parented the probe
  `QProcess`es to the dialog so closing it would kill them. It did not
  kill them: closing deleted them as running children, and `~QProcess`
  then emitted `finished` into `finalize()`'s `QPointer<QDialog>`
  downcast mid-teardown (UBSan-caught undefined behaviour).
- **Fixed:** 2026-09-12 (ANTS-5128, item 5 under "Fix" above). The
  probes moved under a `ProbeHost` child of the dialog, which
  disconnects and kills them in its own destructor.

## How to verify

```bash
cmake --build build --target test_dialogs
ctest --test-dir build -R ReviewChangesDiffCap
# Pre-ANTS-5059-fix: INV-1, INV-2, INV-3 fail with a diagnosable
# expected/actual message each; INV-4 passes.
# Pre-ANTS-5128-fix: INV-5 fails, reporting that closing the dialog
# destroyed every one of its still-running probes. Runs in both the
# Release and ASan/UBSan configurations; the warning is the
# discriminator in both.
```
