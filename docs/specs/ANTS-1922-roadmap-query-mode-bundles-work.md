# ANTS-1922 — `roadmap_query mode:"bundles"` (work-bundle discovery)

**Status:** accepted + implemented on main (2026-06-24) — cold-eyes loops 1–6 folded (loops 1–3 2026-06-20, loops 4–6 2026-06-24); implementation + 13 feature tests (`tests/features/mcp_roadmap_bundles`) landed green. Flips to `shipped X.Y.Z` at the next release.
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-1922 (in-session-2026-05-29 orientation gap; reinforced 2026-06-03). Option (a) already shipped (`session_orient.active_bullets`); this spec covers Option (b).
**Composes with:** `session_orient` (ANTS-1922 Option a, `src/remotecontrol_state.cpp`).
**Refined by:** ANTS-2155 (2026-06-24) — the clustering edge described below (token-Jaccard ≥ 0.50) was reworked because it divided by the union and left long vocabulary-varied headlines as all-singletons on the real roadmap. The shipped edge now DENOISES clustering tokens (drops stop-words, length ≤ 2, pure numbers, file/path identifiers, and document-frequency > max(8, 40 %) corpus-common tokens) and is length-insensitive: ≥ 2 shared denoised tokens AND (overlap coefficient ≥ 0.5 OR ≥ 3 shared tokens OR a shared lane). The envelope additionally carries `total_bundle_count` + `bundles_omitted`. See `docs/standards/mcp-behavioural-notes.md` and `tests/features/mcp_roadmap_bundles` (the `Ants2155*` cases).

## 1. Problem

Picking "the next bundle of related to-dos" is the most common session-opening task, and today it costs 3+ MCP calls plus manual eyeballing:

1. `session_orient` returns one `active_bullet` + section counts + (since Option a) a flat top-20 `active_bullets` list — but flat, ungrouped.
2. A second `roadmap_query` to pull candidate bullets.
3. A third body fetch + manual cross-referencing to see which items belong together.

Lanes alone don't cluster thematic bundles: the diff-viewer bundle (ANTS-1874 / 1875 / 1864) spans **three different lanes**, so a group-by-lane view would scatter it. Two further traps seen in practice (2026-06-03, resolving "the next auto-switcher bundle"):

