# Feature spec: ANTS-1638 + ANTS-1639 — Task List dialog right-click copy + malformed-markup flag

`ClaudeTaskListDialog` gains a right-click context menu on each
QListWidget row with three Copy actions, and prefixes "[malformed] "
to any row whose subject or description carries raw upstream
tool-input markup leaked from a malformed `TaskCreate` call.

## Invariants exercised

- **ANTS-1638 / structured task data stashed on every row.** After
  `rebuild()` runs against a tracker carrying N tasks, each
  `QListWidgetItem` MUST carry the task's `id`, `status`, raw
  `subject`, and full `description` (no truncation) in the four
  `UserRole+1..+4` slots. The context-menu slot reads from these
  slots, NOT from the truncated display text.

- **ANTS-1638 / context menu policy + signal wired.** The list
  widget MUST have `Qt::CustomContextMenu` policy set and the
  dialog MUST be connected to the
  `customContextMenuRequested(QPoint)` signal. Source-scrape
  verifies the wiring.

- **ANTS-1639 / malformed-markup detection prepends "[malformed] ".**
  Subjects containing `</subject>`, `</description>`, `</parameter>`,
  `<parameter name="…">`, `<subject>`, or `<description>` MUST
  cause the rendered row text to be prepended with the literal
  `[malformed] ` flag so the user can spot the upstream encoding
  bug. Detection is in `looksMalformedMarkup`; rendering is in
  `rowText`. Same detection applies to the description field.

- **ANTS-1639 / clean rows pass through unchanged.** Subjects that
  mention angle-brackets in legitimate prose (e.g.
  `Refactor <repo>/<branch> handling`) MUST NOT trip the flag —
  the detector regex requires the *exact* upstream tool-wrapper
  shape, not any `<…>` markup.

Failure modes prevented: silent rendering of raw tool-input
markup as task titles (the 2026-05-19 screenshot bug); copy-menu
slot accidentally reading from the visible-truncated display text
(loses content past the 200-char description cap).
