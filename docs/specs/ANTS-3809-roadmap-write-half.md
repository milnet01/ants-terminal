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
yet built. ANTS-3808 (`RoadmapParse::trailerValuesIn()`, whose per-key values
are § 2.5's refusal predicate and § 2.6's derivation source, and whose
`TrailerMatch::offset` / `anchored` locate and classify what § 2.5 refuses
over) — accepted, not yet built. ANTS-3758 (the render this spec calls) — shipped.
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
    // Every value below reaches an MCP envelope, so every one names a `code`
    // (§ 7 files them): render_gate_unmet, render_failed, store_failed, and
    // the taxonomy's existing write_failed. A caller must be able to branch on
    // `code` alone (`mcp-error-codes.md`), and four of these five have
    // genuinely different remedies — fill in the missing Layman lines, fix the
    // store's contents, fix the store file, re-run the render.
    enum class Result {
        Ok,
        GateUnmet,     // the render's INV-5 gate  → `render_gate_unmet`
        RenderFailed,  // the render could not express the mutated store
                       //                          → `render_failed`
        StoreFailed,   // begin/commit/rollback, or the mutation itself, failed
                       //                          → `store_failed`
        PublishFailed, // committed, but the file write did not land (below)
                       //                          → `write_failed` (existing)
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
    //
    // `outcome` receives the render's own Outcome — the LAST render that ran,
    // so the dry one under `dryRun` and the publishing one otherwise. It is
    // the whole render result and not a bare gateFailures list because three
    // separate obligations need the rest of it: GateUnmet's envelope needs
    // `gateFailures`, `dry_run`'s preview envelope needs `filesWritten` /
    // `itemsRendered` (the markdown path returns a would-be result today and
    // this must not regress — INV-7), and PublishFailed's message needs
    // `committed` + `filesWritten` to tell a total failure from ANTS-3758
    // § 2.7's partial commit. Left untouched when no render ran.
    Result commitAndRender(RoadmapStore &store, qint64 projectId,
                           const QString &projectRoot,
                           const QString &liveRoadmapPath, bool dryRun,
                           const std::function<bool(QString *)> &mutate,
                           RoadmapRender::Outcome *outcome, QString *error);
}
```

**`liveRoadmapPath` is resolved by the caller — `RemoteControl`, from the same
`.ants/project.json` `roadmap` override its markdown path already reads.**
`RoadmapRender::Options` requires it for the reason its own comment gives: the
render's library does not link `projectsettings.cpp` and cannot read that file.
`commitAndRender()` passes it through and resolves nothing itself.

The sequence, step by step. Rows 5 and 7 are the two exits that are not
failures:

| Step | Outcome |
|---|---|
| 1. `store.begin()` fails | `StoreFailed`. Nothing written. |
| 2. `mutate(error)` fails | `rollback()`, `StoreFailed`. |
| 3. `render(…, {liveRoadmapPath, dryRun = true})` → `nullopt` | `rollback()`, `RenderFailed`. |
| 4. …engaged with a non-empty `gateFailures` | `rollback()`, `GateUnmet`. |
| 5. caller asked for `dry_run` | `rollback()`, **`Ok`** — the op's whole preview, read off `*outcome`. |
| 6. `store.commit()` fails | `rollback()`, `StoreFailed`. |
| 7. `render(…, {liveRoadmapPath, dryRun = false})` fails | `PublishFailed` — **the store is already committed and stays so.** |
| 8. everything succeeded | **`Ok`**, store committed and file written. |

**A failed `commit()` at step 6 is rolled back, and a `rollback()` that itself
refuses is folded into the same `StoreFailed`.** SQLite leaves the transaction
open when a `COMMIT` fails, so the `rollback()` is the right call and normally
succeeds; `RoadmapStore::rollback()` "refuses when none is open", so the one
case where it does not is the one where there is nothing left to roll back.
Reporting a second failure there would tell the caller about the recovery
instead of about the fault.

**Step 7 is the one window, and leaving the store ahead is the deliberate
answer rather than a gap.** The store is primary; a file that is stale-behind is
exactly what ANTS-3794's health check is for and what re-running the render
fixes, whereas rolling the store back to match a file that may itself be
half-written (`Outcome::committed == false` with a non-empty `filesWritten` is
ANTS-3758 § 2.7's documented partial commit) would discard the user's edit to
match an artefact. `PublishFailed` is reported with the store change intact and
the remedy named, which is what `*outcome` carries the render result for.

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
`addElement(sectionId, position, "table", payload)` at
**`max(position) + 1` over `listElements(sectionId)`**, or 0 for an empty
section — spelled out because `element` carries `UNIQUE (section_id, position)`
and item rows occupy positions too, so "at the end" left unpinned is a
constraint violation rather than a mis-placement. That is the
`header`-supplied create-the-table case the markdown path already has.

**The op's three markdown-shaped arguments keep their meaning and move inside
the payload.** `header` names the columns, and on the store path it is the
`"header"` key of the canonical-JSON payload the create case writes;
`position: "sorted"` and `sort_col` decide where the new row lands **within
`"rows"`**, which is now an array insert rather than a line splice. None of the
three touches the `element.position` above — that is where the *table* sits in
the section, and these three are about where a *row* sits in the table.

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
argument**, and returns `nullopt` when the project has **no `id_prefix` row** —
an absent row, explicitly not an error. That is a narrower condition than "no
id-bearing item": the row is written by `raiseIdHighWater()`, so a project whose
items were all migrated in without an allocation ever running has ids and no
row. On `nullopt` the op falls back to the prefix the caller passed or derived
(`rlResolveCounterPrefix()`) exactly as it does today, and the corpus floor
below still applies — which is what keeps that case from reissuing an id.

**`id_hint` keeps its meaning and its refusal.** It is a live `append` argument
that pins the id explicitly, and the markdown path refuses `id_taken` when the
hint is at or below the highest existing id for that prefix. On a migrated
project the comparison floor is the same one allocation uses —
`max(idHighWater(), corpusHighWater())` — so a hint above it is honoured and
`raiseIdHighWater()` records it, and a hint at or below it is `id_taken`. The
code is unchanged; only what it is compared against moves.

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
in `flip_batch`'s existing `skipped[]` array, so a mixed batch still applies its
other locators.

**`anchor` needs no rule here because it never reaches the store path**, and
saying it "resolves like `id` and `headline`" would be wrong twice over.
ANTS-3793 § 2.1.1 fills `anchor` — with `sourceStatus` and `passDesignator` —
as **empty** on the store path, "an artefact of a dialect the store path does
not serve"; and the store path serves `ants-v1` only (§ 5), where both
`cmdRoadmapLogFlip` and `cmdRoadmapLogFlipBatch` already refuse an `anchor`
locator with `bad_op_combo` before any backend is chosen. The format-level
refusal fires first, so an `anchor` locator cannot arrive at a zeroed field.

`id` and `headline` do resolve, against `BulletRecord` fields the store path
fills. One caveat on `id`, inherited from the reader rather than introduced
here: ANTS-3793 § 2.1.1 takes `rec.id` **from the rendered head line, not from
the `id` column**, so on an item with an empty `id` column, or an off-grammar
quarantined one, the locator matches what `roadmap_query` returned rather than
what the column holds. That is correct — a caller locates by the id it was
shown — and it is named so an implementer does not "fix" it by matching the
column.

**Two codes for one apparent class, and the split is deliberate.**
`bad_op_combo` above is a **format** refusal: `ants-v1` has no caret anchors and
never did, on every project, migrated or not. `locator_unsupported` is a
**backend** refusal: the locator is fine on this format and cannot be served by
this project's store. Different preconditions, different remedies — "this
locator does not exist here" versus "this project can no longer be addressed
that way" — so a caller branching on `code` alone gets the right answer from
each. The residual inconsistency is that the shipped `bad_op_combo` message
recommends `line_range` as the alternative to `anchor`, advice this spec makes
wrong on exactly the projects in scope; § 7 files the message correction.

### 2.5 `body_shadowed`: a column write the body would hide

**The ops that write a trailer column from the request are `append` and
`append_batch`** — the only two taking `kind` / `source` / `lanes` / `layman` /
`evidence` as arguments. `flip` writes `status`, which is not a trailer key.
`annotate`, `amend_body`, `create_section` and `bundle_row` write no column
*from the request*; the first two do write columns, but only as § 2.6's
re-derivations, which are exempt for the reason at the foot of that section.

Each such write first asks `RoadmapParse::trailerValuesIn()` what the `body`
arriving in the same request yields for that key. **If the body yields a value
for the key and the column value differs from it, the op refuses with
`body_shadowed`, quoting the shadowing text** (located by
`TrailerMatch::offset`, which ANTS-3808 pins as `capturedStart(1)` in QString
positions — UTF-16 code units, not bytes).

**The predicate is value difference alone, over all five keys — `anchored` is
not part of it, and making it part of it would leave the commonest case live.**
The reasoning is short and it is the whole section:

- A rendered bullet is head line, then body, then the canonical trailer lines.
  Every one of the five matchers takes its **first** match over that text, so a
  body occurrence is reached before the column's own line, whatever either
  looks like.
- `TrailerMatch::anchored` is true when the match **begins a line**
  (ANTS-3808 § 2.2.1: `m == capturedStart(0)`, then
  `m == 0 || body.at(m - 1) == '\n'`). So a stale continuation line reading
  `Kind: refactor` is `anchored == true` — and it is the *likeliest* shadowing
  shape there is, because it is what a hand-edited or half-updated bullet
  looks like. A rule that fired only on `anchored == false` would miss it while
  appearing to be the general fix. That was this section's own earlier draft.
- `rxKind`, `rxLayman` and `rxEvidence` carry `^` **with `MultilineOption`**
  (ANTS-3808 § 2.3.1), which is why that shape matches at all; `rxSource` and
  `rxLanes` are un-anchored outright. Neither property discriminates a
  canonical trailer line from prose, so neither can gate the refusal.

**`anchored` still earns its place — in the message, not the predicate.** It is
what separates the two remedies a caller is owed: `anchored == true` is a stale
trailer line in the body, fixed by deleting or correcting that line;
`anchored == false` is a mid-sentence mention, fixed by rewording it or
backticking the key. A refusal that named neither would be a refusal a caller
cannot act on.

**Value difference, not presence, and that distinction is what keeps the rule
from refusing every ordinary bullet.** A migrated item's body agrees with its
columns by construction (ANTS-3808 § 2.3.1), so the body's value equals the
column's and nothing refuses. The rule fires only where a consumer is about to
put a value in the column that the body will out-vote — which is precisely the
write ANTS-3808 § 2.3.1 hands to this spec.

Without the refusal the render emits the canonical column line while the
re-parse reaches the body's occurrence first: a bullet that renders correctly
and re-parses wrongly, which the next migration would commit as data loss.
Refusing is safe because the case is rare *and* self-clearing — the caller fixes
the body and the write goes through. ANTS-3722's backtick guard already treats
the shape as the mistake it usually is.

**`append`'s two rules do not collide, because they act on different keys.**
An `append` request carries both a `body` and up to five columns, so § 2.5 and
§ 2.6 could both claim it; the order is per key and not per op. **A key the
request supplies is the caller's, is written as given, and is shadow-checked
here. A key the request omits is derived from the body under § 2.6 and is never
shadow-checked** — it came from the body, so the body cannot contradict it.
This is also what lets a caller create an item whose `Layman:` line lives in the
body without repeating it as an argument.

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

**A re-derivation is never shadow-checked, and that is not an inconsistency
with § 2.5.** That section guards a column value the body would out-vote; a
derived value *came from* the body, so there is nothing left to out-vote it.
§ 2.5's last paragraph settles the one op where both could apply.

**Three of the five keys go through `setItemField()`; `lanes` and `evidence`
cannot, and the difference is a real API constraint rather than a detail.**
`setItemField(itemPk, field, value, provenance)` takes a **`QString`**, while
`lanes` and `evidence` are JSON-array columns — `ItemWrite` carries them as
`QStringList` and `roadmapstore.h`'s ANTS-3767 comment holds them canonical "on
the way in … it binds every writer, not just the export". Only `setLegend()` and
`addElement(kind='table')` canonicalise for their caller, so this one does not.
So:

| Key | Written as |
|---|---|
| `kind`, `layman`, `source` | `setItemField(pk, key, tv.<key>.value, "asserted")` |
| `lanes`, `evidence` | `setItemField(pk, key, RoadmapStore::canonicalJson(<the split list as a QJsonArray>), "asserted")` — the list forms are `tv.lanesList` / `tv.evidenceList`, which ANTS-3808 § 2.2.1 pins |

`canonicalJson()` already takes a `QJsonValue` rather than a `QJsonObject`
precisely so an array can go through it (its own comment says so, ANTS-3767).
An implementer who passes the raw `tv.lanes.value` string instead writes
un-canonical, un-parseable content into a JSON column, which ANTS-3761's INV-2
column diff then reports as a difference on every export.

**When a re-derivation would clear a column, it does** —
`clearItemField(itemPk, field, "asserted")` and not an empty `setItemField()`,
because `putItem()` binds an empty value as SQL NULL while `setItemField()`
binds the string it is given, and ANTS-3761's INV-2 column diff reads `''` and
NULL as different (`src/roadmapstore.h`, `clearItemField()`'s own comment).

**This spec writes no `history` row.** `appendHistory()` / `maxHistorySeq()`
exist and the migration uses them, so a consumer write that changes a column
without one leaves a gap in the audit trail. That is real and it is deliberately
not here: what counts as one revision when a single op writes `body` plus five
re-derived columns, and whether `dry_run` or a rolled-back transaction consumes
a `seq`, is a second contract. Filed as **ANTS-3822**.

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
  *Test:* `Inv2RenderIsTheOnlyWriter`, which drives one op of each of the eight
  kinds against a migrated fixture and, after each, asserts the roadmap file is
  **byte-identical to a fresh `RoadmapRender::render()` of the same store into a
  scratch root**. A splice fails it whether or not it also wrote through the
  store, because spliced markdown carries the caller's text and not the
  render's. Deliberately not "suppress the publish step and assert nothing was
  written": § 2.1 declares no such seam, `dryRun` rolls the store back too, and
  an invariant whose test needs a hook the contract does not have is untestable
  as written.
- **INV-3** — **Counter allocation on a migrated project floors to both
  high-waters, and `stable_prefix` allocates nothing.** § 2.3 states both rules;
  this asserts them. *Breaks when:* `idHighWater()` alone drives allocation, so
  a store seeded on a fresh clone reissues an id the committed corpus already
  holds; `nullopt` from `idHighWater()` is treated as an error rather than as 0;
  or a `stable_prefix` append seeds the counter high-water from a string id.
  *Test:* `Inv3Allocation`, four cases — a store high-water above the corpus, a
  corpus high-water above the store (the fresh-clone case, and the one that
  fails if the floor is dropped), an `append_batch` of three asserting
  `first_id`, `first_id+1`, `first_id+2` and one high-water raise, and a
  `stable_prefix` append asserting `idHighWater()` is still `nullopt`
  afterwards.
- **INV-4** — **An op that writes `body` leaves the item's five trailer columns
  equal to what `trailerValuesIn()` yields from the new body.** *Breaks when:*
  a body write skips the re-derivation, so a `Layman:` line added by `annotate`
  never reaches the column and the render's gate cannot be cleared; a
  re-derivation that should clear a column writes `''` through `setItemField()`
  instead of NULL through `clearItemField()`; or `lanes` / `evidence` are
  written as the raw pre-split string rather than as `canonicalJson()` of the
  split list, which puts un-parseable content in a JSON column.
  *Test:* `Inv4BodyDerivesColumns`, three cases — `annotate` a `Layman:` line
  onto an item whose `layman` column is empty and assert the column, then assert
  the same op makes the previously gate-failing render succeed; `amend_body` a
  `Lanes: a, b` line and assert the column parses as the JSON array `["a","b"]`;
  and `amend_body` the `Layman:` line away and assert the column is SQL NULL,
  not `''`. The first case's *second* assertion is the one that cannot pass by
  accident: it fails against an implementation that writes the column but
  derives it from the *request* rather than from the resulting body.
- **INV-5** — **A trailer-column write is refused when the item's body yields a
  different value for that key**, over all five keys, whether or not the body's
  match begins a line. *Breaks when:* the refusal is gated on
  `TrailerMatch::anchored == false`, which exempts the commonest shadowing shape
  there is — a stale `Kind:` continuation line, which begins a line and so is
  `anchored == true`; it is scoped to `source`/`lanes` on the reasoning that the
  other three patterns are `^`-anchored (`MultilineOption` makes that false); or
  it is scoped to key *presence* rather than to a differing value, which would
  refuse every ordinary bullet carrying its own `Kind:` line.
  *Test:* `Inv5BodyShadowed`, four cases — a body carrying `Source: x` mid
  sentence plus a column write of `y` (refuses; the message quotes the sentence
  and names the reword remedy); the same body with a matching value (writes); a
  body whose **continuation line begins** `Kind:` plus a differing column write
  (refuses, and the message names the delete-the-stale-line remedy — the case an
  `anchored`-gated rule lets through); and an `append` supplying a body with a
  `Layman:` line and **no** `layman` argument (writes, derived — § 2.6's path,
  which must not be shadow-checked).
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
  honoured for the file and not for the store; or the preview envelope comes
  back empty because the dry render's `Outcome` was discarded, which is a
  regression against the markdown path's `dry_run` and the reason
  `commitAndRender()` takes an `Outcome *` (§ 2.1). *Test:*
  `Inv7DryRunCommitsNothing`, one case per op kind, asserting the
  item/section/element row is unchanged **and** the roadmap file's bytes are
  unchanged **and** the envelope still carries a non-empty would-be result read
  off `*outcome`.
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

**The write transaction is held across both render walks, and that does not
touch ANTS-3793 INV-3's `p95 < 50 ms` read budget.** The concern is real enough
to answer rather than omit: `begin()` is `BEGIN IMMEDIATE`, it is held from step
1 to step 6, and there are **two** connections in the process — ANTS-3793 § 2.2
gives `RemoteControl` and `RoadmapDialog` one each. The store runs in **WAL**
(`RoadmapStore::enableWal()`), where readers and a writer do not block each
other: a dialog read during a verb's held write transaction proceeds against the
last committed snapshot at full speed. What a second *writer* would hit is
`Access::Interactive`'s 5 s busy deadline, and the only second writer is another
`roadmap_log` op — which the verb dispatcher runs serially. No case in this spec
puts a read behind a write.

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
- **A `history` row per consumer write** — **ANTS-3822**, and § 2.6's last
  paragraph says why it is a second contract rather than a line here.

## 6. Tests

`tests/features/roadmap_write_half/`, label `features`, compiled into the
**`test_core` bundle** per `tests/features/README.md` (no `add_executable`) —
the same bundle ANTS-3793 § 6 uses, and for the same reason: it is the only one
linking both `ants_core_lib` and `ants_roadmapstore_lib`.

| Case | Invariants |
|---|---|
| `Inv1RenderFailureRollsBack` | INV-1 |
| `Inv2RenderIsTheOnlyWriter` | INV-2 |
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
dropped, the re-derivation skipped, the shadow check gated on
`anchored == false`, the `dry_run` check removed, and `setElementPayload()`
replaced by an `addElement()`.

## 7. Cross-doc impact

- **`RoadmapStore` gains `setElementPayload()`** (§ 2.2) — a surface addition to
  ANTS-3756, in the shape ANTS-3758 § 2.1 used for `listElements()` and
  ANTS-3793 § 2.2 used for `readProjectByRoot()`. Without it `bundle_row` has no
  store form at all.
- **`docs/standards/mcp-error-codes.md` gains four codes, and reuses a fifth.**
  Every `RoadmapWrite::Result` value reaches an envelope, so every one needs a
  `code` a caller can branch on:

  | `Result` | `code` | New? |
  |---|---|---|
  | `GateUnmet` | `render_gate_unmet` | new |
  | `RenderFailed` | `render_failed` | new |
  | `StoreFailed` | `store_failed` | new |
  | `PublishFailed` | `write_failed` | **existing**, § 4 I/O failure — "file-system write returned an error", whose own example is `roadmap_log` |
  | (§ 2.4 / § 2.5 refusals) | `locator_unsupported`, `body_shadowed` | new |

  Verified 2026-08-04: the standard carries no `store_*`, `render_*`, `sql_*` or
  `db_*` code today, and `io_error`'s own entry says to prefer a specific
  variant "when the failing op is known" — which it is, for all four.
  Recorded while verifying that: **`bad_op_combo` is used 23 times across
  `src/remotecontrol.cpp` and `src/claudeintegration.cpp` and appears nowhere in
  the standard** (re-verified 2026-08-04) — a pre-existing documentation gap
  this spec inherits rather than causes, and not this id's to fix.
- **The shipped `anchor`-on-`ants-v1` refusal message needs one word changed.**
  `cmdRoadmapLogFlip` and `cmdRoadmapLogFlipBatch` both tell the caller to "use
  `id`, `headline`, or `line_range`" — advice § 2.4 makes wrong on a migrated
  project, which is the only kind the store path serves. Drop `line_range` from
  the two messages in the same change. Small, and left unfixed it sends a caller
  from one refusal straight into another.
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
  behaviour of `roadmap_log`: the new refusal codes above and `line_range`'s
  unavailability.
- **`roadmap-data-model.md`'s *What checks this* table** gains INV-1 against the
  store-primary rule its own INV-3 states.
- **The 102 open items with no `Layman:` line are filed as ANTS-3821** (§ 5).
  It blocks nothing in this spec, and it blocks *using* the write half on this
  project — which is why it is filed rather than mentioned.
- **A `history` row per consumer write is filed as ANTS-3822** (§ 2.6, § 5),
  blocked by this id since it is the write path it would hook into.
- **ANTS-3793's § 5 already names this id** as the owner of the eight ops, the
  `line_range` locator, id allocation and the `body_shadowed` refusal. No
  amendment owed there; recorded so a reader does not go looking.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, 136 KB — bounded code windows + the cited passages of ANTS-3793/3808) | 1 / 5 / 9 / 5 / 0 | **20 verified, 20 fixed, 1 dismissed.** Dimensions: dim 5×5, dim 10×3, dim 1×2, dim 2×2, dim 4×2, dim 7×2, dim 12×2, dim 9×1, dim 15×1. **Both lanes independently led on the same CRITICAL, and it inverted the section it was in:** § 2.5 refused a shadowing column write when `TrailerMatch::anchored` was **false**, but ANTS-3808 § 2.2.1 computes `anchored` off `capturedStart(0)`, so a **stale `Kind:` continuation line — the commonest shadowing shape there is — is `anchored == true` and would have been let through**, while INV-5's own third case demanded it be refused. The rule and its test shipped in opposite polarities. Resolved by dropping `anchored` from the predicate entirely (value difference alone, over all five keys, because every matcher takes its FIRST match and the body precedes the trailer in a rendered bullet) and keeping it for the *message*, where it picks between the two remedies a caller is owed. Four more HIGHs, all draft: § 2.4 said `anchor` "resolve[s] against `BulletRecord` fields the store path fills" when ANTS-3793 § 2.1.1 fills it **empty** (the practical outcome is saved by a format-level `bad_op_combo` that fires first — now stated, along with the shipped message that recommends the `line_range` this spec refuses); `append` was caught by both § 2.5 and § 2.6 with no stated order (resolved per KEY, not per op — a supplied column is the caller's and is shadow-checked, an omitted one is derived); § 2.6 could not write `lanes` / `evidence` at all, since `setItemField()` takes a `QString` and only `setLegend()`/`addElement()` canonicalise (now a two-row table naming `canonicalJson()`); and INV-2's test needed a "publish step suppressed" seam § 2.1 never declared (re-expressed as byte-identity against a fresh render, which is stronger and buildable). **One signature change resolved three findings at once** — `commitAndRender()` now takes `RoadmapRender::Outcome *` rather than a bare `QStringList *gateFailures`, which the gate refusal, `dry_run`'s preview envelope (a regression against the markdown path nobody had noticed) and `PublishFailed`'s partial-commit message all needed. Also fixed: three `Result` values reached an envelope with no `code` (§ 7 now maps all five, four new plus the taxonomy's existing `write_failed`); `id_hint` had no store-path rule; the held write transaction was never reconciled with ANTS-3793 INV-3's read budget (answered: WAL, so readers do not block on the writer); `idPrefixFor()`'s `nullopt` was described as "no id-bearing item" when it is "no `id_prefix` row". **One finding dismissed on verification:** a lane said the 26-call-site figure lives in ANTS-3793 § 2.1 and not § 1 — § 1 states it outright. Two follow-ups filed rather than folded: **ANTS-3822** (a `history` row per consumer write) and, from the draft, **ANTS-3821**. The document grew 608 → 789 lines; that is contract the fixes added, but it is the number Phase 5's split call will rest on next loop. Lane spend 154k and 153k **cumulative** (first-turn input ~43k against the 60k per-turn budget). |
| 0-split | 2026-08-04 | 0 (no reviewer dispatched — a document operation) | — | **Split from the 934-line umbrella `ANTS-3793-roadmap-consumer-cutover.md` as the WRITE HALF only.** Not a review loop and not inherited review: the umbrella's three loops ran against a document that no longer exists, so the gate runs from loop 1 on these bytes. Sections carried over: old § 2.4 and INV-4 (renumbered to INV-1 per § 3). Filed findings folded in from [`docs/reviews/ANTS-3793-cold-eyes-loop3-tail.md`](../reviews/ANTS-3793-cold-eyes-loop3-tail.md): **H3** (`id_strategy: "stable_prefix"` allocates no counter id) → § 2.3's last paragraph and INV-3's fourth case; **M6** (`idHighWater()`'s `nullopt` case, `append_batch` contiguity, and `dry_run` under mutate-then-render) → § 2.3 and INV-7. Three defects were found by grounding rather than carried: the render's INV-5 gate is per project and fails on 102 of this roadmap's 327 open items, so every write op would refuse (§ 1, § 2.6, ANTS-3821); `bundle_row`'s read-modify-write has **no store surface** — `addElement()` is INSERT-only and `ElementRow` carries no element id (§ 2.2's `setElementPayload()`); and `line_range` on the store path does not match *nothing*, it matches *everything*, because `firstLine` is 0 and the predicate is `firstLine + 1 >= a` (§ 2.4). The umbrella's ordering (mutate, render, commit) was also corrected: `render()` commits its own files, so that sequence leaves the file ahead of the store on a commit failure — § 2.1 validates with `Options::dryRun` first instead. |
