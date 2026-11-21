# ANTS-3578 — Delta-envelope foundation: content-hash "changed-since" MCP responses (ANTS-3534 / 3544 / 3546) — review loop log

Record for [`../specs/ANTS-3578-delta-envelope-foundation-content-hash.md`](../specs/ANTS-3578-delta-envelope-foundation-content-hash.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1** (2026-07-19) — 3 lanes (foundation / codebase_index+auto-304 /
append-verb+scope), strong-model cold read, all findings verified against source.
`CRITICAL 1 · HIGH 4 · MEDIUM 4 · LOW/INFO 5`. **Not converged** — the draft's
"easy cursors / nearly-free" framing did not hold; findings drove the redesign
above (surfaced to the user, who chose the full corrected build).

- **CRITICAL — client cannot verify a delta.** `etagFor` hashes the canonical Qt
  body before injection; the client (a different serializer) can't reproduce it. →
  Redesigned to a server-side content-hash diff; client applies the delta, never
  re-hashes (INV-3). Client-verifiability claim dropped.
- **HIGH — codebase_index `StaleSet` is cache-anchored, not base-anchored**
  (serve() overwrites the single-slot cache every call). → Delta now diffs vs the
  retained per-etag base; `StaleSet` is internal-only (INV-8).
- **HIGH — auto-304 couldn't serve its small-repeat motivation** (spill is
  large-body only). → Repetition-triggered spill-on-repeat for small bodies (INV-11).
- **HIGH — feedback tail cursor unsafe** (`compactResolved`/`migrateV2` rewrite
  above the watermark). → Content-hash diff, position-independent (INV-6/13).
- **HIGH — dispatch seam / full-body-etag threading broken.** → § 2.1c pins the
  order: full-body etag first, exact-match 304 second, delta third.
- **MEDIUM ×4** — INV-1 now includes Adapter B's shape; § 4 base is hash-sets not
  body-snapshots, hard-capped + named; the etag→base map is first-class in § 2.1b
  (not an open question); changelog `[Unreleased]` handled by content diff, no dated
  boundary needed.
- **LOW/INFO** — staleness is now automatic (content-addressed etag, INV-10, so no
  per-file mtime); caps named (`kDeltaBaseCap`/`kEmittedEtagCap`/`kDeltaBaseTtlMs`);
  FileOutline emission-order determinism flagged (§ 8 / INV-7); `m_emittedEtagLru`
  value trimmed (§ 8); § 7 adds the mcp-tools.md quick-ref.

**Loop 2** (2026-07-19) — 3 lanes, cold re-read of the redesigned spec (no
fix-briefing). `CRITICAL 2 · HIGH 3 · MEDIUM 7 · LOW 7 · INFO 1`. **Not
converged — and diverging: loop 2 surfaced deeper structural mismatches between
the uniform "dominant keyed array" model and the verbs' real responses.** The
uniform content-hash design is sound *in the abstract* but fights the grain of
what the four verbs actually emit:

- **CRITICAL — a delta silently drops non-array envelope-field changes.** `etagFor`
  hashes the WHOLE envelope but the delta covers only the dominant array; a change
  to `next_offset` / `total_count` / lane summaries flips the etag, takes the delta
  path, and reports no element change → client keeps stale envelope fields. Breaks
  INV-4 / "pure optimisation." (verified: roadmap_query `next_offset`,
  codebase_index counts.)
- **CRITICAL — codebase_index's flagship (session_orient summary) response has no
  `files[]` array.** `query()` emits `files[]`+symbols only in lane-selector mode
  (`codebaseindex.cpp:535`); summary mode (n==0/laneFiles, what session_orient
  calls) emits lanes/roles/paths/counts, no per-file symbols (`:403-490`). The
  Adapter-A extractor cannot bind on the high-value case → INV-9 unbuildable.
- **HIGH — feedback_query returns no `findings[]` array** — a text `delta` blob +
  `mapped_ids` + `last_maintainer_line` (`remotecontrol.cpp:12061`), and already
  only the un-triaged tail (ANTS-1961). No array to iterate; needs an unbudgeted
  response reshape, or feedback drops out of Adapter C.
- **HIGH — duplicate keys** (copy-pasted roadmap id, identical changelog
  (version,category,text)) collapse in `map<key,hash>`; a changed twin is missed.
  INV-14 covers only keyless.
- **HIGH — the 16-hex `fullEtag` cannot reconstruct the 64-hex spill handle**
  (`etagFor` truncates to hex16; spill name is full sha256) — § 4/§ 8's
  "recompute the handle from fullEtag" is impossible; the LRU must carry the full
  handle.
- **MEDIUM ×7** — volatile envelope fields (`generated_at_ms`) defeat both the
  exact-match 304 and the auto-304 (never a byte-identical repeat → never fires);
  `compact` recurses into `delta` and prunes empty `added`/`removed` sub-arrays;
  the `(arrayField,keyOf)` extractor registry is unspecified; Adapter-B vs step-4
  precedence undefined; spill-on-repeat can evict an un-paged large offload;
  § 5 mis-ranks feedback as "simplest"; feedback's "whole list re-sent" motivation
  is inaccurate.
- **LOW/INFO ×8** — RAM understated (QString/QHash overhead → multi-MB not
  hundreds of KB); `canonical(element)` undefined; per-poll re-hash CPU omitted;
  lane-mode symbol truncation would flip a per-file hash; INV-9 no threshold;
  key-change→remove+add unstated; §2.x↔§3 restatement (acceptable).

**Assessment:** two cold loops have converted "three easy token-savers" into a
heavy, invasive cross-cutting change whose uniform model fights each verb's real
response shape (no array in codebase_index summary / feedback; volatile + paging
fields everywhere). Per the shortest-correct + push-back-when-simpler rules, the
scope was re-surfaced to the user rather than patched through more loops. _Loop 3
pending that decision._
