# Update-available indicator on the menu bar (ANTS-1124)

User request 2026-04-30: move the "update available" link from the
status bar to the menu bar, immediately to the right of the Help
menu, so it reads as a one-shot call-to-action and frees status-bar
real estate for steady-state telemetry.

Companion to `docs/specs/ANTS-1124.md`. The spec covers the
rationale; this file is the test contract.

## Invariants

Source-grep only — the change is a wiring migration with no new
behavioural surface to drive.

- **INV-1** `mainwindow.h` no longer declares
  `QLabel *m_updateAvailableLabel`. Asserted by negative
  source-grep on the header.
- **INV-2** `mainwindow.h` declares `QAction *m_updateAvailableAction`.
  Asserted by source-grep.
- **INV-3** No `m_updateAvailableLabel` identifier survives in
  `mainwindow.cpp` (no `->show()`, `->setText()`, or wiring of the
  old QLabel). Asserted by negative source-grep on the bare token —
  catches partial-rename leftovers that the narrower
  `addPermanentWidget` grep would miss.
- **INV-4** `mainwindow.cpp` wires the action onto the menu bar
  via `m_menuBar->addAction(m_updateAvailableAction)` *or* an
  `insertAction(...)` form. Asserted by source-grep that
  tolerates either call shape, so a future "insert before some
  other top-level action" refinement doesn't break the test.
- **INV-5** The visibility-toggle lives on the action:
  `m_updateAvailableAction->setVisible(true)` appears in the
  body of `MainWindow::checkForUpdates` (or the lambda it
  installs). Asserted by source-grep.
- **INV-6** The hide path uses
  `m_updateAvailableAction->setVisible(false)` (or equivalent)
  in the same body. Asserted by source-grep.
- **INV-7** `connect(m_updateAvailableAction` is wired to
  `&QAction::triggered`. Asserted by whitespace-collapsed source-
  grep — clang-format may wrap the args across lines, so the
  literal grep normalises whitespace before checking both halves
  appear in the call.
- **INV-8** Action starts hidden — `m_updateAvailableAction->
  setVisible(false)` runs at construction (before / near the
  `m_menuBar->addAction(...)` site). Prevents a flash of empty
  "Update available" text on every startup until the 5-s probe
  fires. Asserted by source-grep.

## CMake wiring

The test target is wired into `CMakeLists.txt` via `add_executable`
+ `add_test` with `LABELS "features;fast"`,
`target_compile_definitions` for `MAINWINDOW_H` /
`MAINWINDOW_CPP` (so the test can `slurp(MAINWINDOW_H)`), and
`target_link_libraries Qt6::Core Qt6::Gui Qt6::Widgets` (the
test references `QString` literals only, but the `Qt6::Widgets`
link is harmless and matches the codebase's existing template).

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that lands ANTS-1124.
git checkout <impl-sha>~1 -- src/mainwindow.cpp src/mainwindow.h
cmake --build build --target test_update_available_menubar
ctest --test-dir build -R update_available_menubar
# Expect INV-1 / INV-2 / INV-3 / INV-4 to fail — pre-fix code
# still has the QLabel + status-bar wiring.
```
