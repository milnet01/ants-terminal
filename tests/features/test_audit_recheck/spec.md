# test_audit_recheck — Feature Spec (ANTS-1513)

## Purpose
Verify a deferred test-audit finding's `file:line` cite is still live
before resuming the work days later — file moves/renames/edits silently
stale the cite. Read-only.

## Invariants

- **INV-1** — given a ROADMAP bullet `[<finding_id>]` whose body cites
  `path:line`, `recheck` returns `found:true`, the parsed
  `cited_file`/`cited_line`, `file_exists:true` when the file is present,
  and `line_still_matches_pattern:true` (+ `matched_pattern_id` /
  `matched_dimension`) when the cited line still trips a pre-pass smell.
- **INV-2** — a cited line that no longer trips any pre-pass pattern
  returns `line_still_matches_pattern:false`.
- **INV-3** — when the cited file is absent, `file_exists:false` (and a
  best-effort git `drift_hint` may be offered; never fatal).
- **INV-4** — an unknown `finding_id` returns `{ok:true, found:false}`
  (not an error).
- **INV-5** — an empty `finding_id` refuses `code:"missing_field"`; an
  unresolvable `caller_cwd` refuses `code:"no_project"`.
- **INV-6** — path confinement: a cite that resolves outside the project
  root (absolute or `..`-traversal) is reported `file_exists:false` and
  never read.

## Test
`tests/features/test_audit_recheck/` (label `features;fast`), driving
`TestAuditEngine::recheck` against a seeded temp project. Verify it fails
against pre-ANTS-1513 source (the verb did not exist).
