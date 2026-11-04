# feedback_log — test contract

Behavioural conformance for the `feedback_log` MCP verb (ANTS-1962).
Drives the pure `FeedbackFile` renderers directly + the
`RemoteControl::cmdFeedbackLog` handler (absolute path,
m_main-independent) for op routing, refusals, file handling, atomicity.

Invariants exercised (docs/specs/ANTS-1962.md §3 / §6):

- T1 — append_finding renders the full template; omitting repro drops
  only the **Repro:** line; **Proposed ID:** always present + blank.
- T2–T4 — retired with `append_tracking` (ANTS-4513). That op, and
  `set_format_version`, now refuse `retired` and leave the file unchanged.
- T5 — refusals: bad op → bad_mode; no findings → bad_args; empty title →
  bad_args; a retired op → retired; bad date → bad_args; non-feedback path
  → not_feedback_file; assign_id on an absent file → not_found. After each
  path refusal, no store row exists for that path (ANTS-4513 INV-13). ANTS-3366: that not_found envelope lists
  sibling `*_Ants_MCP_Feedback.md` files in the dir under `candidates`
  (+ a `hint`).
- T6 — absent file + append_finding creates skeleton (marker + derived H1
  + pointer blockquote); created:true.
- T7 — append-at-end: a finding lands strictly below an existing
  maintainer block (verified via parse placing it in the delta).
- T8 — atomicity: the setForceFeedbackWriteFailForTest seam leaves the
  original byte-identical and returns write_failed.
- T9 (ANTS-3376) — `path` omitted: append_finding derives
  `<caller_cwd-leaf>_Ants_MCP_Feedback.md` at the shared root (parent of
  caller_cwd), creates it, and echoes `path_derived:true`; omitting both
  `path` and a resolvable caller_cwd still refuses `bad_args`.
- ANTS-3426 — when the caller leaf already ends in `_Ants` (a fork like
  `DOOM_Ants`), a `path`-omitted append_finding creates the de-doubled
  convention `DOOM_Ants_MCP_Feedback.md`, NOT the doubled
  `DOOM_Ants_Ants_MCP_Feedback.md` — so a naive append can't fork the
  feedback history into a wrong-named file.
