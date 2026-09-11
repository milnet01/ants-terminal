# Review Changes diff: size cap, off-thread render, generation guard (ANTS-5059)

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

## Invariants

Three checks pin the still-live defect (RED — expected to fail against
current code); one pins behaviour that must not regress once the fix
lands (GUARD — expected to pass now and after the fix).

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

## Regression history

- **Reported:** ANTS-5059 (roadmap item, 2026-09-11) — no size cap on
  the tracked-file diff, GUI-thread HTML build, no stale-round
  cancellation.
- **Fixed:** not yet — this spec and test were authored against the
  still-live defect. All three RED invariants are expected to fail
  until the fix (see "Fix" above) lands; INV-4 is expected to pass
  throughout.

## How to verify

```bash
cmake --build build --target test_dialogs
ctest --test-dir build -R ReviewChangesDiffCap
# Pre-fix: INV-1, INV-2, INV-3 fail with a diagnosable expected/actual
# message each; INV-4 passes. Post-fix: all four pass.
```
