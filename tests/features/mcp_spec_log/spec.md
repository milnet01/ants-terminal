# spec_log — test contract

Behavioural conformance for the `spec_log` MCP verb (ANTS-1963). Drives
the pure `SpecLog` transforms directly + the `RemoteControl::cmdSpecLog`
handler (caller_cwd canonical root, m_main-independent) for routing,
refusals, atomicity.

Invariants exercised (docs/specs/ANTS-1963.md §3 / §6):

- T1 — set_status rewrites only the Status line; no Status line →
  unrecognised_format.
- T2 — append_loop appends a bullet at the end of an existing Cold-eyes
  loop log section, before any trailing `## ` section.
- T3 — append_inv appends INV-N at the end of the Invariants section with
  the *Test:* clause; existing INVs unchanged.
- T4 — append_inv with an existing inv_id → bad_args; malformed inv_id →
  bad_args.
- T5 — append_loop on a spec with no Cold-eyes loop log section creates
  the section at EOF.
- T6 — refusals: bad op → bad_mode; malformed id → bad_id; path "../x" →
  bad_path; neither id nor path → bad_id; absent spec file → not_found.
- T7 — atomicity: the setForceSpecWriteFailForTest seam leaves the
  original byte-identical and returns write_failed.
- T8 — fenced-region safety: a `## ` line inside a fence within a section
  does not prematurely end the section for append.
- T11 — (ANTS-2136) `dry_run:true` returns the resolved landing `line` and
  `bytes` (would-be size, replacing `bytes_written`) for all three ops
  (set_status / append_inv / append_loop) WITHOUT writing the spec; the
  file is byte-identical to its pre-call content. Parity with
  `roadmap_log` / `changelog_log` dry_run.
