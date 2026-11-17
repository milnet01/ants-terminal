# audit_run pinned baseline and confidence floor (ANTS-1706)

`audit_run` can pin a run's findings as a baseline, and every later run
reports which of its findings are new since that baseline. It can also
hide findings below a confidence floor. These are the headless forms of
the audit dialog's "Save baseline", "New only" and confidence filter.

The baseline is `<root>/.audit_cache/baseline-findings.json`, in the
ANTS-1870 findings-sidecar format (version 1) plus a `tools` list. It is
separate from the dialog's `baseline.json`: the dialog keys its baseline
on a line-sensitive `dedupKey` over its own checks, the runner on the
line-insensitive `fp` over external tools.

## Invariants under test

- **INV-1** — `save_baseline:true` writes the baseline owner-only (0600),
  holding this run's findings and the tools that finished `ok`. The
  result's `baseline_saved` gives `path`, `findings_count` and `tools`.
- **INV-2** — When a readable baseline exists, the result carries
  `since_baseline` with `new_count` and `new[]`: the findings whose `fp`
  the baseline lacks. A finding that only moved lines is not new. `new[]`
  drops `fp`; `new_count` is exact. It also carries `baseline_commit` and
  `baseline_saved_at`.
- **INV-3** — With no baseline file, the result has no `since_baseline`.
- **INV-4** — A tool that ran now but is not in the baseline's `tools` is
  named in `since_baseline.tools_not_in_baseline`, and its findings are
  not counted as new.
- **INV-5** — `save_baseline` with a narrowing `scope` (anything but
  `auto`, `full` or empty), `paths` or `checks` refuses `bad_args` and
  writes nothing: a narrowed run would pin a partial baseline.
- **INV-6** — A baseline that does not parse as version 1 gives
  `since_baseline:{unavailable_reason:"baseline_unreadable"}`; one marked
  `truncated` gives `"baseline_truncated"`.
- **INV-7** — `AuditDelta::newSinceBaseline` keeps exactly the current
  findings whose `fp` is absent from the baseline, in order.
- **INV-8** — The MCP envelope copies `since_baseline`,
  `baseline_saved` and `below_confidence_floor_count`, and the handler
  reads `save_baseline` and `min_confidence` (source-scrape:
  the handler needs a live `ClaudeIntegration`).
- **INV-9** — `internal::confidenceFor` scores a headless finding with
  `AuditEngine::computeConfidence`: the severity maps through
  `sarifLevelFor` (error → Critical, warning → Major, note → Minor), the
  tool is the source (`gitleaks` as `secrets`), a path under `tests/` or
  `test/` takes the test-path penalty even without a leading slash, and
  `corroborated` adds the cross-tool bonus.
- **INV-10** — With `min_confidence:N`, findings scored below N leave the
  tool's `after_filter_count`, its samples and `total_actionable`; each
  kept sample carries `confidence`; `below_confidence_floor_count` counts
  what was hidden.
- **INV-11** — `min_confidence` 0 (the default) changes nothing, and
  outside 0-100 refuses `bad_args`.
- **INV-12** — The floor filters `since_baseline.new`, but a baseline
  saved under a floor still holds every finding, so a later run with a
  lower floor does not report the hidden ones as new.

## Out of scope

AI triage (ANTS-6809). The async `audit_poll` envelope, which carries no
per-finding blocks, as for ANTS-1870's `delta`. The floor does not touch
the SARIF, the run's findings sidecar or the since-last-run `delta`: they
record the run, not the view of it.
