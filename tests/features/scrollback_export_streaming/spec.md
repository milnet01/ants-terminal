# Feature spec: scrollback and block exports stream to the file (ANTS-5078)

The contract is [`docs/specs/ANTS-5078-export-streaming.md`](../../../docs/specs/ANTS-5078-export-streaming.md).
Its § 3 invariants are the tests here, under the same numbers.

## Fixtures

- `VtParser` feeds a bare `TerminalGrid`, as `osc133_rerun_safety` does.
- Expected bytes were produced by running the pre-change bodies of
  `exportAsText`, `exportAsHtml` and `exportBlockAsCast` on the same
  fixtures, with the scroll offset at zero. They are literals in the test.
- The cast fixture pins `commandStartMs` and `commandEndMs` through
  `promptRegions()`, because the grid stamps them from the wall clock.
- The cast fixture escapes a quote and a backslash, not a tab. A tab from
  the parser moves the cursor and never reaches a cell.

## Test scope

- INV-1 to INV-8 drive `ScrollbackExporter` directly.
- INV-9 and INV-10 are source scrapes of `TerminalWidget` and `MainWindow`.
  The slices run on a live widget's event loop, which the unit harness does
  not drive.
