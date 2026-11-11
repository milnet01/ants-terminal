# Window size in columns and rows (ANTS-6742)

The user sets the terminal window's size as columns × rows of text,
either once from the View menu or as a fixed size every window opens at.

## Invariants

- **INV-1 — config keys.** `window_fixed_size` (default false),
  `window_columns` (default 120, bounded 20–1000) and `window_rows`
  (default 36, bounded 5–500) round-trip through `Config`.
- **INV-2 — the grid fits.** `TerminalWidget::sizeForGrid(cols, rows)`
  returns the widget size whose grid is exactly `cols` × `rows`:
  resized to it, the widget's grid has those columns and rows.
- **INV-3 — one sum.** `recalcGridSize` and `sizeForGrid` take the
  padding, scroll bar and search bar from one helper, so the two cannot
  disagree.
- **INV-4 — the window follows the terminal.** `resizeToCells` grows or
  shrinks the window by the difference between the current terminal's
  size and `sizeForGrid`, so the window's other parts keep their size.
  It does nothing in quake mode.
- **INV-5 — when the fixed size applies.** The first show applies it
  when `window_fixed_size` is set. The Settings dialog's
  `settingsChanged` handler and `onConfigFileChanged` apply it only
  when the fixed size changed, so another setting never resizes the
  window.
- **INV-6 — the controls.** View > "Resize Window..." asks for columns
  and rows, starting at the current terminal's, and resizes once. The
  Settings dialog's Appearance tab has the fixed-size switch and the two
  numbers.

## Test

`test_window_cell_size.cpp`, suite `WindowCellSize`, in the `test_chrome`
bundle. INV-4's quake guard, INV-5 and INV-6 read the MainWindow and
SettingsDialog sources.
