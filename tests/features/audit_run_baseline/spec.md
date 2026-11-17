# audit_run pinned baseline (ANTS-1706)

`audit_run` can pin a run's findings as a baseline, and every later run
reports which of its findings are new since that baseline. This is the
headless form of the audit dialog's "Save baseline" and "New only".

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
- **INV-8** — The MCP envelope copies `since_baseline` and
  `baseline_saved`, and the handler reads `save_baseline` (source-scrape:
  the handler needs a live `ClaudeIntegration`).

## Out of scope

The confidence floor (the other half of ANTS-1706), AI triage
(ANTS-6809), and the async `audit_poll` envelope, which carries no
per-finding blocks, as for ANTS-1870's `delta`.
