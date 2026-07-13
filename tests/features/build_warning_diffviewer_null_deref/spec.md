# Feature: Silence the diffviewer `positionBackToTop` `-Wnull-dereference` false positives (ANTS-3505)

## Contract

A full optimised (`-O3`) GCC build MUST NOT emit `-Wnull-dereference`
warnings from `diffviewer::show()`'s `positionBackToTop` lambda. The lambda
guards `if (!backToTopGuard || !viewerForBtn) return;` and `if (!vp) return;`
before touching `vp->width()` / `backToTopGuard->width()`, but once `-O3`
inlines `QWidget::width()` through the `QScrollBar::valueChanged` /
`rangeChanged` signal-dispatch machinery that invokes the lambda, GCC loses
the guard and reports 4 *false-positive* `-Wnull-dereference` warnings
(2 "potential", 2 definite) at `qwidget.h:904`.

Same false-positive class as **ANTS-1554** (mainwindow `QHash`) and
**ANTS-3358** (dialogchrome `ChromeGuard`). The fix mirrors them:

1. **A GCC-only scoped diagnostic pragma** brackets the geometry-accessor
   block inside the `positionBackToTop` lambda in `src/diffviewer.cpp`:
   `#pragma GCC diagnostic push` + `ignored "-Wnull-dereference"` … `pop`.
2. **Guarded by `#if defined(__GNUC__) && !defined(__clang__)`** so a clang
   build never sees an unknown pragma.
3. **Tightly scoped** — it wraps only the accessor statements (after the
   null guards), not the whole file, so a genuine null-deref elsewhere in
   `diffviewer.cpp` is still caught.
4. **The runtime null guards are unchanged** — the pragma silences the
   diagnostic, not the `if (!vp) return;` checks.

## Scope

### In scope
- Source-grep contract over `src/diffviewer.cpp` (via `SRC_DIFFVIEWER_CPP`):
  the guarded push/ignored/pop pragma block is present, GCC-guarded, sits
  after the lambda's null guards, and the guards themselves remain.

### Out of scope
- Invoking GCC to assert zero warnings — too heavy for a unit test. The
  reproduce-and-verify (4 → 0) was done by hand when the fix landed; this is
  the cheap static regression lock, as with the ANTS-1554/ANTS-3358 fixtures.

## Regression history

- **ANTS-3505 (in-session-2026-07-13):** found during the ANTS-3358
  full-build verify — 4 `-Wnull-dereference` false positives inlining from
  `diffviewer.cpp:137` (the `positionBackToTop` lambda) via the scrollbar
  signal lambdas at :144 / :154. Reproduced by an `-O3` single-TU compile
  (4 warnings), fixed with a GCC-guarded scoped pragma around the geometry
  block (4 → 0), and locked by the `PragmaGuardPresent` source-grep below.
