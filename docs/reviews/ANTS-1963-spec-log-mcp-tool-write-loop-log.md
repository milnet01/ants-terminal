# ANTS-1963 — `spec_log` MCP tool (write Status / cold-eyes loop / INV entries into `docs/specs/*.md`) — review loop log

Record for [`../specs/ANTS-1963-spec-log-mcp-tool-write.md`](../specs/ANTS-1963-spec-log-mcp-tool-write.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-03)** — 2 cold lanes (code-grounding + standards
  consistency), 0 CRITICAL. Verified + fixed: the "neither `id` nor
  `path`" refusal corrected to `bad_id` (matches the reused `cmdSpecQuery`
  at `remotecontrol.cpp:8217-8220`, was `bad_args`); `bad_id` added to the
  `mcp-error-codes.md` taxonomy (documents existing `spec_query` usage);
  the three overloaded `not_found` uses split — file-absent stays
  `not_found`, missing Status line / Invariants section become
  `unrecognised_format`; section-locate pinned to the *first* `^## `
  heading whose text contains the keyword (substring, case-insensitive);
  the handler-vs-pure-layer refusal split spelled out (`EditResult.code`
  carries only structural codes); `line` defined for the create-section
  path; ANTS-1417 cite widened + the "default already Required" nuance
  added; T8 mapped into INV-8; never-renumber cite broadened to § 3.5 /
  § 5.5. Retained bare `write_failed` (taxonomy-canonical, matches the
  sibling feedback verbs) over the namespaced `roadmap_write_failed` form.
- **Loop 2 (2026-06-03)** — cold re-read, 2 lanes: **0 CRITICAL, 0 HIGH.**
  Polish folded: the `path` arg now routes through
  `PathValidation::validatePath` (ANTS-1295 mandate) instead of an inline
  guard, so the write verb also gets NFC/control-char hardening (a
  deliberate divergence from `spec_query`'s read-only inline check, now
  stated in § 2.1); §2.2 handler-side `bad_args` qualified "arg-shape" to
  distinguish it from the pure-layer duplicate-INV `bad_args`; §7 retensed
  (the `bad_id` taxonomy row already landed in loop 1); noted `phase_*`
  routing is inherited/untested here (no `docs/phases/` dir). Converged.
- **impl (2026-11-03)** — ANTS-6364 amendment, not a review loop; no reviewer was dispatched. INV-10, INV-12 and T16 now name `new_status` on both status ops. INV-12's "the append ops omit the key" was narrowed to append_loop and append_inv, since append_status (ANTS-4959) echoes it.
