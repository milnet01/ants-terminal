# ANTS-4659 — ledger of undeclared MCP arguments

The dispatcher refuses a call naming an argument its verb does not declare
(`tests/features/mcp_undeclared_args_refused/spec.md`). This ledger records
what callers send: one JSONL line per call that carried an
undeclared argument, kept in `~/.cache/ants-terminal/mcp-undeclared-args.jsonl`.

## Invariants

- **INV-1** — A line holds `ts`, `tool` and `keys`: the time, the verb and the
  undeclared argument names. No argument value is stored.
- **INV-2** — A line keeps at most `kMaxKeysPerRecord` names, each cut to
  `kMaxKeyChars`; `keys_dropped` counts the names left out.
- **INV-3** — Appending keeps the newest line and drops the oldest once the
  file passes its byte cap. The file is owner-only (0600).
- **INV-4** — With a ledger path set, a dispatched call carrying an undeclared
  argument appends one line naming the verb and that argument.
- **INV-5** — With no path set, a dispatch writes no file. A call whose
  arguments are all declared appends nothing.
- **INV-6** — `ants-mcpd` sets the path only after its `--call` return, so a
  test running `--call` never writes the user's file. The terminal sets it
  too, since it alone finishes the calls `ants-mcpd` forwards to it.

## Tests

`test_mcp_undeclared_args_survey.cpp`: INV-1 to INV-5 behaviourally; INV-6 by
reading `src/mcpdmain.cpp` and `src/mainwindow_mcp.cpp`.
