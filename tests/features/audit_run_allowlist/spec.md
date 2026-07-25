# Feature spec: ANTS-3615 — headless `.audit_allowlist.json` + `suppressions`

Two related gaps in the headless `audit_run` engine, both cases where the
ANTS-1351 v1 contract documented a suppression surface `runAudit` did not
implement:

1. `req.suppressionsMode` (parsed from the `suppressions` request field) was
   never read anywhere in `runAudit`. A caller passing `"none"` or
   `"path:<file>"` saw no effect **and no refusal** — a silent no-op, which
   is worse than an unadvertised gap.
2. `.audit_allowlist.json` custom-regex filtering existed only on
   `AuditDialog` (`loadAllowlist` / `allowlisted`, both GUI-side), so an
   allowlist entry that hid a finding in the Audit dialog did nothing at all
   under MCP or CI.

The fix wires both, and shares one implementation between the two paths
rather than duplicating the matcher.

## Invariants under test

- **INV-1 / Allowlist suppresses in the headless path.** A
  `.audit_allowlist.json` entry whose `rule` equals the finding's check id,
  whose `path_glob` matches its file, and whose `line_regex` matches its
  message drops that finding from `afterFilterCount` (and from samples and
  the SARIF finding set). `rawCount` is unchanged — it keeps the tool's true
  raw total, matching the learned-FP ledger's existing semantics
  (ANTS-1820), so a suppressed finding still reads as noise in
  `noise_rate_pct`'s denominator.
- **INV-2 / Rule identity is exact.** An entry whose `rule` does not equal
  the finding's check id never suppresses, even when the path and message
  patterns both match. For the line-based tools (cppcheck / clazy /
  clang-tidy / mypy) the check id IS the tool name, matching the GUI's
  `Finding::checkId`; the JSON tools (ruff / bandit / semgrep / gitleaks /
  trivy / **shellcheck** — see `isJsonFindingTool`) key on their own
  `check_id`, a different namespace, so an allowlist entry for one of those
  must name that tool's check id rather than the tool.
- **INV-3 / Hostile entries are dropped, not fatal.** An entry whose
  `line_regex` trips `isCatastrophicRegex`, or fails to compile after
  `hardenUserRegex`, is skipped with a warning and the remaining entries
  still load. A missing or malformed file yields an empty allowlist, never
  an error — an allowlist is an optional noise filter, not a contract.
- **INV-4 / `suppressions` is honoured or refused, never ignored.**
  `"auto"` (or absent) applies both headless suppression sources — the
  learned-FP ledger and the allowlist. `"none"` applies neither.
  `"path:<file>"` reads the allowlist from the named file instead. Any other
  value refuses `bad_args`. The check runs after the cheap argument
  validation and before any tool is resolved or spawned.
- **INV-5 / `path:` cannot escape the project root.** A named suppression
  file that does not canonicalise to a path under the project root refuses
  `bad_path`. Without this the field is an arbitrary-file-read oracle: a
  malformed-JSON warning would confirm the file's existence.
- **INV-6 / One implementation, two callers.** The loader, the matcher and
  the glob compiler live in `AuditEngine` (Qt6::Core-only, so the runner can
  link them); `AuditDialog::loadAllowlist` / `allowlisted` / `globToRegex`
  delegate. `AuditDialog::AllowlistEntry` is an alias of
  `AuditEngine::AllowlistEntry`.

- **INV-7 / `formats` is honoured or refused, never ignored (ANTS-3626).**
  The same silent-no-op class as INV-4, found by the ANTS-1351 cold-eyes
  pass. `formats` was only ever probed via `contains("sarif")` /
  `contains("html")`, so an unrecognised entry was simultaneously
  non-empty — suppressing the `["sarif"]` default — and matched by neither
  branch: no artifact written, `sarif_path` / `cache_path` absent, and
  still `ok:true`. It is the costlier half of the pair, because the
  discarded work is a whole sweep rather than a filter step. Each entry
  must be `"sarif"` or `"html"`, checked alongside the other cheap request
  fields and before the concurrency slot; anything else refuses `bad_args`
  naming the offending value and the accepted set. An **empty** array stays
  valid and keeps the `["sarif"]` default.

- **INV-8 / The document-wide SARIF ceiling reports its shed
  (ANTS-3629).** `writeSarif` bounds the whole document at
  `kSarifFindingsMax` (10 000) results across *every* tool, but only the
  **per-tool** caps set `findings_truncated`. So N tools each
  comfortably under the per-tool cap could sum past the document
  ceiling: results were dropped from the artifact while the envelope
  reported `findings_truncated:false`. That is silent loss in the file a
  reviewer treats as the complete finding set, and unlike INV-4/INV-7 it
  is not a refusal case — the run *succeeds*, just incompletely.
  `writeSarif` now counts what it sheds and reports it through a
  `docTruncated` out-param, which `runAudit` ORs into
  `r.findingsTruncated`. INV-8's fixture is deliberately 4 tools ×
  3 000 findings — a shape where **no** per-tool cap fires, because that
  is precisely the case that escaped. INV-8b pins the other direction so
  the flag cannot degrade into always-true.

  Scope note: the sidecar baseline's own `findings_truncated`
  (`mergedTruncated`) already carries a `> kSarifFindingsMax` size check
  on the merged record and is left alone — it answers "is this baseline
  incomplete", which is a different question from "did the emitted SARIF
  shed results".

## Out of scope

- `.audit_suppress` stays GUI-only. It is keyed by the line-grain
  `Finding::dedupKey` the headless runner never materialises, and the
  drift-resilient learned-FP ledger supersedes it for this path.
- Baseline-diff and the AI-triage stage (ANTS-1706) are a separate item.
