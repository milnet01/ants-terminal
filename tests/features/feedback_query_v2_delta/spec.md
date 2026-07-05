# feedback_query marker-aware v2 delta — feature conformance

Contract for the marker-aware v2 un-triaged delta in `FeedbackFile::parse()` +
`feedback_query`. Full design: `docs/specs/ANTS-3448.md`. This test locks the
invariants.

## What the change does

`parse()` becomes **marker-aware**. On a `<!-- ants-mcp-feedback: 2 -->` (or
higher) file the delta / `mappedIds` / `suspectedUntagged` follow the **v2
inline rule**:

- **Un-triaged** = a `### ` finding whose first `**Proposed ID:**` value holds no
  `ANTS-[0-9]+` and does not begin `n/a`. The delta is the ordered
  concatenation of those blocks (trailing blanks stripped per block).
- **mappedIds** = the union of `ANTS-[0-9]+` from each finding's first
  `**Proposed ID:**` value, contributed only when it is not an `n/a` closure;
  the retained v1 tables are not counted.
- **suspectedUntagged** = `### ` finding-shaped blocks (a `- **What/Repro/
  Impact:**` bullet) with no id line.

The v1 boundary scan (`lastMaintainerLine` / `maintainerBlockCount` /
`trackingRows`) runs unchanged on both versions. `feedback_query` gains
`format_version` + `suspected_untagged[]`. `session_orient feedback_pending`
needs no code change — it reads `deltaPresent`/`deltaLineCount`, now
version-correct.

## Invariants under test (⇢ docs/specs/ANTS-3448.md)

- **INV-1** — `formatVersion` from the first marker (absent/malformed → 0);
  `>= 2` selects the v2 rule, `< 2` the v1 rule.
- **INV-2** — v1 unchanged: a `: 1` fixture's delta/mappedIds are byte-identical
  to the prior output; `suspectedUntagged` empty.
- **INV-3** — v2 delta = ordered concatenation of un-triaged findings; a finding
  with an id or an `n/a` closure, and a `### ` block with no id line, are all
  excluded.
- **INV-4** — v2 `mappedIds` = inline ids only (closure contributes zero); the
  retained v1 table ids are not counted.
- **INV-5** — `suspectedUntagged` lists no-id finding-shaped blocks; bare prose
  excluded; empty on v1.
- **INV-6** — the v1 boundary scan is version-independent.
- **INV-7** — fenced regions inert in both the finding enumeration and the
  finding-bullet scan.
- **INV-8** — `feedback_query` emits `format_version` + `suspected_untagged[]`.
- **INV-9** — `feedback_pending`'s count follows the v2 rule via `parse()`.
- **INV-11** — the v2 delta is a concatenation, NOT a contiguous file slice.
- **INV-12** — `migrateV2` then `parse()` agree on the version via the shared
  `markerVersion`.
