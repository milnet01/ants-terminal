# feedback_log op:"assign_id" — feature conformance

Contract for the v2 inline triage write. Full design: `docs/specs/ANTS-3447.md`.
This test locks the invariants.

## What assign_id does

Fill ONE `### ` finding's `**Proposed ID:**` line in a `*_Ants_MCP_Feedback.md`
file:

1. **Locate** the finding by its verbatim `### ` heading (+ optional
   `heading_line` to disambiguate a repeat), over the fence-aware
   `enumerateFindingBlocks` — 0 matches → `target_not_found`, > 1 →
   `target_ambiguous` (+ `candidates[]`).
2. **Compose** the id-line value from EITHER `ids` (an `ANTS-NNNN` array →
   comma-joined, de-duplicated) OR `closure` (a reason → `n/a — <reason>`, or
   bare `n/a` when empty) — `hasIds XOR hasClosure`.
3. **Write** the canonical line `- **Proposed ID:** <value>`: replace the
   finding's first `**Proposed ID:**` line (placeholder or existing id) when
   present, or insert it as the first body bullet when absent.

Nothing else changes — no other finding, no tracking table, no version marker.

## Invariants under test (⇢ docs/specs/ANTS-3447.md)

- **INV-2** — exactly one finding's id line is replaced (or inserted); every
  other line is byte-identical. The envelope `line` is that finding's 1-based
  `### ` heading line in the ORIGINAL file.
- **INV-3** — the written line is exactly `- **Proposed ID:** <value>`; the
  enumerator reports the block's `idValue` = `<value>` after the write.
- **INV-4** — resolution: 0 matches → `target_not_found`; > 1 (bare heading) →
  `target_ambiguous` + `candidates[]`; `heading_line` disambiguates.
- **INV-5** — request validation: `hasIds XOR hasClosure`; ids match
  `^ANTS-[0-9]+$`; a closure begins `n/a`; a newline in `closure` is folded to a
  space (single-line INV-3).
- **INV-6** — idempotent: re-running with the same value is a byte-identical
  no-op (`changed:false`, `bytes_delta:0`).
- **INV-7** — `dry_run:true` leaves the file byte-for-byte unchanged.
- **INV-8** — a `changed:false` no-op skips the write (mtime untouched).
- **INV-9** — refusals: `bad_args` (predicate / bad ids / empty heading),
  `target_not_found` / `target_ambiguous`, `not_found` (absent file); no
  `not_v2`.
- **INV-10** — fenced `### `/`**Proposed ID:**` lines are inert.
- **INV-11** — the version marker is byte-identical after an assign (no bump,
  no read).
