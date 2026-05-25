# Feature spec: roadmap_query single-item `id` selector (ANTS-1856)

User pain (2026-05-25): asking for one roadmap item (e.g. ANTS-1853)
meant a `roadmap_query` that returned 15 of 214 bullets — the wanted
item wasn't even on the first page — forcing a `grep` of ROADMAP.md or
a `section_index` page (~13 K tokens) just to find a slug. ANTS-1856
adds an `id` arg so "show me ANTS-NNNN" is one small call.

## Surface

`roadmap_query(caller_cwd, id:"ANTS-1853")` → `{ok, bullets:[the
item], count, path, id, found}`. Body included by default. Bypasses the
`status` filter and pagination. Mutually exclusive with `section` and
`mode:section_index`.

## Invariants

- **INV-1 / id read + hygiene.** `cmdRoadmapQuery` reads `id` from
  `req`, truncates to 64 bytes, and replaces C0 control bytes with
  `?` (mirrors the bad_status / bad_section echo hygiene). Source
  anchor: `ANTS-1856` in `remotecontrol.cpp`.
- **INV-2 / exact match bypasses status + pagination.** The id branch
  scans `m_roadmapCacheBullets` for an exact, case-sensitive id match
  and returns BEFORE the status filter and the
  `PaginationEngine::pageBullets` call, so an explicit id request
  returns that item regardless of lifecycle and is never paginated.
  Source anchor: the `if (!idArg.isEmpty())` block precedes the
  `ANTS-1247-INV-2/3` status filter.
- **INV-3 / case-only mismatch → bad_case.** When no exact match
  exists but a case-insensitive match does, the envelope is
  `{ok:false, code:"bad_case", canonical_id:"<exact>"}` — mirrors the
  `section=` bad_case contract (ANTS-1524). Source anchor:
  `canonical_id` + `Qt::CaseInsensitive` in the id branch.
- **INV-4 / unknown id → found:false.** A genuinely-absent id is not
  an error: `{ok:true, count:0, found:false}`. Source anchor:
  `out["found"]` set to `!matches.isEmpty()`.
- **INV-5 / combos rejected.** `id` + `section` and `id` +
  `mode:section_index` each return `{ok:false, code:"bad_mode_combo"}`
  (an id lookup scans the whole roadmap; section discovery / sub-slice
  do not compose). Source anchor: `id selector` rejection messages.
- **INV-6 / body default-on, strip on explicit opt-out.** The id
  branch keeps the bullet body by default and only calls
  `rcStripBodyFields` when the caller explicitly passed
  `include_body:false`. Source anchor:
  `if (hasIncludeBodyArg && !includeBody) rcStripBodyFields(matches)`.
- **INV-7 / schema advertises id.** The roadmap_query tool descriptor
  registers an `id` property. Source anchor: `props["id"]` +
  `ANTS-1856` in `claudeintegration.cpp`.
- **INV-8 / dispatch forwards id.** The mainwindow dispatch lambda
  forwards `id` into `req` (same silent-drop hazard ANTS-1586 fixed
  for include_body). Source anchor: `req["id"] = idArg` + `ANTS-1856`
  in `mainwindow.cpp`.
