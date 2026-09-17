# Feature: audit dialog detection walk, labels and exports

## Invariants

**INV-1 — project detection lists a bounded number of levels.** The
`hasAnyFile` detection lambda in the `AuditDialog` constructor does not use a
recursive `QDirIterator`; it lists directories level by level up to its depth.

**INV-2 — the signal banner prints one percent sign.** No `%%` sits in a
`QString::arg` template for the actionable percentage.

**INV-3 — the HTML report escapes every `<` in its JSON payload.**
`exportHtml` replaces `<` with `\u003c`, which covers `</script>` and `<!--`.

**INV-4 — a tool warning is not labelled a timeout.** A result with
`warning` set is labelled ` (tool issue)`, since a failed start sets it too.

**INV-5 — a failed export open says so.** The SARIF and HTML export buttons
each report `save failed` when the file cannot be opened, as well as when
the commit fails.

**INV-6 — the recent-files filter matches at a path separator.**
`handleCheckOutput` matches a finding's file to a changed file with
`pathSuffixMatches`, the same predicate `visibleSinceBaseline` uses
(`tests/features/audit_dialog_v2` INV-11 checks its behaviour).

**INV-7 — no process outlives its timeout on the stack (ANTS-5083).** The
AuditDialog sources declare no `QProcess` on the stack. The filesystem-type
probe and the git runner hand their process to `releaseProcess`, which
deletes a finished process and leaves a running one to delete itself on
`finished`.

**INV-8 — Semgrep sends no usage metrics (ANTS-5083).** The dialog's
`semgrep` catalogue command passes `--metrics=off`, as the headless runner
does. Under Semgrep's default, a `--config` that pulls from the registry sends
metrics.

**INV-9 — a suppression save is locked (ANTS-5083).** `saveSuppression` takes
a `ConfigWriteLock` on the suppression file before it reads it, as
`appendSnapshot` does for the trend file.

## Rationale

The ANTS-5084 performance pass found each of these. The detection walk
descended every build tree before its depth cap applied. The banner printed
`%%`. A `<!--` in a finding could blank the report. A failed tool start read
as a timeout. A failed export open showed nothing. A changed `oo.cpp` kept
findings in `src/foo.cpp`.

## Test surface

`test_audit_dialog_lows.cpp` reads `src/auditdialog.cpp` (located from the
test's own path) and checks the named regions.

## Regression history

- **ANTS-5084:** the six defects above. Locked by this spec.
