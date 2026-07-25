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

## Out of scope

- `.audit_suppress` stays GUI-only. It is keyed by the line-grain
  `Finding::dedupKey` the headless runner never materialises, and the
  drift-resilient learned-FP ledger supersedes it for this path.
- Baseline-diff and the AI-triage stage (ANTS-1706) are a separate item.
