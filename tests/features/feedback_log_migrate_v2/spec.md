# feedback_log op:"migrate_v2" — feature conformance

Contract for the one-shot mechanical v1→v2 feedback-file migration.
Full design: `docs/specs/ANTS-3446.md`. This test locks the invariants.

## What migrate_v2 does

Two mechanical passes over a `*_Ants_MCP_Feedback.md` file, **leaving the v1
tracking tables in place**:

1. **Marker bump** — replace the first
   `<!-- ants-mcp-feedback: N -->` line (digit optional/malformed → repaired)
   with the canonical `<!-- ants-mcp-feedback: 2 -->`, or insert one as line 1
   when absent.
2. **Stamp un-triaged findings** — insert
   `- **Proposed ID:** _(maintainer to assign)_` as the first body bullet of
   every `### ` block that is **finding-shaped** (§2.4), **below the v1
   watermark**, and has **no** existing `**Proposed ID:**` line.

Nothing else changes: no tracking table is moved, collapsed, or deleted, so the
v1 watermark — and thus the shipped un-triaged delta — is preserved.

**Optional (ANTS-3474) — `backfill_from_tracking:true`.** Real files' findings
already carry a **blank** `- **Proposed ID:** _(maintainer to assign)_` line
(the `append_finding` default), so the stamp pass above rarely fires; their
already-assigned `ANTS-NNNN`s live only in the v1 tracking tables. With this
flag, a third pass replaces each **blank** Proposed-ID line **in place** with a
confident tracking-table id: the finding heading is token-matched against every
tracking row's `item` (overlap-coefficient grouped **per id**), and a single id
that clears a high threshold with a clear margin over the runner-up is stamped
inline; anything ambiguous / folded / low-overlap stays blank (**precision over
recall — never a wrong id**; misses fall to manual `assign_id`). The pass is
position-agnostic (the *match*, not the watermark, gates it) and runs before the
stamp inserts so the original id-line indices stay valid. `backfilled[]` reports
each `{heading, line, id, confidence_pct}`; `dry_run` previews them for review.
Default (flag absent/false) is the mechanical blank-stamp migrate — byte-for-byte
the pre-ANTS-3474 behaviour.

## Invariants under test (⇢ docs/specs/ANTS-3446.md)

- **INV-2** — when migration runs, the output marker is exactly
  `<!-- ants-mcp-feedback: 2 -->` (bumped in place or inserted).
- **INV-3** — a stamp is added iff a `### ` block is finding-shaped, below the
  watermark, and line-less. Prose, already-tagged, and above-watermark
  finding-shaped blocks are never stamped; the last is an `orphan`, a
  below-watermark not-finding-shaped block is `unclassified`.
- **INV-4** — no v1 tracking table is moved/collapsed/deleted;
  `lastMaintainerLine` is unchanged and the v1 delta is preserved modulo the
  inserted stamp lines.
- **INV-5** — every below-watermark line-less `### ` block is either stamped or
  reported in `unclassified[]` (no silent drop).
- **INV-6** — idempotent: a second run on a `: 2` file is a byte-identical no-op
  (`already_v2:true`, `stamped_count:0`).
- **INV-7** — `dry_run:true` leaves the file byte-for-byte unchanged.
- **INV-9** — wrapper refusals: `bad_mode`, `not_found`; a real write yields a
  `: 2` file. No `not_v2` (already-v2 is a success no-op).
- **INV-10** — fenced `### `/table/`**Proposed ID:**` lines are inert.
- **INV-11** — `bytes_delta` is the signed size change. The default
  (blank-stamp) migrate only *adds* stamp lines, so `bytes_delta ≥ 0` and the
  op never shrinks content — the shrink is `compact_resolved`'s job.
  **Exception (ANTS-3474):** under `backfill_from_tracking:true`, replacing a
  `_(maintainer to assign)_` placeholder with a shorter id makes `bytes_delta`
  go negative (INV-12); backfill is the one migrate path that can shrink.
- **INV-12 (ANTS-3474)** — with `backfill_from_tracking:true`, a finding whose
  blank Proposed-ID line's heading confidently + uniquely matches a tracking
  row's `item` gets that row's id stamped inline (recorded in `backfilled[]`);
  an unrelated finding stays blank. Flag absent/false → both stay blank
  (byte-identical to the plain migrate). Never assigns a wrong id.

## Pass / fail

The compiled `test_feedback_log_migrate_v2` exits 0 iff every gtest case passes.
It drives the pure `FeedbackFile::migrateV2` over synthetic v1 fixtures and the
live `RemoteControl::cmdFeedbackLog` wrapper, plus schema/dispatch source-greps.
