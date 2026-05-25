# Fingerprint-keyed learned-FP ledger (ANTS-1708)

The line-grain `.audit_suppress` (keyed by `Finding::dedupKey`, which hashes
`file:line:checkId:title`) loses its suppression the moment code above a finding
shifts its line number. ANTS-1708 adds a parallel, **drift-resilient** ledger
keyed by a line-independent content fingerprint so a learned false positive
stays hidden across edits.

## Surface

- `ants::auditfp::computeFingerprint(file, checkId, message)` — 16-hex,
  line-independent. Strips a leading `<path>:<line>[:<col>]:` prefix from the
  message before hashing.
- `ants::auditfp::loadEntries(projectPath)` — reads
  `<root>/.audit_cache/learned-fp.jsonl` (JSONL).
- `ants::auditfp::fingerprintSet(entries)`.
- `ants::auditfp::appendEntry(projectPath, entry)` — atomic-ish append, 0600,
  mkpath, dedup.
- `AuditEngine::applyLearnedFpSuppressions(findings, fpSet)` — marks matching
  `Finding` objects `suppressed = true` (does not drop — surfaces in SARIF
  `suppressions[]`, mirroring `.audit_suppress`). Consumed by the GUI dialog,
  which produces `Finding` lists.
- The headless `audit_run` runner (`AuditRunner`) does not materialise `Finding`
  objects (v1 heuristic counter), so it consumes the **same ledger** via the
  shared `computeFingerprint` directly: a learned FP recorded in the GUI is
  dropped from the runner's count + samples on every MCP/CI sweep (ANTS-1820).
  Cross-path matching holds for the line-based tools (cppcheck / clazy / mypy /
  shellcheck), whose check id is the tool name — equal to the GUI's
  `Finding::checkId`. JSON tools (semgrep / bandit / ruff / gitleaks / trivy)
  key on the tool's own `check_id`, a different namespace, so they don't
  cross-match (the same v1 limitation under which the runner does no other
  per-finding filtering).

## Invariants

- **INV-1** `computeFingerprint` is line-independent: two findings that differ
  only in their line number (in both the location prefix and nowhere else)
  produce the same fingerprint.
- **INV-2** `computeFingerprint` differs when `file`, `checkId`, or the
  descriptive (location-stripped) message differs.
- **INV-3** `appendEntry` then `loadEntries` round-trips an entry; its
  fingerprint is in `fingerprintSet`.
- **INV-4** `loadEntries` on a missing file returns empty; a malformed JSONL
  line is skipped without discarding valid neighbours.
- **INV-5** `appendEntry` dedups: a second append of the same fingerprint is a
  no-op that leaves exactly one data row on disk.
- **INV-6** `applyLearnedFpSuppressions` marks a finding whose fingerprint is in
  the set as `suppressed`, leaves non-matching findings untouched, and records a
  "learned false positive" note in `aiReasoning` when it was empty.
- **INV-7** (ANTS-1820) The headless runner drops learned FPs: given a non-empty
  ledger, a line-based tool's matching finding is excluded from
  `afterFilterCount` and from `samples`, while `rawCount` keeps the tool's raw
  total. With an empty ledger nothing is suppressed. Cross-path matching holds
  because `computeFingerprint` strips the location prefix, so the GUI's
  full-line `Finding::message` and the runner's already-stripped message hash to
  the same value.

## Out of scope (v1)

The `audit_dismiss` MCP verb (record a verdict from a Claude Code session) is
deferred to a follow-up — see ANTS-1713. Recording in v1 is via the GUI
suppress action, which writes both `.audit_suppress` and this ledger. This
mirrors the sibling `falseposledger`'s read-only-v1 / shell-append posture.
