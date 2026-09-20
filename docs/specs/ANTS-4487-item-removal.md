# ANTS-4487 — Make a roadmap item removable, and make orphaning mean something

**Status:** spec draft (2026-09-20).
**Kind:** implement.
**Source:** ROADMAP ANTS-4487 (cc-feedback-2026-08-18, Fin Break).

**Pairs with:** ANTS-4485, ANTS-4491, ANTS-4500.

**Layman:** A roadmap item added by mistake can now be taken off the roadmap,
and — when it really was a mistake — removed from the database entirely.
Today neither is possible, and the next save writes it straight back.

## 1. Goal

An item added by mistake can be taken off the roadmap by flipping it to
`dropped`, which stops it being rendered and stops it appearing in a default
query. An item that should never have existed can be removed from the store by
a guarded op that names what it will delete before it deletes it. Reverting
`ROADMAP.md` with git still does not remove anything, and the migration says
so in those words.

## 2. Problem

Fin Break appended two items by mistake, reverted `ROADMAP.md` with git, and
re-ran `roadmap_migrate`. It reported both as orphaned. That reads as handled.
It is not: both were still returned by `roadmap_query` as ordinary planned
bullets, and one unrelated `roadmap_log` write rendered both straight back
into the file.

Orphaning is a report, not a removal. `Loader::rebuildElements()` increments a
counter and emits a note:

```cpp
++out.itemsOrphaned;
note("orphaned_item", r.idFold);
```

There is no orphan column and no status change, and the same function re-files
every orphan whose element row it had cleared. ANTS-3765 § 2.7 states this as
intended — the item "retains its row, its history and its identity" — and its
INV-4 requires it to be "retained, re-filed and reported."

So reverting the file cannot undo an append, because the store outlives the
revert. That is counter-intuitive precisely because the file is the thing
under version control.

The render's only per-item exclusion is visibility:

```cpp
bool isRenderable(const RoadmapStore::ItemWrite &it) {
    return it.visibility != QLatin1String("internal");
}
```

Status is not consulted. So even a `dropped` item renders today.

The only route left was a direct `DELETE` against the shared machine-global
sqlite file, which the reporter's sandbox correctly blocked until the user
approved it.

**One premise in the item's body has since expired.** It says `to_status`
cannot reach `dropped`. ANTS-4977 added it: the MCP enum and the validation in
`cmdRoadmapLogFlip`, `cmdRoadmapLogFlipBatch` and `rcdetail::cmdRoadmapLogPassFlip`
all now accept `dropped`, matching the `item.status` CHECK exactly. So half of
the reporter's second remedy is already shipped, and only the render and query
exclusions are missing.

## 3. Scope decisions (agreed with the user)

1. **The status route is the primary answer; the hard delete is the
   exception.** Flipping to `dropped` is reversible, loses nothing, and
   answers the reported harm — the item stops being rendered and stops
   appearing. A destructive op on a machine-global store with no undo is
   reserved for the case where the row itself should not exist.
2. **The hard delete is specified here but must not be implemented until the
   user confirms it.** § 4.4 is complete so the review can judge it; § 9
   records the gate. This is stated rather than assumed because the store is
   shared across every project on the machine.

## 4. Surface

### 4.1 `dropped` stops the render

`isRenderable()` gains a status term.

```cpp
bool isRenderable(const RoadmapStore::ItemWrite &it) {
    return it.visibility != QLatin1String("internal")
        && it.status     != QLatin1String("dropped");
}
```

An excluded item increments the existing `itemsExcluded` counter, so the
render already reports it and no new field is added.

This does not reach the render's unfiled-item refusal. That refusal fires on
`ref.sectionId == 0` before renderability is consulted, so a dropped item must
stay filed. It does.

### 4.2 `dropped` leaves the default query

`roadmap_query`'s default `status` is `all`, which is why a dropped item is
returned today. The default becomes "every status except `dropped`". Asking
for `dropped` explicitly, or for `all`, still returns it.

`all` continues to mean all. Redefining it would make the dropped items
unreachable by any argument, which is the opposite of reversible.

### 4.3 Orphaning keeps its name and states what it is

The word stays, because the behaviour it names — retain, re-file, report — is
what ANTS-3765 § 2.7 requires and is correct. What changes is that the note is
no longer silent about its own limits. The migration response gains a sentence
on the `orphaned_item` note:

```
N item(s) are in the store and absent from the source. They are retained and
re-filed, not removed — reverting ROADMAP.md does not delete anything. To take
one off the roadmap, flip it to `dropped`.
```

### 4.4 The guarded removal op

`roadmap_log op:"remove"`, handled by a new
`RemoteControl::cmdRoadmapLogRemove` in `src/remotecontrol_roadmap_log.cpp`,
store-backed projects only.

Arguments: `id` (required, matched verbatim and case-sensitively — no
locator, no headline, no anchor), and `dry_run`.

**The guard the reporter proposed does not discriminate, and is not the one
used.** "Refuse if the item has relationships or history" sounds like "refuse
unless it was a mistake", and it is not: `history` rows are written by edits,
not by creation, so a legitimate item nobody has edited has none. Measured
against the live store, about half of all items have no history row, and
`relationship`, `feedback_ref` and `citation` are empty across every project:

```
sqlite3 -readonly ~/.local/share/ants-terminal/roadmap.sqlite \
  "SELECT (SELECT COUNT(*) FROM item),
          (SELECT COUNT(DISTINCT item_pk) FROM history);"     # 7234 | 3699
sqlite3 -readonly ~/.local/share/ants-terminal/roadmap.sqlite \
  "SELECT (SELECT COUNT(*) FROM feedback_ref),
          (SELECT COUNT(*) FROM relationship),
          (SELECT COUNT(*) FROM citation);"                    # 0 | 0 | 0
```

So that guard would permit removing roughly half the corpus while feeling
strict. **And taken literally it would forbid the removal this op exists
for**: § 3's recommended route is a flip to `dropped`, a flip is an edit, and
the store write path records edits in `history` — so using the primary route
would permanently disqualify the item from the exception route.

**`history` is therefore not a guard. It is the item's own audit trail, and it
is cascaded.** The guards are about bindings *outside* the item:

- **The id is passed verbatim.** No locator resolution, so nothing can be
  removed by a near-miss on a headline. **The mechanism, not just the
  intent:** resolve with `RoadmapStore::findItem()`, whose query folds case
  (`id_fold = lower(?)`), then compare the stored `item.id` to the argument
  byte for byte and refuse `id_case_mismatch` on any difference. Without that
  second step `ants-4487` would delete `ANTS-4487`.
- **The id must not appear in the published `ROADMAP.md`.** The test is the
  file's *text*, not its parsed bullets: an id cited inside another item's
  body or layman line is a live citation, and on a destructive op the
  conservative reading is the correct one. Refuses `id_in_published_roadmap`.
- **No external reference.** `feedback_ref`, `relationship` (either end) and
  `citation` each refuse, naming the table: `item_referenced`. These bind the
  item to something outside itself. All three are empty store-wide today, so
  this guard is protection against a future state rather than a live filter.
- **`dry_run` evaluates every guard and reports it** as *would refuse*,
  alongside the counts, rather than returning a refusal envelope. A preview
  that stopped at the first guard could never show the blast radius.

**`dry_run` opens the write.** It runs `BEGIN IMMEDIATE`, performs the
cascade, counts what it removed, and rolls back — it does not count with
read-only queries first. This project has learned the same lesson three times
on `dry_run` paths: a measurement taken after the rollback reads the state the
operation *would replace* and reports it confidently. The counts must be taken
inside the transaction, which means there must be one.

**The deletion is a hand cascade in FK order**, because the schema declares
`REFERENCES` and no `ON DELETE CASCADE` anywhere.
`RoadmapStore::deregisterProject()` already implements this cascade for a
whole project and is the shape to follow: one transaction, children first,
`relationship` cleared from both ends.

```
element → history → feedback_ref → relationship (src_pk or dst_pk) →
citation → item
```

In practice only `element`, `history` and `item` carry rows for a removable
item, the other three being guarded above. They stay in the cascade because a
guard is a runtime check and the cascade is the contract: if a guard is ever
relaxed, the cascade must already be correct.

`PRAGMA foreign_keys` is on per connection via `RoadmapStore::applyPragmas()`,
so a wrong order fails loudly rather than orphaning rows.

### 4.5 Refusing a duplicate append

