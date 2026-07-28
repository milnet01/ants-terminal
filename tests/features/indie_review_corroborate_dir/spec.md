# indie_review_corroborate_dir

Feature-conformance spec for **ANTS-1282**:
`IndieReviewEngine::corroboratedFindingsFromDir` reads per-lane
review reports from a directory of `*.md` files and corroborates
them server-side, saving the parent (orchestrator) context the
~14 × ~8 KiB transit of the inline-map shape.

Full design: [docs/specs/ANTS-1282.md](../../../docs/specs/ANTS-1282.md).

## Invariants

- **INV-2** — Output-shape parity. For any pair of equivalent
  inputs (same lane → text mapping, one passed inline, one read
  from disk), `findings` are identical.

- **INV-3** — Path-traversal guard. Absolute paths are rejected
  (empty result). Paths whose canonical resolution escapes
  `projectPath` are rejected (empty result).

- **INV-4** — Non-`.md` files in `reports_dir` are ignored, not
  rejected. Sub-directories not recursed.

- **INV-5** — Lane name = filename stem (`vtparser.md` → lane
  `vtparser`). Hidden files (`.foo.md`) skipped.

- **INV-6** — Missing-directory tolerance. If `reports_dir`
  resolves to a non-existent path, return empty list (no crash).

- **INV-9** (ANTS-3713) — External reports dir. Lane reports may
  live outside `projectPath` (the session scratchpad), read via
  `corroboratedFindingsFromCanonicalDir`, which takes a directory
  the caller has already anchored. The project-relative entry
  point `corroboratedFindingsFromDir` is unchanged and still
  refuses the same absolute path, so INV-3 holds for every
  caller that has not opted in via `allow_outside_project`.

- **INV-8** — File-size cap. Each `*.md` file is truncated at
  64 KiB at read time, matching v1 `extractFileLineCitations`'s
  scan window.

## Out of scope

- Testing the MCP-layer XOR rejection — that's the handler's
  responsibility; will be covered indirectly by future
  `mcp_indie_review_tools` shape tests if added.
