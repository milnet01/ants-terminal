# Review Changes — quoted paths and probe rounds (ANTS-6620)

The Review Changes dialog (`src/diffviewer.cpp`) runs five git probes per
refresh round and renders their output.

## Invariants

- **INV-1** — a path `git status --short` prints in C quotes (one holding a
  space) or with octal escapes (a non-ASCII one) is read as the real path. An
  untracked file with such a name shows its content under New files, not
  `(unreadable)`, and a modified tracked one gets its `(N lines)` suffix.
- **INV-2** — a new round cancels the round still running. After the dialog
  opens and Refresh is clicked twice before any probe is reaped, one
  `git status` probe is running, not three.

## Test

`test_review_changes_probe_rounds.cpp`: a real git repository in a temporary
directory and a real `diffviewer::show()`, headless.
