# roadmap_backup_health — reporting a backup that stopped

Feature contract for **ANTS-3794**.
Parent spec: [`docs/specs/ANTS-3794-roadmap-store-backup.md`](../../../docs/specs/ANTS-3794-roadmap-store-backup.md)

§ 6.1 of the parent assigns **INV-11** to this directory.

## What this locks

**INV-11 — `RoadmapBackupHealth::assess()` returns an empty object when both
jobs are healthy or there is no store, and otherwise names exactly the
unhealthy jobs with the parent's § 2.5 state.**

- Both records healthy → empty.
- No store → empty, even with no records at all.
- A missing record → `never_run`.
- `error` set → `failing`, even with a recent `success`.
- `success` empty, or older than `kStaleAfterSecs` → `stale`. The boundary is
  checked on both sides with an injected `nowUtc`.
- Only the unhealthy job is listed, and `hint` names that job's timer.

## ANTS-5247 — the snapshot folder

**`RoadmapBackupHealth::snapshotDest(configDir, stateDir, storePath)` returns the folder
the pre-migration snapshot defaults to, and where it came from.** A non-empty
`configDir` wins (`config`); else the snapshot record's `dest=`
(`backup_record`); else nothing. An empty `dest=` counts as unset.
ANTS-5621: the record is used only when its `store=` is absent, empty or
names `storePath` (directory's canonical path plus file name); a record
naming another store gives nothing. `Ants5621SnapshotDestNamesItsStore`
holds it, a symlinked directory and a not-yet-created store file included.
Parent:
`docs/specs/ANTS-3855-roadmap-migrate-verb.md` INV-15.
