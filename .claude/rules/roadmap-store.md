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
  - There is no `ON DELETE CASCADE`. Deleting a project means deleting
    element → history → feedback_ref → relationship → citation → message →
    item → section → id_prefix → project, in that order, with
    `PRAGMA foreign_keys = ON`. `relationship` and `message` clear both ends.
  - Back it up with sqlite3 `.backup`, never `cp`.
  - A `kSchemaVersion` bump locks every older build out of every project.
    Ask first whether the value can be derived instead.
