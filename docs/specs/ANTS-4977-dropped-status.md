# ANTS-4977 — publish `dropped` roadmap items with a 🚫 marker

**Status:** spec draft (2026-09-19).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-4977 (DOOM_Ants_MCP_Feedback.md 2026-09-08; user decisions 2026-09-19).
**Supersedes:** the `dropped` half of ANTS-3758 INV-4, ANTS-3757 INV-5, and ANTS-3793 § 2.1.2's `dropped` exclusion (§ 2.9 below).

## 1. Problem

An item whose outcome is "closed, deliberately not done" has no status a
caller can set. `roadmap_log` accepts four statuses, so such an item is
flipped to ✅ and the roadmap claims work that never happened. A ✅ on a
security finding reads as fixed to every status scan, to `review-ledger` and
to `feedback_log op:"compact_resolved"`, with only prose saying otherwise.

The store already has the value. `RoadmapStore::createSchema` gives
`item.status` a CHECK over `'planned','in-progress','shipped','considered','dropped'`.
What exists around it was built to HIDE it:

1. `roadmap-data-model.md` § 7.3 and § 7.5 exclude `dropped` items from the
   published render **as policy**, and its anti-patterns list names
   "Publishing a `dropped` or `internal` item".
2. `roadmap-format.md` § 3.11 makes any fifth status emoji an anti-pattern.
3. `roadmaprender.cpp::emojiFor` returns an empty string for `dropped`, and
   `isRenderable` drops the item from every rendered file (ANTS-3758 INV-4).
4. `roadmapsource.cpp::appendRecord` leaves it out of the store read seam
   (ANTS-3793 § 2.1.2).
5. No write verb accepts it. `cmdRoadmapLogAppend`, `cmdRoadmapLogFlip`,
   `cmdRoadmapLogFlipBatch` and `cmdRoadmapLogAppendBatch` validate against
   the four statuses. `roadmapmigrate.cpp::statusFromMarker` maps any
   unrecognised marker to `planned`, so migration cannot produce it
   (ANTS-3757 INV-5).

**The user decided on 2026-09-19 to show dropped items in `ROADMAP.md` with
🚫, knowing the policy above.** A reader of the file should see the decision.
Counted as closed: out of every active queue, never counted as shipped.

## 2. Surface

### 2.1 The marker and the word

| Store word | Marker | UTF-8 | Pass keyword | GFM |
|---|---|---|---|---|
| `dropped` | 🚫 U+1F6AB | `F0 9F 9A AB` | `dropped` | `- [x] 🚫 ` |

`roadmapparse.h` gains `kEmojiDropped` beside `kEmojiConsidered`. Every
status table that lists the four markers lists five. That covers
`roadmapparse.cpp::stripInlineEmoji`, `detectRoadmapFormat`'s bullet regex,
`projectlayoutengine.cpp`'s `kAntsPrefixes`, `remotecontrol.cpp`'s
`rcStatusWord` / `rcStatusEmoji` / `kAdapterEmoji*` / `walkAntsV1Bullets`,
`roadmapmigrate.cpp::passKeyword` and `keywordIsNamed`, the
`kUnrecognisedFormatHint` text, and `roadmapmigrateverb.cpp`'s skeleton legend.

`dropped` is **closed and not shipped**. It is never "active" and never
"shipped". It never carries a `shipped` date.

### 2.2 Render and read seam

- `emojiFor("dropped")` returns 🚫.
- `isRenderable` excludes `internal` items only. A dropped item renders in
  its section, in position order, like any other item.
- `renderLegend`'s `kStatusOrder` gains `dropped` after `considered`.
  `renderLegend` emits only the keys a project's stored legend holds, and no
  stored legend holds `dropped`. So where a project has a stored legend without
  that key, the row reads the default `🚫 Dropped (closed, not done)`. The
  default is rendered, never written to `project.legend`.
- `roadmapsource.cpp::appendRecord` no longer skips a dropped item, on either
  dialect. `legendByEmoji` no longer skips its (now non-empty) marker.
- `isOpen` keeps listing planned, in-progress and considered, so a dropped
  item never blocks `rotate_minor`'s `minor_not_closed` guard and never trips
  the Layman gate. **A dropped item needs no Layman line.**

