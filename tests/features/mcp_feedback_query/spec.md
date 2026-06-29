# feedback_query — test contract

Behavioural conformance for the `feedback_query` MCP verb (ANTS-1961).
Drives the pure `FeedbackFile::parse` directly for the delta/mapped-id
contract, plus the `RemoteControl::cmdFeedbackQuery` handler (with an
absolute `path`, m_main-independent) for envelope + refusal coverage.

Invariants exercised (see docs/specs/ANTS-1961.md §3 / §6):

- T1 — multi maintainer block, mixed heading forms: delta starts at the
  first contributor heading after the LAST maintainer block; envelope
  carries all INV-8 fields.
- T2 — a fenced `## Active` below the last maintainer block is not a
  boundary; the delta is not split.
- T3 — zero maintainer blocks: delta = everything after the H1 title;
  `mapped_ids` empty.
- T4 — fully-triaged (no contributor heading after the last maintainer
  block): empty delta, `delta_present:false`, `mapped_ids` populated.
- T5 — an ANTS-NNNN cited in contributor prose below the watermark is NOT
  in `mapped_ids`; one inside a maintainer table IS.
- T6 — `###`/deeper headings inside blocks are inert.
- T7 — refusals: missing path → `bad_args`; non-feedback basename →
  `not_feedback_file`; non-existent feedback file → `not_found`.
- ANTS-3366 — a `not_found` envelope lists sibling
  `*_Ants_MCP_Feedback.md` files in the same dir under `candidates` (+ a
  `hint`); a dir with no siblings yields no `candidates` key.
- T8 — byte cap: a delta larger than max_bytes returns the head,
  `truncated:true`, full `delta_line_count`.

Out of scope: the ETag 304 short-circuit (injected centrally at the
dispatch site, not in the handler) and the live-corpus smoke check (T9 in
the spec — environment-dependent, skipped here).
