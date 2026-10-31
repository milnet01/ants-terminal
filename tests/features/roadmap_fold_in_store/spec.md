# Feature: a fold-in on a store-served project writes through the store

ANTS-5229. `RoadmapFoldIn::insertBlock` spliced its block into `ROADMAP.md`. On a
project the store serves, that file is a rendered output, so the bullets existed
nowhere in the store. The next `roadmap_log` write then refused: ANTS-4141's guard
would not drop an id the store never held.

User decision, 2026-09-18: on a migrated project the fold-in writes through the
store, with the same write sequence `roadmap_log` uses, and the file is re-rendered
from it. A project served from markdown keeps the splice.

`test_roadmap_fold_in_store.cpp` checks every invariant.

**INV-1 — the next write is not refused.** After a fold-in on a served project,
`RoadmapWrite::commitAndRender` returns `Ok`.

**INV-2 — the block's bullets are store rows.** Each has its id, status, headline,
kind, source and lanes as the block gives them, is dated today, and appears in the
re-rendered file.

**INV-3 — an open bullet the block gave no Layman gets one.** The block comes from a
tool, and the render refuses an open item with none. A `**Layman:**` line in the
block is kept.

**INV-4 — placement.** A block's `###` heading becomes a section under the named
heading, and later sections keep their order and their items. Folding in again under
the same `###` heading adds to that section. A block with no heading files its
bullets in the named section.

**INV-5 — prose is kept.** A narrative block, a heading and prose with no bullets,
is in the store and survives a later write.

**INV-6 — a refusal writes nothing.** An unknown heading, or an id the store already
holds, returns false and leaves the file and the store as they were.

**INV-7 — markdown projects are unchanged.** On a project the store does not hold,
the block is spliced into the file after the heading.
