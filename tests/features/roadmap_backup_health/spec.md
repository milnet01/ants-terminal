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
