# feedback_log op:"compact_shipped" (ANTS-3421)

Test contract for the maintainer compaction op. Full design +
invariants: `docs/specs/ANTS-3421.md`.

Drives `RemoteControl::cmdFeedbackLog` live (op:`compact_shipped`) over an
in-`QTemporaryDir` `*_Ants_MCP_Feedback.md` fixture that carries: an H1
title/banner, single-finding triaged contributor blocks above the
watermark (one terse), a multi-`###` block, an already-compacted stub, a
duplicated heading, a maintainer tracking block (✅/📋 rows + a reopened
`✅`-then-`📋` id), and an un-triaged finding below the watermark.

- **INV-1** applied target's body → the exact `→ shipped <id>, confirmed
  <session> <date> (write-up compacted, ANTS-3421)` stub, heading verbatim.
- **INV-2** an id that is not ✅ in an effective tracking row → `not_shipped`.
- **INV-3** a heading below the watermark → `in_delta`.
- **INV-4** a maintainer tracking block → `maintainer_block`.
- **INV-5** 0 heading matches → `target_not_found`; >1 → `target_ambiguous`
  (+candidates); `heading_line` disambiguates; a non-boundary `heading_line`
  → `target_not_found`.
- **INV-6** `parse()` `mappedIds` / `delta` / `trackingRows` byte-identical
  before vs after a collapse.
- **INV-7** idempotent — a stub body → `already_compacted`; re-run is a no-op.
- **INV-8** atomic + partial — mixed batch applies the valid target, skips
  the rest; `dry_run` leaves disk unchanged.
- **INV-9** `bytes_saved` = signed Σ(before − after); a terse block ≤ 0 is
  still applied.
- **INV-10** bottom-up — two targets both collapse without corrupting the
  lower one's range.
- **INV-11** the H1 title/banner → `title_block`.
- **INV-12** gate order — below-watermark + un-shipped id → `in_delta`
  (gate 4), not `not_shipped` (gate 5).
- **INV-13** two targets on the same block → both `duplicate_target`.
- **INV-14** a block with ≥2 `###` findings → `multi_finding`.
- **INV-15** effective-status supersession — a reopened `✅`-then-`📋` id →
  `not_shipped`.
- Request-shape: empty `targets`, a target missing `id`/`heading`, or a
  bad id → `bad_args`.

Pre-fix, `op:"compact_shipped"` is not in the op enum, so `cmdFeedbackLog`
returns `bad_mode` and every collapse assertion fails.
