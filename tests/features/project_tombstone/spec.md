# project_tombstone — feature contract

Design contract:
[`docs/specs/ANTS-5366-deregistered-project-tombstone.md`](../../../docs/specs/ANTS-5366-deregistered-project-tombstone.md).
That document owns the surface and the reasoning; this file records what the
test binary asserts and how.

## What the feature is

Deregistering a project keeps its `project` row, stamped with
`deregistered_at`, instead of deleting it. The kept row keeps the project's
mailbox reachable and stops SQLite reusing its `project_id`. Every roadmap
reader hides the row unless asked for it.

## Routing

Every case runs against a store inside a `QTemporaryDir`. **Never
default-construct `RoadmapStore` in a test**: its default path is the
developer's real machine-global store.

| Invariant | Route |
|---|---|
| INV-1 to INV-6 | `RoadmapStore` directly |
| INV-7 | `RoadmapMigrateVerb::run()` against the same store file |
| INV-8 | `RoadmapExport::exportProject()` then `RoadmapExport::runImportCommand()` |
| INV-9 | `tests/features/roadmap_store_upgrade/`, not this file |

## Invariants

The design contract's INV-1 to INV-8, each asserted as written there:

- **INV-1** — deregistering keeps the row with the passed timestamp, and the
  project's items and sections are gone.
- **INV-2** — no `message` row is deleted, at either end.
- **INV-3** — mail sent to the deregistered slug succeeds, the inbox resolved
  from its root lists it, and `slugCandidates()` offers the slug.
- **INV-4** — `readProject()`, `readProjectBySlug()`, `readProjectByRoot()`
  and `listProjects()` hide the row by default, while a live sibling is still
  returned. `Visibility::IncludeDeregistered` returns it.
- **INV-5** — after deregistering the highest id, a new project's id is
  greater.
- **INV-6** — `registerProject()` on the deregistered root returns the same id
  and clears `deregistered_at`.
- **INV-7** — `roadmap_migrate` from a new root asking for the departed slug
  refuses `slug_collision` with `deregistered:true`, and writes no project row.
- **INV-8** — a restore of the project's own export at its own root succeeds,
  keeps the `project_id`, and brings its items back.

## Red proofs

Each case was run red on its own assertion by breaking the behaviour it
locks, then restored. The commit that added this file names the breaks.
