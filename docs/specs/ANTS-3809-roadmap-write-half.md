# ANTS-3809 — the write half: mutate the store, render, then commit

**Status:** spec draft (2026-08-04).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3809 (ANTS-3793 cold-eyes loop-3 split, 2026-08-03).
Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`,
which the user cut four ways after its cold-eyes run stopped at the loop cap.
This part carries the umbrella's § 2.4 and its INV-4, plus the filed findings
H3 and M6. See the loop log's `0-split` row.
**Covers:** ANTS-3809 — the **write** half of the consumer cutover only.
**Blocked by:** ANTS-3793 (the read seam: `RoadmapSource::migratedProject()`,
`storeFor()` and the owner wrapper this spec's § 2.1 extends) — accepted, not
yet built. ANTS-3808 (`RoadmapParse::trailerValuesIn()` and its
`TrailerMatch::anchored`, which § 2.5's refusal is built on) — accepted, not yet
built. ANTS-3758 (the render this spec calls) — shipped.
**Blocker for:** ANTS-3794 (publish + health checks).
**Pairs with:** ANTS-3810 (the round-trip oracle, which is what proves the
render this spec commits behind is lossless).

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 The write shape](#21-the-write-shape-validate-commit-publish) ·
[2.2 The eight ops](#22-the-eight-ops) ·
[2.3 Id allocation](#23-id-allocation) ·
[2.4 Locators the store cannot serve](#24-locators-the-store-cannot-serve) ·
[2.5 `body_shadowed`](#25-body_shadowed-a-column-write-the-body-would-hide) ·
[2.6 Body writes re-derive their columns](#26-body-writes-re-derive-their-columns)) ·
[3. Invariants](#3-invariants) ·
[4. RAM, latency and build cost](#4-ram-latency-and-build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

ANTS-3793 moves the consumers' **reads** onto the store. Their **writes** stay
where they are: every `roadmap_log` op splices markdown text and commits it with
`QSaveFile`. On a migrated project that is divergence by construction — the read
serves the store, the write edits the file, and the next read serves a store
that never saw the edit.

The surface, measured 2026-08-04:

```
$ awk '/^QJsonDocument RemoteControl::cmdRoadmapLog\(/,/^}/' src/remotecontrol.cpp \
    | grep -o 'op == QStringLiteral("[a-z_]*")' | sed 's/.*("//;s/")//' | sort -u
amend_body annotate append_batch bundle_row create_section flip flip_batch
$ grep -c 'QSaveFile [a-z]*(roadmapPath)' src/remotecontrol.cpp
10
```

Seven named ops plus `append`, the unnamed fallthrough
(`RemoteControl::cmdRoadmapLog`) — eight. The `grep -c` counts matching *lines*,
not call sites; the two coincide here only because each construction sits on its
own line, and the figure is a scale indicator rather than a count to implement
against.

**Three things make this more than a mechanical port, and each is a section
below.**

1. **The render is a gate, not a formatter.** `RoadmapRender::render()` refuses
   to write anything for a project holding an open item with no `layman`
   (`src/roadmaprender.cpp`, `isOpen(it->status) && it->layman.isEmpty()` →
   `Outcome::gateFailures`; ANTS-3758 INV-5). Measured on this project's own
   roadmap:

   ```
   $ awk 'function close_b() { if (inb && open) { o++; if (!lay) n++ } }
     /^- / { close_b(); inb=1; open=($0 ~ /^- (📋|🚧|💭)/);
             lay=($0 ~ /(\*\*)?[Ll]ayman:/); next }
     inb && /^[[:space:]]*(\*\*)?[Ll]ayman:/ { lay=1 }
     /^#/ { close_b(); inb=0 }
     END { close_b(); print o, n }' ROADMAP.md
   327 102
   ```

   327 open bullets, **102 of them with no `Layman:` line** (2026-08-04;
   `rxLayman` matches both the plain and the bold form, so the pattern above is
   the parser's own acceptance set). So on the day this project migrates, *every*
   write op would refuse — the gate is per project, not per item. That is a
   precondition this spec has to state and give a remedy for, not a detail.

2. **Two ops are not item writes.** `create_section` writes a `section` row;
   `bundle_row` edits one `element` row's payload — and the store has **no
   surface that can do the second at all** (§ 2.2).

3. **Two request shapes have no store meaning.** `line_range` addresses bullets
   by markdown line number, which ANTS-3793 § 2.1.1 fills with 0 on the store
   path; and `id_strategy: "stable_prefix"` allocates no counter id, so routing
   allocation through the store's high-water would break every stable-id project
   (`src/remotecontrol.cpp` — "stable_prefix path skips the counter entirely").

## 2. Surface

### 2.1 The write shape: validate, commit, publish

**On a migrated project every op becomes: mutate the store, render, then
commit.** No op grows a markdown writer of its own; the splice paths stay on the
unmigrated branch, unchanged, and are deleted by the id that retires markdown —
not this one.

**The ordering is the contract, and it is not the obvious one.**
`RoadmapRender::render()` writes its files itself and commits them per file
(`std::vector<std::unique_ptr<QSaveFile>> staged`, `src/roadmaprender.cpp`), so
a sequence that renders before committing the store leaves the *file* ahead of
the store when the store commit then fails — the wrong direction, because
`roadmap-data-model.md` INV-3 makes the store primary. `Options::dryRun`
("computes everything and writes nothing") is what makes the right sequence
expressible: **validate with a dry render, commit the store, publish with a real
one.**

```cpp
// src/roadmapwrite.h — declaring src/roadmapwrite.cpp, a new TU in
// ants_roadmapstore_lib beside roadmaprender.cpp. One function, because the
// ordering below is INV-1 and eight call sites would each get a chance to
// write it differently.
namespace RoadmapWrite {
    enum class Result {
        Ok,
        GateUnmet,     // the render's INV-5 gate — `gateFailures` is filled
        RenderFailed,  // the render could not express the mutated store
        StoreFailed,   // begin/commit/rollback, or the mutation itself, failed
        PublishFailed, // committed, but the file write did not land (see below)
    };

