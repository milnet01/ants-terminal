# indie_review_* — citations that resolve, files worth reviewing, lanes that admit their size

ANTS-4095 / ANTS-4096 / ANTS-4100. Three defects reported from two
projects, all of the same shape: the verb returned a well-formed,
plausible answer that was wrong, and nothing in the envelope said so.

**ANTS-4095.** `indie_review_corroborate` read 8 real lane reports and
returned `findings:[]`. Every citation in them was a bare basename
(`d_main.c:1049`) because that is what a brief shows a reviewer, while the
file lives at `linuxdoom-1.10/d_main.c`. `extractFileLineCitations`
resolves a citation by joining it to the project root, so all of them
canonicalised to nothing and were dropped. The empty result is
indistinguishable from "no two lanes agreed", which is a plausible and
reassuring thing to read, so a caller accepts it.

The reporting session named `total_input_bytes:0` as the tell. It is not:
that field is 0 **by design** on the `reports_dir` path (ANTS-1282 —
the orchestrator never paid the context cost). Designing to that claim
would have produced a diagnostic that fires on every healthy disk-path
call. The bold-wrapping and `423-424` range shapes it also named already
parsed correctly.

**ANTS-4096.** Two defects in the ANTS-3709 computed fallback. (1) It
filtered generated files by *prefix* only (`moc_`/`ui_`/`qrc_`), so a
`shaders/` directory partitioned into 22 `*.spv.h` byte-array headers and
none of the 19 hand-written GLSL sources beside them — which were absent
from `CodebaseIndex::isIndexableSuffix` entirely, even though
`find_definition` (ANTS-3558) and `file_outline` (ANTS-3800) have both
admitted shader stages since. (2) `suggestedMerges` skipped the ANTS-3507
file-list fallback's boilerplate summary but not the ANTS-3709 computed
one, whose lanes are near-identical BY CONSTRUCTION; one run emitted 38
suggestions, including merging two disjoint slices of one directory.

**ANTS-4100.** A module map listing one directory yields one lane
covering the whole application (96 files, 21k LoC, spanning crypto, a
vault, importers, services and 40 UI modules). That is a defensible thing
for the verb to return and an indefensible thing to return silently: an
empty `suggested_merges` reads as "this partition is fine". Splitting the
same project by cohesion surfaced 3 critical and 11 high findings the
single lane missed.

Deliberately NOT built: recursion into a too-coarse lane. The reporting
session offered it as the better half of a two-part fix and the cheap half
as sufficient — "silence is the real problem, not the coarseness". A
verb that re-partitions on its own would be guessing at cohesion from
directory names, which is the same mistake one level down.

## INVs

- **INV-1** (a unique basename resolves) — a citation naming a file
  without its directory resolves to that file when the basename occurs
  exactly once among the project's reviewable sources, and the resulting
  finding carries the full project-relative path.
  *Test:* `Inv1UniqueBasenameResolves`.
  *Breaks when:* resolution only joins the citation to the project root.

- **INV-2** (an ambiguous basename does not) — when two reviewable files
  share a basename, a citation naming it resolves to neither. Corroborating
  two lanes that cited different files would manufacture agreement.
  *Test:* `Inv2AmbiguousBasenameStaysDropped`.
  *Breaks when:* the index keeps first-wins instead of poisoning the key.

- **INV-3** (resolution failure is observable) — when reports were read
  and citations were matched but none resolved, the envelope's
  `citations_seen` is non-zero and `citations_resolved` is 0. The pair is
  what distinguishes a parse failure from a genuine empty result;
  `total_input_bytes` is not, being 0 by design on the `reports_dir` path.
  *Test:* `Inv3UnresolvedCitationsAreCounted`.
  *Breaks when:* the counters are dropped, or count per-regex-pass so a
  token matched by both passes inflates `citations_seen`.

- **INV-4** (shaders are reviewable, generated output is not) — a
  directory holding both `*.spv.h` generated headers and hand-written
  `.comp`/`.frag`/`.vert` sources partitions to the sources and excludes
  every generated header.
  *Test:* `Inv4ShadersInGeneratedHeadersOut`.
  *Breaks when:* the generated filter tests prefixes only, or the
  indexable-suffix gate drops shader extensions.

- **INV-5** (a fallback summary never drives a merge) — lanes carrying the
  ANTS-3709 computed-partition summary template yield no merge
  suggestions, however similar their summaries are.
  *Test:* `Inv5ComputedFallbackSuggestsNoMerges`.
  *Breaks when:* the guard keys on one fallback's wording rather than the
  shared `grouped by` stem.

- **INV-6** (a lane's size is measured, not assumed) — `laneFileCount`
  expands a `sourcePaths` entry that names a directory, counting the
  reviewable files beneath it rather than returning 1.
  *Test:* `Inv6LaneFileCountExpandsDirectories`.
  *Breaks when:* the count is `sourcePaths.size()`.

- **INV-7** (a real declared partition is not flagged) — a lane naming a
  handful of files stays under `kMaxReviewableFilesPerLane`, so this
  project's own module-map lanes (largest: 14 files) never trip the
  signal.
  *Test:* `Inv7SmallLaneIsNotCoarse`.
  *Breaks when:* the threshold is lowered to where a declared partition
  fires, which would train callers to ignore it.
