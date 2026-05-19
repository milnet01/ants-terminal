# Feature spec: test_audit MCP bundle fold-in 2026-05-19

Behavioural tests covering the cross-session-report fixes folded in
on 2026-05-19:

- **ANTS-1615 / fold_in headline accepts `headline` / `summary` /
  `claim` AND refuses on empty.** When the caller passes an actionable
  entry whose headline-bearing field is missing or empty across all
  three accepted names, `foldIn()` MUST refuse with
  `{ok:false, code:"bad_actionable"}` rather than silently emitting a
  `**.**` bullet. When any of the three names carries a non-empty
  value, the bullet renders with that text (long headlines truncated
  to 120 chars with " …" suffix).

- **ANTS-1616 / pre-pass pattern set covers C/C++/Qt smells.** The
  pre-pass regex table MUST hit at least the following pattern ids on
  representative C++/Qt fixture content: `qt_msleep`, `cpp_exit`,
  `stderr_fail_print`, `qputenv_call`, `cpp_sleep_for`. v1's Python-
  only set produced a 0-hit rate across the whole repo on 2026-05-18.

- **ANTS-1617 / synthesis parser counts `### [SEV] dimension: <name>`
  heading-findings AND `## Findings (JSON)` blocks.** Two report
  shapes produced by RetroDB / 3D-Engine subagents previously yielded
  `severity_histograms:{}` because the parser only recognised the
  `- [SEV]` bullet form. The new parser MUST attribute heading-form
  findings to the dimension named in the heading, and MUST parse the
  JSON array inside a fenced `## Findings (JSON)` block, attributing
  each entry's `severity` field to its `dimension` field (or the
  current section dimension when omitted).

Failure mode: silent `**.**` bullets / `severity_histograms:{}` /
zero-hit pre-pass were all "all clean" failure shapes that the
orchestrator couldn't distinguish from a healthy run.
