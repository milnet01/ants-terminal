# Help → Report an Issue (ANTS-6911)

User request 2026-11-24: let people report a problem on GitHub from inside the
app, as easily as possible, with a plain warning before any log is shared.

## Invariants

- **INV-1** — the Help menu has a `Report an &Issue...` action that opens the
  dialog with the debug log's path (`DebugLog::logFilePath()`).
- **INV-2** — the link opens `issues/new` on the project's repository, naming
  `bug_report.yml` for a bug and `feature_request.yml` for an idea.
- **INV-3** — the title and the system details reach GitHub unchanged:
  every reserved character, a `+` included, is percent-encoded, so none
  splits the query or turns into a space.
- **INV-4** — each form the link names exists under `.github/ISSUE_TEMPLATE/`
  and has a field with `id: environment`, the field the link fills in.
- **INV-5** — the log warning and the Open Log Folder button appear only when
  the debug log, or its rotated `.1` copy, exists. The link never carries the
  log or its path.

*Test:* `test_report_issue.cpp` builds links and decodes them, reads the two
form files, builds the dialog with and without a log file, and reads
`setupHelpMenu`'s source for INV-1.