1. A 📋 item can be **already resolved** by a shipped sibling whose bullet status was never flipped (ANTS-1919's Option C shipped via ANTS-1926 but stayed 📋). A naive "group active items" view surfaces it as actionable work that's actually done.
2. A 📋 item can be **gated/blocked** (ANTS-1948 was gated-out by ANTS-1946), with that fact buried in the bullet body — so it reads as ready-to-implement when it isn't.

The grouping logic, the active-item predicate, and a headline-similarity primitive all already exist in `cmdRoadmapQuery` — this item wires them into one response.

## 2. Surface

A new `mode:"bundles"` on the existing `roadmap_query` verb (`RemoteControl::cmdRoadmapQuery`, `src/remotecontrol_roadmap_query.cpp`). No new MCP tool, no new file, no new external lib.

### 2.1 Dispatch + guards

- Add `"bundles"` to the mode allow-list in `cmdRoadmapQuery` (current set: `bullets` / `section_index` / `headline_only`; unknown modes refuse `code:"bad_mode"`).
- Add **three** combo guards for `bundles` — `bundles`+`section`, `bundles`+`id`, `bundles`+`ids` — each refusing `code:"bad_mode_combo"`, modelled on the three `mode == "section_index"` guards in the chain at `2806-2867` (`section_index`+`section` at `2806`, +`id` at `2826`, +`ids` at `2853`). Do **not** add `id`+`section` / `ids`+`id` / `ids`+`section` guards — those are mode-independent and already fire (`2833` / `2846` / `2860`); a `bundles` request never sets `id`/`ids` alongside `section` without one of the new mode guards tripping first. `bundles` is a whole-roadmap rollup, same rationale as `section_index`.
- `status` is **not** a selector in bundles mode — bundles always operates on the **active** subset (📋 planned + 🚧 in-progress); grouping shipped/all items has no use case. A passed `status` is ignored (not a refusal — friendlier; documented in the tool description).
- Insert the branch as an **early return** like `section_index`, placed *after* `m_roadmapCacheBullets` is populated (~3029) and after the combo guards, so it iterates the already-cached bullet array and returns its own envelope. ETag wrapping stays automatic (dispatch-layer `applyEtagPattern`).

### 2.2 Active-item source + predicate

Reuse the canonical active predicate already inline in the status filter (`src/remotecontrol_roadmap_query.cpp`): **active ⟺ status == 📋 OR status == 🚧**. Iterate `m_roadmapCacheBullets` (a `QJsonArray`; each object carries `id`, `status`, `headline`, `headline_oneline`, `lanes` (always present, possibly empty), `body` (truncated to `kRoadmapQueryBodyCap = 2000`), `section_slug`). Build a working vector of the active objects once.

### 2.3 Clustering — headline-token connected components

Bundles are the **connected components** of a graph over active items where an edge joins items `i`,`j` when their headlines are token-similar:

```
edge(i, j)  ⟺  jaccard(tokens(i.headline), tokens(j.headline)) >= 0.50
                AND shared-token-count >= 2
```

- **Why token-similarity, not lane:** lane-as-edge collapses every member of a broad lane (`claude`, `mcp`) into one mega-bundle, and the motivating diff-viewer bundle spans 3 lanes — token overlap is the signal that actually groups it. Lane is used only as a **label** (§2.4).
- **Why 0.50 (vs the 0.60 dup-detector bar):** bundling tolerates looser grouping than near-duplicate detection (which needs high precision to block a re-file). The `>= 2` shared-token floor still suppresses stop-word-pair false-fires.
- **Reuse:** factor the inner Jaccard scoring out of `rcComputePossibleDuplicates` (`remotecontrol.cpp`, currently hardcodes the 0.60 gate) into a small helper:
  ```cpp
  // Returns the token-Jaccard ratio of two already-tokenised headlines,
  // or -1.0 when fewer than `minShared` tokens overlap (the stop-word
  // floor). Caller applies its own threshold. (ANTS-1922 — 2nd call-site
  // of the rcComputePossibleDuplicates scoring, Rule-of-Three extract.)
  double rcHeadlineJaccard(const QSet<QString> &tokA,
                           const QSet<QString> &tokB,
                           int minShared = 2);
  ```
  `rcComputePossibleDuplicates` is refactored to call `rcHeadlineJaccard(...) >= 0.60` (no behaviour change); bundles mode (clustering) calls `>= 0.50`; the sibling-✅ check (§2.4) calls `>= 0.60`. The `>= 2` shared-token floor is enforced **solely** by the helper's `-1.0` sentinel (a sub-floor pair returns `-1.0`, which fails every caller's `>=` threshold) — callers apply only their ratio threshold and must **not** re-implement a separate count check. Where the invariants below say "with ≥2 shared tokens", that is a restatement of this sentinel behaviour, not a second test to code.
- **Tokens** come from `RoadmapParse::normaliseHeadline(headline)` then split on space — the same normalisation the dup-detector uses (`RoadmapParse::normaliseHeadline`, `roadmapparse.cpp`). Note exactly what that normalisation does and does **not** do: it lowercases, collapses runs of whitespace, and strips *trailing* punctuation only — specifically the chopped set `.` `,` `;` `:` `!` `?` plus a trailing space (in `RoadmapParse::normaliseHeadline`); a trailing `)` or `"` is **not** stripped, so a test author predicting tokens must account for that. It does **not** strip internal punctuation (`auto-switcher` stays one token) and there is **no stop-word list** — the `>= 2` shared-token floor is the *entire* stop-word-pair defence. Do not assume a stop-word filter exists.
- **Union-find** over the active vector: `O(n²)` edge scan, `O(n)` parent array. Each headline is normalised + tokenised **once** into a `QSet<QString>` up front (these same token sets feed the §2.4 sibling-✅ check, so they are built for every bullet, not only active ones).
- **Singletons are kept** — an active item with no edge is a 1-item bundle, so the view shows *all* active work, never silently hiding items.

### 2.4 Per-bundle + per-item annotations

For each component:

