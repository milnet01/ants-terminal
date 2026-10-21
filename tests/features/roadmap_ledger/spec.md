# Feature: roadmap_query mode:"ledger" (ANTS-5516)

`mode:"ledger"` reports in-progress items nobody has touched for
`stale_days` (default 14) and commits in `range` (default: newest stable
tag..HEAD) whose subject names no roadmap id. The pure half lives in
`src/roadmapledger.cpp`; these tests cover it.

- **`RoadmapLedger.StaleIsAtLeastStaleDaysUntouchedOldestFirstUndatedFirst`**
  — an item untouched exactly `stale_days` days is stale, one day fewer is
  not; an item with no date is stale and listed first; the rest run oldest
  first.
- **`RoadmapLedger.CommitsWithoutIdMatchTheWholeIdOnly`** — an id anywhere in
  the subject counts; an id inside a longer word (`XANTS-12`, `ANTS-12a`) does
  not.
- **`RoadmapLedger.NewestStableTagComparesNumbersAndSkipsSuffixes`** —
  `v0.7.114` beats `v0.7.99`; a suffixed tag is not a release; no stable tag
  gives an empty name.
