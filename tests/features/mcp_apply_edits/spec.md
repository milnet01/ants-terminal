# Feature test — `apply_edits` MCP tool (ANTS-2022)

Behavioural invariants drive the pure `ApplyEdits::applyToContent` helper;
wiring invariants source-scrape the registration sites. Full design
contract: [`docs/specs/ANTS-2022.md`](../../../docs/specs/ANTS-2022.md).

| # | Test | Asserts |
|---|------|---------|
| W1 | `WiringContract` | `cmdApplyEdits` declared + calls `ApplyEdits::applyToContent` + `validatePath` + `QSaveFile` + `fsyncParentDir`; `apply_edits` registered in claudeintegration (schema + `C::Required`); `registerToolProvider("apply_edits"` in mainwindow. |
| B1 | `UniqueAbsentDuplicate` (INV-1) | unique `old` applies; 0 occurrences → `skipReason:"not_found"`; >1 without `replace_all` → `"ambiguous"`. |
| B2 | `ReplaceAll` (INV-2) | `replace_all` replaces every occurrence; `replacements` = count; 0 occurrences still `not_found`. |
| B3 | `TrailingNewline` (INV-8) | a file ending in `\n` keeps exactly one; a file without one keeps none. |
| R1 | `Ants3711RangeReplacesAndKeepsTrailingNewline` (INV-12) | a guarded `start_line`/`end_line` range replaces exactly those lines and preserves the trailing newline. |
| R2 | `Ants3711EmptyNewDeletesTheRange` (INV-13) | an empty `new` removes the lines outright — no blank line left behind, since "delete these 76 lines" is the motivating case. |
| R3 | `Ants3711StaleRangeRefusesAndLeavesFileIntact` (INV-14) | drifted coordinates → `range_mismatch` skip, file byte-identical; past EOF → `range_out_of_bounds`. |
| R4 | `Ants3711SelectorArgumentRules` (INV-15) | `old` + a range → `bad_args`; neither → `bad_args`; a range without `expect_first_line`/`expect_last_line` → `bad_args`; half a range → `bad_args`. Whole-call refusals, not per-edit skips: the request is malformed, not the file surprising. |
| R5 | `Ants3711EarlierEditShiftsLaterRangeAndTheGuardCatchesIt` (INV-16) | edits compose against one working content, so an earlier line-count change shifts a later range — and the guard turns that into a skip rather than a write to the wrong lines. |

R1–R5 drive `RemoteControl::cmdApplyEdits` directly rather than the pure
helper. That became possible with ANTS-3725; before it, `RemoteControl
rc(nullptr)` segfaulted inside root resolution, which is why the older
wrapper invariants below are source-scraped instead.

INV-3..INV-7, INV-9 and INV-11 (atomic per-file write, skipped[] accounting,
fail-closed bad_path, caller_cwd Required, size cap, count balance) live in
the `cmdApplyEdits` wrapper and are covered by the W1 source-scrape plus the
pure-helper behaviour above (the helper does only the in-memory transform).

Label: `features;fast`. Verify each fails against pre-implementation source.

## ANTS-4838 — per-edit counts and `expect_count`

`edit_replacements` lists each applied edit's own count as
`{index, replacements}`, in input order, in a real run and a dry run alike; an
edit whose file failed to commit is not listed. An optional per-edit
`expect_count` (a non-negative integer, `old` form only) skips the edit as
`count_mismatch`, with `match_count` and `expect_count`, when the number of
matches differs, before the edit reaches the file. `expect_count` on a line
range is refused `bad_args`. *Test:*
`McpApplyEdits.Ants4838PerEditCountsAndExpectCount`.
