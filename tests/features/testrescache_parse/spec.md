# testrescache_parse — feature-conformance contract

**Theme:** `TestResCache::parseCtestOutput` — the pure ctest-log parser
behind `test_results` and `focused_test`. Full spec:
`docs/specs/ANTS-1300.md` § 3.4.

`docs/specs/ANTS-1300.md` § 3.4 and `tests/features/mcp_test_results/`
document the wiring around this function; neither exercises the parser
against a real ctest log. This test calls
`TestResCache::parseCtestOutput` directly with verbatim ctest output
and checks what it returns. `tests/features/mcp_focused_test/spec.md`
names this parser as "reused" coverage for `focused_test`'s ctest
parsing; this is that coverage.

## Invariants

- **INV-1**: a log whose only per-test line is a real ctest `***Failed`
  line — the dot-fill running directly into the `***` marker, no space
  between them — yields that test's name in `failingTests`.
- **INV-2**: a log mixing passing tests with one `***Failed`, one
  `***Exception: SegFault`, and one `***Timeout` line names every
  non-passing test in `failingTests` and no passing one.
- **INV-3**: `passed`/`failed`/`total` match the log's own summary
  footer on both fixtures above, before and after the parser is fixed.

## Out of scope

- The MCP wiring around this function (`op=record`/`op=read`,
  refusal codes, the wire envelope) — covered by
  `tests/features/mcp_test_results/`.
- `focused_test`'s own ctest invocation — covered by
  `tests/features/mcp_focused_test/`.
- Statuses outside `Failed`/`Exception: <reason>`/`Timeout` (e.g.
  `Not Run`) — not exercised here.
