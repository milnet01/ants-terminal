# DEC mode 2026 sync output unified snapshot path (ANTS-1148)

Companion test for `docs/specs/ANTS-1148.md`. The full design
rationale, transition table, and "what is NOT snapshotted"
contract live in the spec; this file documents only what
`test_sync_output_snapshot.cpp` itself asserts.

This test is source-grep only. The bug is behavioural and a
genuine pixel-level regression test would need offscreen
QPainter rendering + manual paint cycles — doable but
heavyweight and out of scope here. The structural invariants
below lock the shape that prevents the regression; the smoke
command at the spec's tail exercises the actual paint path
manually.

## Invariants pinned by this test

- **INV-1** `src/terminalwidget.cpp` declares the file-scope
  helper `bool batchEntersSyncOutput(const VtBatch &batch)`
  inside an anonymous namespace; body checks each
  `CsiDispatch` action for `intermediate == "?"`,
  `finalChar == 'h'`, and `p == 2026` in `params`.
- **INV-2** `terminalwidget.cpp::onVtBatch` invokes
  `batchEntersSyncOutput(*batch)` BEFORE the
  `m_grid->processAction` loop.
- **INV-3** Pre-scan disjunction:
  `(m_syncOutputActive || batchEntersSyncOutput(*batch)) && m_frozenScreenRows.empty()`
  guards `captureScreenSnapshot()`. Covers both BSU and
  eviction-recovery (resize / RIS / alt-screen).
- **INV-4** End-of-loop snapshot clear when sync is no longer
  active and scroll-up isn't holding the snapshot. Guard:
  `m_scrollOffset == 0 && !m_frozenScreenRows.empty()` (drops
  the prior `wasSync &&` gate that left same-batch BSU+ESU
  stranded).
- **INV-5** `m_syncTimer` slot calls `clearScreenSnapshot()`
  when force-ending sync (and `m_scrollOffset == 0`).
- **INV-6** `terminalwidget.h` declares
  `int m_frozenCursorRow` and `int m_frozenCursorCol`.
- **INV-7** `captureScreenSnapshot()` body stores
  `m_grid->cursorRow()` / `cursorCol()` into the new members.
- **INV-8** `clearScreenSnapshot()` body resets
  `m_frozenCursorRow = 0` and `m_frozenCursorCol = 0`.
- **INV-9** Cursor-position policy centralised in two
  inline accessors (`effectiveCursorRow()` / `effectiveCursorCol()`)
  declared on `TerminalWidget`. `paintEvent` calls each
  accessor ≥ 3 times (cursor draw + under-cursor glyph +
  autocomplete-ghost render). `blinkCursor` calls each accessor
  exactly once (the partial-update rect — must target the
  cell paintEvent will draw, not the live cursor cell).
  Neither function contains `m_grid->cursorRow()` or
  `m_grid->cursorCol()` in the post-fix body. Other reader
  functions (keyPressEvent, inputMethodQuery,
  clickToMoveCursor, findMatchingBracket, toggleFoldAtCursor,
  toggleBookmark, updateSuggestion, lastCommandOutput) keep
  their live reads — they're typing / IME / semantic / output
  paths that want the real cursor.

INV-10 (smoke verification of pixel-level atomicity) is in the
spec's "Smoke step" section; not enforced here.