### 2.3 Parse and migration

- **ants-v1:** a bullet opening `- 🚫 ` parses with `status` = `kEmojiDropped`.
  `statusFromMarker` maps 🚫 to `dropped`, and its fallback for an unknown
  marker stays `planned`.
- **pass-headings:** `passStatusKeyword("dropped")` returns `dropped`, and
  `passStatusEmoji("dropped")` returns 🚫. The reader,
  `parsePassHeadingBullets`, maps the keywords `dropped`, `abandoned` and
  `wontfix` to 🚫, and its bare-glyph map (a Status line holding only an
  emoji) maps 🚫 to `dropped`. Its fallback for an unknown keyword stays 📋.
  `keywordIsNamed` gains the same three words, so they migrate `asserted`.
- **GFM:** `applyGfmFlip` writes a dropped item as `- [x] 🚫 <text>`. The box
  is checked because the item is closed. Its `kEmojiPrefixes` strip list gains
  🚫, so a flip out of `dropped` removes the marker. `walkGfmBullets`' inline
  override chain (`tryStrip`) gains a 🚫 arm, so the line reads back as
  dropped. `gfmStatusFromCheckbox` is unchanged.
- `roadmapmigrate.cpp::makeItem` sets `closed` for `shipped` and `dropped`.

### 2.4 Write verbs

`roadmap_log`'s `status` enum (append, append_batch) and `to_status` enum
(flip, flip_batch) gain `dropped`, and the 🚫 glyph is accepted wherever a
status emoji is today. `rlCanonicalToStatus` gains no synonym. `dropped` is
the one spelling a caller sends (author's call: one spelling is one thing to
learn).

`rlStampShipped` already clears `shipped` on a move out of `shipped` and sets
it only on a move into `shipped`. So a move to `dropped` leaves no ship date
without a change. INV-6 locks that.

Every `bad_status` message that lists the four statuses lists five.

### 2.5 Query and aggregates

- `roadmap_query`'s `kAcceptedStatusFilters` and the MCP `status` enum gain
  `dropped`, which selects 🚫 only. `active` stays 📋 + 🚧, `shipped` stays ✅,
  and `all` includes 🚫. The section-scoped and full-file predicates in
  `cmdRoadmapQuery` gain the one arm each.
- `mode:"section_index"` counts a dropped item in `total_count` only, as it
  already counts `considered`. A `status:"dropped"` filter widens to `all`
  there, as `considered` does.
- `mode:"report"` is unchanged. `kOpenStatusIn` and `buildRoadmapReportEnvelope`
  already exclude `dropped` from `open`, and `by_status` already carries it.
- `mode:"bundles"`, `current_state` and `session_brief` read 📋 and 🚧 only,
  so a dropped item never appears in them. No change.

### 2.6 Feedback files

`feedbackfile.cpp`'s `openIds` and `remotecontrol_feedback.cpp`'s id
resolution treat a 🚫 id as **closed**, beside ✅. `feedback_query` reports
its `mapped_id_status` as 🚫, and `feedback_log op:"compact_resolved"`
collapses its write-up as it does a shipped one. That is the reporter's
payoff: a closure the tooling can read without parsing prose.

### 2.7 Roadmap dialog

- `RoadmapDialog`'s status filter gains a fifth status checkbox,
  `🚫 Dropped`, and `Filter` gains `ShowDropped`. That makes six checkboxes
  with "Currently being tackled".
- `kStatusLabels` gains `🚫` → "Dropped", and its `static_assert` moves to 5.
- `renderHtml`'s `BulletKind` gains `Dropped`, so a 🚫 bullet obeys the filter
  instead of falling to `Other`, which always renders.
