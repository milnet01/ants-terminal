# pruned_walk — a directory walk that never lists what it excludes

ANTS-5058 / ANTS-5062. Two engines walked a project tree with
`QDirIterator::Subdirectories` and only decided a directory was noise —
`build/`, `build-asan/`, `node_modules/` — after every file under it had
already been listed. The review corroboration walk and the test-audit
partition walk both paid for descending into every build tree on every
round, on the GUI thread. `src/prunedwalk.h` is the shared primitive both
are meant to route through: a walk that decides at the directory, before
recursing into it, not at the file, after.

## Invariants exercised by this test

- **INV-1** (a pruned directory is never descended) — `PrunedWalk::walkFiles`
  never calls its file callback for anything under a directory its prune
  callback names, and the entry count it reports proves the walk skipped
  that directory rather than listing it and dropping the result.
- **INV-2** (the cap counts what the walk visits, not what it accepts) — a
  directory the walk prunes must not spend the entry cap on the files
  inside it; a file outside a pruned directory is still reached, and the
  walk is not reported as capped, however many files the pruned directory
  holds.

## Scope

`PrunedWalk::walkFiles` itself, in isolation. Not `IndieReviewEngine` or
`TestAuditEngine` — their wiring to this primitive is a source-scrape
invariant in `tests/features/test_audit_walk_exclusions/spec.md`, which
already carries the build-tree-exclusion contract both engines share.

## Regression history

The stub committed alongside this spec keeps today's behaviour: one
`QDirIterator` over every file, with a pruned file dropped only after being
listed, so the entry count and the callback-call count both still cost the
descent. Fixing `walkFiles` to recurse itself and never list a pruned
directory is the ANTS-5058 / ANTS-5062 fix; this test is written to fail
against the stub and pass once that fix lands.
