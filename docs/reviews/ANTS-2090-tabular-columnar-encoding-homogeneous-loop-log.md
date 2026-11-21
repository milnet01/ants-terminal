# ANTS-2090 — Tabular (columnar) encoding for homogeneous-array MCP replies — review loop log

Record for [`../specs/ANTS-2090-tabular-columnar-encoding-homogeneous.md`](../specs/ANTS-2090-tabular-columnar-encoding-homogeneous.md) § Cold-eyes loop log.

## Cold-eyes loop log

Independent cold reviewers (no author intent), each briefed on the spec +
cited code (`mcpprojection.{cpp,h}`, `claudeintegration.cpp`,
`remotecontrol.cpp`) + cross-ref docs. Every finding verified against source
before fixing; loops 2–3 briefed cold (no prior-fix list).

**Loop 1 — 0C / 2H / 3M / 4L, all verified & fixed.**
- H1 — gating ambiguity: spec implied a tool-name predicate but the verb list
  diverges from `isFieldProjectionTool`/`isOffloadEligible` (`find_caller` in
  neither). Fixed §2.1/§2.4: `tabularize` is predicate-free (self-guarding per
  array); the verb list is advertise-only.
- H2 — source-scrape INVs anchored on line numbers (drift hazard). Fixed
  INV-1/INV-8/§6 to assert ordering **by symbol**, not line number.
- M1 — request arg `format` collides in name with `roadmap_query`'s `format`
  *response* field. Renamed the arg to `encoding` throughout.
- M2 — length-preservation unstated. Added to INV-3 (`__rows__` one entry per
  element; sibling counts stay consistent).
- M3 — `fields=` composition unstated (tabularize does not bail on `fields=`,
  unlike `appendReadHints`). Documented in §2.4 + added a test case.
- L1 grep guidance noise; L2 missing-key⟺null collapse; L3 dead doc-links for
  code-only ANTS-2091/2085; L4 NDJSON/TOON fork left unexplained — all folded.

**Loop 2 — 0C / 1H / 1M / 2L, all verified & fixed.**
- H — the loop-1 M1 fix mis-cited the `format` field (`remotecontrol.cpp:247`
  etc. are `roadmap_log` refusal paths, value `"pass-headings"`). Corrected to
  `remotecontrol.cpp:4423`, `cmdRoadmapQuery`, value `"github-task-list"`
  (ANTS-1428). *(The loop catching a citation error introduced by the previous
  loop's fix is exactly why loop 2+ runs cold.)*
- M — INV-7/§2.3 promised lexicographic `__cols__` but the union-across-elements
  order isn't lexicographic unless the accumulator sorts. Pinned §2.3 step 2 to
  a sorted accumulator and added a column-order test to INV-7.
- L — wire example invented a `count` sibling `roadmap_query`'s list shape
  doesn't emit (dropped it); §6 count-test phrasing generalised.

**Loop 3 — 0C / 0H / 0M / 0L / 2 INFO. Clean pass.**
All citations (dispatch order, `remotecontrol.cpp:4423`, the `fields=` bail,
the null-prune, predicate membership) re-verified against source; example
zips back and columns are lexicographically sorted; structure conforms to
`specs.md`. INFO-1 (example headline still read "TOON") folded; INFO-2
(placeholder slug) left as illustrative. Converged.
