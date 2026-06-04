# ledger_write_safety — file-write safety bundle (ANTS-1988 / ANTS-1989)

## Problem

Two cross-cutting persistence bugs flagged by indie-review #7 (2026-06-04):

- **ANTS-1988** — private cache directories were created with `QDir::mkpath`,
  which honours the process umask (typically 0755). The directory's name,
  mtimes and child listing were briefly enumerable by other local users
  before any chmod landed; several sites never tightened at all. The cache
  holds model-switch timestamps, commit SHAs and audit findings.

- **ANTS-1989** — the model-switch ledgers and a couple of audit JSON/JSONL
  files did an unlocked read-modify-write: two Ants instances both read,
  both append, and the last atomic rename silently drops the other's record.

## Fix

- Route private-cache dir creation through `secureio.h ensurePrivateDir`
  (ANTS-1821): each missing component is born 0700, a pre-existing 0755 leaf
  is tightened.
- Wrap the read-modify-write cycles in `configbackup.h ConfigWriteLock`
  (advisory `flock(2)` on `<path>.lock`, 5 s deadline). On timeout the write
  proceeds best-effort — a 5 s wait implies a hung/stale holder (the RMW is
  microseconds), so dropping the record would be the worse outcome.

## Surface

- `src/modelswitchledger.cpp` — `writeLinesAtomic` (0700), `appendRecord` +
  `writeRecords` (lock).
- `src/modelnearmissledger.cpp` — same two fixes (same bug class).
- `src/auditfpledger.cpp` — `appendEntry` 0700 + lock spanning dedup+append.
- `src/auditdialog.cpp` — `appendSnapshot` 0700 + trend.json lock;
  `saveBaseline` 0700.
- `src/auditcache.cpp`, `src/auditrunner.cpp`, `src/auditautofix.cpp` —
  0700 cache dirs.

## Invariants

- **INV-1** — `ModelSwitchLedger::appendRecord` to a not-yet-existing nested
  dir creates that dir at mode 0700 (not 0755).
- **INV-2** — `ModelNearMissLedger::appendRecord` does the same.
- **INV-3** — the lock does not break the happy path: two sequential appends
  both survive (the lock is released between calls), and a `<path>.lock`
  sibling exists after a write.
- **INV-4** — wiring: every cited site routes through `ensurePrivateDir`
  (the bare `QDir().mkpath` idiom is gone at that site).
- **INV-5** — wiring: every cited RMW site holds a `ConfigWriteLock`.
