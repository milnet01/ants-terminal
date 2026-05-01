# Roadmap Kind-faceted filter (ANTS-1106)

User request 2026-04-30: add a secondary faceted strip under the
ANTS-1100 status-filter tabs so the user can narrow the rendered
roadmap to one or more `Kind:` values (`implement`, `fix`,
`audit-fix`, `review-fix`, `doc`, `doc-fix`, `refactor`, `test`,
`chore`, `release`, plus the de-facto `research` / `ux` values).
Companion to `docs/specs/ANTS-1106.md` — the design + threat-
model lives there; this file is the test contract.

## Invariants

Source-grep + behavioural drive of `RoadmapDialog::renderHtml`.

- **INV-1** `renderHtml` accepts a defaulted 7th parameter
  `const QSet<QString> &kindFilter`. Asserted by source-grep on
  the header.
- **INV-2** Empty filter passes through unchanged. A 3-bullet
  synthetic markdown rendered with an empty `kindFilter`
  contains all three bullets in the output HTML.
- **INV-3** `kindFilter = {"implement"}` keeps only the bullet
  whose body has `Kind: implement` and drops the other two.
- **INV-4** `kindFilter = {"fix", "doc"}` (multi-value) keeps
  bullets whose `Kind:` is in either, OR-include semantics.
- **INV-5** A bullet with no `Kind:` line is excluded when the
  filter is non-empty. (When the filter is empty, the bullet
  passes — covered by INV-2.)
- **INV-6** The dialog's layout adds a Kind row containing a
  `QLabel("Kind:")` plus at least three checkbox object names
  matching `roadmap-filter-kind-implement`,
  `roadmap-filter-kind-fix`, and `roadmap-filter-kind-doc`.
  Asserted by source-grep on `roadmapdialog.cpp`.
- **INV-7** Each Kind checkbox has an `objectName` matching
  `roadmap-filter-kind-<kind>`. Asserted by source-grep.

## CMake wiring

`add_executable` with `tests/features/roadmap_kind_facets/test_*.cpp`
plus `src/roadmapdialog.cpp` (renderHtml is a static method —
linking the cpp is sufficient). Link `Qt6::Core Qt6::Gui
Qt6::Widgets`. `target_compile_definitions` for
`ROADMAPDIALOG_H` / `ROADMAPDIALOG_CPP` so the source-grep INVs
have file paths.

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that lands ANTS-1106.
git checkout <impl-sha>~1 -- src/roadmapdialog.cpp src/roadmapdialog.h
cmake --build build --target test_roadmap_kind_facets
ctest --test-dir build -R roadmap_kind_facets
# Expect every INV to fail: renderHtml has 6 args, no Kind row.
```