- `bundle_label` — derived from the tokens shared by `>= ceil(size/2)` of the members' headlines, after dropping a small fixed stop-word set (`a an the of for to and or in on vs with` + the em-dash) so a label never reads `"the for"`. This label stop-word set is **label-only** — it is hardcoded in the bundles branch and is deliberately distinct from tokenisation (§2.3 notes `RoadmapParse::normaliseHeadline` has no stop-word list; clustering uses the raw tokens, only labelling drops stop-words). Selection is fully deterministic: sort the qualifying tokens by **(frequency descending, token ascending)** and take the first 3, joined by a single space. Fallback chain when no token clears the `ceil(size/2)` bar (e.g. a singleton whose only tokens are all stop-words): the most-common lane among members (case-insensitive tally, tie-break ascending on the *case-folded* lane key — distinct from the emitted-`lanes` union below, which sorts the case-sensitive verbatim values); if members have no lanes at all, the final fallback is the lowest member id. The label is **cosmetic** — not INV-locked beyond "non-empty and deterministic" (INV-9).
- `lanes` — the distinct union of members' lanes, **sorted ascending** for determinism (for orientation; may be empty). The emitted union is **case-sensitive verbatim** — `claude` and `Claude` would appear as two entries; only the `bundle_label` *fallback tally* (above) folds case. (Roadmap lanes are author-written free text; v1 does not normalise their case in the emitted union.)
- `items[]` — one object per member, **token-frugal** (no full body): `{id, status, headline_oneline, lanes}` plus the two refinements below when they fire. **Items within a bundle are sorted by `id` ascending** (per the §2.5 id-ordering rule) so the envelope is byte-stable across runs (union-find walk order is otherwise unspecified).

Per-item refinement flags (omitted entirely when they don't fire, per the project's compact-envelope convention):

- **`possibly_resolved_by` / `possibly_resolved_score`** (refinement 1) — for each active item, take the **max** `rcHeadlineJaccard(item.tokens, shipped.tokens)` over the ✅ (shipped) subset of `m_roadmapCacheBullets`, reusing the §2.3 token sets (built for every bullet, not only active). The check uses **headline** tokens only (always full — the `kRoadmapQueryBodyCap` 2000-char body truncation is irrelevant here, it never reads the body). If that max `>= 0.60` (an explicit "you may have already done this" bar — higher than the 0.50 clustering bar because this warning wants precision; the `>= 2`-token floor still applies via the `rcHeadlineJaccard` sentinel, §2.3), attach `possibly_resolved_by` = the winning ✅ id and `possibly_resolved_score` = the score **computed exactly as the existing dup-detector does** — `static_cast<int>(jaccard * 100.0 + 0.5)` (in `rcComputeDuplicateIds`), giving a 60–100 integer. Reusing that one canonical rounding (not a generic `std::round`) keeps the score byte-identical to the dup-detector and stable across the `rcHeadlineJaccard` extraction (INV-11). This reuses the extracted helper on the cached token sets — **no `BulletRecord` re-parse**, so it does not call `rcComputePossibleDuplicates` (whose `QVector<BulletRecord>` signature would force a marshal/parse and break the warm-cache, no-parse contract of INV-8).
- **`gate_note` / `blocked`** (refinement 2) — scan the item's cached body (≤2000 chars) line by line (a "line" = the text between `\n` separators in the cached body) for the **first** line containing a case-insensitive directional gate/blocker marker; surface that line **trimmed first, then capped to ≤160 chars** (trim-then-cap, for byte-stability under INV-11), and set `blocked:true`. Match predicate for every marker: a **case-folded substring** test on the line (no word-boundary/order logic) — except the `until` rule, which fires when a line contains `until ` **and** (`lands` **or** `ships`), in any order. Marker set (small, fixed, **directional** — each names *this* item as the blocked/superseded party): `blocked by`, `gated`, `depends on`, `waiting on`, `parked`, `superseded`, `wait for ` (ANTS-3389 — the imperative prose form, e.g. "Wait for P10 to close before drafting"; the trailing space keeps it off `wait format…`), and the `until`+`lands`/`ships` co-occurrence. **`blocks ` is deliberately excluded** — an item that *blocks others* is itself actionable, and the bare verb false-fires on prose like "blocks the cursor". New helper `rcExtractGateNote(const QString &body) -> QString` (empty = no marker). Only the canonical `blocked by` form is INV-locked (INV-5); the rest of the marker set is best-effort. Caveats: the 2000-char body cap is a **hard character cut**, not a line boundary, so a marker that sits past 2000 chars — or on the single line straddling the 2000-char boundary — is missed. Acceptable for v1: gate notes are conventionally near the bullet head.

### 2.5 Response envelope

```json
{
  "ok": true,
  "mode": "bundles",
  "active_total": 42,
  "bundle_count": 11,
  "truncated": false,
  "bundles": [
    {
      "bundle_label": "diff viewer",
      "lanes": ["difftools", "diffviewer", "claudediff"],
      "size": 3,
      "items": [
        { "id": "ANTS-1864", "status": "📋", "headline_oneline": "...", "lanes": ["difftools"] },
        { "id": "ANTS-1874", "status": "📋", "headline_oneline": "...", "lanes": ["diffviewer"],
          "gate_note": "blocked by ANTS-1931 (composer exposure)", "blocked": true },
        { "id": "ANTS-1875", "status": "📋", "headline_oneline": "...", "lanes": ["claudediff"],
          "possibly_resolved_by": "ANTS-1926", "possibly_resolved_score": 71 }
      ]
    }
  ],
  "etag": "<auto>"
}
```

