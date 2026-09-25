# ANTS-4485 — Locate a write target in the store on a store-backed project

**Status:** implemented (2026-09-25); cold-eyes loops 1 + 2 folded (2026-09-20).
**Kind:** implement.
**Source:** ROADMAP ANTS-4485 (cc-feedback-2026-08-18, Fin Break).

**Pairs with:** ANTS-4487, ANTS-4491, ANTS-4500.

**Layman:** Two tools currently disagree about which roadmap items exist — one
reads the database, the other reads the file — so an item you can look up
cannot be edited. Both will read the database.

## 1. Goal

On a project whose `ROADMAP.md` is generated, `roadmap_log`'s locating write
ops resolve their target against the store, as `roadmap_query` already does.
An item the query returns can be written. Where store and file disagree, the
refusal says which holds the item instead of reporting it absent.

## 2. Problem

`roadmap_query` answers from the store. `roadmap_log`'s locating writes walk
the parsed `ROADMAP.md`. For an item present in the store and absent from the
file, the query returns it and the write refuses `bullet_not_found`.

Fin Break hit this on `FIBR-0281`: the query showed it, the write could not
reach it, and the only route out was to hand-write the bullet back into the
generated file and re-ingest — the hand-editing the store exists to remove.
The same session saw a `dry_run` report one status for an id while the query
reported another for the same id in the same minute.

Two things make it worse than a mismatch. The addressing model is invisible
from outside, surfacing only as a contradiction. And it removes the escape
hatch: when the store and the file disagree, the verb that could repair the
store is the one that refuses, because it reads the file.

The file walk is deliberate rather than accidental. `cmdRoadmapLogFlip`'s own
comment gives the reasoning — on a migrated project "the file is the render's
own output, so the same walk finds the same bullet." That holds while the file
is current. It is exactly false in the state this item is about.

## 3. Scope decisions (agreed with the user)

None. Every choice below follows from § 2 and from the precedent in § 4.1.

## 4. Surface

### 4.1 The precedent already exists

`RemoteControl::cmdRoadmapLogAmendField` is already store-backed. It walks no
markdown: it resolves its project through
`RemoteControl::roadmapSectionOpTarget()` and then

```cpp
const auto pk = store.findItem(target->projectId, id, &err);
if (!pk)
    return rlErr(QStringLiteral("bullet_not_found"), /* … */);
```

So the shape this spec generalises is shipped and in use. What it lacks is the
non-id locators and the divergence message.

### 4.2 The handlers in scope

Eight op names are served by four handlers.

| Handler | File | Ops |
|---|---|---|
| `RemoteControl::cmdRoadmapLogFlip` | `src/remotecontrol_roadmap_log.cpp` | `flip`, `annotate` |
| `RemoteControl::cmdRoadmapLogFlipBatch` | `src/remotecontrol_roadmap_log_batch.cpp` | `flip_batch`, `annotate_batch` |
| `RemoteControl::cmdRoadmapLogAmendBody` | `src/remotecontrol_roadmap_log.cpp` | `amend_body`, `amend_headline`, `set_body` |
| `RemoteControl::cmdRoadmapLogAmendField` | `src/remotecontrol_roadmap_log.cpp` | `amend_field` — already store-backed |

There is no shared locator resolver today; each handler open-codes its walk.
This spec adds one, so the rule is stated once.

### 4.3 The shared resolver

A new `rcdetail::rlLocateTarget()` resolves a locator on a store-backed
project, beside the existing `rcdetail::rlStoreItemPk()` in
`src/remotecontrol_roadmap_query.cpp` and declared in
`src/remotecontrol_internal.h`.

```cpp
namespace rcdetail {
struct LocateOutcome {
    qint64  itemPk   = 0;       // 0 = not resolved
    QString id;                 // the stored item's, when resolved
    QString headline;
    QString status;             // the stored status word
    QString code;               // refusal code when itemPk == 0
    QString error;              // human-facing message
    bool    inFileOnly = false; // found in the file, absent from the store
};

LocateOutcome rlLocateTarget(RoadmapStore &store, qint64 projectId,
                             const QString &locId, const QString &locHeadline,
                             const QStringList &fileIds,
                             const QStringList &fileHeadlines);
}
```

Resolution order is unchanged from the file walk — id, then anchor, then
headline — because changing it would change which bullet an ambiguous locator
selects.

- **id** — `RoadmapStore::findItem(projectId, id)`, whose query is
  `SELECT item_pk FROM item WHERE project_id = ? AND id_fold = lower(?)`.
