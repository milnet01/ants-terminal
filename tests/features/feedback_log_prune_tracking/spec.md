# Feature: `feedback_log op:prune_tracking`

**ANTS-3442.** A maintainer op that removes superseded duplicate maintainer
tracking-table rows from a `*_Ants_MCP_Feedback.md` file, keeping each id's
authoritative last-per-id row plus every heading / header / separator. Full
contract + cold-eyes log: `docs/specs/ANTS-3442.md`.

## Scope

- `FeedbackFile::pruneTracking(content, opts)` — pure two-stage selection +
  bottom-up rewrite (ants_core_lib; no MainWindow).
- `TrackingRow::line` — additive 1-based source line populated by
  `FeedbackFile::parse()`.
- `RemoteControl::cmdFeedbackLog` op-switch routes `op:"prune_tracking"`
  (parity with `compact_shipped`): absent-file `not_found`, empty
  `scope_ids` → `bad_args`, atomic `QSaveFile`, `dry_run` preview.
- `feedback_log` inputSchema declares `prune_tracking` in the op enum +
  the `scope_ids` property.

## Invariants tested (map to docs/specs/ANTS-3442.md §3)

- **INV-2/7** — a row all of whose ids have a later duplicate row is removed;
  the fixture's `First look`/`Started` (ANTS-0001) and `Look7` (ANTS-0007) go,
  their last rows (`Shipped`/`Ship7`) stay.
- **INV-3** — a multi-id row survives if it is last-of-any-id: `Batch A + B`
  (ANTS-0002, ANTS-0003) stays (last of ANTS-0002).
- **INV-4** — an `n/a` row is never removed (`Prose note` stays).
- **INV-2b** — two Stage-1-marked rows (`R1`/`R2`) whose only citation of
  `ANTS-0009` is their notes cell are both PINNED (kept), so `ANTS-0009` is not
  dropped from `mappedIds`.
- **INV-5** — every maintainer heading + header + separator survives.
- **INV-6** — `scope_ids:["ANTS-0001"]` removes only the ANTS-0001 rows, leaving
  `Look7` (ANTS-0007) in place.
- **INV-9** — idempotent: a second prune removes nothing.
- **INV-12** — an explicitly empty `scope_ids:[]` → `bad_args`.
- **INV-13** — prune on an absent file → `not_found`.
- Envelope + `dry_run` (no write) + schema/dispatch source-grep.

## Method

`QTemporaryDir` holds a synthetic `*_Ants_MCP_Feedback.md` fixture whose
`ANTS-0001` spans three maintainer tables (📋→🚧→✅). Pure-helper tests call
`FeedbackFile::pruneTracking` directly; live tests drive
`RemoteControl::cmdFeedbackLog`. Source-grep tests lock the schema + dispatch.