- **`items[]` vs the ROADMAP sketch** — ANTS-1922's bullet sketches the return as `{bundle_label, ids[]}`; this spec emits `items[]` of objects rather than a bare `ids[]`, because the per-item `possibly_resolved_by` / `gate_note` refinements (§2.4) must attach to each member and a flat id array cannot carry them. The `bundle_label` half of the sketch is unchanged. This is a deliberate enrichment, not a contract mismatch.
- **Id ordering** (used identically for `items[]` sort, the bundle size-tie-break, and the `bundle_label` lowest-id fallback): compare the integer suffix ascending — the `NNNN` in `ANTS-NNNN` — so `ANTS-999` precedes `ANTS-1000` (a plain string sort would invert these); tie-break lexicographically on the full id for any non-conforming id. One rule, three call-sites, so INV-11 byte-stability holds.
- `bundles` sorted by `size` descending (the biggest coherent group is the likeliest "next bundle"), ties broken by lowest member id (per the id-ordering rule above) for determinism (this already places 1-item bundles last, since they are the smallest).
- `active_total` is always the **true** active count (📋 + 🚧), independent of truncation. `bundle_count` is the number of bundles **emitted** in `bundles[]`.
- **Truncation** is a rare backstop, not the common path: with `headline_oneline` (not full `headline`) keeping each item small, a realistic active set fits well inside the cap. The `section_index` soft-cap is `kSoftCapBytes = 20480` (~20 KB — defined as `PaginationEngine::kSoftCapBytes` in `src/paginationengine.h:17`; bundles mode references that existing constant, it does **not** hardcode `20480`), but `section_index` reaches it via `PaginationEngine::pageBullets` / `measureCutPoint`, which slices a **flat array of same-shaped objects** by `offset`/`limit` and emits `next_offset`. That pager does **not** map onto variable-size bundles whose `items[]` must stay intact, so bundles mode does **not** reuse it. Instead bundles mode runs its own measure loop: serialise bundles in sorted order, accumulating byte size, and stop before the first bundle that would cross `kSoftCapBytes`; the bundles already emitted are kept whole (a bundle's `items[]` is never split). When that fires, `truncated:true` is set; **v1 emits no `next_offset`** — the dropped bundles are not retrievable in a follow-up call (a known v1 limitation: the cap is a safety backstop, and the dropped bundles are the trailing entries in the sorted (size-desc, id-asc) order — the smallest-or-equal, lowest-priority groups; pagination is deferred to a follow-on if truncation is ever observed to fire in practice). The emitted sizes then sum to **≤** `active_total` (dropped items are not re-emitted elsewhere). When `truncated:false`, the emitted sizes sum to **exactly** `active_total` (INV-3).

## 3. Invariants

- **INV-1** — `mode:"bundles"` returns `{ok:true, mode:"bundles", bundles, bundle_count, active_total, truncated}` (all six keys always present); every id in every bundle's `items[]` is an active item (status 📋 or 🚧); no ✅ item appears. *Test:* feature test asserts the always-present key set (incl. `truncated`) + that a seeded ✅ bullet never appears in `items`.
- **INV-2** — Two active items joined by an edge (normalised-headline token Jaccard ≥ 0.50 with ≥2 shared tokens) land in the **same** bundle; two with neither a direct edge **nor any transitive path** of edges land in **different** bundles. Bundling is the transitive closure of the edge relation (A–B and B–C ⟹ A, B, C share one bundle even if A and C share no tokens directly). *Test:* fixture with (i) a 3-item thematic cluster spanning distinct lanes, (ii) a transitive A–B–C chain where A and C share no tokens directly, and (iii) two mutually-unrelated items — asserts (i) and (ii) each collapse to one bundle and (iii) stays separate.
- **INV-3** — When `truncated:false`, every active item appears in exactly one bundle (singletons form 1-item bundles) and `sum(bundle.size) == active_total`. When `truncated:true`, emitted bundles are whole (never item-split) and `sum(bundle.size) <= active_total`. *Test:* feature test sums emitted bundle sizes on a small (non-truncating) fixture and asserts equality with the active count.
- **INV-4** — An active item whose **max** `rcHeadlineJaccard(item.tokens, shipped.tokens)` over the ✅ subset is ≥ 0.60 (with ≥2 shared tokens) carries `possibly_resolved_by` (the winning ✅ id) + `possibly_resolved_score` = `static_cast<int>(jaccard*100 + 0.5)` (60–100, the canonical dup-detector rounding); an item with no ✅ match ≥ 0.60 carries neither key. *Test:* fixture pairs a 📋 item with a near-identical ✅ sibling (asserts both keys present, score == the exact expected integer) and an unrelated 📋 item (asserts both keys absent); plus two **boundary** pairs: one whose raw Jaccard is just below 0.60 (asserts keys absent — a ratio that would round to score 60 but is below the gate does not attach) and one whose raw Jaccard is exactly ≥ 0.60 (asserts keys **present** with score 60 — locking the inclusive `>=` gate so an implementer's `>` slip is caught).
- **INV-5** — An active item whose body contains the canonical `blocked by` marker, the `wait for ` prose marker, or a line matching the `until`+(`lands`|`ships`) co-occurrence rule, carries `gate_note` (non-empty, ≤160 chars) + `blocked:true`; an item with no directional marker carries neither key; an item whose text contains the bare verb `blocks` (e.g. "blocks the cursor") does **not** set `blocked` (the `blocks ` marker is deliberately excluded). The remaining single-word markers (`gated`/`depends on`/`waiting on`/`parked`/`superseded`) are best-effort, not INV-locked. *Test:* fixture with one "blocked by …" body, one "deferred until ANTS-X lands" body, one clean body, one "blocks the cursor" body, and one "Wait for P10 to close before drafting" body; asserts present / present / absent / absent / present respectively.
- **INV-6** — `mode:"bundles"` combined with `section`, `id`, or `ids` refuses with `code:"bad_mode_combo"`. *Test:* feature test asserts the refusal for each combo.
- **INV-7** — A passed `status` arg is ignored in bundles mode (no refusal; the active subset 📋/🚧 is always used). *Test:* feature test passes `status:"all"` against a fixture containing ✅ items and asserts only active items appear.
- **INV-8** — The branch reads only `m_roadmapCacheBullets` (no re-parse) when the cache is warm, and never mutates it; the ✅-sibling check (INV-4) operates on token sets derived from the cached array, calling `rcHeadlineJaccard` (not `rcComputePossibleDuplicates`, whose `BulletRecord` signature would force a parse). *Test:* source-grep that the bundles branch takes no non-const reference to the cache and calls no `parseBullets` on the warm path.
- **INV-9** — `bundle_label` is always a non-empty string and **deterministic** for a given member set: qualifying shared tokens (label stop-words dropped) sorted by (frequency desc, token asc) take the first 3; else the most-common lane (alphabetical tie-break); else the lowest member id. *Test:* feature test asserts every emitted bundle has a non-empty label, that two calls on the same fixture yield identical labels, and includes an **all-empty-lanes, no-majority-token** bundle that forces and asserts the lowest-member-id final fallback.
- **INV-10** — An empty active set (no 📋/🚧 items) yields `{ok:true, mode:"bundles", bundles:[], bundle_count:0, active_total:0, truncated:false}`. *Test:* fixture with only ✅ items asserts the empty envelope.
- **INV-11** — The full envelope is **byte-stable** across repeated calls on unchanged input: bundles sorted by size desc then lowest member id; `items[]` sorted by id asc; per-item and per-bundle `lanes` sorted asc. *Test:* feature test calls the verb twice on one fixture and asserts identical serialised output.
- **INV-12** — Truncation slices the `bundles[]` array (drops whole trailing bundles), never a bundle's `items[]`; when it fires, `truncated:true` and the kept bundles are intact. *Test:* a test-only soft-cap override seam — `RemoteControl::setBundleSoftCapOverride(int bytes)` (the same test-only override-seam *idea* as ANTS-2154's `mcp::setSpillDirOverride`, but structurally a `RemoteControl` member — not a namespaced free function — since the cap default lives with the cache; `0` = use the default `kSoftCapBytes`) — lets a small fixture force truncation; the feature test sets a low cap, asserts `truncated:true`, asserts the emitted bundles' `items[]` are each whole (sizes match their pre-truncation membership), and asserts `sum(emitted sizes) < active_total`.
- **INV-13** — The `bundle_label` lane-fallback tally folds case: members with lanes `claude` and `Claude` contribute to the *same* lane bucket when picking the most-common lane for the label. *Test:* fixture bundle with mixed-case duplicate lanes and no token majority asserts the label resolves to that single folded lane (not split across cases).

## 4. RAM / build cost

- No persistent state added. Transient per-call working set: one `QSet<QString>` of tokens per bullet (built for **all** bullets, since the ✅-sibling check needs shipped-item tokens too) + a `QVector` of active-item views + an `O(active)` union-find parent array, all freed on return.
- Two pairwise passes: clustering is `O(active²)` (~150² ≈ 22 K comparisons for ~150 active items); the ✅-sibling check (INV-4) is `O(active × shipped)` (~150 × ~1500 ≈ 225 K comparisons on a large, mostly-shipped roadmap). Each comparison is a `QSet` intersection over short headlines — together sub-10 ms, well under the 100 ms roadmap-cache TTL. If profiling ever shows the `O(active × shipped)` term dominating, the ✅ set can be pre-filtered by lane; not needed for v1.
- Token-set memory: one small set per bullet (a few dozen short strings each) — for a ~1500-bullet roadmap, low single-digit MB transient, freed on return. The token sets are built **per call** (not memoised alongside `m_roadmapCacheBullets`), so a warm-cache bundles call still pays O(total bullets) of headline tokenisation each time; budgeted above as sub-10 ms. If a very large roadmap ever pushes this past the 100 ms TTL, the sets can be cached lockstep with the bullet cache (a follow-on; not needed for v1).
- Response size bounded by the existing ~20 KB soft-cap (§2.5); no unbounded growth.
- No new build target, no new external library (`rcHeadlineJaccard`, `rcExtractGateNote` are static helpers in `remotecontrol.cpp`).

## 5. Out of scope

- **Recent-commit affinity clustering** — no roadmap-item→commit map exists (commits aren't id-indexed); would need new `git log` subject-scanning for `[ANTS-NNNN]`. Weak signal for un-started work anyway. Deferred — tracked as a follow-on note on ANTS-1922.
- **Lane-as-clustering-edge** — deliberately rejected for v1 (mega-bundle risk). Revisit only if field use shows token-only clustering under-groups same-lane items; tracked as a follow-on note.
- **Embedding bundles inline in `session_orient`** — Option (a)'s flat `active_bullets` already covers the orient surface; bundles stays a dedicated `roadmap_query` call to keep the orient envelope small.

## 6. Tests

Feature test: `tests/features/mcp_roadmap_bundles/` (`spec.md` + `test_roadmap_bundles.cpp`, added to the `test_claude` bundle's `SOURCES` — not a standalone target). Covers the behavioural invariants (INV-1..INV-7, INV-9..INV-13) against small hand-authored ROADMAP fixtures written to a `QTemporaryDir`; only INV-8 (no-parse) is a source-grep surface. The fixtures exercise: a multi-lane thematic cluster, a transitive A–B–C chain, mutually-unrelated singletons, a 📋/✅ near-duplicate pair plus a sub-0.60 boundary pair, an all-empty-lanes/no-majority bundle (lowest-id label fallback), a mixed-case-lane bundle (INV-13), a "blocked by" body, a "deferred until ANTS-X lands" body, a "blocks the cursor" false-positive body, an all-✅ roadmap (empty-set, INV-10), and — via the `setBundleSoftCapOverride` seam — a forced-truncation case (INV-12). Label `features;fast`. Per project convention, each assertion is verified to FAIL against pre-implementation code (the unknown `mode` refuses `bad_mode`) before the implementation is wired.

## 7. Cross-doc impact

- **`roadmap_query` tool description** (`src/claudeintegration.cpp`) — document `mode:"bundles"` (active-only, ignores `status`, refuses `section`/`id`/`ids`).
- **CHANGELOG.md** — new entry under the next version (`changelog_log op:add`).
- **CLAUDE.md** — the MCP behavioural-notes pointer (`docs/standards/mcp-behavioural-notes.md`) gains the `roadmap_query mode:"bundles"` note; the always-loaded preamble is untouched.
- **ROADMAP.md** — flip ANTS-1922 to ✅ on ship (Option b complete), or annotate with the deferred follow-ons (recent-commit affinity, lane-edge).
- No README / PLUGINS.md change (no user-facing UI, no Lua surface).

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-1922-roadmap-query-mode-bundles-work-loop-log.md`](../reviews/ANTS-1922-roadmap-query-mode-bundles-work-loop-log.md).
