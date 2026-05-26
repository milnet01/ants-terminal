# `workspace_search` payload knobs — feature-conformance test

Locks the invariants in `docs/specs/ANTS-1876.md`. Two new opt-in args:
`max_match_bytes` (per-match `text` / `headline` clip) and
`headline_only` (drop snippets, emit `{file, line, headline}` triples).

## Anchors

| INV | Test                                              | What it checks |
|-----|---------------------------------------------------|----------------|
| INV-1 | `Inv1MaxMatchBytesParse`                        | Arg parsed; out-of-range falls back to 0 (default). |
| INV-2 | `Inv2TextClippedToBudgetExactByteCount` + sibs  | Clipped fields exactly `max_match_bytes` bytes (payload + 3-byte ellipsis); short fields emitted verbatim. |
| INV-3 | `Inv3ClipDoesNotSplitCodePoints`                | UTF-8 boundary preserved. |
| INV-4 | `Inv4DedupKeyUnaffectedByClip`                  | Dedup runs before clip (ordering test). |
| INV-5 | `Inv5HeadlineOnlyKeySet`, `Inv5AlsoAtNeverClipped` | `headline_only:true` emits `{file, line, headline}`; `also_at` shape unchanged. |
| INV-6 | `Inv6EchoActivationGated`, `Inv6NoEchoOnError`  | Envelope echo only when feature active + ok:true. |
| INV-7 | `Inv7ToolsListEnumerates`                       | Both `props["max_match_bytes"]` and `props["headline_only"]` exist in the `workspace_search` `tools/list` block. |

## Pre-fix verification

Before the fix lands, the literals `"max_match_bytes"` and
`"headline_only"` are absent from `cmdWorkspaceSearch` /
`tools/list` builder, and the clip helper (`rcClipMatchBytes` or
equivalent) is undefined. After the fix the tests turn GREEN.
