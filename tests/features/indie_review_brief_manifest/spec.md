# indie_review_brief_manifest

Feature-conformance spec for **ANTS-1281**:
`IndieReviewEngine::assembleBriefManifest` returns a brief without
inlined source bodies; subagent reads files itself.

Full design: [docs/specs/ANTS-1281.md](../../../docs/specs/ANTS-1281.md).

## Invariants

- **INV-2** — `manifest.sourcePaths` reflects `lane.sourcePaths`,
  minus any path that fails the under-project canonicalisation
  guard. For a lane pointing at real, in-repo files, the lists
  match exactly.

- **INV-4** — Path-traversal tightening. A lane whose `sourcePaths`
  contains an entry that canonicalises outside `projectPath` is
  dropped from BOTH `manifest.sourcePaths` AND the source-paths
  list rendered into `manifest.brief`. v1's `assembleBrief`
  drops only the file body; the listed name remained. INV-4
  verifies the v2 tightening.

- **INV-5** — Read-instruction sentinel. `manifest.brief` contains
  the verbatim substring
  `"Read each source file in the list above using your Read tool"`.
  This signals to the dispatched subagent that the absence of
  source-body sections in the brief is intentional, not a bug.

- **INV-6** — `manifest.contractDocs` is exactly
  `["docs/standards/coding.md", "docs/standards/testing.md",
  "docs/standards/documentation.md"]` in that order (v1 brief's
  authored order). Not alphabetical (alpha would put
  `documentation.md` second). Not configurable in v1.

- **INV-token-cost** — Token-cost regression: a synthetic lane
  with one 100 KiB source file produces a `manifest.brief` whose
  UTF-8 byte size is **< 8 KiB**. Proof the body-stripping
  actually happened. (v1 `assembleBrief` for the same lane would
  produce ~100 KiB.)

- **INV-no-file-marker** — `manifest.brief` does **not** contain
  the substring `"=== file: "`. v1 brief uses this marker to
  delimit inlined per-file bodies; absence proves bodies are not
  inlined.

## Out of scope

- Testing `cmdIndieReviewBrief` JSON response shape — that lives
  in `mcp_indie_review_tools/` (added as a substring assertion on
  the tool description, separate test).
- Testing `assembleBrief` (v1) — already covered indirectly by
  `mcp_indie_review_tools/` shape-grep and by the existing
  contract.

## Pre-fix verification

Verify this test fails on a pre-fix build:

```bash
git stash
ctest --test-dir build -R 'IndieReviewBriefManifest'
# expect: FAIL — symbol assembleBriefManifest doesn't exist yet
git stash pop
```
