# themedstylesheet extraction (ANTS-1147)

Companion test for `docs/specs/ANTS-1147.md`. The full
contract — surface-design choice, behavioural-parity statement,
include-hygiene plan, three-call-site chip-helper consolidation,
cache-and-compare optimization shape — lives in the spec; this
file documents only what
`test_themedstylesheet_extraction.cpp` itself asserts.

This test is hybrid: source-grep for INVs 1-7 (mirrors the
`claude_statusbar_extraction` shape), plus unit-level helper
calls for INV-8 (the helpers are pure functions so they can be
exercised without instantiating any QWidget).

## Invariants pinned by this test

- **INV-1** `src/themedstylesheet.h` declares
  `namespace themedstylesheet` with the six public function
  signatures byte-for-byte (`buildAppStylesheet`,
  `buildMenuBarStylesheet`, `buildStatusMessageStylesheet`,
  `buildStatusProcessStylesheet`, `buildGitSeparatorStylesheet`,
  `buildChipStylesheet`).
- **INV-2** `src/themedstylesheet.cpp` contains the migrated QSS
  selectors byte-for-byte (app-level QSS landmarks, SVG
  data-URI markers, chip QSS markers). `QProgressBar::chunk` is
  excluded from this list — non-distinctive marker shared with
  `claudestatuswidgets.cpp` (ANTS-1146).
- **INV-3** `mainwindow.cpp` no longer contains the migrated QSS
  body (substring grep for distinctive markers must return zero).
- **INV-4** `mainwindow.cpp::applyTheme` calls each of the six
  helpers (substring grep on each `themedstylesheet::build*(`).
- **INV-5** `MainWindow::refreshRepoVisibility` calls
  `themedstylesheet::buildChipStylesheet` — the chip helper is
  reused across all three pre-1147 chip-style call sites.
- **INV-6** `MainWindow::updateStatusBar` body contains the
  cache-and-compare guard:
  - `mainwindow.h` declares `m_lastBranchChipValid` and
    `m_lastBranchChipQss`. (The original spec authorised four
    members — the latter two were dropped in the post-1147 debt
    sweep as write-only state; the QSS string itself encodes
    (theme × primary × margin) so the string-compare is sufficient.)
  - `updateStatusBar`'s body contains
    `themedstylesheet::buildChipStylesheet(`,
    `newQss != m_lastBranchChipQss`, AND
    `m_lastBranchChipValid = true`.
- **INV-7** Two-sided LoC anchor:
  - `themedstylesheet.cpp` LoC ≥ 200.
  - `mainwindow.cpp` LoC strictly decreases by ≥ 200 vs. the
    parent commit.
- **INV-8** Unit-level helper tests — direct calls into
  `themedstylesheet::*` from this test, asserting the returned
  `QString` for a fixed theme contains expected substrings:
  - `buildAppStylesheet(...)`: `QMainWindow { background-color:`,
    `QPushButton:hover:enabled`, `data:image/svg+xml;utf8`.
  - `buildChipStylesheet(theme, "#00ff00", 4)`: `color: #00ff00`,
    `margin: 2px 6px 2px 4px`, `border-radius: 3px`,
    `font-weight: 600`.
  - `buildChipStylesheet(theme, "#ff0000", 0)`: `margin: 2px 6px 2px 0`.