    // begin → mutate → dry render → commit → real render.
    //
    // `mutate` performs the op's store writes and returns false on failure
    // (setting *error). It is a callback rather than a shape this header
    // enumerates because the eight ops have nothing in common but the
    // transaction around them.
    //
    // Under `dryRun` the sequence stops after the dry render and rolls back —
    // so a caller's `dry_run:true` commits nothing on EITHER path, which is
    // what it already means on the markdown path.
    Result commitAndRender(RoadmapStore &store, qint64 projectId,
                           const QString &projectRoot,
                           const QString &liveRoadmapPath, bool dryRun,
                           const std::function<bool(QString *)> &mutate,
                           QStringList *gateFailures, QString *error);
}
```

The sequence, and what each failure does:

| Step | On failure |
|---|---|
| 1. `store.begin()` | `StoreFailed`. Nothing written. |
| 2. `mutate(error)` | `rollback()`, `StoreFailed`. |
| 3. `render(…, {liveRoadmapPath, dryRun = true})` → `nullopt` | `rollback()`, `RenderFailed`. |
| 4. …engaged with a non-empty `gateFailures` | `rollback()`, `GateUnmet`, ids copied out. |
| 5. caller asked for `dry_run` | `rollback()`, `Ok`. **The op's whole preview.** |
| 6. `store.commit()` | `rollback()`, `StoreFailed`. |
| 7. `render(…, {liveRoadmapPath, dryRun = false})` | `PublishFailed` — **the store is already committed and stays so.** |

**Step 7 is the one window, and leaving the store ahead is the deliberate
answer rather than a gap.** The store is primary; a file that is stale-behind is
exactly what ANTS-3794's health check is for and what re-running the render
fixes, whereas rolling the store back to match a file that may itself be
half-written (`Outcome::committed == false` with a non-empty `filesWritten` is
ANTS-3758 § 2.7's documented partial commit) would discard the user's edit to
match an artefact. `PublishFailed` is reported to the caller with the store
change intact and the remedy named.

**The op reaches the store through one new owner member, the write-side
companion to ANTS-3793's `roadmapBullets()`:**

```cpp
// RemoteControl — resolves the same dispatch the read seam does, and hands
// back the process-owned store rather than records. nullopt with
// `*why == None` means NOT migrated: splice markdown exactly as today.
struct RoadmapWriteTarget { RoadmapStore *store = nullptr; qint64 projectId = 0; };
std::optional<RoadmapWriteTarget>
roadmapWriteTarget(const QString &projectRoot, const QString &markdown,
                   RoadmapSource::ReadError *why, QString *error);
