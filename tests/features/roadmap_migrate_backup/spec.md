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
  silently. A caller that wants the migration anyway says so explicitly.

## Notes

`VACUUM INTO` is the mechanism, not sqlite3's C backup API. Qt wraps that API
nowhere, so the C route would add a build dependency on `sqlite3.h`; the
QSQLITE driver owns sqlite today. `VACUUM INTO` gives the same consistency
under WAL and runs through `QSqlQuery`. It cannot run inside a transaction,
which is why INV-6 orders the snapshot before `begin()`.
