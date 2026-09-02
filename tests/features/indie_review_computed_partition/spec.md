# indie_review_computed_partition — a computed partition beats an empty one

ANTS-3709. Every `indie_review_partition` deriver read a *document* — the
`.indie-review/partition.json` override, CLAUDE.md's `## Module map` of
`- <name> — <summary>` subsystems, and (ANTS-3507) its `- <path> — <desc>`
file-list variant. A project that describes its layout in prose therefore
got `{lanes:[], sparse_partition:true}` even though the server already held
the file tree: `codebase_index` listed all 195 source files in the same
session, and `.ants/project.json` declared `source_roots`.

The cost lands on the orchestrator, which hand-derives the partition
instead — 16 lanes built from `find` + `wc -l` on the reported run. That is
the single most expensive orchestrator step in a sweep and the one the verb
exists to remove, and a hand-built partition is unreproducible run-to-run,
which quietly breaks the "an issue not raised again proves the fix held"
property multi-loop review depends on. Reported by DOOM Ants
(`DOOM_Ants_Ants_MCP_Feedback.md`, 2026-07-28).

Deliberately NOT built: line-range sub-lanes for very large files. `Lane`
has no line-range field, and adding one reaches into brief assembly — a
file-count split covers the "one useless 195-file lane" case at a fraction
of the surface.

## INVs

- **INV-1** (prose layout still partitions) — with no override, no
  `## Module map`, and source files under two directories,
  `IndieReviewEngine::deriveComputedPartition` returns one lane per
  containing directory, named after that directory.
  *Test:* `Inv1ProseLayoutYieldsDirectoryLanes`.
  *Breaks when:* the deriver reads a document instead of the tree.

- **INV-2** (declared source_roots bound the walk) — when
  `.ants/project.json` declares `source_roots`, only files under those
  roots are partitioned. With nothing declared and no `src/`, the project
  root is walked.
  *Test:* `Inv2DeclaredSourceRootsBoundTheWalk`.
  *Breaks when:* the walk hardcodes `src/`.

- **INV-3** (a big flat directory splits) — a directory holding more than
  25 indexable files becomes numbered sub-lanes (`dir (1/N)` …), so a flat
  project does not collapse into one unreviewable lane. Paths are sorted,
  so the split is deterministic across re-derivations.
  *Test:* `Inv3LargeDirectorySplitsIntoSubLanes`.
  *Breaks when:* the per-lane cap is dropped.

- **INV-4** (the >1-lane guard survives) — a project whose files all sit in
  one small directory yields an empty computed partition, so the caller
  keeps its `sparse_partition` refusal path rather than being handed a
  single lane dressed up as a partition.
  *Test:* `Inv4SingleDirectoryYieldsEmpty`.
  *Breaks when:* the guard is relaxed to `>= 1`.

- **INV-5** (a declared partition is never shadowed) — a parseable module
  map still wins: `derivePartition` is unchanged, and the MCP handler only
  reaches for the computed partition when the map yielded ≤1 lane. When it
  does, the envelope carries `derived: true` plus `derived_from`, so a
  computed guess is never passed off as a declared partition, and the
  existing `sparse_partition_hint` still fires (committing
  `.indie-review/partition.json` remains the fix).
  *Test:* `Inv5HandlerLabelsDerivedAndKeepsHint` (source-grep over
  `cmdIndieReviewPartition`).
  *Breaks when:* the fallback overwrites a good partition, or ships
  unlabelled.

- **INV-6** (the suffix filter reports what it drops) — the walk admits only
  suffixes `CodebaseIndex::isIndexableSuffix` accepts, which is deliberately
  narrower than "source" so that count → outline → symbol query cover the
  same files. When an `UnassignedSources *` is passed,
  `deriveComputedPartition` records every file that gate skipped, as a total
  and per lowercased suffix. Noise directories and generated sources are
  eliminated BEFORE the gate, so build output cannot bury the signal.
  *Test:* `Inv6SuffixFilteredFilesAreReported`.
  *Breaks when:* the filters are reordered so the gate sees noise, or the
  suffix list is widened instead — which would count files the outline
  cannot read, the drift the in-step rule exists to prevent.

- **INV-7** (the reporter is optional) — the one-argument form still works
  and is what the dialogs call, so reporting is additive.
  *Test:* `Inv7NullReporterIsHarmless`.
  *Breaks when:* the out-param stops defaulting.

- **INV-8** (the handler surfaces it, for whichever partition the reply
  carries) — `cmdIndieReviewPartition` passes the reporter to the computed
  walk and emits `unassigned_count` / `unassigned_by_suffix` /
  `unassigned_reason` / `unassigned_hint`. The reported set always describes
  the partition in the envelope: the computed walk's reporter is used on the
  `derived` path, and `unassignedForLanes` answers the same question about the
  declared lanes otherwise (ANTS-4786). A count describing the other partition
  would be worse than no count, which is what the original `derived` gate
  protected — the gate is now a branch rather than a silence.
  *Test:* `Inv8HandlerReportsUnassignedForTheCarriedPartition` (source-grep
  over `cmdIndieReviewPartition`).
  *Breaks when:* the reporter is not passed — the counts then stay zero and
  the envelope is silent again while every field still exists.

- **INV-9** (a declared partition is measured for coverage too, ANTS-4786) —
  `unassignedForLanes` walks the same source roots under the same noise and
  generated-output filters, subtracts what the lanes cover, and returns the
  remainder by suffix. It applies NO suffix filter: on the computed path the
  uncovered set is exactly what the suffix gate dropped, so one rule serves
  both paths and only the cause differs. The optional sample is sorted before
  it is capped, so two runs over one tree name the same files.
  *Test:* `Inv9DeclaredPartitionCoverageIsMeasured`,
  `Inv9SampleIsDeterministicAndCapped`.
  *Breaks when:* the coverage walk and the partition walk resolve different
  source roots, or the sample is capped in directory-walk order.
