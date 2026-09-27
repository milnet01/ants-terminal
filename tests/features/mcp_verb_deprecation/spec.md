# Deprecated MCP verbs are marked, still work, and are counted (ANTS-5485)

The user decided on 2026-09-27 to retire obsolete verbs in two steps: mark and
measure for one release, then remove the ones nobody called. This covers step
one. The table is `src/mcpdeprecation.cpp`.

## Invariants

- **INV-1** — every verb in the table is a listed tool, and its `tools/list`
  description begins with `DEPRECATED (ANTS-5485): use <replacement> instead`.
  A verb outside the table carries no such line.
  *Test:* `Inv1ListedAndMarked`.
- **INV-2** — a deprecated verb still answers, and its reply carries
  `deprecated: {replacement, tracking:"ANTS-5485"}`. This holds on a refusal
  too, since a caller that meets a refusal most needs to learn the replacement.
  *Test:* `Inv2CallStillAnswersAndSaysSo` (through the built ants-mcpd).
- **INV-3** — each call to a deprecated verb appends one JSON line
  `{at, verb, caller_cwd}` to `deprecated-calls.jsonl` beside the roadmap
  store, created owner-only (0600). A call to any other verb writes nothing.
  *Test:* `Inv2CallStillAnswersAndSaysSo` and `Inv3OtherVerbsAreNotRecorded`.

## Why the log is its own file

ants-mcpd's per-process usage files are removed when their process exits
(measured 2026-09-27: after an MCP reconnect only the live processes' files
remained), so they cannot answer what was called across a release.

## Reload

The table is compiled in. A change reaches sessions with an ants-mcpd rebuild
and an MCP reconnect; the terminal is not relaunched.

## Build

Compiled into the bundle that holds `tests/features/standalone_mcp_server/`,
whose `mcpd_session.h` it reuses. The child server inherits the bundle's
XDG sandbox, so the log it writes is the one the test reads.
