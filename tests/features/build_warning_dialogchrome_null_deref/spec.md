# Feature: Silence the ChromeGuard `-Wnull-dereference` false positives (ANTS-3358)

## Contract

A full optimised (`-O3`) GCC build MUST NOT emit `-Wnull-dereference`
warnings from `DialogChrome::ChromeGuard`. The `ChromeGuard` method
cluster (`onShow` / `recenter` / `positionGrip` / `saveSize`) touches the
dialog through a `QPointer<QDialog> m_dlg` and guards every access first
(`if (!m_dlg) return;`, or an inline `&& m_dlg`). But once `-O3` inlines
`QWidget::rect/width/height/parentWidget` down to
`QScopedPointer<QObjectData>::operator->` (qscopedpointer.h:93), GCC can
no longer see the guard and reports ~33 *false-positive*
`-Wnull-dereference` warnings against the Qt headers, attributed back to
these method bodies.

This is the same false-positive class as **ANTS-1554** (which scoped the
identical pragma around the one `QHash::remove` site in `mainwindow.cpp`
and locked it with a source-grep test). The fix here mirrors it exactly:

1. **A GCC-only scoped diagnostic pragma** brackets the `ChromeGuard`
   method cluster in `src/dialogchrome.cpp`:
   - opens with `#pragma GCC diagnostic push` +
     `#pragma GCC diagnostic ignored "-Wnull-dereference"`,
   - closes with a matching `#pragma GCC diagnostic pop`.

2. **The pragma block is guarded by
   `#if defined(__GNUC__) && !defined(__clang__)`** so a clang build never
   sees an unknown-pragma warning (clang doesn't emit this false positive).

3. **The suppression is tightly scoped** — push before the first guarded
   method (`onShow`), pop after the last (`saveSize`). It does NOT wrap the
   whole file, so a genuine null-deref introduced elsewhere in
   `dialogchrome.cpp` is still caught.

4. **The guarded accessors keep their runtime null checks.** The pragma
   silences the *diagnostic*, not the guard — `recenter`, `positionGrip`
   and `saveSize` still early-return on a null `m_dlg`.

## Scope

### In scope
- Source-grep contract over `src/dialogchrome.cpp` (via
  `SRC_DIALOGCHROME_CPP`): the guarded push/ignored/pop pragma block is
  present, GCC-guarded, and correctly ordered around the method cluster.
- Presence of the runtime guards the pragma stands in for
  (`if (!m_dlg) return;` in `recenter` / `positionGrip`).

### Out of scope
- Actually invoking GCC to assert zero warnings — that needs a full
  optimised compile, too heavy for a unit test. The reproduce-and-verify
  (33 warnings → 0) was done once by hand when the fix landed; this test
  is the cheap static regression lock, exactly as the ANTS-1554 fixture is.

## Regression history

- **ANTS-3358 (in-session-2026-07-13):** a full `cmake --build build`
  emitted ~33 `-Wnull-dereference` false positives inlining from
  `dialogchrome.cpp:58-104` (the `ChromeGuard` geometry accessors).
  Reproduced by an `-O3` compile of the single TU (33 warnings), fixed
  with a GCC-guarded scoped pragma around the method cluster (33 → 0),
  and locked by the `PragmaGuardPresent` source-grep below. Deferred from
  the ANTS-2175/2176 bundle to keep that change surgical.