- **headline** — `RoadmapStore::listItems(projectId)`, comparing the
  **request's** `headline` against each row's stored `headline` as the file
  walk compares them: normalised, then hashed (`rcNormaliseHeadline`,
  `rcFnv1a64`). A headline that matched a rendered bullet therefore still
  matches its row. The resolver takes the locator strings and the file's ids
  and headlines, never a parsed file bullet, so the file is consulted only on
  a miss. More than one match is `bullet_ambiguous`; none is
  `bullet_not_found`.
- **anchor** — resolved through the file, then mapped to a row by id. An
  anchor is a file artefact and the store holds no anchor column, so this
  locator keeps its present behaviour.
- **`line_range`** — refused on a store-backed project, because the store
  path zeroes `firstLine` and a range starting at line 1 would match every
  bullet. **Today that refusal exists only in `cmdRoadmapLogFlipBatch`**;
  verified 2026-09-20, `cmdRoadmapLogFlip` and `cmdRoadmapLogAmendBody` carry
  no equivalent. So this is new work in two of the four handlers rather than
  preserved behaviour, and INV-6 is a changed-behaviour case there.

### 4.4 The divergence message

`inFileOnly` is set when the store does not resolve the locator and the file
walk does. That is the reverse of the reported defect and means the file is
ahead of the store — the state the existing divergence guard in
`RoadmapWrite::commitAndRender()` refuses, so the write must not proceed.

The refusal names the cause rather than reporting absence:

```
roadmap_log: "<locator>" is in ROADMAP.md but not in this project's store.
The store is the source of truth for this project, so the write would drop
it. Run roadmap_migrate to import it, then retry.
```

The reported case — present in the store, absent from the file — needs no
message at all. It simply resolves and writes, which is the fix.

### 4.5 Suggestions take their candidates from the store

`rcRankIdsBySharedPrefix()` and the headline token-Jaccard ranker keep their
ranking unchanged, but on a store-backed project they take their candidates
from the store's item list rather than from the file's bullets. A suggestion
drawn from the file would name bullets the write cannot reach.

The resolver returns the refusal; the caller builds the suggestion list, as it
does today. `LocateOutcome` carries no suggestion channel.

### 4.6 A markdown project is untouched

`RemoteControl::roadmapWriteTarget()` already returns no store for a project
that is not migrated. Those projects keep the file walk unchanged; the file is
their source of truth.

## 5. Invariants

- **INV-1** — On a store-backed project, an id `roadmap_query` returns is
  writable by every locating op.
  *Test:* `tests/features/roadmap_store_locate/test_store_locate.cpp`, case
  `queryableIsWritable` — insert an item **and its `element` row** into the
  store without rendering, then exercise one op per handler in § 4.2's
  table — `flip`, `flip_batch`, `amend_body`, `amend_field` — and assert each
  succeeds. The `element` row is required: the render hard-refuses an item
  with `sectionId == 0`, so an unfiled item fails for a reason unrelated to
  locating.
  *Breaks when:* any one handler resolves its locator against the file.

- **INV-2** — A locator present in the file and absent from the store refuses
  with a message naming the divergence, and writes nothing.
  *Test:* `tests/features/roadmap_store_locate/test_store_locate.cpp`, case
  `fileOnlyRefusesWithCause` — add a bullet to the file by hand, assert the
  refusal text names the store and the remedy, and the file is unchanged.
  *Breaks when:* the file-only case falls through to a bare
  `bullet_not_found`.

- **INV-3** — Locator precedence is id, then anchor, then headline, on both
  backends.
  *Test:* `tests/features/roadmap_store_locate/test_store_locate.cpp`, case
  `locatorPrecedenceHolds` — supply an id and a headline selecting different
  items, assert the id wins.
  *Breaks when:* the store resolver orders its branches differently.

- **INV-4** — A markdown project's locate behaviour is byte-identical to
  before this change.
  *Test:* `tests/features/roadmap_store_locate/test_store_locate.cpp`, case
  `markdownUnchanged` — run each op against an unmigrated fixture and assert
  the envelopes match golden files at
  `tests/features/roadmap_store_locate/golden/markdown-<op>.json`, **captured
  from the pre-change binary and committed in the same commit as the change**.
  Generating them from the post-change build makes the case pass
  unconditionally.
  *Breaks when:* the store path is entered for a project with no store row.

- **INV-5** — A headline matching more than one stored item refuses
  `bullet_ambiguous` rather than writing to one of them.
  *Test:* `tests/features/roadmap_store_locate/test_store_locate.cpp`, case
  `ambiguousHeadlineRefuses` — store two items with one headline, assert the
  refusal and that neither changed.
  *Breaks when:* the store resolver takes the first row.

- **INV-6** — `line_range` is refused on a store-backed project by **every**
  locating handler, not only `flip_batch`.
  *Test:* `tests/features/roadmap_store_locate/test_store_locate.cpp`, case
  `lineRangeRefusedEverywhere` — pass a `line_range` to each handler in
  § 4.2's table and assert each refuses. Red before the change for `flip` and
  `amend_body`, green for `flip_batch`, which already refuses.
  *Breaks when:* the refusal is added to the shared resolver but a handler
  resolves its locator before calling it.

