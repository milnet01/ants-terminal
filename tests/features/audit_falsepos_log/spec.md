# Feature: `audit_falsepos_log` MCP write verb

Contract for the write side of the false-positive ledger
`.ants_review_falsepos.jsonl`. Design: [`docs/specs/ANTS-2129.md`](../../../docs/specs/ANTS-2129.md);
standard: [`docs/standards/audit-false-positives.md`](../../../docs/standards/audit-false-positives.md).

Driven via `RemoteControl::cmdAuditFalseposLog` (m_main-independent —
`RemoteControl(nullptr)`), with `caller_cwd` pointing at a `QTemporaryDir`.
Round-trips verified through `ants::falsepos::loadEntries`.

- **INV-1** — appending to a pre-populated ledger leaves every prior byte
  unchanged; the new record is a strict byte-suffix of the file.
- **INV-2** — each record is bracketed by a leading + trailing `\n`; appending
  to a file ending in a partial (un-terminated) line still parses the new
  record cleanly via `loadEntries` (orphan skipped).
- **INV-3** — an embedded newline in `claim`/`rationale`/`lane` is JSON-escaped
  (record stays one physical line); `loadEntries` round-trips it.
- **INV-4** — absent/empty `claim` or `rationale` → `bad_args`, file untouched.
- **INV-5** — absent `timestamp` defaults to today; a malformed present
  `timestamp` → `bad_args`.
- **INV-6** — a `rationale` over the cap is trimmed to ≤ 1024 sliced units +
  trailing `…` (assert `≤`, never `==`).
- **INV-7** — a worst-case multibyte record ≥ 3.5 KiB after trim → `bad_args`,
  file untouched; an all-ASCII control record succeeds.
- **INV-8** — appending into a dir with no ledger creates it (`created:true`,
  mode 0644); `loadEntries` returns the one written entry.
- **INV-9** — a non-regular file at the ledger path (directory; symlink) →
  `write_failed`, nothing written (symlink target untouched).
- **INV-10** — non-canonical / absent / empty `review_kind` → `bad_args`;
  a canonical value succeeds.
- **INV-11** — success envelope `{ok, path, bytes_appended, created,
  timestamp, review_kind}`; `bytes_appended` equals the written record length
  (single-writer temp dir: also `size_after - size_before`).
- **INV-12** — round-trip: written record re-parses to a `LedgerEntry` whose
  fields match what was written (control-char-free tags).
- **INV-13** (ANTS-4105) — the success envelope names its own consumer and
  routes a mis-aimed call: `consumed_by` says the AI-review briefs read this
  ledger, and `hint` says it does NOT suppress `audit_run` findings — for a
  static-analysis TOOL finding, `audit_dismiss` writes the fingerprint ledger
  that `suppressions:"auto"` filters on. The verb description and
  `selection_hint` carry the same routing. The two ledgers are deliberately
  NOT merged: their keys differ (a prose claim vs a file+rule+message hash),
  so matching one against the other would be fuzzy by construction. A session
  logged six `audit_run` findings here, re-ran the sweep and watched all six
  return; both verbs were behaving as designed and nothing on the wire said
  which was which, while `/audit` step 10.5 names this one as the route.
- **no_project** — an absent / non-directory `caller_cwd` → `no_project`.
