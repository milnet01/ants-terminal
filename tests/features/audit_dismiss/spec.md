# Feature spec: ANTS-1713 — `audit_dismiss` MCP verb

ANTS-1708 shipped the fingerprint-keyed learned-false-positive ledger
(`<root>/.audit_cache/learned-fp.jsonl`) and the Audit dialog's recording
path. ANTS-1820 then taught the headless `audit_run` engine to read it. What
was missing was the write side for a Claude Code session: a session that had
just reasoned a finding to be a false positive had no way to say so, so only
the GUI could teach the ledger.

`audit_dismiss` closes that. One dismissal suppresses the finding on every
later sweep — GUI and headless alike — and survives edits that shift its line,
because the fingerprint is line-independent.

Distinct from `audit_falsepos_log` (ANTS-2129), which writes the **prose**
ledger `.ants_review_falsepos.jsonl` that the review *skills* read. This
writes the **fingerprint** ledger the audit *engine* filters on.

## Invariants under test

- **INV-1 / `file` + `message` is the preferred input.** The server computes
  the fingerprint via `ants::auditfp::computeFingerprint(file, rule, message)`
  — the same hash the engine looks up — and reports `computed:true`. The
  entry lands in `<root>/.audit_cache/learned-fp.jsonl`.
- **INV-2 / an explicit `fingerprint` is accepted.** 16 lowercase hex chars,
  reported with `computed:false`. A malformed value refuses `bad_args` rather
  than writing an entry nothing will ever match.
- **INV-3 / required inputs are enforced.** A missing/blank `rule`, or
  neither `fingerprint` nor `file`+`message`, refuses `bad_args`. An
  unresolvable `caller_cwd` refuses `no_project`.
- **INV-4 / `dry_run` writes nothing.** It validates, computes the
  fingerprint, and reports the would-be record; the ledger file is not
  created.
- **INV-5 / the dismissal actually suppresses.** After `audit_dismiss`, the
  headless parse path (`parseWithSuppression` fed from
  `ants::auditfp::loadEntries` + `fingerprintSet` — exactly what `runAudit`
  does) drops that finding from `afterFilterCount` while `rawCount` keeps the
  tool's true raw total. This is the end-to-end proof, not just that a line
  was appended.
- **INV-6 / re-dismissing is a no-op, not an error.** Recording the same
  fingerprint twice returns `ok` and does not duplicate the entry
  (`appendEntry`'s existing dedup).
