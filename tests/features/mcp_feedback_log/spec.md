# feedback_log — test contract

Behavioural conformance for the `feedback_log` MCP verb (ANTS-1962).
Drives the pure `FeedbackFile` renderers directly + the
`RemoteControl::cmdFeedbackLog` handler (absolute path,
m_main-independent) for op routing, refusals, file handling, atomicity.

Invariants exercised (docs/specs/ANTS-1962.md §3 / §6):

- T1 — append_finding renders the full template; omitting repro drops
  only the **Repro:** line; **Proposed ID:** always present + blank.
- T2 — append_tracking renders the fully-qualified watermark; matches the
  ANTS-1961 anchor regex.
- T3 — round-trip: un-triaged delta → append_tracking with an ANTS id →
  delta empties + id in mapped_ids.
- T4 — empty ids → `n/a`; multi-id row joined; a 4th Notes column iff any
  row has notes.
- T5 — refusals: bad op → bad_mode; no findings → bad_args; empty title →
  bad_args; bad status → bad_status; non-ANTS id → bad_args; bad date →
  bad_args; non-feedback path → not_feedback_file; append_tracking on
  absent file → not_found. ANTS-3366: that not_found envelope lists
  sibling `*_Ants_MCP_Feedback.md` files in the dir under `candidates`
  (+ a `hint`).
- T6 — absent file + append_finding creates skeleton (marker + derived H1
  + pointer blockquote); created:true.
- T7 — append-at-end: a finding lands strictly below an existing
  maintainer block (verified via parse placing it in the delta).
- T8 — atomicity: the setForceFeedbackWriteFailForTest seam leaves the
  original byte-identical and returns write_failed.
