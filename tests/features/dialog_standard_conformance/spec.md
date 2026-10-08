# Feature: Settings and Review Changes follow the dialog standard

Source: ANTS-5109 (code-quality-review-2026-09-11, lane
app-entry-dialogs). Standard: `docs/standards/dialogs.md` D1 and D2.

## Invariants

- **INV-1 / D2 minimum size.** `src/diffviewer.cpp` calls
  `dialog->setMinimumSize(` on the Review Changes dialog.
- **INV-2 / D1 theme colours.** Neither `src/diffviewer.cpp` nor
  `src/settingsdialog.cpp` writes a `color:` followed by a `#rrggbb`
  literal. Status colours come from the active theme's `ansi[]`
  entries instead.
- **INV-3 / parented colour picker.** No `QColorDialog::getColor` call
  in `src/settingsdialog.cpp` passes `nullptr` as its parent, so the
  picker opens over Settings rather than as a free-standing window.

## Test scope

Source scrape only (`test_dialog_standard_conformance.cpp`, bundle
`test_dialogs`). Before the ANTS-5109 fix INV-1, INV-2 and INV-3 each
fail.
