# Review Changes dialog: scroll position preserved across live updates

## Problem

The 0.7.32 live-update path for the Review Changes dialog called
`QTextEdit::setHtml()` unconditionally on every probe completion —
each `QFileSystemWatcher` debounce tic, each manual Refresh click,
each settled in-flight refresh. Setting HTML on a `QTextEdit`
re-parses the document and snaps the vertical scroll bar back to
the top, also discarding any selection or text-cursor position.

User report 2026-04-25 (post-0.7.32):
> "When using the Review Changes dialog, the constant resetting of
> the text means that if I scroll, it resets to the beginning every
> refresh. That means I can't scroll basically."

On a long diff with active live updates the dialog became
effectively unscrollable — every wheel-flick raced with the next
refresh.

## Fix (0.7.37)

Two-layer guard around `viewerGuard->setHtml(html)` inside the
`finalize` lambda in `MainWindow::showDiffViewer`:

1. **Skip identical renders.** A `std::shared_ptr<QString> lastHtml`
   lives in `showDiffViewer`'s scope and is captured by both
   `runProbes` and `finalize`. When the new render is byte-identical
   to `*lastHtml`, `finalize` returns early — `setHtml` is never
   called, and selection, cursor, and scroll are byte-perfectly
   preserved. This is the common case during idle live-update tics
   (branch metadata refreshes that don't change anything visible).

2. **Restore scroll on real changes.** When the content does change,
   `finalize` captures the vertical and horizontal scroll-bar values
   before calling `setHtml`, then restores them after, clamped to
   `bar->maximum()` so a shorter render after a commit doesn't
   over-scroll. Selection is lost in this branch (Qt re-parses the
   HTML into a fresh `QTextDocument`), but the scroll position
   stays visually stable — the user's wheel gesture lands on the
   same content it would have without the live refresh.

3. **First-render carve-out.** On the very first finalize call
   (`lastHtml->isEmpty()`), restoration is suppressed so the dialog
   opens scrolled to the top, not at some leftover scroll-bar
   value from an unrelated viewer.

## Invariants

Source-grep harness, no display required. Every invariant pins one
piece of the fix that, if reverted, re-opens the bug.

- **INV-1** (`lastHtml` shared cache exists):
  `showDiffViewer` declares an `auto lastHtml = std::make_shared<QString>()`
  before `runProbes` is constructed. A `static QString` or per-call
  local would not survive across `runProbes` invocations.
- **INV-2** (`lastHtml` threaded through both lambdas):
  Both the `runProbes` and `finalize` capture lists include
  `lastHtml`. Without this, `finalize`'s comparison always sees an
  empty cache and the early-return never fires.
- **INV-3** (skip identical renders):
  `finalize` contains an `if (*lastHtml == html)` guard followed by
  a `return;`. Without the guard, `setHtml` runs on every probe
  completion regardless of content, re-opening the bug.
- **INV-4** (scroll-bar capture):
  `finalize` reads `verticalScrollBar()->value()` (and the
  horizontal counterpart) BEFORE the `setHtml` call. Capturing
  after would read the post-`setHtml` value (always 0).
- **INV-5** (scroll-bar restore):
  `finalize` calls `verticalScrollBar()->setValue(...)` AFTER the
  `setHtml` call, clamped via `std::min(..., bar->maximum())`.
  Without the clamp a longer-then-shorter content sequence over-
  scrolls and the bar pins at the bottom of an old position.
- **INV-6** (first-render carve-out):
  The capture+restore pair is gated on `!isFirstRender`, where
  `isFirstRender = lastHtml->isEmpty()` is sampled before the cache
  update. Without this, the very first render restores a scroll
  value from whatever leftover state the QTextEdit had — usually 0
  (harmless), but the explicit guard documents intent.

## How to verify pre-fix code fails

```bash
git checkout 07a9976 -- src/mainwindow.cpp
cmake --build build --target test_review_changes_scroll_preserve
ctest --test-dir build -R review_changes_scroll_preserve
# Expect: every invariant fails — pre-0.7.37 source has no `lastHtml`,
# no skip-identical guard, no scroll-bar capture/restore.
```
