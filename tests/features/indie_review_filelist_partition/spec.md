# Feature spec: indie_review partition file-list module-map fallback (ANTS-3507)

Follow-up to ANTS-3481, which added the honest `module_map_unparseable`
refusal when a project's `## Module map` heading is present but its bullets
are a `- <path> — <description>` file list (finbreak's shape) rather than the
`- `name` — summary` subsystem shape `SubsystemMap::parse` reads. That refusal
is correct but leaves a common map shape needing a hand-authored
`.indie-review/partition.json` before the auto-reviewer will run.

## Fix

`IndieReviewEngine::derivePartition` gains a fallback: when the subsystem-shape
parse (`SubsystemMap::cachedLanes` → `sourcePathsForLane`) derives **zero**
lanes, group the module-map's listed file paths by their **top-level
directory** — one lane per top dir (`app/…`, `tests/…` → lanes `app`, `tests`).

- Path tokens are harvested from each bullet's **pre-separator prefix** (so a
  path named in the description isn't grouped); backticks are tolerated.
- A token counts as a path only if it contains a `/` (a bare word is a
  subsystem name `SubsystemMap` already tried).
- Each entry must canonicalise **inside the project** (`isInsideProject`) and
  **exist** on disk (file or directory) or it is dropped.
- The fallback returns a partition **only when grouping yields >1 lane**. A
  single lane (everything under one top dir) is no more reviewable than the
  refusal, so the caller keeps its `module_map_unparseable` /
  `sparse_partition` path.

The change is confined to `derivePartition`; the override path, the
subsystem-shape path, and every caller (`indie_review_orchestrate` /
`indie_review_partition` / `indie_review_brief`) are unchanged — they simply
receive a non-empty partition where they previously got an empty one.

## Surface

- A project whose root `CLAUDE.md` has a `## Module map` of `- <path> — <desc>`
  bullets under ≥2 top-level dirs → `derivePartition` returns one lane per top
  dir, each carrying the existing listed paths.
- A file-list map under a single top dir → empty partition (caller keeps its
  refusal / sparse hint).
- A subsystem-shape map still partitions by named subsystem (fallback never
  engages when the primary parse yields lanes).

## Invariants

- **INV-1 / two top-level dirs → one lane each.** A `## Module map` file list
  naming paths under `app/` and `tests/` yields exactly two lanes, named
  `app` and `tests` (alphabetical), each `sourcePaths` holding the listed
  paths under that dir. Behavioural via `IndieReviewEngine::derivePartition`.
- **INV-2 / single top-level dir → empty (guard).** A file list entirely under
  `app/` yields an empty partition — grouping produces one lane, which the
  `>1` guard rejects so the caller keeps its refusal. Behavioural.
- **INV-3 / subsystem shape unshadowed (regression).** A backticked
  `- `foo` — summary` map whose name resolves to `src/foo.cpp` still yields a
  lane named `foo` (not a top-dir lane `src`); the fallback never engages when
  the primary parse is non-empty. Behavioural.
- **INV-4 / non-existent paths dropped.** A file list where one path exists and
  one does not (`app/ghost.py`) groups only the existing path; the missing
  entry never appears in any lane's `sourcePaths`. Behavioural.
- **INV-5 / traversal rejected.** A file-list entry that escapes the tree
  (`../../../etc/passwd`) is dropped by the `isInsideProject` guard — no lane
  is named `..` and no `sourcePaths` entry contains `..`. Behavioural.
