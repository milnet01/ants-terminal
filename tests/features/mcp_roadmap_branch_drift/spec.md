# mcp_roadmap_branch_drift — feature-conformance test

Locks the wire and dispatcher contract for the ANTS-1583
`roadmap_branch_drift` MCP verb. Asserts spec invariants INV-1..INV-11
via source-grep + a small runtime classifier check.

## Invariants

| ID | Source | Check |
|---:|--------|-------|
| INV-1  | spec § 3 | Verb is registered with `CallerCwdContract::Required` and the schema lists `caller_cwd` in `required[]`. Source-grep on `claudeintegration.cpp`. |
| INV-2  | spec § 3 | Verb is in the etag allowlist (`isEtagSupportedTool`). |
| INV-3  | spec § 3 | SHA detector regex has the `// ANTS-1583` anchor comment + lookahead `(?=[0-9a-f]*[a-f])`. |
| INV-4  | spec § 3 | Reachability classifier runtime — runtime test against a real `QTemporaryDir` git fixture (3-commit chain A→B→C, orphan D). Asserts A/B/C reachable, D `sha_not_in_HEAD`, fabricated `sha_not_in_git`. |
| INV-5  | spec § 3 | Short-SHA prefix lookup — runtime test that `C.left(7)` resolves to C. |
| INV-6  | spec § 3 | `max_drift` clamp `[1, 100]` + `drift_truncated` flag literal. |
| INV-7  | spec § 3 | `caller_cwd` declared required in the schema. |
| INV-8  | spec § 3 | No `--contains` fork-per-SHA. Source-grep anti-pattern guard on the handler body. |
| INV-9  | spec § 3 | Envelope emits integer-typed `scanned_bullets` / `with_sha` / `drift_count`. |
| INV-10 | spec § 3 | Handler reuses `collectGitSnapshot(` rather than forking a fresh `rev-parse HEAD`. |
| INV-11 | spec § 3 | `no_git_state` error code is listed in `docs/standards/mcp-error-codes.md`. |

## Strategy

Runtime helper: build a 3-commit `QTemporaryDir` git fixture
(commits A, B, C linear; HEAD at C plus an orphan commit D). Drive
the verb against the fixture via the canonical handler.

Source-grep checks lock the dispatcher / descriptor / contract /
taxonomy wiring (these are unfalsifiable at runtime without
spinning up a full Qt MCP server).