`op:"append"` already computes `possible_duplicates` — after writing. It
scored the reporter's second bad item at 100. The check moves before the
insert: a score of 100 refuses with `duplicate_item`, naming the id it
matched, unless `force: true` is passed.

Below 100 the score stays advisory and is reported as it is today. A
threshold lower than exact-match would refuse legitimate sibling items, which
is a worse failure than the one being fixed.

## 5. Invariants

- **INV-1** — A `dropped` item is not written into `ROADMAP.md`.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `droppedNotRendered` — flip an item to `dropped`, render, assert its id is
  absent from the output and `itemsExcluded` rose.
  *Breaks when:* `isRenderable()` consults visibility alone.

- **INV-2** — A `dropped` item is absent from a default query and present when
  asked for by status.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `droppedHiddenByDefault` — assert absent with no `status`, present with
  `status:"dropped"` and with `status:"all"`.
  *Breaks when:* the default filter is applied to `all` as well.

- **INV-3** — Flipping to `dropped` and back restores the item unchanged.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `dropIsReversible` — flip to `dropped`, flip to `planned`, assert every
  column matches the pre-drop row.
  *Breaks when:* the drop path mutates anything but `status`.

- **INV-4** — `op:"remove"` refuses an id present in the published roadmap
  file, and writes nothing.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `removeRefusesRenderedId` — render, then attempt removal, assert the
  refusal is `id_in_published_roadmap` and the row survives. A second leg
  covers the prose branch: unrender the item but cite its id inside another
  item's body, and assert it still refuses.
  *Breaks when:* the file check is skipped, or it tests parsed bullets rather
  than the file's text and so misses the prose citation.

- **INV-5** — `op:"remove"` refuses an item an *external* table references,
  naming the table, and does not refuse on `history` alone.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `removeRefusesReferenced` — insert a `citation` row, attempt removal, assert
  the refusal is `item_referenced` and names `citation`; then remove it, flip
  the item to `dropped` and back so it carries `history` rows, and assert
  removal now succeeds.
  *Breaks when:* an external table is left out of the check, or `history` is
  treated as one — which would make § 3's recommended route disqualify the
  item permanently.

- **INV-6** — A successful `op:"remove"` leaves no row in any table that
  referenced the item.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `removeLeavesNoOrphans` — give the item `element` and `history` rows, remove
  it, and assert no row for it survives in any of the six FK columns. The
  `history` rows are what make this falsifiable: a cascade that deletes only
  `element` and `item` leaves them behind and the case goes red.
  *Breaks when:* the hand cascade omits a table.

- **INV-7** — `dry_run` on `op:"remove"` changes nothing.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `removeDryRunIsInert` — dry-run a removal, then assert through queries on
  the same connection that every table's rows for that item are unchanged by
  count and by column value, and that the report names the same rows a real
  run deletes. **Not a byte comparison of the store file:** it runs in WAL, so
  connection setup and checkpointing move bytes with no mutation and a byte
  assertion would be flaky against correct code.
  *Breaks when:* the blast radius is measured after the transaction rolls
  back, which reads the state the operation would replace.

- **INV-8** — `op:"append"` refuses an exact duplicate unless forced.
  *Test:* `tests/features/roadmap_item_removal/test_item_removal.cpp`, case
  `appendRefusesExactDuplicate` — append twice, assert the second refuses
  `duplicate_item` and no row was written, then assert `force:true` writes it.
  *Breaks when:* the duplicate check stays after the insert.

## 6. Failure modes

**The published roadmap cannot be read.** `op:"remove"`'s file check has no
answer, so it refuses `roadmap_unreadable` rather than assuming the id is
absent. Assuming absence
is the one wrong direction: it permits the destructive branch on no evidence.

**The item is the last one in its section.** Removal leaves an empty section,
which renders as a heading with no bullets. That is a legitimate state and the
render already produces it; no section is deleted as a side effect.

**A concurrent write.** `op:"remove"` runs inside the same `BEGIN IMMEDIATE`
sequence as every other store write, so a second migration blocks rather than
racing.

**`dropped` used as a workflow state rather than a removal.** A project may
legitimately mean "decided against" rather than "added by mistake". Both want
the item off the rendered roadmap, so the behaviour is right for each; § 4.2
keeps it retrievable.

## 7. Tests

