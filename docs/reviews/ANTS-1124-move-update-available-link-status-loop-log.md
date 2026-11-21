# ANTS-1124 — Move "update available" link from status bar to menu bar — review loop log

Record for [`../specs/ANTS-1124-move-update-available-link-status.md`](../specs/ANTS-1124-move-update-available-link-status.md) § Cold-eyes review fold-in (2026-05-01).

## Cold-eyes loop log

- F1 — INV-4 widened to accept `addAction` *or* `insertAction`
  (whichever Qt idiom the implementer reaches for).
- F2 — INV-3 broadened from the narrow `addPermanentWidget` grep
  to "no `m_updateAvailableLabel` identifier survives in
  `mainwindow.cpp`" — catches partial-rename leftovers.
- F3 — capture-mechanism softened: member / `QAction::setData` /
  lambda capture all acceptable.
- F4 — INV-7 now whitespace-collapses before grepping, so a
  clang-format line wrap between the `connect` args doesn't
  produce a false negative.
- F5 / F8 — new INV-8 asserts the action starts hidden.
- F7 — pre-fix verification recipe uses `<impl-sha>~1`.
- F15 — CMake wiring acceptance bullet added in the test spec.
