# ANTS-2160 — Per-project Ants settings file (`.ants/project.json`) — review loop log

Record for [`../specs/ANTS-2160-per-project-ants-settings-file.md`](../specs/ANTS-2160-per-project-ants-settings-file.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-24).** Two independent cold reviewers
  (spec-vs-code; spec-vs-standards). Verified + folded: spec routing
  also branches to `docs/phases/` and a third consumer
  (`current_state.spec_path`, § 1 item 5) was unlisted (added to § 1,
  § 2.3; phases scoped out); `scanLayout` receives a canonical cwd;
  `source_roots:[]` / `docs_dir:""` semantics and full
  replacement-vs-union pinned (INV-10/11); dedup key + dir-vs-file
  validation added (INV-9/INV-5); Status token, numbered sections,
  per-INV `*Test:*` clauses, `features;fast` label +
  "fails-against-pre-fix" line + named bundle (`test_claude`),
  `Layman:` line, § Cross-doc impact, § Security boundaries added for
  `docs/standards/specs.md` conformance; citation lines corrected
  (spec_log `:9449`, spec_query `:11942`).
- **Loop 2 (2026-06-24).** Verified + folded: `candidates()` /
  `walkDocs()` each have THREE call-sites (build / stale / refresh), so
  a build-only hook would revert to defaults on a warm-cache refresh —
  resolved by loading settings INSIDE those helpers and locking it as
  INV-12; the proposed `load()` mtime cache was REMOVED for simplicity,
  dropping the former cache invariant; mixed valid/invalid arrays
  (partial keep, INV-4) and JSON-`null` values (INV-6) defined; § Tests
  trimmed to a `Test N ⇒ INV-N` index; an internal cross-reference to
  § Out of scope was corrected.
- **Loop 3 (2026-06-24).** Verified + folded: the load-time path check
  is `PathValidation::isInsideProject` (the disk-derived-config helper),
  not `validatePath` (the MCP-arg envelope validator) — § 2.2 / § 8 and
  INV-4/5/8 reworded accordingly; § 4 + the walk INVs now state the
  observable is the served JSON `files[]` (since `candidates()` is an
  unexported static) and that `serve`-driven cases pass a
  `cachePathOverride` to avoid real-cache test coupling; recommended
  sections reordered (Out of scope ahead of Cross-doc impact / Security)
  per `specs.md` § 3/§ 4, with all internal § cross-references updated;
  `scanLayout` described by its real `absoluteCwd` parameter; § 1 item 7
  citation split into constants vs probe; the "verified by the agent
  map" meta-citation dropped.
- **Loop 4 (2026-06-24).** Verified + folded: the walk invariants
  (INV-2/9/11/12) and § 4 cited a served `files[]` array that
  `CodebaseIndex::query()`'s summary path does NOT return (it emits
  `file_count`/`lanes`/`languages`/`roles`/`empty`; the full `files[]`
  lives only in `toJson()` and the `lane=` selector). Re-anchored those
  test surfaces to the exported pure builders `build()` / `refresh()`
  and their in-memory `Index.files` (+ `refresh`'s `*refreshedOut` for
  INV-12), removing the now-unnecessary `serve` / `cachePathOverride`
  path. The cross-reference/consistency/conformance lane returned zero
  findings.
- **Loop 5 (2026-06-24).** Zero CRITICAL/HIGH; three MEDIUM precision
  nits folded: INV-10's test surface still said "served output" (missed
  in the loop-4 re-anchor) → now `build()`'s `Index.files`; INV-5 now
  pins the dir-vs-file check to the consumer (`candidates()` /
  doc-finders) and gives an observable discriminator (file-typed
  `source_roots` → fallback to `src`, non-empty `Index.files`, vs a
  walked-to-empty bug); INV-9 now states its dedup is NEW logic (current
  `candidates()` does none) with a count-2-fails discriminator.
