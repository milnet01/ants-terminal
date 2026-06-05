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

INV-3..INV-11 (atomic per-file write, skipped[] accounting, fail-closed
bad_path, caller_cwd Required, size cap, count balance) live in the
`cmdApplyEdits` wrapper and are covered by the W1 source-scrape plus the
pure-helper behaviour above (the helper does only the in-memory transform).

Label: `features;fast`. Verify each fails against pre-implementation source.
