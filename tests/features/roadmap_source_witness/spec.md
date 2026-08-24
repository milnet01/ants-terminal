# roadmap_source_witness — `roadmap_query` says which backend answered

ANTS-4402. Contract for the source witness on `roadmap_query`'s envelope.

## Why

ANTS-3793 made the store primary for roadmap reads. ANTS-4141 found the write
path unsafe and prescribed the workaround this project has followed since —
edit `ROADMAP.md` by hand. Together those two mean **a hand edit lands in a
file no reader reads**, and until this feature nothing in the envelope said so:
`roadmap_query` named `ROADMAP.md` as its `path`, returned `ok:true`, and
served the store.

Measured on this project 2026-08-15: 59 ids present in the file and absent from
the store; a bullet filed by hand returned in `missing_ids`; two bullets that
are `✅` in the file reported as `📋`.

## Invariants

- **INV-1** — On a project with no store, the envelope carries
  `source: "markdown"` and **none** of `file_ahead_of_store`,
  `file_highest_id`, `store_high_water`. The markdown backend cannot be stale
  with respect to itself, so it never warns. *Test:* `Inv1MarkdownSource`.
- **INV-2** — On a migrated project the envelope carries `source: "store"`.
  This is the field whose absence made the defect invisible: it sits beside a
  `path` naming a file the answer did not come from. *Test:* `Inv2StoreSource`.
- **INV-3** — When `ROADMAP.md` holds an id above the store's high-water mark,
  the envelope carries `file_ahead_of_store: true` with both
  `file_highest_id` and `store_high_water`, so the reader can see the gap
  rather than infer it. *Test:* `Inv3FileAheadOfStoreWarns` — migrate, then
  append a bullet by hand exactly as ANTS-4141's workaround instructs.

**One-directional, deliberately.** An id above the store's mark *proves* the
read is stale; the warning's absence proves nothing, because a status flip or a
body edit moves no id. Both of those were measured as live shapes on this
project and neither is detectable this cheaply. Reconciling the two backends is
ANTS-4141's job and needs the render fixed first — this feature makes the
divergence visible, it does not resolve it.

## INV-6 — a freshly migrated project is not ahead of its own store

- **INV-6** — Immediately after a migration, with no append in between, the
  envelope carries **no** `file_ahead_of_store`, and `store_high_water` is at
  least `file_highest_id`. The store holds every id the file declares, so the
  warning must not fire. *Test:*
  `Inv6FreshlyMigratedProjectIsNotAheadOfItsStore`.

Added by ANTS-4636. `RoadmapStore::idHighWater()` reads the `id_prefix` row,
and `roadmapstore.h` states that **migration does not write one** — only an
id-allocating append does. So a migrated-but-never-appended project reported a
store high-water of 0 while holding every id in its file, and any
`file_highest_id > 0` pinned the warning on permanently.

Album_Builder hit it with 357 items and read the envelope as "the migration
silently wrote nothing", which is the natural reading of
`file_ahead_of_store: true` beside `store_high_water: 0`. This suite's own
fixture hit it with one item; that is the baseline firing ANTS-4633 filed and
could not account for, and it is why INV-4's case had to assert the witness was
*unchanged* across a hand flip rather than absent.

**Both reports proposed a prefix mismatch, and the prefix was never at fault.**
`RoadmapFoldIn::maxDeclaredId()` filters strictly by prefix, so a wrong prefix
would have zeroed `file_highest_id` as well and fired nothing at all. The two
numbers disagreeing is itself the proof that the prefix resolved correctly.

The high-water mark is now the **higher** of the allocator's counter
(`idHighWater`) and the ids the items actually hold (`maxAllocatedId`, added by
ANTS-4631 for this class of problem). Either alone is wrong in a different
direction: the counter is absent until the first append, and the item-derived
figure sits below the counter once an allocated id is deleted. A staleness
warning whose whole value is being rare must not fire on a healthy project.
