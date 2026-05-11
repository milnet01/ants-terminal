# roadmap_shortcuts_cheatsheet — feature contract

Locks ANTS-1236: RoadmapDialog v2 keyboard-shortcut cheatsheet overlay.
Full design rationale + re-open conditions live in
[`docs/specs/ANTS-1236.md`](../../../docs/specs/ANTS-1236.md). This file
restates the invariants that the test asserts so the test reads as a
contract rather than a regression-only ratchet.

## Invariants under test

These mirror `docs/specs/ANTS-1236.md` § 4. The IDs are qualified
(`ANTS-1236-INV-N`) per § 7 of that spec — bare `INV-N` is ambiguous
once two specs share a test directory tree.

- **ANTS-1236-INV-1.** The data table `kRoadmapShortcuts` lives in
  `src/roadmapdialog.cpp` and has exactly 9 rows at ship.
- **ANTS-1236-INV-2.** `roadmapShortcutRows()` returns one entry per
  `kRoadmapShortcuts` row, in declaration order, with the keys column
  rendered as `QString::fromUtf8(row.keys)` and the action column
  rendered as `RoadmapDialog::tr(row.action)`.
- **ANTS-1236-INV-3.** Pressing `?` on the dialog (without the search
  box focused) opens `RoadmapShortcutsDialog`. Pressing `?` again or
  `Esc` closes it.
- **ANTS-1236-INV-4.** With the search box focused, `?` is appended to
  the search predicate verbatim. The cheatsheet does NOT open.
- **ANTS-1236-INV-5.** The cheatsheet `QTableWidget` has two columns
  (`Shortcut`, `Action`) and exactly `std::size(kRoadmapShortcuts)`
  rows. Cell text matches `QString::fromUtf8(row.keys)` and
  `RoadmapDialog::tr(row.action)` — the qualified call is mandatory
  because the strings live in `RoadmapDialog`'s tr() context.
- **ANTS-1236-INV-6.** Re-opening the cheatsheet reuses the same
  instance (QPointer identity).
- **ANTS-1236-INV-7.** `setWindowTitle(tr("Roadmap Keyboard
  Shortcuts"))` is set so screen readers announce the dialog by name.
- **ANTS-1236-INV-8.** The feature test asserts
  `roadmapShortcutRows().size() == 9` (exact match). A future
  shortcut addition that doesn't bump both the data table AND this
  test count fails the suite.

## Test shape

Single C++ file `test_roadmap_shortcuts_cheatsheet.cpp` bundled into
the `test_dialogs` GoogleTest target. Two halves:

1. **Pure assertions** — `roadmapShortcutRows()` size + content, plus
   source-grep guards against the literal byte sequences `kStatusLabels`
   / `kRoadmapShortcuts` and the `keyPressEvent` override.
2. **Live-widget assertions** — construct a `RoadmapDialog` against a
   tmpdir fixture, `QTest::keyClick(&dialog, Qt::Key_Question)`,
   assert child / visibility / window title / re-open identity.

Exits 0 iff every invariant holds.
