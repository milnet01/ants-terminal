# roadmap_migrate_backup — the pre-migration snapshot

Feature contract for ANTS-4499.
Parent: [`docs/specs/ANTS-3855-roadmap-migrate-verb.md`](../../../docs/specs/ANTS-3855-roadmap-migrate-verb.md)

## Problem

`roadmap_migrate` rewrites rows in a store shared by every project on the
machine, with no snapshot and no undo. The obvious workaround does not work:
the store runs in WAL, so a plain file copy misses whatever is still in the
`-wal` and produces a file that looks right.

`tools/roadmap-store-backup.sh` covers the weekly cadence. It cannot protect a
migration that runs between two of its snapshots.

## Contract

- **INV-1** — `RoadmapStore::snapshotTo()` writes a snapshot that opens as a
  database and carries the rows the live store held when it was called,
  including rows still resident in the `-wal`. A plain file copy fails this.
- **INV-2** — the snapshot is mode 0600. It holds every project's roadmap.
- **INV-3** — a caller never observes a partial snapshot at the destination.
  The write goes to a temp path and is renamed.
- **INV-4** — the snapshot is rolling: a second call replaces the first, and
  the replaced file is not left behind under either name.
- **INV-5** — the destination is never named `roadmap-*.sqlite`. That glob is
  what `tools/roadmap-store-backup.sh` prunes, so a matching name would cost
  the weekly rotation one of its kept snapshots.
- **INV-6** — a real `roadmap_migrate` run takes the snapshot before it opens
  its transaction, and reports where it went.
- **INV-7** — a `dry_run` takes no snapshot. It writes nothing, so there is
  nothing to protect.
- **INV-8** — a failed snapshot refuses the migration rather than proceeding
  silently. A caller that wants the migration anyway says so explicitly. *(ANTS-5247)* A failed snapshot to a default folder is first retaken
  beside the store; the refusal fires when `backup_to` fails, or the retake does.
- **INV-9** *(ANTS-5466)* — a file already at the destination is replaced
  only if it is a SQLite database. Anything else refuses, names the file, and
  is left untouched. `backup_to` is not confined to a project, so a wrong path
  must not delete an unrelated file. The same holds for a leftover
  `<destination>.partial`, except that an empty one is cleared: that is what
  a run killed before it wrote anything leaves.

- **INV-10** *(ANTS-5247)* — with no `backup_to`, a `snapshotDir` that exists
  receives `pre-migrate.sqlite`, and `backup_path_source` names its rung.
- **INV-11** *(ANTS-5247)* — with no folder named, the snapshot goes beside
  `run()`'s `storePath`, never beside the real store; source `beside_store`.
- **INV-12** *(ANTS-5247)* — a snapshot to the folder that fails is retaken
  beside `storePath`, and `backup_fallback` names the folder and its error.
- **INV-13** *(ANTS-5247)* — a `snapshotDir` that does not exist is never
  created, and falls back as INV-12 does. A folder on an unmounted drive would
  otherwise be recreated on the system drive.

Parent for INV-10..13: ANTS-3855 INV-15.

## Notes

`VACUUM INTO` is the mechanism, not sqlite3's C backup API. Qt wraps that API
nowhere, so the C route would add a build dependency on `sqlite3.h`; the
QSQLITE driver owns sqlite today. `VACUUM INTO` gives the same consistency
under WAL and runs through `QSqlQuery`. It cannot run inside a transaction,
which is why INV-6 orders the snapshot before `begin()`.