- `renderCardsHtml` gains a `dropped` chip count and a `passesFilter` arm.
- Presets: `Full` includes Dropped. `History` is Done + Dropped, because both
  are closed (author's call). `Current`, `Next` and `FarFuture` exclude it.
- `Config::roadmapStatusFilters` gains the key `dropped`. **An absent key
  reads as shown**, so an existing user's saved filter does not hide items
  it never knew about (author's call).

### 2.8 What does not change

Fold-in writers (`auditengine.cpp`, `indiereviewengine.cpp`,
`debtsweepengine.cpp`, `coldeyesengine.cpp`, `testauditengine.cpp`) emit 📋
only. `coldeyesengine.cpp::activeSpecIds` and
`debtsweepengine.cpp::detectRoadmapShippedWithoutCommit` key on 📋 / 🚧 / ✅
and stay correct. `roadmapexport.cpp::kStatusOrder` already lists `dropped`.
The store schema is untouched. **No `kSchemaVersion` bump.**

### 2.9 Standards and superseded clauses

- **`docs/standards/roadmap-format.md`** (this project is upstream of the
  global copy): § 3.3 gains the 🚫 row and a sentence that any status may
  move to 🚫 and back. § 3.10.1's GFM table gains `[x] 🚫`. § 3.11's
  anti-pattern names five markers, not four, and its "Mixing `[ ]` / `[x]`
  task-list syntax with the emoji status system" bullet carves out the GFM
  adapter's own forms, `- [ ] <emoji>` and `- [x] 🚫`. § 3.7's released-block sentence
  allows 🚫 beside ✅. § 3.10.5 names the `dropped` pass keyword.
- **`docs/standards/roadmap-data-model.md`**: § 7.3 drops "no markdown
  serialisation" and the exclusion-as-policy paragraph. § 7.5 excludes
  `internal` only. The anti-pattern becomes "Publishing an `internal` item".
  The id-floor paragraph stops listing `dropped` among the ids no committed
  file carries. § 7.3.1's word table gains the row `dropped` · `abandoned` ·
  `wontfix` → `dropped`, `asserted`.
- **ANTS-3758 INV-4**, **ANTS-3757 INV-5**, **ANTS-3757 § 2.7**'s word table
  and "never produced" sentence, and **ANTS-3793 § 2.1.2** are
  annotated in place, "`dropped` half superseded by ANTS-4977", and never
  renumbered.

Each standard edit changes what a conformer writes, so each runs its own
`review-contract` gate, at genre `standard`, before the code that relies on it
ships.

## 3. Invariants

- **INV-1** — A `dropped` item renders in `ROADMAP.md` as `- 🚫 [ID] **headline**`
  in its own section. A project whose stored legend lacks `dropped` renders
  the default `🚫 Dropped (closed, not done)` row. An `internal`
  item still does not render. Breaks when `emojiFor` returns an empty string
  or `isRenderable` still excludes `dropped`. *Test:*
  `tests/features/roadmap_dropped_status` — a fixture store with one dropped
  and one internal item, and a stored legend without a `dropped` key: the
  rendered text contains the dropped id, not the internal one, and the default
  legend row.
- **INV-2** — A migrated 🚫 bullet round-trips. Migration stores `dropped`,
  and re-rendering reproduces the bullet line byte for byte. Breaks when
  `statusFromMarker` falls through to `planned`. *Test:*
  `tests/features/roadmap_dropped_status` — migrate an ants-v1 fixture holding
  a 🚫 bullet, assert the stored status, render, and compare the line.
- **INV-3** — Pass-headings: a `dropped` item writes `- **Status**: dropped`,
  and `dropped`, `abandoned`, `wontfix` and a bare `🚫` read back as 🚫, not
  📋, with `asserted` provenance. Breaks when the reader's fallback is reached. *Test:*
  `tests/features/roadmap_dropped_status` — each keyword through
  `parsePassHeadingBullets`, and one write through `passStatusKeyword`.
- **INV-4** — GFM: a flip to `dropped` writes `- [x] 🚫 <text>`, and the walker
  reads it back as 🚫. Breaks when `applyGfmFlip` writes an unchecked box.
  *Test:* `tests/features/roadmap_dropped_status` — a GFM fixture flipped
  through `cmdRoadmapLogFlipForTest`, then read with `cmdRoadmapQueryForTest`.
- **INV-5** — `roadmap_log` accepts `dropped` and 🚫 on append, append_batch,
  flip and flip_batch, on a store-backed project and a markdown-backed one.
  An unknown status still refuses `bad_status`, now listing five. Breaks when
  any of the four validators still lists four. *Test:*
  `tests/features/roadmap_dropped_status` — one call per op per backend.
- **INV-6** — A flip from `shipped` to `dropped` clears the item's `shipped`
  date, and no path into `dropped` sets one. Breaks when `rlStampShipped`
  treats `dropped` as shipping. *Test:* `tests/features/roadmap_dropped_status`
  — flip an item to shipped, then to dropped, and read the `shipped` column.
- **INV-7** — `roadmap_query status:"dropped"` returns exactly the dropped
  items. `active` and `shipped` return none of them, and `all` returns all of
  them. `section_index` counts them in `total_count` only. Breaks when a
  predicate misses its arm. *Test:* `tests/features/roadmap_dropped_status`.
- **INV-8** — A feedback finding whose id is 🚫 is closed: it is not in
  `openIds`, and `compact_resolved` collapses it. Breaks when resolution
  still reads "closed" as ✅ only. *Test:* `tests/features/roadmap_dropped_status`.
- **INV-9** — The roadmap dialog filters 🚫 like any other status. With
  Dropped unticked no 🚫 card renders, and `History` shows ✅ and 🚫. An
  absent `dropped` config key reads as ticked. Breaks when a 🚫 bullet falls
  to `BulletKind::Other`. *Test:* `tests/features/roadmap_dropped_status`
  dialog half, in the dialogs bundle.
- **INV-10** — A roadmap whose first status bullet is 🚫 is detected as
  ants-v1, by `detectRoadmapFormat` and by `project_layout`. Breaks when either
  regex lists four markers. *Test:* `tests/features/roadmap_dropped_status`.
- **INV-11** — A dropped item is never open. `isOpen("dropped")` is false, so
  it needs no Layman line and never blocks `rotate_minor`. Breaks when `isOpen`
  switches to "not shipped". *Test:* `tests/features/roadmap_dropped_status` —
  a flip to dropped of an item with no Layman line succeeds.

## 4. RAM / build cost

None beyond one emoji constant and one filter bit per record. No new target
beyond the test files; no library added.

## 5. Out of scope

- The **global** copy of `roadmap-format.md` under `~/.claude/standards/`. This
  project is upstream, and the global copy is corrected to match from a
  `~/.claude` session, which alone may edit it. Filed there at implementation.
- Status synonyms on the MCP side (`abandoned`, `wontfix`). Callers send
  `dropped`. The pass-headings READER takes the synonyms because they already
  occur in hand-written files.
- A `resolution` reason for a drop. The body note carries it, as today.

## 6. Tests

Feature test: `tests/features/roadmap_dropped_status/`. Covers INV-1, INV-2,
INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9, INV-10 and INV-11.
Label `features;fast`. The dialog half (INV-9) is a second source file in the
dialogs bundle. The store and verb halves go in `test_claude`. Verify each
test fails against pre-fix source first.

## 7. Cross-doc impact

`docs/standards/roadmap-format.md`, `docs/standards/roadmap-data-model.md`,
`docs/standards/mcp-behavioural-notes.md` (its `status != 'shipped'` note),
ANTS-3758, ANTS-3757, ANTS-3793, the `roadmap_log` and `roadmap_query` schema
descriptions in `claudeintegration.cpp`, and CHANGELOG.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-19 | 3, cold, identical shared packet | 3 | 2 | 1 | 0 | Verified 6, fixed 6, dismissed 0. All three lanes independently found that `won't fix` can never match the pass-headings keyword capture (it stops at the apostrophe): dropped. Also fixed: `renderLegend` skips a key the stored legend lacks, so a default `🚫 Dropped (closed, not done)` row is now specified; the GFM reader's `tryStrip` chain and `applyGfmFlip`'s prefix list were wrongly called already-capable; § 7.3.1's and ANTS-3757 § 2.7's word tables and § 3.11's mixing anti-pattern were missing from § 2.9; the reader's bare-glyph map (found resolving a lane's open question). Four other open questions resolved clean (`closed` is migration-only; `isOpen` callers; task-list parse goes through `stripInlineEmoji`). |