```

It is `RoadmapSource::storeFor()` plus `RoadmapSource::migratedProject()` and
nothing else — the same two calls, the same three outcomes, the same
process-owned `Access::Interactive` connection. A second dispatch rule here
would be a second answer to "is this project migrated", and the read and write
halves of one verb call must not be able to disagree.

**`RoadmapDialog` gains no write member.** It reads the roadmap and does not
write it; the 26 call sites ANTS-3793 § 1 counted are reads.

**Two response fields ride the read seam and are not restated here.**
`possible_duplicates` (`rcComputePossibleDuplicates`) and `return:
"headline_only"`'s `post_bullets` are both computed from `BulletRecord`s, so on
a migrated project they are served by ANTS-3793's wrapper like every other read.

### 2.2 The eight ops

| Op | Store writes |
|---|---|
| `append` | `putItem(ItemWrite{…, sectionId, position})` — which inserts the `element` row itself (`INSERT INTO element … 'item'`) — then `raiseIdHighWater()` (§ 2.3) |
| `append_batch` | N × the above in the one transaction, ids contiguous (§ 2.3) |
| `flip` | `setItemField(itemPk, "status", <lifecycle word>, "asserted")`, plus the body append below when a `note` is given |
| `annotate` | the body append alone — no status write. It is the flip that changes no status, and `cmdRoadmapLog` already routes both to one handler; that stays one path |
| `flip_batch` | N × `flip`, one transaction |
| `amend_body` | `setItemField(itemPk, "body", …, "asserted")` with the located single-line `old_text` replaced |
| `create_section` | `addSection(projectId, slug, title, level, position, parentId)`, plus the renumber below |
| `bundle_row` | read-modify-write of one `kind='table'` element (below) |

Every op that writes `body` also re-derives that item's trailer columns — § 2.6,
and it is what makes § 1's gate remediable.

**`create_section` renumbers, and it is safe to.** The op inserts after a named
section, so every later section's `position` shifts by one, applied with
`updateSection(sectionId, title, level, position, parentId)` in the same
transaction. `section` is **deliberately not** `UNIQUE (project_id, position)`
(`src/roadmapstore.cpp`, the DDL comment says so outright), so the renumber has
no transient-collision problem — and it is still required rather than optional,
because `sectionOrderLess()`'s key is `(position, slug)` and two sections left
sharing a position would order by slug, not by where the caller put them.

**`bundle_row` needs a store surface that does not exist, and this is the
spec's one schema-adjacent addition.** ANTS-3756 § 2.3 and its INV-24 make
`element.payload` **canonical JSON** when `kind = 'table'` — the
`roadmap_store_schema` test asserts it against `{"rows":[["x"]],"header":["h"]}`
— so the op is a read-modify-write of one element's payload, not an append of a
new element and not a rendered markdown row. But `addElement()` is INSERT-only,
`clearSectionElements()` is the whole section, and `ElementRow` carries
`position`, `kind`, `payload`, `itemPk` and `itemIdFold` and **no element id**.
There is no way to reach one element's payload through the declared surface. So:

```cpp
// src/roadmapstore.h — keyed on UNIQUE (section_id, position), which is the
// only stable handle listElements() hands back. Canonicalises for
// kind='table' exactly as addElement() does, and refuses kind='item'
// outright for the reason addElement() does: putItem()/fileItem() stay the
// only ways an item is filed (INV-10, INV-20).
bool setElementPayload(qint64 sectionId, int position, const QString &payload,
                       QString *error = nullptr);
