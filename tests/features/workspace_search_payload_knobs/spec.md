# `workspace_search` payload knobs — feature-conformance test

Locks the invariants in `docs/specs/ANTS-1876.md`. Two new opt-in args:
`max_match_bytes` (per-match `text` / `headline` clip) and
`headline_only` (drop snippets, emit `{file, line, headline}` triples).

## Anchors

| INV | Test                                              | What it checks |
|-----|---------------------------------------------------|----------------|
| INV-1 | `Inv1MaxMatchBytesParse`                        | Arg parsed. **(ANTS-3548)** absent → 512 default (default-ON clip); explicit `<= 0` opts out (0 = no clip); out-of-range *positive* clamps into `[50, 10000]`. |
| INV-1 (ANTS-3548) | `Inv20DefaultOnClip`                | `maxMatchBytes` initialised to `kDefaultMaxMatchBytes` (512) so an absent arg clips by default. |
| INV-1 (ANTS-3548) | `Inv21ExplicitOptOut`               | An explicit `max_match_bytes <= 0` disables the clip (the off switch); schema `default` is 512 and schema `minimum` is 0 (so the 0 opt-out is in-range). |
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

**ANTS-3548 fail-first basis:** the amendment's literals already exist,
so its RED proof is different — pre-3548 `maxMatchBytes` inits to `0`
(clip off by default) and the schema has `default 0` / `minimum 50`, so
`Inv20DefaultOnClip` (expects init `kDefaultMaxMatchBytes`) and
`Inv21ExplicitOptOut` (expects schema `default 512` / `minimum 0`) fail
against pre-3548 code and turn GREEN only after the default flips.

## ANTS-5052 — rg stdout byte ceiling

`rcRunRg` (the one rg call site `cmdWorkspaceSearch`, `cmdCitedBy` and
`cmdCoChangeFamily` share) waits for rg to finish and takes its whole
stdout, bounded only by rg's wall-time budget — not by output size. The
fix reads stdout while rg runs and kills it once a byte ceiling is
passed. This section locks `cmdWorkspaceSearch`'s share of that contract;
`cited_by` and `co_change_family` carry their own case in their own
`spec.md`.

A test-only seam makes the ceiling reachable from a small fixture:
`RemoteControl::setRgStdoutCapOverride(bytes)` overrides the default
ceiling for the instance it is called on. Behavioural, against a real
fixture tree and a real rg, guarded on rg's presence — same pattern as
the ANTS-4901 cases in `workspace_search_enclosing_symbol`.

| Test | What it checks |
|---|---|
| `Ants5052CountOnlyReportsOutputCap` | `count_only` reports `truncated:true` once the output ceiling is hit. |
| `Ants5052FilesOnlyReportsOutputCap` | `files_only` reports `truncated:true` once the output ceiling is hit. |
| `Ants5052DefaultModeReportsOutputCap` | The default row mode reports `truncated:true` once the ceiling is hit, even when `max_results` would not otherwise have been reached. |
| `Ants5052GuardNoOverrideReturnsEverything` | Guard, must hold before and after the fix: with no override, every match comes back, `truncated` stays false, and `count_only`'s `count` equals the real match total. |

**Pre-fix state:** `setRgStdoutCapOverride` is a stub — it sets a field
`rcRunRg` never reads, and `RgRun::outputCapped` is never set. None of
the three envelopes above folds an output-cap signal into `truncated`
today, so every case except the guard is expected to fail against the
current tree.

**Out of scope for this section**, filed separately: line-by-line
parsing of rg's stdout, `rg --count` for the counting modes, and
`co_change_family`'s bounded min-heap.
