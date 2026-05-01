# Smooth-scroll snapshot on scroll intent (ANTS-1118)

User report 2026-04-30 + indie-review L2 fold-in 2026-05-01:
the smooth-scroll path defers snapshot capture until the first
`smoothScrollStep` tick commits a non-zero `intStep`, opening
a 16–32 ms window where `onVtBatch` sees `m_scrollOffset == 0`
and lets the grid mutate the rows the user is scrolling past.
Once the snapshot finally captures, it captures post-mutation
state.

Companion to `docs/specs/ANTS-1118.md`. Spec covers root cause
+ rationale; this file is the test contract.

## Invariants

Source-grep on `terminalwidget.cpp`. The fix is a wiring move,
not new behaviour to drive — captureScreenSnapshot /
setScrollbackInsertPaused already exist; they need to be
called from the right place.

- **INV-1** `TerminalWidget::wheelEvent` body calls
  `captureScreenSnapshot()` in a branch gated on the user
  wheeling up from offset=0 with no existing snapshot — i.e.
  the source contains both `m_scrollOffset == 0` and
  `m_smoothScrollTarget > 0` (or `m_smoothScrollTarget >= ...`
  with a positive RHS) and `m_frozenScreenRows.empty()` near
  the `captureScreenSnapshot()` call. Asserted by source-grep
  on the wheelEvent body region.
- **INV-2** The same wheelEvent branch calls
  `m_grid->setScrollbackInsertPaused(true)`. Asserted by
  source-grep.
- **INV-3** `TerminalWidget::smoothScrollStep` body, in the
  timer-stop branch where `std::abs(m_smoothScrollTarget) <
  0.01`, calls `updateScrollBar()` so any stranded
  intent-captured snapshot is cleaned up.
- **INV-4** Regression guard — the existing onVtBatch call
  to `m_grid->setScrollbackInsertPaused(m_scrollOffset > 0)`
  is preserved.

## CMake wiring

Test target wired in `CMakeLists.txt` via `add_executable` +
`add_test` with `LABELS "features;fast"`,
`target_link_libraries Qt6::Core`,
`target_compile_definitions` for `TERMINALWIDGET_CPP`.

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that lands ANTS-1118.
git checkout <impl-sha>~1 -- src/terminalwidget.cpp
cmake --build build --target test_scroll_snapshot_intent
ctest --test-dir build -R scroll_snapshot_intent
# Expect INV-1, INV-2, INV-3 to fail: pre-fix wheelEvent
# has no intent-side snapshot trigger; smoothScrollStep
# stop-branch has no cleanup call.
```
