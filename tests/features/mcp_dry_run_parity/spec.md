# mcp_dry_run_parity — dry_run preview on mutating verbs (ANTS-2227, part 1)

## Problem

Only roadmap_log / changelog_log / spec_log expose `dry_run:true` (ANTS-2077 /
2136). Every other mutating verb writes immediately, with no "show me what this
would change" pre-flight. Part 1 adds a uniform `dry_run` to the verbs with
bounded write seams: **apply_edits** (the biggest blast-radius verb),
**project_settings** (init/set), **feedback_log** (append_finding /
append_tracking) and **audit_falsepos_log**.

## Surface

- `ants::falsepos::appendEntry(projectPath, entry, dryRun)` — the would-be
  append result without the O_APPEND write (the only one of the four that
  delegates the write to a module; the preview shares the write path's code).
- Per-handler `dry_run` gate in `src/remotecontrol.cpp` (cmdApplyEdits,
  cmdProjectSettings, cmdFeedbackLog, cmdAuditFalseposLog).
- A uniform `makeDryRunProp` schema-prop factory in
  `src/claudeintegration.cpp`, declared on all four descriptors.

## Invariants

- **INV-1 no write under dry_run** — `appendEntry(dir, e, /*dryRun=*/true)`
  returns `ok` with `created` + `bytesAppended` set but leaves
  `.ants_review_falsepos.jsonl` absent on disk.
- **INV-2 preview can't drift** — the `bytesAppended` a dry-run reports equals
  the bytes a subsequent real append writes (same record-building code path).
- **INV-3 validation still fires** — a dry-run with an invalid `review_kind`
  refuses with `bad_args` (the preview runs the full validation, it does not
  short-circuit to a fake success).
- **INV-4 handler gates** — each of the four handlers reads `dry_run` and
  diverges to a preview that returns `dry_run:true` *before* its write
  (apply_edits' would-write branch, project_settings' writeOut early-return,
  feedback_log's pre-QSaveFile preview, audit_falsepos_log passing the flag to
  appendEntry). Source-scrape — the handlers need a full RemoteControl to run.
- **INV-5 schema parity** — `makeDryRunProp` exists and is declared
  (`props["dry_run"] = makeDryRunProp()`) on all four descriptors.
