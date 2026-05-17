# Feature spec: MCP tool descriptor `version` field (ANTS-1354)

The `tools/list` response had no per-tool version field. If
`tools/call` response shapes evolve (e.g. ANTS-1294 wrap format
v2), consumers had no machine-readable way to detect the schema
they're talking to — only the fuzzy "did this field appear in the
output?" check.

Adds a `version: "1.0"` field to every tool descriptor by default,
with a per-tool override path for incompatible bumps.

## Invariants

- **INV-1 / every emitted tool descriptor carries `version`.**
  The `tools/list` builder runs a finalisation pass over the
  `tools` array that sets `version: "1.0"` on every entry that
  didn't explicitly set its own version. Anchor: `ANTS-1354` in
  `src/claudeintegration.cpp` near `result["tools"] = tools`.
- **INV-2 / SemVer-of-tools policy documented at the call site.**
  The finalisation block carries a comment explaining the MAJOR /
  MINOR / PATCH semantics for per-tool versioning so a future
  contributor reading the code understands when to bump.
- **INV-3 / per-tool override is non-destructive.** If a future
  tool sets its own `version` (e.g. `t["version"] = "2.0"` when
  shipping an incompatible schema change), the default-fill loop
  MUST NOT overwrite it. Asserted via the `if
  (!t.contains("version"))` guard in the helper.

## Test scope

Source-scrape against `src/claudeintegration.cpp` for the
finalisation loop + the SemVer policy comment + the
non-destructive guard. No functional test of an outgoing
`tools/list` envelope is necessary — the source-scrape locks the
invariant at the emission site, which is where drift would
introduce a regression.
