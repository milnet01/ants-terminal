# roadmap_export_concurrency — the export's write lock

Feature contract for **ANTS-3761**.
Parent spec: [`docs/specs/ANTS-3761-roadmap-export-format.md`](../../../docs/specs/ANTS-3761-roadmap-export-format.md)

§ 6 of the parent assigns **INV-9** to this directory, and only INV-9. It is
separate from `roadmap_export_roundtrip/` because it is the only invariant here
about the *file system* rather than the bytes: it holds a lock and observes
what the writer does about it.

## What this locks

**INV-9 — every export write path acquires `ConfigWriteLock`, and aborts
loudly when it cannot.**

Three claims, all in one test because they are one sequence:

1. **With the lock held, the export fails and reports.** The guard is advisory
   and its header leaves the choice to the caller; § 2.6 removes that choice
   here, because the model's § 9 makes a silent backup failure worse than no
   backup — it stops anyone checking.
2. **It wrote no bytes and left no temp file.** A truncated backup that looks
   complete is the failure the parent spec exists to prevent, and an orphaned
   `<dest>.XXXXXX` beside it is the same lie one directory entry over. The
   directory listing is the assertion, not the destination's absence alone.
3. **Released, the same call succeeds.** Without this the first two would pass
   against a writer that fails for any reason at all.

**Phrasing matters and the test is written to it.** `flock(2)` is advisory, so
a non-cooperating writer *can* interleave — the testable claim is about our
writer, not about the file.

## Must fail first

Each mutation applied to `src/roadmapexport.cpp`, built, run, reverted:

- `exportProject()` ignoring `!lock.acquired()` — the failure § 2.6 names →
  **RED** on claim 1 (the export succeeds) and on claim 2 (the file exists).
- The abort path calling `f.commit()` instead of `f.cancelWriting()` → **RED**
  on claim 2 (a truncated destination is left behind).

## Cost note

The test takes about five seconds, and the reason is `ConfigWriteLock`'s own
contract rather than anything here: it polls `flock` 100 × 50 ms before
reporting a failed acquire, so claim 1 cannot be observed sooner than that
deadline. Reusing the project's existing lock beats a second locking scheme
(`coding.md` — reuse before rewriting), and its deadline comes with it.