```

An op targeting a section with no `kind='table'` element creates one with
`addElement()` at the end position, which is the `header`-supplied
create-the-table case the markdown path already has.

### 2.3 Id allocation

On the markdown path `append` reads its high-water from `.roadmap-counter`.
On a migrated project it reads the store — **and keeps the corpus floor**:

> **allocated = max(`idHighWater(projectId, prefix)`,
> `RoadmapFoldIn::corpusHighWater(projectRoot, prefix)`) + 1**, then
> `raiseIdHighWater(projectId, prefix, allocated)`. The counter file is not
> written.

**The floor is not belt-and-braces.** `roadmap-format.md` § 3.5.1 defines
`.roadmap-counter` as *"a derived, per-machine cache — NOT source (ANTS-3450)"*,
gitignored, whose true value is the highest id across the committed corpus; both
existing allocators already floor to `RoadmapFoldIn::corpusHighWater()`
(`src/remotecontrol.cpp`, the `append` and `append_batch` paths). Swapping in
`idHighWater()` alone would silently drop that floor, which is the mechanism
that stops a fresh clone reissuing a live id. § 7 amends the standard to say
which carrier is authoritative during the interim; it does not overturn
ANTS-3450.

**`idHighWater()` returning `nullopt` is not an error** — its own comment says
"Absent row ⇒ nullopt, which is not an error", and it is the state of every
project until its first store-side allocation. Treat it as 0 and let the corpus
floor supply the value, which is what the markdown path already does for an
absent counter file.

**`idPrefixFor(projectId)` supplies the prefix `idHighWater()` takes as an
argument**, and returns `nullopt` for a project with no id-bearing item yet — an
absent row, explicitly not an error. On `nullopt` the op falls back to the
prefix the caller passed or derived (`rlResolveCounterPrefix()`) exactly as it
does today: a project with nothing to be consistent with cannot have drift.

**`append_batch` allocates contiguously from one read.** `first_id + i` for
`i` in `[0, n)`, one `raiseIdHighWater()` with the last, inside the one
transaction — matching the markdown path's contiguity guarantee, which callers
rely on to cite ids before the batch returns.

**`id_strategy: "stable_prefix"` is untouched by cutover.** It is a live
argument under which `append` and `append_batch` allocate no counter id and take
the caller's `stable_id` verbatim (`src/remotecontrol.cpp` — "stable_prefix
strategy skips the counter machinery entirely"; `append_batch` requires one per
bullet, ANTS-2078). Everything above is scoped to `id_strategy: "counter"`. The
item is still `putItem()`ed with its stable id; `idHighWater` /
`raiseIdHighWater` are not consulted and not written, because a stable string id
is not a counter value and seeding a counter from one would corrupt the next
counter-project allocation on the same store.

### 2.4 Locators the store cannot serve

**`line_range` refuses with `locator_unsupported`.** It is a `flip_batch`-only
locator (`cmdRoadmapLogFlipBatch`; the single `flip` handler's mention of it is
a hint string pointing at the batch op) and it resolves by comparing
`b.firstLine + 1` against the range. ANTS-3793 § 2.1.1 fills `firstLine` and
`lastLine` with **0** on the store path, so on a migrated project the predicate
`0 + 1 >= a && 0 + 1 <= z` is true for **every** bullet whenever the range
starts at line 1 — a locator that silently flips the whole roadmap, which is
strictly worse than one that matches nothing. It is refused per locator, landing
in `flip_batch`'s existing `skipped[]` array alongside the `bad_op_combo` refusal
`anchor` already takes on an ants-v1 roadmap, so a mixed batch still applies its
other locators.

`id`, `headline` and `anchor` need no change: they resolve against
`BulletRecord` fields the store path fills.

### 2.5 `body_shadowed`: a column write the body would hide

**The ops that write a trailer column from the request are `append` and
`append_batch`** — the only two taking `kind` / `source` / `lanes` / `layman` /
`evidence` as arguments. `flip` writes `status`, which is not a trailer key;
`annotate`, `amend_body`, `create_section` and `bundle_row` write no column at
all. § 2.6's re-derivations do write columns and are exempt, for the reason at
the foot of that section.

Each such write first asks `RoadmapParse::trailerValuesIn()` what the `body`
arriving in the same request yields for that key. **If the body yields a value,
the column value differs from it, and that match's `TrailerMatch::anchored` is
false, the op refuses with `body_shadowed`, naming the shadowing sentence**
(quoted from `TrailerMatch::offset`, which ANTS-3808 pins as `capturedStart(1)`
in QString positions).

**The test is `anchored`, uniformly across all five keys, and not "which
patterns carry `^`".** `rxSource` and `rxLanes` are un-anchored outright, but
`rxKind`, `rxLayman` and `rxEvidence` are `^`-anchored **with
`MultilineOption`**, so a continuation line that merely *begins* `Kind:` in
prose matches them too (ANTS-3808 § 2.3.1). Scoping the refusal to the
un-anchored pair — as an earlier draft did — leaves that case live, and
ANTS-3808's INV-3 has no exclusion clause to absorb it. `TrailerMatch::anchored`
exists for exactly this question and is computed per match, not per pattern.

Without the refusal the render emits the canonical column line while an
un-anchored re-parse reaches the prose sentence first: a bullet that renders
correctly and re-parses wrongly, which the next migration would commit as data
loss. Refusing is safe because the case is rare *and* self-clearing — the caller
rewrites the sentence or backticks the key, and the write goes through.
ANTS-3722's backtick guard already treats the shape as the mistake it usually is.

`locator_unsupported` and `body_shadowed` are **two codes for two faults**, with
unrelated remedies ("use a different locator" versus "rewrite or backtick a
sentence"); `docs/standards/mcp-error-codes.md` requires a caller to branch on
`code` alone. Both are new to that taxonomy, and so is the third (§ 2.6);
§ 7 files all three.

### 2.6 Body writes re-derive their columns

**Any op that writes `body` re-derives the item's five trailer columns from the
new body through `trailerValuesIn()` and writes each one that changed**, in the
same transaction, with provenance `asserted`.

This is what makes § 1's gate remediable, and without it the write half is
unusable on this project on day one. `Layman:` is a *body* line in markdown and
a *column* in the store. An `annotate` or `amend_body` that adds the line to the
body but not the column leaves the render's `layman.isEmpty()` gate failing
forever, with no op able to clear it — 102 items on this project's roadmap
(§ 1). Re-deriving fixes that with the accessor ANTS-3808 already exports, and
it keeps the two representations from drifting in every other direction too.

The two directions do not collide: § 2.5 guards a **column** write the body
would shadow, and this rule derives a **column** write *from* the body, where
the body is by definition the authority. A re-derivation whose match is
un-anchored is written, not refused — the value it writes is the one the body
already carries, so there is nothing for the body to shadow.

**When a re-derivation would clear a column, it does** —
`clearItemField(itemPk, field, "asserted")` and not an empty
`setItemField()`, because `putItem()` binds an empty `body` as SQL NULL while
`setItemField()` binds the string it is given, and ANTS-3761's INV-2 column diff
reads `''` and NULL as different (`src/roadmapstore.h`, `clearItemField()`'s own
comment).

**A render gate that is still unmet after a write refuses with
`render_gate_unmet`**, carrying the offending ids from `Outcome::gateFailures`,
and the store is rolled back (§ 2.1 step 4). The message names the remedy: give
each listed item a `Layman:` line.

## 3. Invariants

Renumbered from the umbrella, matching the sibling parts' convention (ANTS-3793
and ANTS-3808 both renumbered from 1). The mapping, so the split record on the
ROADMAP bullet resolves: **umbrella INV-4 → INV-1**. Every other invariant here
is new to this part.

- **INV-1** — **A `roadmap_log` op whose validating render does not succeed
  leaves the store unchanged.** The render runs under `Options::dryRun` *before*
  `commit()`; `nullopt`, or an engaged `Outcome` with a non-empty
  `gateFailures`, rolls the transaction back and refuses. *Breaks when:* the
  store transaction commits before any render runs; the validating render is
  the real one, so a gate failure leaves files already staged; or a
  `gateFailures` result is treated as success because the `Outcome` was engaged.
  *Test:* `Inv1RenderFailureRollsBack`, three cases — an op on a project holding
  an open item with no `layman` (asserts `render_gate_unmet`, and that the
  item's pre-op field values are intact), an op whose render is failed by
  filing an item in no section (ANTS-3758 INV-4's refusal), and a successful
  op (asserts the field *did* change, so the case can tell a rollback from a
  no-op).
- **INV-2** — **No op writes roadmap markdown by hand on a migrated project.**
  Every byte of a migrated project's roadmap files is written by
  `RoadmapRender::render()`. *Breaks when:* an op keeps its splice path behind
  the store write "to be safe", or writes the file itself after mutating.
  *Test:* `Inv2NoSplice`, which drives one op of each of the eight kinds against
  a migrated fixture with `RoadmapWrite::commitAndRender()`'s publish step
  suppressed, and asserts the roadmap file is byte-unchanged — a splice would
  have written it regardless.
- **INV-3** — **Counter allocation on a migrated project floors to both
  high-waters, and `stable_prefix` allocates nothing.** The id is
  `max(idHighWater(), corpusHighWater()) + 1` under `id_strategy: "counter"`,
  and `raiseIdHighWater()` records it; under `stable_prefix` neither is read or
  written and the caller's `stable_id` is used verbatim. *Breaks when:*
  `idHighWater()` alone drives allocation, so a store seeded on a fresh clone
  reissues an id the committed corpus already holds; `nullopt` from
  `idHighWater()` is treated as an error rather than as 0; or a `stable_prefix`
  append seeds the counter high-water from a string id.
  *Test:* `Inv3Allocation`, four cases — a store high-water above the corpus, a
  corpus high-water above the store (the fresh-clone case, and the one that
  fails if the floor is dropped), an `append_batch` of three asserting
  `first_id`, `first_id+1`, `first_id+2` and one high-water raise, and a
  `stable_prefix` append asserting `idHighWater()` is still `nullopt`
  afterwards.
- **INV-4** — **An op that writes `body` leaves the item's five trailer columns
  equal to what `trailerValuesIn()` yields from the new body.** *Breaks when:*
  a body write skips the re-derivation, so a `Layman:` line added by `annotate`
  never reaches the column and the render's gate cannot be cleared; or a
  re-derivation that should clear a column writes `''` through `setItemField()`
  instead of NULL through `clearItemField()`.
  *Test:* `Inv4BodyDerivesColumns`, which `annotate`s a `Layman:` line onto an
  item whose `layman` column is empty and asserts the column, then asserts the
  same op makes the previously gate-failing render succeed. The second assertion
  is the one that cannot pass by accident: it fails against an implementation
  that writes the column but derives it from the *request* rather than from the
  resulting body.
- **INV-5** — **A trailer-column write whose current body carries a different
  value at an un-anchored match is refused, not written.** The test is
  `TrailerMatch::anchored`, over all five keys.
  *Breaks when:* the refusal is scoped to `source`/`lanes` on the reasoning that
  the other three patterns are `^`-anchored — `MultilineOption` makes that
  false; or it is scoped to key *presence* rather than to a differing value,
  which would refuse every ordinary bullet that carries its own `Kind:` line.
  *Test:* `Inv5BodyShadowed`, three cases — a body carrying `Source: x` mid
  sentence plus a column write of `y` (refuses, and the message quotes the
  sentence); the same body with a matching value (writes); and a body whose
  **continuation line begins** `Kind:` plus a differing column write (refuses —
  the case an un-anchored-pair-only rule lets through).
- **INV-6** — **`line_range` is refused on a migrated project and never
  resolves.** *Breaks when:* the locator is left to resolve against the store
  path's zeroed `firstLine`, where a range beginning at line 1 matches every
  bullet in the project. *Test:* `Inv6LineRangeRefused`, a `flip_batch` with two
  locators — one `line_range` covering `[1, 10]` and one `id` — asserting the
  range lands in `skipped[]` with `locator_unsupported`, that the `id` locator
  still applied, and that **no other item's status changed**, which is what
  distinguishes a refusal from a silently empty match.
- **INV-7** — **`dry_run: true` commits nothing, on either path.** The store
  transaction is rolled back after the validating render, and no file is
  written. *Breaks when:* the mutate-then-render sequence is entered without
  checking the flag, so the preview is produced by a real write; or the flag is
  honoured for the file and not for the store. *Test:* `Inv7DryRunCommitsNothing`,
  one case per op kind, asserting the item/section/element row is unchanged
  **and** the roadmap file's bytes are unchanged, against a preview envelope
  that still carries the would-be result.
- **INV-8** — **`bundle_row` edits one existing `kind='table'` element's
  canonical-JSON payload.** *Breaks when:* the op appends a second `table`
  element to the section, or writes a rendered markdown row into a column
  ANTS-3756 INV-24 defines as canonical JSON. *Test:* `Inv8BundleRow`, two
  cases — a second row appended to a section that already has a table (asserts
  one `kind='table'` element still, and `canonicalJson()` round-trips the
  payload), and a first row on a section with none (asserts exactly one element
  was created).

## 4. RAM, latency and build cost

**The added cost of a write op is two full render walks, and the file write is
not new.** `render()` is `listSections()` + `listItems()` + one `readItem()` per
item + `listElements()` per section — the same N+1 walk ANTS-3793 § 2.1.3
describes, and this spec runs it twice (validate, then publish). On this
project, the corpus's largest roadmap:

| Term | Value (2026-08-04) |
|---|---|
| Items walked per render | 1,844 (`grep -cE '^- (📋\|🚧\|✅\|💭)' ROADMAP.md`) |
| Render walks per op | 2 |
| Roadmap files rewritten per op | 3 — the live file plus 2 archives (`ls docs/roadmap/*.md \| wc -l` → 2) |
| Bytes rewritten per op | ~3.1 MB (`wc -c ROADMAP.md` → 3,110,905, plus 12 KB of archives) |

**The 3.1 MB rewrite is not a regression.** Today's splice path reads the whole
file, edits a `QStringList` and writes the whole file back through `QSaveFile` —
the same bytes. What is new is the second store walk, and the two archive files
the render rewrites whether or not they changed (`render()` stages every file it
emits; it has no unchanged-skip). The archives are 12 KB, so the cost is
mtime churn rather than I/O, and it is worth naming because a `git status` after
an unrelated `flip` will show them.

**RAM** is `bulletsFromStore()`'s peak plus one rendered-text buffer, both
already priced by ANTS-3793 § 4; this spec adds no long-lived state. The
`RoadmapStore` connection is ANTS-3793 § 2.2's process-owned one, shared with
the read half — a write-side connection of its own would double the page cache
and could deadlock against the read on `BEGIN IMMEDIATE`.

**Build cost:** one new TU, `src/roadmapwrite.cpp`, in `ants_roadmapstore_lib`
beside `roadmaprender.cpp`. No new link edge: the callers are in
`ants_core_lib`, which ANTS-3793 § 4 already gives a `PRIVATE` edge to
`ants_roadmapstore_lib`. No new external library.

**A latency budget is deliberately not asserted here.** ANTS-3793's INV-3 pins
`p95 < 50 ms` for a *read*, which every consumer performs on every verb call;
a write op is rare, already pays a 3.1 MB file write, and a number nothing
measures is a comment (`spec-format.md` § 5.8). If ANTS-3816's batched reader
lands, both render walks get it for free.

## 5. Out of scope

- **The read seam** — the resolver, the dispatch marker and the dialog are
  **ANTS-3793**, whose `storeFor()`, `migratedProject()` and `roadmapBullets()`
  this spec consumes as a fixed contract.
- **`item.body`'s storage rule, the trailer suppression and
  `trailerValuesIn()`** — **ANTS-3808**. § 2.5 and § 2.6 consume its
  `TrailerMatch`; neither defines it.
- **The round-trip oracle and relationship acyclicity** — **ANTS-3810**. That
  spec is what proves the render this one commits behind is lossless; until it
  lands, INV-1's guarantee is that a render *failure* is caught, not that a
  render *success* is faithful.
- **Backfilling the 102 missing `Layman:` lines.** § 2.6 gives the ops that can
  fix them one at a time and INV-1 makes the refusal loud; a corpus sweep is a
  content task, not a code one, and it is **ANTS-3821** (filed by this spec).
- **Deleting the markdown splice paths.** They serve every unmigrated project
  for as long as the rollout takes, and the id that retires them is not this one.
- **Non-emoji formats.** The store path serves `ants-v1` only, by the same
  § 2.2 dispatch the read half uses; a migrated GFM or pass-headings project
  keeps splicing markdown. Permanent for this spec, not deferred: ANTS-3758's
  render emits `ants-v1` and nothing else.
- **An `element` reader keyed by element id.** § 2.2's `setElementPayload()` is
  keyed on `(section_id, position)` because that pair is `UNIQUE` and is what
  `listElements()` already returns. Exposing `element_id` would be a wider
  surface than one op needs.

## 6. Tests

`tests/features/roadmap_write_half/`, label `features`, compiled into the
**`test_core` bundle** per `tests/features/README.md` (no `add_executable`) —
the same bundle ANTS-3793 § 6 uses, and for the same reason: it is the only one
linking both `ants_core_lib` and `ants_roadmapstore_lib`.

| Case | Invariants |
|---|---|
| `Inv1RenderFailureRollsBack` | INV-1 |
| `Inv2NoSplice` | INV-2 |
| `Inv3Allocation` | INV-3 |
| `Inv4BodyDerivesColumns` | INV-4 |
| `Inv5BodyShadowed` | INV-5 |
| `Inv6LineRangeRefused` | INV-6 |
| `Inv7DryRunCommitsNothing` | INV-7 |
| `Inv8BundleRow` | INV-8 |

Each case builds its store by migrating a small markdown fixture, so the
starting state is one the migration can actually produce — a hand-built store
can hold rows the loader never writes, and an invariant asserted against one is
asserted against a state the product cannot reach.

**Never default-construct `RoadmapStore` in a case.** `defaultPath()` resolves
under `XDG_DATA_HOME`, i.e. the developer's real roadmap store; every case
passes an explicit temp path. `Access` is the **third** constructor parameter,
after `historyCapBytes`, and consumers use `Access::Interactive`.

Per the project test convention, each case is verified to **fail against
pre-fix source** before the implementation is restored. For INV-2 and INV-6 that
is immediate — today's code splices and resolves respectively. For INV-1,
INV-3, INV-4, INV-5, INV-7 and INV-8 the pre-fix state has no store write path
at all, so the must-fail-first proof is run against the *first* implementation
with the rule under test removed: the rollback deleted, the corpus floor
dropped, the re-derivation skipped, the `anchored` test replaced by the
un-anchored-pair scope, the `dry_run` check removed, and `setElementPayload()`
replaced by an `addElement()`.

## 7. Cross-doc impact

- **`RoadmapStore` gains `setElementPayload()`** (§ 2.2) — a surface addition to
  ANTS-3756, in the shape ANTS-3758 § 2.1 used for `listElements()` and
  ANTS-3793 § 2.2 used for `readProjectByRoot()`. Without it `bundle_row` has no
  store form at all.
- **`docs/standards/mcp-error-codes.md` gains three codes** —
  `locator_unsupported` (§ 2.4), `body_shadowed` (§ 2.5) and
  `render_gate_unmet` (§ 2.6). All three are new to the taxonomy.
  Recorded while verifying that: **`bad_op_combo` is used 23 times across
  `src/remotecontrol.cpp` and `src/claudeintegration.cpp` and appears nowhere in
  the standard** (verified 2026-08-03) — a pre-existing documentation gap this
  spec inherits rather than causes, and not this id's to fix.
- **`roadmap-format.md` § 3.5.1** is amended to say which id carrier is
  authoritative during the interim: on a migrated project the store's
  `id_high_water` row, still floored to the committed corpus. It does **not**
  overturn ANTS-3450's "the counter is a derived cache, not source" — § 2.3 is
  that rule applied to a second carrier.
- **`RemoteControl` gains one `roadmapWriteTarget()` member** (§ 2.1), the
  write-side companion to ANTS-3793's `roadmapBullets()`.
- **`src/roadmapwrite.cpp` joins `ants_roadmapstore_lib`** and
  **`docs/subsystems.md`** gains `roadmapwrite`. Not `CLAUDE.md` — its module
  map has been a pointer since ANTS-1292.
- **`docs/standards/mcp-tools.md`'s per-verb notes** gain the migrated-project
  behaviour of `roadmap_log`: three new refusal codes and `line_range`'s
  unavailability.
- **`roadmap-data-model.md`'s *What checks this* table** gains INV-1 against the
  store-primary rule its own INV-3 states.
- **The 102 open items with no `Layman:` line are filed as ANTS-3821** (§ 5).
  It blocks nothing in this spec, and it blocks *using* the write half on this
  project — which is why it is filed rather than mentioned.
- **ANTS-3793's § 5 already names this id** as the owner of the eight ops, the
  `line_range` locator, id allocation and the `body_shadowed` refusal. No
  amendment owed there; recorded so a reader does not go looking.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-split | 2026-08-04 | 0 (no reviewer dispatched — a document operation) | — | **Split from the 934-line umbrella `ANTS-3793-roadmap-consumer-cutover.md` as the WRITE HALF only.** Not a review loop and not inherited review: the umbrella's three loops ran against a document that no longer exists, so the gate runs from loop 1 on these bytes. Sections carried over: old § 2.4 and INV-4 (renumbered to INV-1 per § 3). Filed findings folded in from [`docs/reviews/ANTS-3793-cold-eyes-loop3-tail.md`](../reviews/ANTS-3793-cold-eyes-loop3-tail.md): **H3** (`id_strategy: "stable_prefix"` allocates no counter id) → § 2.3's last paragraph and INV-3's fourth case; **M6** (`idHighWater()`'s `nullopt` case, `append_batch` contiguity, and `dry_run` under mutate-then-render) → § 2.3 and INV-7. Three defects were found by grounding rather than carried: the render's INV-5 gate is per project and fails on 102 of this roadmap's 327 open items, so every write op would refuse (§ 1, § 2.6, ANTS-3821); `bundle_row`'s read-modify-write has **no store surface** — `addElement()` is INSERT-only and `ElementRow` carries no element id (§ 2.2's `setElementPayload()`); and `line_range` on the store path does not match *nothing*, it matches *everything*, because `firstLine` is 0 and the predicate is `firstLine + 1 >= a` (§ 2.4). The umbrella's ordering (mutate, render, commit) was also corrected: `render()` commits its own files, so that sequence leaves the file ahead of the store on a commit failure — § 2.1 validates with `Options::dryRun` first instead. |
