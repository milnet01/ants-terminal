---
paths:
  - "src/roadmap*"
  - "src/remotecontrol_roadmap*"
  - "tests/features/roadmap*/**"
---

# The roadmap store

- **The roadmap store is machine-global**
  (`~/.local/share/ants-terminal/roadmap.sqlite`, mode 0600):
  - `roadmap_migrate` refuses a root under the system temp dir. The guard is
    in the handler, not in `RoadmapMigrateVerb::run()`.
  - There is no `ON DELETE CASCADE`. Deregistering a project deletes
    element → history → feedback_ref → relationship → citation → item →
    section → id_prefix, in that order, with `PRAGMA foreign_keys = ON`.
    `relationship` clears both ends. The `project` row and its `message`
    rows are kept, with `deregistered_at` set (ANTS-5366); no project row
    is ever deleted.
  - Back it up with sqlite3 `.backup`, never `cp`.
  - A `kSchemaVersion` bump locks every older build out of every project.
    Ask first whether the value can be derived instead.
