# changelog_log op:normalize — canonicalise [Unreleased] layout

**ID:** ANTS-3495, ANTS-3381
**Kind:** enhancement
**Source:** 3D_Engine/Vestige feedback 2026-07-10

## Problem

`changelog_log op:add` fires a non-blocking advisory when `## [Unreleased]`
is malformed, but offers no companion op to *fix* the layout — the caller
must hand-normalise. ANTS-3495 shipped the policy-free subset: reorder the
`### <category>` blocks into canonical Keep-a-Changelog order. It deferred
relocating stray interleaved prose, because "the prose's intended home" is
under-specified — the scanner can tell a line is wedged between blocks but
not whether it is a continuation, a stray paragraph or a mis-formatted
bullet.

ANTS-3381 closes that half against a **decided policy** (accepted by the
user 2026-08-14): fold a flagged line into the nearest preceding bullet as
a two-space continuation, always previewable via `dry_run`. The two
alternatives put to the user and declined were a read-only richer warning
and a per-line confirmation prompt.

The policy has one accepted failure mode: a paragraph that was meant to
stand alone gets absorbed by the entry above it. The `moves[]` preview is
the only thing that catches it, which is why it is reported unconditionally
rather than only under `dry_run`.

Because a flagged line always sits after its bullet with nothing but
blanks, continuations and HTML comments between them, the fold is an
**in-place re-indent** — the line count never changes, so no content moves
and the reorder that follows is unaffected.

## Surface

`ChangelogLog::normalizeUnreleased(markdown) -> NormalizeResult`
(`src/changeloglog.{h,cpp}`), driven by `op:"normalize"` in
`RemoteControl::cmdChangelogLog` (the remotecontrol TUs). Descriptor +
op-enum in `src/claudeintegration.cpp`.

Response: `{ok, op, file, changed, order_before[], order_after[],
prose_moved, moves[]?, bytes_written}` — `bytes` + `dry_run:true` under
preview; no write and `changed:false` when already canonical AND no prose
folded. `moves[]` is `[{from_line, under_line, text}]`, one entry per fold,
in file order, omitted when empty. A non-blocking `advisory` names any
interleaved prose the fold could not reach.

## Invariants

- **INV-1** — an out-of-canonical-order section is reordered: `order_after`
  is the canonical permutation, `changed` is true, and every block's bullets
  travel under their own heading (content non-destructive).
- **INV-2** — an already-canonical section with no foldable prose is a
  no-op: `changed` is false and the markdown is byte-identical to the input.
- **INV-3** — a preamble paragraph directly under `## [Unreleased]` (before
  the first `### ` heading) is preserved untouched.
- **INV-4** — a non-canonical `### ` heading sorts last, and a duplicate
  category keeps its original relative order (stable sort).
- **INV-5** — refusals: `not_unreleased` (no `## [Unreleased]`),
  `feature_grouped_section` (dated `### ` topics, not flat categories).
- **INV-6** — prose a fold cannot reach is left in place and surfaced via
  the `advisory` (`malformed_section`). A heading of any depth between the
  prose and the nearest bullet is a barrier: a `#### ` sub-heading does not
  end the category block, so the line is still flagged, but indenting it
  would not place it under that bullet, so the fold is declined.
- **INV-7** — handler write path: `op:"normalize"` rewrites CHANGELOG.md
  atomically with the reordered body and reports `bytes_written`.
- **INV-8** — handler `dry_run:true`: nothing is written; the response
  carries `dry_run:true` and the would-be `order_after`.
- **INV-9** — handler no-op: an already-canonical file is not rewritten
  (`changed:false`, contents unchanged).
- **INV-10** — (ANTS-3381) a stray flush-left line after a bullet is folded
  into that bullet as a two-space continuation, in place: the line count is
  unchanged, `prose_moves` carries its `from_line`/`under_line`/`text`, and
  the advisory clears. Prose alone sets `changed`, even when the category
  order was already canonical.
- **INV-11** — every stray line folds under its OWN nearest preceding
  bullet, reported in file order, and each fold travels with its block
  through the reorder in the same call. A bare `---` is a thematic break,
  not a bullet, so it folds like prose.
- **INV-12** — handler write path with prose: the fold is written and
  reported as `prose_moved` + `moves[]`; `bytes_written` is the two indent
  bytes per fold (a pure reorder still reports 0, per INV-7).
- **INV-13** — handler `dry_run:true` with prose: nothing is written and
  `moves[]` previews every fold. The preview is not write-only — it is the
  only guard against the policy's accepted failure mode.

## Tests

`tests/features/changelog_log_normalize/test_changelog_log_normalize.cpp`
(bundle: `test_claude`) — pure-helper INV-1..6 + INV-10..11, behavioural
INV-7..9 + INV-12..13 over a `QTemporaryDir` project, driving
`RemoteControl::cmdChangelogLog` directly.
