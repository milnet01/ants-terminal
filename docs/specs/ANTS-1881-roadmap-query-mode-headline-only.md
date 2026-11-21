# ANTS-1881 — `roadmap_query` `mode:"headline_only"`

**Status:** spec draft, cold-eyes loops 1-10 folded (2026-08-26).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-1881 (vestige-feedback-2026-05-26 Wishlist #1).
**Pairs with:** ANTS-1437 (introduced `mode` arg + `section_index`),
ANTS-1521 (`headline_oneline` companion field, ROADMAP-only),
ANTS-1517 (`include_body` opt-in pattern, ROADMAP-only),
ANTS-1499 (etag 304 pattern, ROADMAP-only),
ANTS-1720 (`fields=` projection, ROADMAP-only).

**Layman:** Add a "just the headlines" mode to the roadmap-query
tool so Claude can list what's in a section without pulling all the
multi-paragraph body text — roughly a 10× smaller answer on dense
bundle sections.

## 1. Problem

The 80 % case for `roadmap_query section=<slug>` is *"what is in this
section?"* — bullet bodies are not needed at that step. Today the only
two emission shapes are:

- **Default `mode:"bullets"`** — every bullet carries `id`, `status`,
  `headline`, `headline_oneline`, `kind`, `lanes[]`, `section_slug`,
  plus optional `body` / `format` / `synthetic` / `anchor` / `bold_id` /
  `id_inferred` (ANTS-4575).
- **`mode:"section_index"`** — *per-section* roll-up, no individual
  bullets.

There is no shape between the two. A caller that just wants the bullet
catalogue (id + status + headline + slug) inside a single section pays
~10× the bytes for fields they will discard. On dense bundle sections
where a single bullet body is 2–5 KB, the wire cost dominates the
caller's harness budget.

## 2. Surface

### 2.1 Arg

A further value for the existing `mode` arg on `roadmap_query`:
`mode:"headline_only"`. It joins `cmdRoadmapQuery`'s `kModes` allow-set
(`src/remotecontrol_roadmap_query.cpp`); the rejection envelope
(`code:"bad_mode"` + 64-byte / control-char `?` echo) is unchanged.
Modes added later, and the `sections` alias, belong to their own specs —
this document does not restate that list's membership.

```cpp
if (!kModes.contains(mode)) {
    // existing bad_mode emission
}
```

### 2.2 Per-bullet projection

Three selector shapes in `cmdRoadmapQuery` reach a bullet emit, and
each needs projection applied. **Three shapes, but FOUR call sites** —
the list path is two separate loops, and wiring only one is the failure
mode this section exists to prevent:

1. **The list path** — builds `m_roadmapCacheBullets` (full-file path)
   or `m_roadmapSectionCache[slug]` (section path). These are TWO
   distinct emit loops, not one: each has its own `filtered`, its own
   `rcProjectHeadlineOnly(filtered)` call and its own `pageBullets`.
   Wire only one and the mode still emits fat bullets on whichever of
   the section-scoped or full-file call was missed — silently, since
   INV-5's coverage grep is satisfied by the helper's own definition.
   INV-6's anchor is what catches it: it asserts
   `rcProjectHeadlineOnly(filtered)` occurs exactly twice.
2. **The `id` branch** — builds `matches[]` by scanning
   `m_roadmapCacheBullets` for the matching id, and returns before
   reaching the list emit.
3. **The `ids` branch** — the same shape for an id set, de-duplicated.
   It projects, and then ANTS-4400's `input_index` loop writes a fifth
   key back onto each bullet. That ordering is the contract, not drift:
   INV-2 exempts this branch, and ANTS-4712 settled it there.

All three live in `src/remotecontrol_roadmap_query.cpp`. When
`mode == "headline_only"`, every one projects each output object to
**exactly four keys** — plus `input_index` on the `ids` branch alone,
per INV-2:

| Key                | Source                                              |
|--------------------|-----------------------------------------------------|
| `id`               | `Bullet::id` (QString)                              |
| `status`           | `Bullet::status` (status emoji QString)             |
| `headline_oneline` | `rcHeadlineOneline(Bullet::headline)`               |
| `section_slug`     | `Bullet::sectionSlug`                               |

No other keys are emitted (no raw `headline`, no `body`, no `kind`,
no `lanes`, no `format`, no `anchor`, no `bold_id`, no `synthetic`).
`input_index` on the `ids` branch is the one exemption, and INV-2 owns
its scope.
Empty string values are permitted (and expected for rollup / narrator
entries — see §2.3) — INV-2 governs the *key set*, not value
non-emptiness.

The narrowing is mode-driven, not flag-driven — `include_body`,
`format`, anchor opt-ins are silently ignored in this mode (consistent
with `section_index`'s ignore-set per INV-7 of ANTS-1437).

### 2.3 Combinator support

| Combinator                 | Behaviour                                  |
|----------------------------|--------------------------------------------|
| `status="active"`          | Honoured on the list path — narrows the bullet set, then projects each kept bullet to the four keys. **Not on the `id` / `ids` selectors, which return before the status filter** (ANTS-1726's documented bypass, anchored by `roadmap_query_by_ids::Inv2BypassesStatusAndPagination`). Do not "fix" those branches to honour it. |
| `filter`                   | Honoured — ANTS-3698 accepts it as an alias for `status`; sending both refuses `bad_op_combo` (ANTS-6365). Behaviour from that point is `status`'s row above, unchanged. Added by ANTS-4703: this verb's own envelope echoes the applied lifecycle as `filter`, so a caller reading a response and hand-writing the next call sends `filter:"active"` — which was an unrecognised arg, dropped silently, and answered with the FULL set under an echo that reads as confirmation a filter was applied. |
| `section="<slug>"`         | Honoured — same flow as bullets-mode. |
| `id="<ANTS-NNNN>"`         | Honoured — single bullet, projected. The id-branch in `cmdRoadmapQuery` projects its `matches[]` array to the four-key shape before emission; the envelope still carries `id`, `found`, `count`, `path`, `bullets[]`, `etag`, `mode` (echoed when caller passed `mode=` per the `hasModeArg` gate), plus `duplicate_ids[]` when applicable. |
| `offset`, `limit`          | Honoured on the list path — pagination operates on the *projected* set via `PaginationEngine::pageBullets`; envelope fields (`offset`, `limit`, `total`, `truncated`, `next_offset`) carry through. **Not on the `id` / `ids` selectors**, which return before any `pageBullets` — same bypass as `status` above. |
| `max_results`              | Honoured — ANTS-4701 accepts it as an alias for `limit`; sending both refuses `bad_op_combo` (ANTS-6365). Behaviour from that point is `limit`'s row above, unchanged. Added because `workspace_search` spells this cap `max_results` and the name carried across was dropped into `ignored_args` and answered with the full set — which under this mode is every row. |
| `include_section_headers`  | Honoured. Rollup bullets (themselves gated by `include_section_headers:true`, per `cmdRoadmapQuery`'s `shouldDropUnnumbered` predicate) project to `{id:"", status, headline_oneline:"", section_slug}` — all four keys present, two with empty-string values. |
| `include_narrator_bullets` | Honoured. Same shape as rollup retention — narrator bullets (empty `id`, non-empty `headline`) project to `{id:"", status, headline_oneline, section_slug}`. |
| `fields=[...]`             | Honoured — every verb honours `fields=` (ANTS-4524); there is no allowlist to join and no per-descriptor wiring. Top-level projection still works (e.g. `fields:["bullets","etag"]`). Inside each bullet the four-key shape is the mode's contract, not a `fields` projection. |
| `etag_match`               | Honoured — see §2.5. |
| `include_body`             | Ignored (no body in this mode). Echo unchanged for caller transparency. |
| `ids`                      | Honoured — the `ids` branch projects its `matches[]` to the four-key shape, then ANTS-4400's annotation adds `input_index`, so this path emits five keys. INV-2 owns the exemption. The singular `id` branch takes no annotation and emits four. |
| `query`, `regex`, `whole_word` | Honoured on the list path — `applyQueryFilter` runs on the filtered set *before* projection, so the keyword still matches body text this mode does not emit. **With `id`, `ids` or `mode:"bundles"` it REFUSES `bad_mode_combo`** rather than bypassing. Note the contrast with `status` and `offset`/`limit` above, which those selectors bypass SILENTLY — do not generalise either behaviour to the other. |
| `max_body_bytes`           | Ignored (no body to cap in this mode), as `include_body` is. |
| `section_etag_match`       | Honoured — read on the `section=` path, which this mode uses. Its short-circuit returns `{ok, unchanged, section, section_etag, path}` and no bullets, so the four-key contract does not arise. The section etag keys on the section's bytes and not on `mode`, so a caller holding one taken under `mode:"bullets"` is answered `unchanged` here; INV-4's cross-mode rule covers only the dispatcher's file etag. Confirmed against the code (ANTS-4725): `rcComputeSectionEtag` takes the section slice and nothing else, and the short-circuit returns from inside the `section=` branch before any `headline_only` projection runs. So the cross-mode answer is real, and a caller that varies `mode` between calls must not reuse a section etag expecting the payload shape it asked for. |
| `check_sync`, `scope`, `since`, `until`, `slugs_only`, `include_section_etags` | No effect here — the first four belong to `mode:"report"`, the last two to `mode:"section_index"`. Read and ignored rather than refused, so a caller passing one gets this mode's ordinary output. |

### 2.4 Envelope

```jsonc
{
  "ok": true,
  "mode": "headline_only",      // echoed only when caller passed mode= arg
  "path": "/abs/path/ROADMAP.md",
  "filter": "active",
  "bullets": [
    { "id":"ANTS-1881", "status":"📋",
      "headline_oneline":"…", "section_slug":"…" },
    …
  ],
  "etag": "<sha256-hex16 over the projected payload>",
  // pagination fields when applicable: offset, limit, total,
  // truncated, next_offset
}
```

The `legacy_format_sections[]` array and the per-section
`legacy_format:true` flag are `mode:"section_index"`-only
diagnostics (emitted inside the section_index branch of
`cmdRoadmapQuery`); bullets-mode never emits them, so
headline_only — which runs through the bullets-emission code
paths (full-file loop, section loop, `id` and `ids` branches) — also never
reaches those emitters. No special carve-out language is needed;
the absence is mode-shape-natural.

The `mode` field is echoed in the envelope **only when the caller
passed `mode=` explicitly** — matching the bullets-mode id-branch
precedent (`cmdRoadmapQuery`'s `hasModeArg` gate in
`src/remotecontrol_roadmap_query.cpp`), not the section_index branch's
unconditional emit. Rationale: headline_only is a
bullets-projection variant; matching the bullets-mode echo gate
keeps default-call envelopes byte-identical with bullets-mode
default-call envelopes (§6 Migration's back-compat claim).

Confirmed against the code (ANTS-4725): every `mode` echo on a
bullets-shaped path is `hasModeArg`-gated — the full-file list loop, the
`section=` loop, and the `id` and `ids` branches alike. Only the
`section_index` and `bundles` branches emit unconditionally, and
`bundles` emits a literal rather than the caller's argument. The
back-compat claim above therefore rests on a gate that is uniform across
the paths it depends on.

The `duplicate_ids[]` diagnostic IS retained — it's a top-level
roadmap-integrity warning that benefits any caller iterating
bullets, and its payload is trivially small (only present on
collision).

### 2.5 ETag interaction

`roadmap_query` is in `ClaudeIntegration::isEtagSupportedTool`
(`src/claudeintegration.cpp`), which is the precondition for the
dispatcher's etag-injection layer to fire. The etag is computed at
the dispatcher (`ClaudeIntegration::applyEtagPattern` — wraps every
`roadmap_query` response after `cmdRoadmapQuery` returns, so the
*projected* bytes are what gets hashed). Consequently:

- Repeating a `mode:"headline_only"` call with `etag_match` set to the
  prior response's etag returns `{ok:true, unchanged:true, etag:"<same>"}`
  whenever the projected payload is byte-identical to the previous one.
- A bullets-mode call with the headline-only etag does **not** 304
  (different payload bytes → different etag).

### 2.6 Cache reuse

No new cache fields. The existing `m_roadmapCacheBullets` is the full-
fat shape; the projection happens at emit time inside the bullet-emit
loop. The cache predicate (path + mtime + 100 ms TTL) is unchanged.

## 3. Invariants

- **INV-1** — `cmdRoadmapQuery` accepts `mode:"headline_only"`; a value
  outside its `kModes` allow-set returns `code:"bad_mode"` with the
  existing 64-byte + control-char `?` echo hygiene. *Test:* `tests/features/roadmap_query_headline_only/test_roadmap_query_headline_only.cpp::Inv1ModeAcceptedAndRejected`.
- **INV-2** — `mode:"headline_only"` projection emits exactly the
  key set `{id, status, headline_oneline, section_slug}` per bullet,
  plus `input_index` on the `ids` selector alone (ANTS-4712).
  Extra keys fail; missing keys fail. The `ids` exemption is
  deliberate, not tolerated drift: the projection runs before
  ANTS-4400's `input_index` annotation, and that ordering is the
  contract rather than an accident. Results on that path come back in
  DOCUMENT order, so a caller zipping them against its own input array
  mis-pairs silently — which is the defect `input_index` was added to
  close, and it closes it least well in the mode that carries the
  least. Stripping the key to hold a four-key shape would reintroduce
  that defect for the leanest caller. The singular `id` selector takes
  no annotation and emits four. Empty-string values are
  permitted for the rollup case (`id:""` + `headline_oneline:""`,
  per `isRollupBullet` in `src/remotecontrol_roadmap_query.cpp`);
  narrator entries carry `id:""` with non-empty `headline_oneline`
  (`isNarratorBullet` in the same file defines a narrator
  as empty `id` AND non-empty `headline`; from there
  `rcHeadlineOneline(b.headline)` is necessarily non-empty too).
  This matches bullets-mode's narrator emission, so INV-3
  combinator-equivalence on `id` arrays holds. Bullet-level
  `fields=` projection is NOT supported in this mode: the four-key
  shape is the *mode contract*, not a `fields`-narrowed default —
  `fields:["id"]` does NOT narrow the per-bullet object to one key
  (top-level `fields=` still works, see §2.3). Rationale:
  layering `fields=` over a mode whose entire purpose is a fixed
  narrow shape would re-narrow a deliberately narrow shape — the
  contract is owned by the mode, not the projection arg.
  *Test:* `…::Inv2KeySetExactlyFour`, `…::Inv2RollupEmpty`,
  `…::Inv2NarratorHeadlineNonEmpty`;
  `tests/features/roadmap_query_by_ids/test_roadmap_query_by_ids.cpp::Inv2IdsBranchKeepsInputIndex`
  anchors the `ids` exemption and its ordering. That anchor is a
  source scrape, like every test in both suites: the feature bundles
  do not link `cmdRoadmapQuery`, so no test in either suite calls the
  verb. The key set is therefore locked by reading the handler, not by
  observing a response.
  `…::Inv2FieldsDoesNotNarrowBullets` anchors on the mode value
  appearing in the sources; it does not exercise the `fields=` clause.
- **INV-3** — Combinator equivalence with bullets-mode. The bullet
  `id` array (in array order, positional equality — not set-based;
  grounded in the shared `m_roadmapCacheBullets` iteration order)
  under `mode:"headline_only" + status:"<f>" + section:"<s>"` equals
  the bullet `id` array under `mode:"bullets" + status:"<f>" +
  section:"<s>"` for every supported `<f>` and `<s>` — only the per-
  bullet shape differs. **Compared over the filtered set before
  pagination**, or with both calls passing a `limit` below the
  auto-truncate threshold: INV-6 makes the emitted pages differ in
  length by design, so a parity test over emitted `bullets[]` would red
  against correct code.
  *Test:* `…::Inv3CombinatorIdParity` — a source scrape, like every
  anchor in this suite (§4). It does not make the two calls this
  invariant compares, so the parity itself is unobserved here.
- **INV-4** — ETag short-circuits over the projected payload.
  (a) A second `mode:"headline_only"` call with `etag_match` set to the
  first call's etag returns `{ok:true, unchanged:true, etag}` when the
  projected payload is byte-identical. (b) The same etag passed to a
  `mode:"bullets"` call does NOT 304 (response bytes differ → etag
  differs).
  *Test:* `…::Inv4SameModeShortCircuit` (sub-claim a) +
  `…::Inv4CrossModeNoShortCircuit` (sub-claim b). Both are source
  scrapes over `src/claudeintegration.cpp` — the dispatcher, where the
  etag is applied — rather than live calls; see § 4. Neither observes a
  304, so the short-circuit itself is unpinned by this suite.
- **INV-5** — Combinator coverage. Every argument `cmdRoadmapQuery`
  reads — its own `mode` selector and the dispatcher's `caller_cwd`
  aside — is accounted for in §2.3: either the table gives it explicit
  behaviour under `mode:"headline_only"`, or the table names it as
  having no effect here. An argument added to the verb owes a row.
  All three selector shapes of §2.2 project their output.
  *Test:* `…::Inv5IdSelectorProjected` asserts that the singular `id`
  branch's emit site is projected — a source scrape over the handler, not
  a live call, so it does not observe the emitted key set.
  `…::Inv5CombinatorCoverageSourceGrep` asserts only that the shared
  projection helper is *named* somewhere in the concatenated
  `remotecontrol` sources — which its own definition satisfies — so it
  vouches for nothing behavioural and does not compare the read-set
  against §2.3. The `ids` branch IS anchored, by INV-2's
  `Inv2IdsBranchKeepsInputIndex`. What stays open is the read-set
  comparison against §2.3, which no anchor makes.
- **INV-6** — Pagination operates on the projected set.
  `PaginationEngine::pageBullets` measures the projected `QJsonArray`,
  so more `headline_only` bullets fit in one auto-truncate slice than
  the same fixture's bullets-mode call. `total` reflects the full
  filtered set, not the per-page set (matches the existing pagination
  contract).
  *Test:* `…::Inv6PaginationOnProjectedSet`.
  *Restored by ANTS-3577 (2026-07-20):* the shipped code had drifted to
  projecting `page.slice` AFTER the fat measure (measuring the unprojected
  row and over-dropping); the fix projects `filtered` BEFORE pagination at
  both bullet branches, and the INV-6 test now pins projection-before-
  pagination rather than merely asserting `pageBullets` is called.
- **INV-7** — `tools/list` schema carries the value. The `mode` enum in
  the tool descriptor for `roadmap_query` (`src/claudeintegration.cpp`
  `tools/list` builder) contains `headline_only`, alongside the values
  other specs own, so client-side schema validators do not reject it.
  Per §2.1 this document does not restate the enum's membership.
  *Test:* `…::Inv7ToolsListEnumerates` — run the test the way a
  human would: `grep -nP 'modeEnum\.append\("headline_only"\)'
  src/claudeintegration.cpp` (the descriptor's enum population site,
  NOT the descriptor's prose `description` string — the prose may
  contain the literal `"headline_only"` too once §7 lands, but the
  test verifies the enum population, so the anchor is the
  `modeEnum.append("headline_only")` token itself). Matches in the
  `cmdRoadmapQuery` allow-set (`src/remotecontrol_roadmap_query.cpp`),
  in a test fixture, or in a comment do NOT count.
- **INV-8** — Top-level diagnostic parity by shared lineage.
  `mode:"headline_only"` projects from the same code paths as
  `mode:"bullets"` (full-file emit loop, section emit loop, `id` and
  `ids` branches), so it emits the same top-level diagnostic set:
  `duplicate_ids[]` is present iff the same fixture under
  `mode:"bullets"` would emit it, with byte-identical contents (the
  diagnostic is computed over the shared `m_roadmapCacheDuplicateIds`
  cache, not the projected bullets). `legacy_format_sections[]` and
  per-section `legacy_format:true` are emitted only inside the
  `mode == "section_index"` branch of `cmdRoadmapQuery` —
  neither bullets-mode nor headline_only reaches them, so they
  are naturally absent in both modes.
  *Test:* `…::Inv8DuplicateIdsParity` asserts only that
  `m_roadmapCacheDuplicateIds` is REFERENCED in the handler. It makes no
  positional assertion, so nothing pins the emit outside the
  `headline_only` branch and nothing observes the payload — the parity is
  reasoned, not tested.

## 4. Tests

`tests/features/roadmap_query_headline_only/`:

- **`spec.md`** — mirrors INV-1..INV-8 in test-anchor form; several
  invariants carry more than one anchor.
- **`test_roadmap_query_headline_only.cpp`** — the INV anchors named in
  §3, each a scrape over source. Most read the concatenated
  `remotecontrol` sources; **INV-4 and INV-7 read
  `src/claudeintegration.cpp` instead**, because the etag wrapper and the
  descriptor's enum both live at the dispatcher. An anchor pointed at the
  wrong one of those two finds nothing.

**The suite does NOT call the verb.** The feature bundles do not link
`cmdRoadmapQuery`, so every anchor here reads source rather than a
response — the same shape as
`test_roadmap_query_section_index.cpp`, which is a scrape for the same
reason. Label: `features;fast`.

**What that costs, stated because a scrape and a behavioural test read
alike in a green run:** no anchor in this suite observes an emitted key
set, a 304, a page slice or a `duplicate_ids` payload. Each invariant
above is pinned to the source construct that produces the behaviour, and
the behaviour itself is unobserved here. Linking `cmdRoadmapQuery` into
the bundle would change that, and is not this spec's deliverable.

Per the project test convention, an invariant's test is first verified
to FAIL against pre-fix code, then GREEN after the fix lands. **That
applies to the anchors scraping constructs this change ADDS** — INV-1,
INV-2, INV-5, INV-6 and INV-7. **INV-4 and INV-8 are green in both
states by design** and must not be edited until they red: both pin
constructs that predate this change (`isEtagSupportedTool`,
`applyEtagPattern`, `m_roadmapCacheDuplicateIds`), which is the point —
§2.5 records that `roadmap_query` is already in
`ClaudeIntegration::isEtagSupportedTool`, so this change adds no gate
for INV-4's anchors to detect. An implementer holding red-first as a
blanket exit condition would edit a correct test until it failed.

## 5. RAM / build cost

Zero new state. The projection is per-bullet at emit time
(`QJsonObject` construction of four scalars), strictly cheaper than
the current bullets-mode emit. Estimated diff size:

- ~40 LoC — four call sites in `cmdRoadmapQuery` (the full-file emit
  loop, the section emit loop, the `id` branch, the `ids` branch, per
  §2.2), calling one shared
  projection helper that lives with the other `rc*` projections in
  `src/remotecontrol.cpp`.
- ~60 LoC tool-descriptor schema update — enum + per-arg
  `modeProp["description"]` clause + top-level
  `roadmapTool["description"]` selection-hint addendum (three
  touchpoints, see §7).
- ~180 LoC test — source-scrape harness (`srcgrep.h`) + the INV
  anchors named in §3. No fixture: the suite builds no ROADMAP.md and
  makes no call.

Total: **~280 LoC** including tests.

## 6. Migration

None. Additive arg value with no default behaviour change. Existing
`roadmap_query{}` and `roadmap_query{mode:"bullets"}` calls continue
to emit byte-identical envelopes (except the `mode` echo, which is
present only when the caller passed it — pre-existing convention).

## 7. Tool-descriptor update

Three touchpoints in `src/claudeintegration.cpp`'s `roadmap_query`
descriptor block — all must be updated in the same change:

1. **`modeProp["enum"]`** — the per-arg enum gains the literal
   `"headline_only"` alongside `"bullets"` and `"section_index"`.
2. **`modeProp["description"]`** — the per-arg description string
   gains one technical clause documenting the new value (caller is
   the JSON schema reader / tool-validator).
3. **`roadmapTool["description"]`** — the top-level tool description
   currently runs ~70 lines of `QStringLiteral` enumerating the
   modes ("Optional `mode` — \"bullets\" (default) /
   \"section_index\" …"). Add a *short* mode-selection hint here,
   not a full payload-shape paragraph — the top-level description is
   the caller's "should I pick this tool" surface; the per-arg
   description owns the payload contract.

Suggested per-arg description clause (technical):

> `mode:"headline_only"` — return `{id, status, headline_oneline,
> section_slug}` per bullet, skip `body`/`lanes`/`kind` (~10× smaller
> payload on dense bundle sections; composes with `section=`,
> `status=`, pagination, and ETag).

Suggested top-level description addendum (selection hint):

> / \"headline_only\" (ID + one-line headline + slug per bullet — use
> when you only need the bullet catalogue, not the bodies; composes
> with `section=`).

## 8. Cross-doc impact

- `CLAUDE.md` — no change required (the tool-authoring contract list
  is unchanged; only an enum value is added).
- `docs/standards/mcp-tools.md` — no change required (the umbrella
  checklist references the existing enum-validation pattern).
- `tests/features/README.md` — no change (one new feature dir,
  follows the existing pattern).

## 9. Cold-eyes loop log

Rows live in [`../reviews/ANTS-1881-roadmap-query-mode-headline-only-loop-log.md`](../reviews/ANTS-1881-roadmap-query-mode-headline-only-loop-log.md).
