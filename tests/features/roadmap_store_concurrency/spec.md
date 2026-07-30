# roadmap_store_concurrency — two writers against one roadmap store

Feature contract for **ANTS-3756** INV-15 and INV-16.
Parent spec: [`docs/specs/ANTS-3756-roadmap-store-schema.md`](../../../docs/specs/ANTS-3756-roadmap-store-schema.md)

The second writer is not hypothetical. `src/main.cpp`'s `--quake` / `--dropdown`
options make a dropdown instance alongside a regular window the shipped
configuration, and both instances open the same store.

## What this locks

**INV-15 — two processes opening a store that does not exist produce exactly
one schema.**

Two children `fork(2)`ed against one fresh path, released together through a
pipe. Exactly one reports that it created the schema; the other reports that it
opened a store already at `user_version = 1` and created nothing. Neither may
fail: the loser waits out the write lock, it does not error.

The observable is `RoadmapStore::createdSchema()`, and it exists because
**after the fact the winner and the loser are indistinguishable** — both end
holding one set of tables at `user_version = 1`. `CREATE TABLE IF NOT EXISTS`
succeeds for both and reports nothing, which is the design INV-15 forbids and
which passes every after-the-fact assertion. The discriminator has to be
observed at the moment it is made.

The raced store is then compared against one a single process creates in a
fresh directory: same table set, exactly. That is what "one set of tables"
means, and deriving it from a solo store rather than a hardcoded list keeps the
assertion true as the schema grows.

**INV-16 — a write that cannot take the lock fails and reports.**

Two legs, because the deadline and the policy fail independently:

- The deadline is **applied**: a freshly-opened store's connection reports
  `PRAGMA busy_timeout` = 5000 ms. SQLite's own default is 0 — an immediate
  `SQLITE_BUSY` — so this is not a slower version of the contract but a
  different one.
- The policy **holds**: with one connection holding a write transaction open,
  a `putItem()` on another fails, returns a non-empty error, and leaves no row
  behind. It is never retried silently and never dropped — a lost roadmap write
  is invisible to the user.

The policy leg shortens the deadline to 100 ms **on the blocked connection
only**. The contract under test is fail-and-report, not the constant; the
constant is the first leg's job, and honouring it here would cost the suite
five seconds to re-assert something already asserted.

## Must fail first

Each mutation applied to `src/roadmapstore.cpp`, built, run, reverted:

- Creation gated on `CREATE TABLE IF NOT EXISTS` instead of on `user_version`
  read inside `BEGIN IMMEDIATE` → **RED**: both children report `created`.
- `putItem()` swallowing the failed `BEGIN IMMEDIATE` and returning a pk →
  **RED**: the policy leg would otherwise pass a lost write off as a successful
  one.
- `kBusyTimeoutMs` drifted 5000 → 100 → **RED**: the deadline leg reads 100.
- `createSchema()`'s `user_version` fast path removed → **RED**: opening takes
  the full deadline while another connection holds the write lock.
- `journal_size_limit` moved off the per-connection list → **RED**. This is the
  sibling project's live bug, reproduced deliberately.
- `Access::Bulk` ignored, so every connection gets the interactive deadline →
  **RED**.

**One named break does not redden its leg, and the reason is worth recording.**
Dropping `PRAGMA busy_timeout` from the connection pragmas entirely leaves the
deadline leg **green**, because Qt's QSQLITE plugin calls `sqlite3_busy_timeout`
with a 5000 ms default of its own (the `QSQLITE_BUSY_TIMEOUT` connect option) —
measured here, not read from a doc. So the leg asserts the **effective**
deadline, which is the contract that matters, and the constant-drift mutation
above is what proves it has teeth. The pragma stays in the store because a
durability contract should not rest on an undocumented driver default that a Qt
upgrade can change underneath it.

## The connection profile

Two further tests came out of a review of RetroDB, a sibling project on this
machine that fought "database is locked" for several releases.

**`ConnectionProfileIsPerConnection`** asserts every connection-scoped pragma,
both access profiles, *and the two that are deliberately off*. The last part is
the point: `mmap_size` and `temp_store = MEMORY` are ordinary performance wins
that here would convert a disk cost into resident memory and break ANTS-3761
INV-12's 4 MiB export budget. Asserting their **defaults** is what stops a
later performance sweep enabling them without meeting that budget.

`journal_size_limit` gets its own assertion because it reads like a file
setting and is not one — set it, reconnect, read it back, and SQLite answers
`-1`. RetroDB classified it as file-level and moved it to a once-per-boot init
path, which left it not in force on any connection that serves a request. That
mutation is in the must-fail list below.

**`OpenDoesNotContendWithAnActiveWriter`** holds a write transaction open and
opens a second store against the same file. It must succeed, and quickly. This
caught a real defect: `createSchema()` took `BEGIN IMMEDIATE` on every open
just to read `user_version`, so every ordinary open queued behind any active
writer — the full 5000 ms deadline. The suite ran 5118 ms; it now runs 115 ms.

**What INV-15 found that a cold read did not.** `PRAGMA journal_mode = WAL`
takes an EXCLUSIVE lock **below** the busy handler, so `busy_timeout` does not
cover it at all: two instances opening the same fresh store raced on that one
statement and the loser failed the open outright, on 18 of 25 runs, with
`busy_timeout` already at 5000. `RoadmapStore::enableWal()` now supplies the
retry SQLite declines to, bounded by the same deadline so there is no second
timeout constant.
