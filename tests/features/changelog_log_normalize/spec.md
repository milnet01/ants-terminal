# changelog_log op:normalize — canonicalise [Unreleased] category order

**ID:** ANTS-3495
**Kind:** enhancement
**Source:** 3D_Engine/Vestige feedback 2026-07-10

## Problem

`changelog_log op:add` fires a non-blocking advisory when `## [Unreleased]`
is malformed, but offers no companion op to *fix* the layout — the caller
must hand-normalise. The safe, policy-free subset (per the ROADMAP scoping
note) is to reorder the `### <category>` blocks into canonical
Keep-a-Changelog order; relocating stray interleaved prose is a deferred
follow-up because "the prose's intended home" is under-specified.

## Surface

`ChangelogLog::normalizeUnreleased(markdown) -> NormalizeResult`
(`src/changeloglog.{h,cpp}`), driven by `op:"normalize"` in
`RemoteControl::cmdChangelogLog` (`src/remotecontrol.cpp`). Descriptor +
op-enum in `src/claudeintegration.cpp`.

Response: `{ok, op, file, changed, order_before[], order_after[],
bytes_written}` — `bytes` + `dry_run:true` under preview; no write and
`changed:false` when already canonical. A non-blocking `advisory` names any
interleaved prose that survives the reorder.

## Invariants

- **INV-1** — an out-of-canonical-order section is reordered: `order_after`
  is the canonical permutation, `changed` is true, and every block's bullets
  travel under their own heading (content non-destructive).
- **INV-2** — an already-canonical section is a no-op: `changed` is false and
  the markdown is byte-identical to the input.
- **INV-3** — a preamble paragraph directly under `## [Unreleased]` (before
  the first `### ` heading) is preserved untouched.
- **INV-4** — a non-canonical `### ` heading sorts last, and a duplicate
  category keeps its original relative order (stable sort).
- **INV-5** — refusals: `not_unreleased` (no `## [Unreleased]`),
  `feature_grouped_section` (dated `### ` topics, not flat categories).
- **INV-6** — after the reorder, interleaved non-heading prose still present
  is surfaced via the `advisory` (`malformed_section`), honest that
  op:normalize reorders category order only.
- **INV-7** — handler write path: `op:"normalize"` rewrites CHANGELOG.md
  atomically with the reordered body and reports `bytes_written`.
- **INV-8** — handler `dry_run:true`: nothing is written; the response
  carries `dry_run:true` and the would-be `order_after`.
- **INV-9** — handler no-op: an already-canonical file is not rewritten
  (`changed:false`, contents unchanged).

## Tests

`tests/features/changelog_log_normalize/test_changelog_log_normalize.cpp`
(bundle: `test_claude`) — pure-helper INV-1..6 + behavioural INV-7..9 over a
`QTemporaryDir` project, driving `RemoteControl::cmdChangelogLog` directly.
