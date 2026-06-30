# mcp_dry_run_parity — dry_run preview on mutating verbs (ANTS-2227)

## Problem

Only roadmap_log / changelog_log / spec_log expose `dry_run:true` (ANTS-2077 /
2136). Every other mutating verb writes immediately, with no "show me what this
would change" pre-flight. This feature adds a uniform `dry_run`:

- **Part 1** — verbs with bounded write seams: **apply_edits** (the biggest
  blast-radius verb), **project_settings** (init/set), **feedback_log**
  (append_finding / append_tracking) and **audit_falsepos_log**.
- **Part 2** — the ID-allocating ROADMAP fold-in family: **indie_review_fold_in**,
  **cold_eyes_fold_in** and **debt_sweep_defer**. These bump `.roadmap-counter`
  (allocateIds) AND insert into ROADMAP.md (insertBlock); dry_run must skip
  BOTH while still previewing the would-be IDs + rendered block.

Remaining (ANTS-2227 tail): **test_audit_fold_in** (struct-based, inline
provider lambda) and **debt_sweep_apply_fix** (shell-exec — needs the fix
script's own dry-run mode).

## Surface

- `ants::falsepos::appendEntry(projectPath, entry, dryRun)` — the would-be
  append result without the O_APPEND write (the preview shares the write code).
- `RoadmapFoldIn::peekIds(projectPath, n)` — the IDs allocateIds WOULD return,
  WITHOUT bumping `.roadmap-counter` (via inspectCounter; empty on any counter
  error, mirroring allocateIds' refusal). The fold-in dry_run primitive.
- Per-handler `dry_run` gate in `src/remotecontrol.cpp` (cmdApplyEdits,
  cmdProjectSettings, cmdFeedbackLog, cmdAuditFalseposLog, cmdIndieReviewFoldIn,
  cmdColdEyesFoldIn, cmdDebtSweepDefer).
- A uniform `makeDryRunProp` schema-prop factory in
  `src/claudeintegration.cpp`, declared on all seven new descriptors.

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
  (`props["dry_run"] = makeDryRunProp()`) on all seven new descriptors.
- **INV-6 peekIds (part 2)** — `peekIds(root, N)` returns the same IDs
  `allocateIds(root, N)` would (`current+1 … current+N`) but leaves
  `.roadmap-counter` unbumped; a fresh/absent counter peeks as `1…N` and is
  never created. The fold-in handlers route `dryRun ? peekIds : allocateIds`
  and skip `insertBlock` under dry_run (source-scrape).
