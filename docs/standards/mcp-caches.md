# MCP cache layer — keying & relocation contract (ANTS-1439)

This document is the canonical record of how every Ants MCP cache is
keyed and how each behaves when a project directory is **relocated**
(the user moves the repo to a new absolute path). It exists so the
next person to add a path-keyed cache — or the next person to hit a
relocation symptom — has the contract written down instead of
re-deriving it from six call sites.

Provenance: a Vestige CC session (2026-05-16) raised this as a
*defensive observation*, not a bug anyone hit. The trigger was real
— this workstation moved its work tree `/mnt/Storage → /mnt/Games`
on 2026-05-08 — but no cache returned a wrong answer as a result.
The point of the audit was to confirm that, and to write the contract
down before the next relocation.

## The invariant

**A path-keyed cache may go cold or leave orphans across a
relocation. It must never *shadow*.**

- **Cold** — the new path starts with an empty cache; first calls pay
  the recompute cost. Acceptable.
- **Orphan** — the old path's on-disk entries linger as dead bytes
  that nothing reads. Acceptable (subject to GC, below).
- **Shadow** — a lookup under the *new* path returns data that
  belongs to the *old* path. **Forbidden.** This is the only failure
  mode that produces a wrong answer with no error, so it is the line
  every cache must stay behind.

The way every current cache stays behind that line is the same:
the absolute path (or its `caller_cwd`) is *part of the key*, so a
relocation changes the key, so old entries become unreachable rather
than mis-served. New caches MUST preserve this property — never key a
cache on something coarser than the full canonical path when entries
are project-scoped.

## Cache inventory

| Cache | Spec | Storage | Keyed by | Relocation profile |
|-------|------|---------|----------|--------------------|
| `session_memory` | ANTS-1283 / ANTS-1336 | Disk: `~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json` | `sha256(canonical cwd)` (one file per project) | **Orphan.** Old-SHA file persists; new-SHA file is fresh. Different files → no shadow. |
| `project_layout` | ANTS-1430 | Inside `session_memory` (well-known key) | Inherits per-cwd-SHA isolation | **Orphan.** Same profile as `session_memory`. |
| idempotent-read TTL cache (100 ms) | ANTS-1357 | In-process | `(tool, sha256(args))` where args carry `caller_cwd` | **Cold.** Relocation changes the args hash → old entries unreachable. Process death wipes it. |
| `verify_changes` build cache | ANTS-1359 | In-process | project root + git HEAD + porcelain SHA + trust outcome | **Cold.** In-process only; process death wipes it. |
| `mcp_trace` ring buffer | ANTS-1360 | In-process | n/a (append-only ring) | **No hazard.** Not path-keyed. |
| `roadmap_query` parse cache | ANTS-1117 | In-process | `(path, mtime)` + 100 ms TTL | **Cold.** Keyed on the absolute path + mtime; a moved file is a new key. |
| `cold_eyes` partition cache | ANTS-1319 | In-process | `(path, scope, stamp)` + 5 s TTL | **Cold.** In-process; path-keyed. |
| `audit_run` `.audit_cache/` | ANTS-1555 | Disk: `<root>/.audit_cache/` | Lives *inside* the project tree | **Moves with the tree.** Relocating the directory carries the cache with it; paths inside are relative to the moved root. No external stale key. |

Net of the audit: every entry above is **Cold** or **Orphan** — none
**Shadow**. The two defensive gaps that remain are cosmetic, not
correctness:

1. **Orphaned bytes accumulate** — one dead `session_memory` file per
   abandoned path. Bounded by how often the user relocates projects
   (rare); each file is small JSON.
2. **Cold-start cost** — the new path recomputes on first use. A
   one-time, sub-second cost per tool.

## Future work (deferred until a symptom is reported)

Neither item below is scheduled — they are the documented response if
orphan accumulation or relocation friction ever becomes a real
complaint:

- **Stale-entry GC.** A `session_memory` sweep at session start that
  drops on-disk buckets last-touched more than N days ago. Closes
  gap (1).
- **`migrate-cwd <old> <new>` MCP verb.** An explicit, opt-in
  re-key of a project's `session_memory` bucket from the old path's
  SHA to the new one, for users who *want* their stored notes to
  follow a deliberate move. Closes gap (2) for intentional
  relocations without weakening the no-shadow invariant (it is an
  explicit caller action, not an automatic guess).

## Checklist for adding a new path-keyed cache

1. Is an entry project-scoped? Then the canonical path (or its SHA,
   or `caller_cwd`) MUST be part of the key. Never key on something
   coarser.
2. Where does it live — in-process (Cold on restart) or on disk
   (Orphan on relocation)? State which in the cache's own spec.
3. Add a row to the inventory table above in the same commit.
4. If it stores on disk under `~/.cache`, consider whether the GC
   sweep should cover it.