## 6. Failure modes

**The store read fails.** `findItem()` distinguishes "no such item" (`nullopt`
with the error clear) from a query failure. A failure is `store_failed` and
does not fall back to the file — falling back would write against an index the
caller has not been told was used.

**The file cannot be parsed.** Neither the id nor the headline locator
consults the file to *resolve*; both consult it on a miss, to decide
`inFileOnly` (§ 4.4). So where the file cannot be read, those two still
resolve, and a miss degrades to a bare `bullet_not_found` with no divergence
message. The anchor locator needs the file to resolve at all and refuses.

**Both store and file hold the locator, disagreeing on which item.** The store
wins, because it is the source of truth for the render. The file's copy is
stale output by definition.

**An item with an empty `id` column.** `findItem()` keys on the `id` column,
so it misses one, and `rlStoreItemPk()` step 2 is a headline-equality scan
rather than an id extraction. Such an item is reachable only by its exact
stored headline, never by the id locator.

**An explicit id locator does not fall through.** A miss goes to § 4.4's
`inFileOnly` check and then refuses. Falling through to a headline scan would
resolve a *different* stored item that happens to share the file bullet's
headline — writing to an item the caller never named — and it would contradict
both § 4.3's precedence and INV-2.

## 7. Tests

All cases live in `tests/features/roadmap_store_locate/`, paired with
`spec.md`, compiled into an existing bundle's `SOURCES` list.
`build_target_for` names the bundle.

**Not every case can be red before the change, and § 4.3 says why.** INV-1,
INV-2, INV-5 and INV-6 cover changed behaviour and are seen to fail against
pre-change code. INV-3 and INV-4 assert behaviour this change
*preserves*, so they pass on both sides by construction. They are regression
guards, verified instead by asserting they go red against a deliberately
broken resolver.

| Invariant | Case |
|---|---|
| INV-1 | `queryableIsWritable` |
| INV-2 | `fileOnlyRefusesWithCause` |
| INV-3 | `locatorPrecedenceHolds` |
| INV-4 | `markdownUnchanged` |
| INV-5 | `ambiguousHeadlineRefuses` |
| INV-6 | `lineRangeRefusedEverywhere` |

Fixtures use a throwaway store under the test's own temp root. `RoadmapStore`'s
default path is the real machine-global store, so every case passes an
explicit path.

Label: `features`.

## 8. Alternatives considered (and rejected)

**Leave the locate on the file and only improve the message.** The item's own
fallback suggestion. Rejected: it makes the contradiction legible without
restoring the escape hatch, so the store-only item is still unwritable and the
hand-edit is still the only repair.

**Flag store-only items in `roadmap_query` so a caller can see the divergence
before attempting a write.** Rejected as the primary fix for the same reason —
it reports the problem rather than removing it. Worth having, and out of scope
here; ANTS-4462's `check_sync` already answers the file-versus-store question
on demand.

**Render before every write so the file is never behind.** Rejected: a render
of a large project is far too slow to ride every call, and it would make every
write depend on the render gate passing.

## 9. Out of scope

- Removing an item that should not exist — tracked by ANTS-4487.
- `id_origin` staying `synthesised` after the file starts declaring the id —
  tracked by ANTS-4343.
- Surfacing store-only items in `roadmap_query`'s own envelope — deferred; not
  yet queued.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `test_store_locate.cpp::queryableIsWritable` |
| INV-2 | `test_store_locate.cpp::fileOnlyRefusesWithCause` |
| INV-3 | `test_store_locate.cpp::locatorPrecedenceHolds` |
| INV-4 | `test_store_locate.cpp::markdownUnchanged` |
| INV-5 | `test_store_locate.cpp::ambiguousHeadlineRefuses` |
| INV-6 | `test_store_locate.cpp::lineRangeRefusedEverywhere` |
| The anchor locator keeps file semantics | **nothing** — no test asserts an anchor's file dependence; it is stated in § 4.3 and unchanged by this work |
| § 4.5's suggestions come from the store, not the file | **nothing** — a suggestion list is advisory and no invariant binds it; a wrong source names unreachable bullets without failing a write |

## 11. Cross-doc impact

- `docs/standards/mcp-behavioural-notes.md` — the `roadmap_log` notes gain the
  store-backed locate and the divergence refusal.
- `docs/standards/mcp-error-codes.md` — no new code; the divergence reuses
  `bullet_not_found` with a distinct message.
- `CHANGELOG.md` — a bullet stating what shipped.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-4485-store-backed-locate-loop-log.md`.
