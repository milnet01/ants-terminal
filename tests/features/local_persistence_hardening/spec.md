# Local-persistence write-safety hardening

Status: ✅ shipped
Kind: security
Source: indie-review #6 fold-in 2026-05-22 (ANTS-1821 / 1822 / 1823 / 1824 / 1836)

## Problem

Several local-persistence write paths shared the same unsafe shapes flagged
by the indie-review #6 sweep:

- **ANTS-1821** — `SessionManager::sessionDir` (and `SessionMemoryEngine`'s
  store dir) created the directory with `QDir::mkpath` under the process
  umask (→ 0755) and then chmod'd to 0700, leaving a TOCTOU window in which
  the dir was group/world-listable; the chmod return was swallowed.
- **ANTS-1822** — `SessionManager::cleanupOldSessions` swept `*.tmp` with a
  glob broader than the two temp names it owns, so it could delete a
  foreign or in-flight `.tmp` another tool dropped in the sessions dir.
- **ANTS-1823** — `SessionMemoryEngine::execute`'s read-modify-write was
  unlocked, so two concurrent same-cwd CC sessions could last-writer-wins
  drop a key; `workflow_state` set compounded it with two separate unlocked
  cycles (TTL purge + Set).
- **ANTS-1824** — the learned-FP and autofix JSONL appenders wrote the
  record and its newline in two `write()` calls; a concurrent appender
  could interleave between them, corrupting the JSONL.
- **ANTS-1836** — `TestAuditDialog`'s report writer used a plain
  `QFile(WriteOnly|Truncate)` with a discarded `write()` return (torn file
  on disk-full, no 0600, silent failure).

## Surface

- `ants::ensurePrivateDir(dir)` in `src/secureio.h` — the shared atomic
  private-dir helper (ANTS-1821).
- `SessionMemoryEngine::mutateLocked(path, mutator)` in
  `src/sessionmemoryengine.{h,cpp}` — the shared locked read-modify-write
  primitive (ANTS-1823).

## Invariants

- **INV-1** — `ensurePrivateDir` creates a missing directory at mode 0700
  (born private; never 0755 even briefly).
- **INV-2** — `ensurePrivateDir` is idempotent: a second call on an
  existing 0700 dir succeeds and does not recreate it.
- **INV-3** — `ensurePrivateDir` tightens a pre-existing 0755 leaf dir to
  0700 (subsumes the old unconditional-chmod intent) and returns true.
- **INV-4** — `ensurePrivateDir` rejects a symlink in place of the leaf
  (returns false, does not follow it).
- **INV-5** — `mutateLocked` writes only when the mutator returns true; a
  read-only mutator (returns false) leaves no store file on disk.
- **INV-6** — `mutateLocked` enforces the total-bytes cap with code
  `cap_exceeded` and does not write past it.
- **INV-7** — `mutateLocked` round-trips: a write then a re-load returns
  the mutated content.
- **INV-8 (wiring)** — `sessionDir` uses `ensurePrivateDir` and no longer
  calls `mkpath`+`setPermissions`; `cleanupOldSessions` globs only
  `session_*.dat.tmp` / `tab_order.txt.tmp`; the workflow_state set routes
  through `mutateLocked`; the JSONL appenders write a single `record + '\n'`
  buffer; `writeReport` uses `QSaveFile` + `setOwnerOnlyPerms`.
