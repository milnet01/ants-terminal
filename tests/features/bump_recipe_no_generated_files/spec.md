# bump_recipe_no_generated_files — the bump recipe never edits a generated file (ANTS-4529)

`ROADMAP.md` is not a source file. It is rendered from the roadmap store by
`RoadmapRender::render()` (`src/roadmaprender.cpp`), which replays the root
section's stored `intro` **verbatim** and rewrites the whole file on every
`roadmap_log` write of any op.

`.claude/bump.json` used to list `ROADMAP.md` among its version-bearing files
and hand-edit `**Current version:** {OLD}` in the markdown. That edit lives
outside the store, so the next `roadmap_log` write discarded it — reported as
`discarded_external_edits` — and the banner silently reverted to the previous
release. It fired four times in one session on 2026-08-19, on `op:annotate`,
`op:create_section` and `op:flip`. The trigger is not a particular op: every
op re-renders the file, so the first write after a repair reverts it.

`packaging/check-version-drift.sh` also checked `ROADMAP.md`, which made the
revert survivable — the gate caught it — but only when a human remembered to
run the gate, and it re-opened version drift on a release commit that had
already looked clean.

The fix removes the duplicate rather than synchronising it. The version number
in the banner duplicated `CMakeLists.txt`'s `project(... VERSION X.Y.Z)`, the
project's single source of truth, and the sentence holding it already links
`CHANGELOG.md`, which carries every shipped version, dated. So the banner
states no version, `ROADMAP.md` leaves the bump recipe, and the drift gate
stops checking it. Nothing is left to sync, revert, or re-check.

This test locks that state against a well-meaning re-add — the failure mode
`ROADMAP.md` was in the recipe for, twice (ANTS-2163, then ANTS-4529).

## Invariants

- **INV-1** — no `files[].path` in `.claude/bump.json` names a file the
  roadmap render generates (`ROADMAP.md`, or anything under `docs/roadmap/`).
  *Test:* `RecipeNamesNoGeneratedFile`.
- **INV-2** — `packaging/check-version-drift.sh` runs no `check` against
  `ROADMAP.md` or `docs/roadmap/`. *Test:* `DriftGateChecksNoGeneratedFile`.
- **INV-3** — the rendered `ROADMAP.md` banner carries no version number, so
  there is nothing for either of the above to have to keep in step.
  *Test:* `RenderedBannerStatesNoVersion`.

## What this deliberately does NOT lock

The store's root-section `intro` bytes. The banner's *wording* is prose and
free to change; what INV-3 forbids is the reappearance of a version string in
the first lines of the generated file.

`README.md` keeps its version banner and its drift check. It is a hand-written
source file, so the recipe's edit to it survives.
