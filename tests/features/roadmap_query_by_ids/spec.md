# Feature spec: roadmap_query multi-item `ids` selector (ANTS-1726)

User pain (2026-05-21): when continuing a bundle, a session that knows N
related ids (e.g. ANTS-1719..1724) but not their section had to either
pull `status:all` (~12 K tokens) and scan, or fire N individual
`roadmap_query(id=…)` calls — both expensive. ANTS-1726 adds an `ids`
array arg so N bullets come back in a single small call. Pairs with the
already-shipped singular `id` selector (ANTS-1856).

## Surface

`roadmap_query(caller_cwd, ids:["ANTS-1719","ANTS-1721","ANTS-1853"])` →
`{ok, bullets:[matching items, document order], count, path, ids,
matched_ids, missing_ids}`. Body included by default. Bypasses the
`status` filter and pagination (same as `id`). Mutually exclusive with
`id`, `section`, and `mode:section_index`.

## Invariants

- **INV-1 / ids read + per-element hygiene.** `cmdRoadmapQuery` reads
  `ids` from `req` as a JSON array, coerces each element to string,
  truncates to 64 bytes, and replaces C0 control bytes with `?`. Empty
  array (zero elements) → behaves identically to absent `ids` (falls
  through to the normal list path). Source anchor: `ANTS-1726` in
  `remotecontrol.cpp`.
- **INV-2 / document-order match.** The ids branch iterates
  `m_roadmapCacheBullets` once (document order) and keeps any bullet
  whose id is in the requested set. Result preserves the roadmap's
  document order, not the order ids were passed. Source anchor: the
  `idsArg`-driven match loop precedes the status filter +
  `PaginationEngine::pageBullets` call.
- **INV-3 / matched / missing accounting.** The envelope carries
  `matched_ids:[…]` (the subset of input ids that matched at least one
  bullet) and `missing_ids:[…]` (the rest), so a caller can spot
  typos/stale ids without diffing the input against `bullets[].id`.
  Source anchor: `out["missing_ids"]` + `out["matched_ids"]` in the
  ids branch.
- **INV-4 / combos rejected.** `ids` + `id`, `ids` + `section`, and
  `ids` + `mode:section_index` each return
  `{ok:false, code:"bad_mode_combo"}`. The same rationale as singular
  `id` — multi-id lookup scans the whole roadmap; sub-slice and section
  discovery do not compose. Source anchor: `ids selector` rejection
  messages in `cmdRoadmapQuery`.
- **INV-5 / body default-on, strip on explicit opt-out.** Identical to
  the singular `id` branch (ANTS-1856 INV-6): body kept unless the
  caller explicitly passed `include_body:false`. Source anchor:
  `if (hasIncludeBodyArg && !includeBody) rcStripBodyFields(matches)`
  in the ids branch.
- **INV-6 / schema advertises ids.** The `roadmap_query` tool
  descriptor registers an `ids` property of type array with item type
  string. Source anchor: `props["ids"]` + `ANTS-1726` in
  `claudeintegration.cpp`.
- **INV-7 / dispatch forwards ids.** The mainwindow `roadmap_query`
  provider forwards `ids` to `cmdRoadmapQuery`. ANTS-3422 retired the
  hand-maintained per-arg forward (the silent-drop hazard ANTS-1586 +
  ANTS-1856 kept re-fixing) in favour of a verbatim
  `rcDelegate(&RemoteControl::cmdRoadmapQuery)` forward that passes the
  whole args object through, so `ids` reaches the handler by
  construction. Source anchor: `rcDelegate(&RemoteControl::cmdRoadmapQuery)`
  + `ANTS-3422` in `mainwindow.cpp`.
- **INV-8 / unknown ids stay non-error.** A request whose every id is
  unknown is `{ok:true, count:0, bullets:[], missing_ids:[all],
  matched_ids:[]}` — mirrors the singular `id` INV-4 contract.
- **INV-9 / dedup preserves first occurrence.** Duplicate ids in the
  input array are de-duped (each id matched at most once); preserves
  the FIRST occurrence's position when accounting for `matched_ids` /
  `missing_ids` ordering relative to the input list.
- **INV-10 / array size cap.** `ids` is hygiene-capped at 100 elements.
  Beyond that the call refuses `{ok:false, code:"bad_args"}` so a
  malformed/looping caller can't blow the cache scan budget.
- **INV-11 / string coercion (ANTS-3541).** A caller who passes `ids`
  as a comma/whitespace-joined STRING (`"ANTS-1719,ANTS-1721"`) has it
  coerced to the array form (split on `[,\s]+`) instead of silently
  falling through to the full unfiltered list. Saves the retry
  round-trip. Source anchor: the `idsVal.isString()` branch + the
  `[,\s]+` split in `cmdRoadmapQuery` (`ANTS-3541`).
- **INV-12 / present-but-invalid refuses (ANTS-3541).** A present `ids`
  that is neither array nor string (number/bool/object) refuses
  `{ok:false, code:"bad_args"}`; likewise a NON-EMPTY `ids` (array or
  coerced string) whose every element is non-string / empty /
  control-only refuses rather than returning the full list. An empty
  array / empty string keeps the documented absent fall-through
  (INV-1), guarded by `idsPresentNonEmpty`. Source anchors:
  `"got a non-string scalar"` and
  `"ids contained no valid id after hygiene"` in `cmdRoadmapQuery`.
