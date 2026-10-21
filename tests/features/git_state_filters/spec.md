# git_state_filters — ANTS-5508 conformance

Pure-parser feature test for the helpers behind five `git_state` options.
Each helper takes git's output bytes and returns plain structs, so every
case runs on literal fixtures: no git, no QProcess, no repository.

## The options

- `op:"diff"` takes `paths[]`, as `op:"log"` already did.
- `op:"diff"` with `added_lines:true` returns only the lines the diff added,
  with their line numbers in the new file. `added_match` keeps the added
  lines containing its text, case-sensitive, and implies `added_lines`.
- `op:"log"` with `grep` keeps commits whose message contains its text,
  ignoring case.
- `op:"log"` with `stat:true` gives each commit a `files[]` of
  `{path, added, removed}`.
- `op:"rename_sweep"` lists every line still using the name `old`, matched
  as a whole word, in the tracked files. With `range` it searches the
  range's last commit; without, the working tree. `paths[]` narrows it.

## Invariants covered

- **INV-1 (added lines)** — `GitWrap::addedLines` returns each `+` line of a
  file's hunks with its post-image line number and without the marker.
  Context lines advance the count; `-` lines and `\ No newline` do not.
- **INV-2 (added match)** — a non-empty match keeps only the added lines
  containing it, case-sensitive; an empty match keeps them all.
- **INV-3 (log stat)** — `GitWrap::parseLogNumstat` splits records on
  `0x1e`, a record's fields on `0x1f`, and reads the numstat lines after
  the `0x1d` that ends the fields. A binary file's `-` counts read as `-1`.
  A commit touching no file has an empty file list.
- **INV-4 (grep hits)** — `GitWrap::parseGrepZ` reads `git grep -n -z`
  output into `{path, line, text}`, dropping the `<rev>:` prefix git puts
  before each path when a revision was searched.
- **INV-5 (range tip)** — `GitWrap::rangeTip` gives the revision a range
  ends at: `B` for `A..B` and `A...B`, `HEAD` for `A..`, and a lone
  revision unchanged.

## MCP layer (not asserted here)

`runDiffOp`, `runLogOp` and `runRenameSweepOp` map these structs to the
envelopes above. An added-lines diff is
`{files:[{path, added:[{line, text}]}], totals:{files, lines}}`, omitting
files with no kept line; it refuses `bad_args` beside `hunks`. A rename
sweep is `{old, rev | worktree, hits:[{path, line, text}], files, truncated?}`;
it refuses `bad_args` without `old`, and exit 1 from `git grep` means no hit.
Checked by calling the ops through `ants-mcpd`, as `git_diff_hunks` is.

## Out of scope

- Untracked files: `git grep` searches tracked files only.
- Rename paths in `stat`: git's `old => new` form is passed through as the path.
