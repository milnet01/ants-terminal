# git_state_truncation — a capped git read says so

Feature contract for the ANTS-5098 finding that `git_state`'s `status` and
`diff` (numstat) branches ignore `GitWrap::Result::stdoutTruncated`.

`GitWrap::run` caps git's stdout at `GitWrap::kStdoutCapBytes`. Past it, a
large repository's file list and totals came back incomplete and unmarked,
and the last line could be a path cut mid-name. The `diff` hunks branch
already set `truncated` (ANTS-1839).

## What this locks

- **INV-1** — `op:"status"` on output past the cap sets `truncated:true`, and
  every `files[].path` is a whole path git reported. The cut last line is
  dropped, never emitted as a path.
- **INV-2** — `op:"diff"` with numstat (no `hunks`) does the same, and its
  `totals.files` counts only the whole lines kept.

## Test

`test_git_state_truncation.cpp` builds one temporary repository with enough
long-named files that both reads pass the cap: `status` while they are
untracked, then `diff` with `staged:true` after `git add`.
