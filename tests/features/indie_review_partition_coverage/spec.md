# The committed review partition keeps covering every source file

**Why this exists.** ANTS-4793 committed `.indie-review/partition.json` — 32
lanes naming all 313 files under `src/` exactly once — because the module map
`docs/subsystems.md` left 189 of them in no lane at all, `mainwindow.cpp`
among them. That fixed the coverage gap. **Nothing kept it fixed.**

The partition is a static list of paths. The day a source file is added it
belongs to no lane, and **the failure is silent**: `indie_review_partition`
reports the override it was handed and has no way to know a file is missing
from it. Every subsequent review sweep then skips that file while reporting a
clean partition — which is precisely the shape ANTS-4785 and ANTS-4786
describe, recreated one level above the map they describe.

**No review run can catch this class**, which is what makes it worth a static
check. A lane reviews the files it is given; a file in no lane is dispatched
to nobody, so there is no run in which its absence shows up as anything.

**Why the file set comes from the engine and not from this test.** What counts
as a reviewable source file is already decided in three places
`IndieReviewEngine::deriveComputedPartition` consults — the noise-directory
filter, the generated-file filter, and `CodebaseIndex::isIndexableSuffix`,
which is deliberately narrower than "source" and excludes shell and CSS. A
test replicating that would be a fourth copy, and would drift from the
engine's answer the first time any of the three changed. Calling the engine
means a change to what counts as reviewable moves this test with it.

## The invariants

**INV-1 — coverage.** Every file `deriveComputedPartition` places in a lane
when walking the real tree also appears in some lane of the committed
`.indie-review/partition.json`. This is the one that fires when a source file
is added.

**INV-2 — no dangling path.** Every path the committed partition names exists
on disk. This is the one that fires when a file is renamed or deleted, which
leaves a lane pointing at nothing and is equally invisible to a review run.

**INV-3 — a partition, not a covering.** No path appears in two lanes. A
duplicate is not harmful to review in itself, but it is the signal that the
author lost track of which lane owns what — the state the whole file exists to
prevent.

## Direction, and what is deliberately not asserted

**INV-1 is one-directional on purpose.** It requires the engine's set to be a
subset of the committed set, never the reverse. A lane may legitimately name a
path the engine's walk does not return — a header the suffix filter drops, a
file under a root the walk does not cover — and forbidding that would make the
committed partition unable to be *more* complete than the derivation it
replaced. INV-2 already stops that permission from covering a path that is
simply wrong.

**Lane sizing is not asserted.** ANTS-4793 records that `claude-integration`
is 16,972 lines and cannot be split, because `Lane` carries no line-range
field. A size assertion would either fail permanently on that lane or be
written to exempt it, and neither says anything about coverage.

**The partition's *content* is not asserted** — which file belongs in which
lane is a judgement, and this test checks only that every file belongs in one.