All cases live in `tests/features/roadmap_item_removal/`, paired with
`spec.md`, compiled into an existing bundle's `SOURCES` list.
`build_target_for` names the bundle. Each is seen to fail against pre-change
code before the change is restored.

**The suite lands in two parts, because § 9 gates the destructive op.**
INV-1, INV-2, INV-3 and INV-8 cover the status route and the duplicate
refusal, and land with this spec. INV-4, INV-5, INV-6 and INV-7 exercise
`op:"remove"` and land *with it*, once the user confirms § 4.4 — wiring them
in beforehand would ship a permanently red suite against an op that
deliberately does not exist yet.

| Invariant | Case |
|---|---|
| INV-1 | `droppedNotRendered` |
| INV-2 | `droppedHiddenByDefault` |
| INV-3 | `dropIsReversible` |
| INV-4 | `removeRefusesRenderedId` (gated, § 9) |
| INV-5 | `removeRefusesReferenced` (gated, § 9) |
| INV-6 | `removeLeavesNoOrphans` (gated, § 9) |
| INV-7 | `removeDryRunIsInert` (gated, § 9) |
| INV-8 | `appendRefusesExactDuplicate` |

INV-7's case must measure inside the transaction. A check placed after the
rollback reads the state the operation would replace and reports it
confidently — the failure this project has met repeatedly on `dry_run` paths.

Fixtures use a throwaway store under the test's own temp root, never the
machine-global store.

Label: `features`.

## 8. Alternatives considered (and rejected)

**Exclude orphaned items from the render, so reverting the file becomes the
removal path.** The reporter's third remedy. Rejected: it makes a git revert
silently destructive, and it contradicts ANTS-3765 § 2.7's INV-4, which
requires an absent item to be retained and re-filed. Renaming the concept was
considered instead and also rejected — the behaviour is correct, so § 4.3
states it rather than renaming it.

**Use `visibility = 'internal'` to hide a mistaken item.** It already excludes
from the render and needs no code. Rejected: `visibility` means "not for
publication", not "not real", and overloading it would make the two
indistinguishable in every future query.

**Hard delete as the only route.** Rejected per § 3 decision 1: irreversible,
on a machine-global store, for a problem a status flip solves.

**Refuse an append scoring below 100 as a possible duplicate.** Rejected: it
would refuse legitimate sibling items, a worse failure than the one fixed.

## 9. Out of scope

- **Implementing § 4.4's `op:"remove"` before the user confirms it.** The
  design is specified so the review can judge it; the destructive verb is
  gated on that confirmation.
- Undoing an append that has already been committed to git — outside the
  store's reach.
- Bulk removal. `op:"remove"` takes one id; a batch form is deferred and not
  yet queued.
- Making `roadmap_query`'s `scope:"all"` reporting exclude dropped items —
  deferred; not yet queued.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `test_item_removal.cpp::droppedNotRendered` |
| INV-2 | `test_item_removal.cpp::droppedHiddenByDefault` |
| INV-3 | `test_item_removal.cpp::dropIsReversible` |
| INV-4 | `test_item_removal.cpp::removeRefusesRenderedId` — lands with `op:"remove"` per § 9 |
| INV-5 | `test_item_removal.cpp::removeRefusesReferenced` — lands with `op:"remove"` per § 9 |
| INV-6 | `test_item_removal.cpp::removeLeavesNoOrphans` — lands with `op:"remove"` per § 9 |
| INV-7 | `test_item_removal.cpp::removeDryRunIsInert` — lands with `op:"remove"` per § 9 |
| INV-8 | `test_item_removal.cpp::appendRefusesExactDuplicate` |
| § 4.3's migration wording | **nothing** — no test asserts response prose; a reviewer reads it |

## 11. Cross-doc impact

- `docs/specs/ANTS-3765-roadmap-migration-load.md` — § 2.7 gains a pointer to
  the `dropped` route, since it is the section that states orphaning retains.
- `docs/standards/roadmap-format.md` — the `dropped` status gains its render
  and query consequences.
- `docs/standards/mcp-error-codes.md` — five new refusal codes:
  `duplicate_item` (§ 4.5), and `op:"remove"`'s `id_case_mismatch`,
  `id_in_published_roadmap`, `item_referenced` and `roadmap_unreadable`
  (§ 4.4, § 6).
- `CHANGELOG.md` — a bullet stating what shipped.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-4487-item-removal-loop-log.md`.
