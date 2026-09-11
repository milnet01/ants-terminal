# FileContentCache — one locked, bounded cache for the review engines (ANTS-5056)

`BriefDispatch`, `ColdEyesEngine`, `IndieReviewEngine` and `DebtSweepEngine`
each kept their own function-static `QHash` of file contents keyed by path
and modification time, commented as single-threaded. Since ANTS-2132 the
brief, partition and sweep verbs run on the MCP worker while the Cold-eyes
and Independent Review dialogs, the audit dialog's debt scan, and the
inline `indie_review_dispatch` path fill the same shape of cache from the
GUI thread. A concurrent insert or clear on an unguarded `QHash` is
undefined behaviour. Three of the four copies also had no size bound, so a
debt sweep kept every source body it had ever read for the life of the
process.

`src/filecontentcache.h` (namespace `FileContentCache`) replaces all four:
one cache, one lock, one byte budget (`kByteBudget`). Each engine's
`slurpUtf8` becomes a call into it.

## Invariants

- **INV-1 (byte budget holds under load)** — after `clear()`, reading
  several files whose combined decoded size runs well past half of
  `kByteBudget`, `cachedBytes()` never exceeds `kByteBudget` at any point
  in the sequence, and every read still returns that file's real content.
- **INV-2 (an oversized file is served, not retained)** — a file whose own
  decoded size alone exceeds `kByteBudget` is still returned in full and
  correctly; the cache holds nothing that would push `cachedBytes()` past
  `kByteBudget`.
- **INV-3 (freshness and read mode are both part of the key)** — a file
  rewritten with a new modification time returns its new content on the
  next read. Reading the same path with `textMode=false` and with
  `textMode=true` returns different content once the file has CRLF line
  endings — `textMode=true` collapses them to LF — so the two modes must
  not share a cache entry.
- **INV-4 (concurrent access does not corrupt state — guard)** — several
  threads repeatedly reading a shared set of files, while another thread
  repeatedly calls `clear()`, must not corrupt what a read returns and
  must not crash the process. This is a guard rather than a strict
  contract: an unlocked cache can pass this by luck as easily as it can
  crash, so a clean run here proves nothing on its own — it is meant to be
  run under the sanitizer build, where a genuine data race is far more
  likely to be caught.
- **INV-5 (every engine goes through the shared cache)** — none of
  `briefdispatch.cpp`, `coldeyesengine.cpp`, `indiereviewengine.cpp` or
  `debtsweepengine.cpp` defines its own per-file static cache any more;
  each calls `FileContentCache::slurpUtf8`, and `BriefDispatch`'s call
  passes `textMode=true` — the mode its own removed cache used to get by
  opening with `QIODevice::Text` directly.

## Rationale

ANTS-5056: every one of the four review engines is reachable from both the
GUI thread (a dialog the user opened) and the MCP worker (a verb an
assistant session called) for the same class of file read. The shared
process holds every open terminal tab, so heap corruption from a racing
`QHash` insert is not confined to a review feature. The missing byte bound
on three of the four copies is a second, independent defect the same fix
closes: an unbounded cache accumulates every source body a debt sweep ever
read.

## Scope

In scope: the shared header's budget, freshness and read-mode-keying
contract (INV-1–INV-3), a concurrency guard over it (INV-4), and that all
four engines are actually wired to it rather than keeping a shadow copy of
their own (INV-5).

Out of scope: the fix's own lock implementation (a mutex is the obvious
choice; this spec does not pin the type). The eviction policy is
unspecified beyond "stays within budget" — which entry gets evicted first
is an implementation detail, not a contract. The four engines' own review
logic is untouched by this change and is not re-tested here. INV-4 stands
in for the GUI-thread-plus-MCP-worker scenario with ordinary threads; it
does not drive a real `QCoreApplication` event loop or the actual dialogs.

## Test scope

Behavioural against `FileContentCache::slurpUtf8` / `cachedBytes` /
`clear`, headless, Qt::Core only. INV-1–INV-4 write temporary files under
`QTemporaryDir` and read them back through the cache; INV-3 pins exact
modification times with `utimensat` (this codebase is Linux-only) rather
than relying on filesystem mtime resolution across a sleep. INV-5 is a
source-grep: each engine's `.cpp` is read, comments stripped, and checked
for the retired cache shape's absence and the shared call's presence.

## Regression history

- **Introduced:** each engine grew its own static cache independently, one
  per review feature, commented single-threaded at the time — true until
  ANTS-2132 put the brief/partition/sweep verbs on the MCP worker while the
  dialogs kept filling the same caches from the GUI thread.
- **Fixed:** ANTS-5056 — `src/filecontentcache.h` centralises the cache
  behind a lock and a byte budget; this spec locks that contract and the
  wiring that makes it apply to all four call sites.
