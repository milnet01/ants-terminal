# feedback_log op:"compact_resolved" — feature-conformance contract

Full design: [`docs/specs/ANTS-3443.md`](../../../docs/specs/ANTS-3443.md).
Format standard: `docs/standards/mcp-feedback-files.md` (§ "Maintainer
compaction (v2 — compact_resolved)").

`compact_resolved` collapses each **shipped** v2 finding's write-up to a
`→ shipped ✅ (write-up compacted, ANTS-3443)` stub that keeps the `### `
heading and the finding's first `**Proposed ID:**` line verbatim. Gating is
per-finding, on the injected roadmap sets (pure helper) or the live
`ROADMAP.md` (wrapper). This test drives the pure
`FeedbackFile::compactResolved` over synthetic v2 content plus a live
`cmdFeedbackLog` drive, and source-greps the schema/dispatch wiring.

## What this test asserts (spec §§ 2.5–2.9, INV-2…INV-13)

- **Mixed block (§2.10)** — one collapse (all-✅), three distinct skips:
  `has_open_id` (+`open_ids`), `roadmap_unresolved_ids` (+`unresolved_ids`),
  `no_shippable_id` for an `n/a` closure that *names* a ✅ id. The id-less
  "Positive note" is **not** a finding — absent from both `collapsed[]` and
  `skipped[]` (INV-8/12).
- **Stub bytes (§2.7/INV-3)** — the collapsed body is exactly `heading →
  blank → the retained **Proposed ID:** line → the breadcrumb → blank`.
- **Proposed-ID lift (INV-3)** — a finding whose id line sits *below* its
  What/Repro bullets: the first such line is relocated to directly under the
  heading, verbatim; a second id line is dropped with the body.
- **n/a closure-wins (INV-2/7)** — `n/a — folded into ANTS-1525` with
  `ANTS-1525` ✅ → `no_shippable_id`, never collapsed; `ids` still carries the
  incidental `ANTS-1525`.
- **multi-id partial (INV-2)** — `ANTS-1525, ANTS-1579` with only one ✅ →
  `has_open_id`, not collapsed.
- **duplicate ids (ANTS-3739)** — an id named twice on one `**Proposed ID:**`
  line reports once in `ids`, first occurrence wins, matching `assign_id`'s
  documented de-duplication.
- **archive rotation (§2.5 gate 3)** — an id absent from `roadmapIds` →
  `roadmap_unresolved_ids`, not collapsed (checked before the ✅ gate).
- **idempotency (INV-6)** — feeding the collapsed output back collapses
  nothing; every stub trips `already_compacted` (a `→ shipped` body line,
  checked before the roadmap gates) — even when its id is reopened ✅→🚧.
- **delta invariance (INV-5)** — a collapsed finding retains its shippable id,
  so `enumerateFindingBlocks` still reports its id line after the collapse.
- **fence safety (INV-8)** — a `### ` heading and a `→ shipped` line pasted
  inside a ``` fence are ignored (not enumerated / do not trip
  `already_compacted`).
- **signed bytes_saved (INV-13)** — a present v2 file with zero collapsible
  findings → `findings_collapsed:0, bytes_saved:0`, per-finding `skipped[]`.
- **wrapper (INV-9/11)** — `dry_run` leaves the file byte-identical;
  `not_v2` on a `: 1` file; `not_found` on an absent file;
  `roadmap_unavailable` when no `ROADMAP.md` resolves; a real write emits the
  stub + a live-roadmap-driven envelope.
- **schema/dispatch** — the `feedback_log` inputSchema enumerates the op;
  `cmdFeedbackLog` routes it to `FeedbackFile::compactResolved`.

Exit non-zero on any failed assertion (gtest).
