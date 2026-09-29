# ANTS-4079 — item links: write, guard, read and migrate roadmap relationships

**Status:** spec draft (2026-09-29).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-4079 (in-session-2026-07-30), with ANTS-3748 and ANTS-3827 folded in by user decision 2026-09-29 ("one design for all three").
**Covers:** ANTS-4079, ANTS-3748, ANTS-3827.
**Pairs with:** `docs/standards/roadmap-data-model.md` § 6 (the relationship model this spec implements), ANTS-3810 (whole-store acyclicity).

**Layman:** A roadmap item can say "blocked by X" or "split from Y", tools can ask what blocks what, and an item cannot be marked done while a piece split off from it is still open.

## 1. Problem

The store has a `relationship` table and a data model for it, and nothing uses either.

1. **Nothing writes a link.** `RoadmapStore::relateItems()` and `RoadmapStore::relateCrossProject()` have no production caller, only tests (`tests/features/roadmap_store_schema`, `tests/features/roadmap_export_roundtrip`). `RoadmapExport::rebuildProject()` restores exported rows, and that is the only other writer. So a "blocked by" or "split from" fact lives only as prose, and `rcdetail::rcExtractGateNote()` guesses at it from words like "blocked by".
2. **A split parent over-claims (ANTS-3748).** When one item is split into parts, a feedback file cites the parent id. `RemoteControl::cmdFeedbackQuery()` resolves that id alone, and `rlStoreFlipOrAnnotate()` lets the parent flip to ✅ with parts still open. The reporting project then reads "delivered" for half-done work. Hit twice in one triage pass (ANTS-3718, ANTS-3707).
3. **Migration drops the two converted types (ANTS-3827).** `roadmap-data-model.md` § 6 says `relates-to` is converted from `Dependencies:` and `specified-by` from `Spec:`. No code in `src/roadmapmigrate*.cpp` reads either, so the table is empty after every migration, and ANTS-3810's round-trip test excludes relationships only because nothing converts them.

## 2. Surface

### 2.1 What is stored

No schema change. The `relationship` table (`RoadmapStore::createSchema()`) already holds all six types of § 6 and its three uniqueness indexes; `kSchemaVersion` stays 3.

Two sources, by type, following § 6's authored/converted split:

| Type | Source of truth | Written by |
|---|---|---|
| `splits-from`, `blocked-by`, `duplicate-of`, `supersedes` | the row | `roadmap_log op:"link"` / `op:"unlink"` (§ 2.2); import of their trailer line (§ 2.5) |
| `relates-to`, `specified-by` | the item's own `Dependencies:` / `Spec:` body line | re-derived from the body at migration and on every body write (§ 2.6) |

Every row goes through `relateItems()` (same project) or `relateCrossProject()` (another project's id). `specified-by` targets a path, so it needs a third store method, `relateDocument(type, srcPk, dstPath, error)`, inserting the `dst_path` form. No other code inserts a row, except `RoadmapExport::rebuildProject()`, which restores rows an export already normalised.

### 2.2 Writing: `roadmap_log op:"link"` and `op:"unlink"`

Store-only, like `move_section`. One call writes or removes one or more edges in one commit and one render.

```
op:"link" | "unlink"
id:      the source item            (required)
type:    splits-from | blocked-by | duplicate-of | supersedes   (required)
targets: ["ANTS-12", "VEST-0040", …]   (required, non-empty)
dry_run: bool
```

- A target with this project's prefix must resolve in the store, else the call refuses `link_target_not_found` naming it. A target with another registered project's prefix is stored cross-project, resolved or not, since that project may not have filed it yet.
- `id` equal to a target refuses `bad_args`.
- A new `splits-from`, `blocked-by`, `duplicate-of` or `supersedes` edge that would close a cycle among edges of the same type refuses `link_cycle`, with `cycle:[ids…]` in the reply. Checked over the full store, as § 6 requires.
- `relates-to` and `specified-by` are refused `bad_args` here: they come from the body (§ 2.6).
- Re-adding an existing edge, or removing an absent one, succeeds and reports it in `unchanged:[…]`.
- Reply: `{ok, op, id, type, linked:[…] | unlinked:[…], unchanged:[…]}` plus the usual render fields.

### 2.3 The guard: a split parent cannot close early (ANTS-3748)

A flip to `shipped` of an item that other items name with `splits-from`, where any of those parts is not `shipped` or `dropped`, refuses `open_parts` with `parts:[{id, status}]`. It applies to `op:"flip"` and to each locator of `op:"flip_batch"`, where it lands in `skipped[]` like any other per-locator refusal.

`blocked-by` does not refuse anything. A flip to `in-progress` or `shipped` of an item with an open blocker succeeds and carries a `blocked_by_open:[ids]` warning. Work sometimes starts before a blocker formally closes, and a refusal there would teach callers to delete true links.

### 2.4 Reading

- `roadmap_query` with `id` or `ids`: each bullet carries `links` when it has any, and omits the key when it has none:

  ```
  links: { blocked_by:[…], blocks:[…], splits_from:[…], parts:[…],
           duplicate_of:[…], supersedes:[…], superseded_by:[…],
           relates_to:[…], specified_by:[paths…] }
  ```
  Each list holds ids (paths for `specified_by`); empty lists are omitted. `blocks`, `parts` and `superseded_by` are the reverse direction, so "what blocks X" and "what X blocks" are one fetch. List and section queries do not carry `links`, to keep them lean.
- `feedback_query`: an id in `mapped_ids` that has parts also gets an entry in `mapped_id_parts: {id: [{id, status}]}`. `mapped_id_status` keeps its current meaning, the id's own status.

### 2.5 Rendering and import of the authored types

`RoadmapRender::bulletText()` composes one trailer line per authored type that has rows, after the existing trailers, in the table order of § 2.1:

```
  Splits-from: ANTS-3718.
  Blocked-by: ANTS-12, VEST-0040.
```

`RoadmapRender::passBlockText()` writes the pass-headings form, `- **Blocked-by**: ANTS-12`, beside its Status line.

On import, `RoadmapParse::trailerValuesIn()` reads these four keys on all three dialects: at a line start, optionally bold, and for pass-headings with the colon outside the bold. A value is a comma-separated id list. Each id becomes a row of that type, subject to § 2.2's rules; an id that cannot be stored is kept in `extras["unresolved_links"]` rather than dropped. A trailer line is a structured field, not prose, so § 6's rule that migration harvests nothing for `blocked-by` from prose still holds.

The four keys join the recognised-trailer lists (`lineBeginsTrailerDecl()` and its regexes). A body write whose `new_text`, `note` or `body` declares one of them at a line start refuses `body_shadowed`, pointing at `op:"link"`: the rows are the truth, so a hand-written line would be overwritten by the next render.

### 2.6 Converted types (ANTS-3827)

At migration and on every body write (the path `rlDeriveTrailerColumns()` already takes), the item's `Dependencies:` and `Spec:` lines are read:

- each comma-separated `Dependencies:` value that resolves to an item id becomes a `relates-to` row (`relateItems()` normalises the direction per § 6); a value that does not stays in `extras["unconverted_dependencies"]`;
- each `Spec:` path becomes a `specified-by` row via `relateDocument()`.

The body keeps both lines as written, and the render composes nothing for these two types, so the file round-trips byte for byte. On a body write the item's `relates-to` and `specified-by` rows are replaced by the new derivation in the same transaction.

### 2.7 Choices and rejected alternatives

- **One design for all three items** — the user, 2026-09-29.
- **Rows, not trailer lines, are the truth for the authored types** — the author. Rejected: a body line as the source, the way `Kind:` is. A link names another item, so a typo in prose would silently point at nothing; `op:"link"` can refuse it, and a line cannot.
- **`blocked-by` warns rather than refuses** — the author, for the reason in § 2.3.
- **No link arguments on `op:"append"`** — the author. Rejected: `splits_from` on append (ANTS-3748's option a). It adds a second writer for one type; `append` then `link` is two calls and one code path.

## 3. Invariants

- **INV-1** — `op:"link"` writes a row that `roadmap_query id:` then reports under `links`, and the reverse id reports it under the reverse key. Breaks if the write or the reverse read is missing. *Test:* `tests/features/roadmap_item_links`.
- **INV-2** — `op:"link"` to an unfiled same-project id refuses `link_target_not_found` and writes nothing; to an unfiled id of another registered project it succeeds as a cross-project row. Breaks if either target class is treated like the other. *Test:* `tests/features/roadmap_item_links`.
- **INV-3** — An edge that would close a cycle of one type refuses `link_cycle` naming the cycle; the same pair under a different type does not. Breaks if the cycle check ignores type or is skipped. *Test:* `tests/features/roadmap_item_links`.
- **INV-4** — Flipping a parent to `shipped` while a `splits-from` part is `planned` refuses `open_parts` listing that part; once the part is `shipped` or `dropped`, the same flip succeeds. In `flip_batch` the refusal is per locator. Breaks if the guard is absent, counts `dropped` as open, or refuses the whole batch. *Test:* `tests/features/roadmap_item_links`.
- **INV-5** — A flip of an item with an open `blocked-by` target succeeds and carries `blocked_by_open`. Breaks if it refuses or stays silent. *Test:* `tests/features/roadmap_item_links`.
- **INV-6** — Render then re-import of a project with rows of all four authored types restores the same rows, on ants-v1 and pass-headings. Breaks if a trailer is not rendered, not parsed, or parsed on one dialect only. *Test:* `tests/features/roadmap_item_links`.
- **INV-7** — Migrating an item whose body carries `Dependencies: ANTS-1, not-an-id` and `Spec: docs/specs/X.md` writes one `relates-to` row, one `specified-by` row, keeps `not-an-id` in `extras`, and leaves the rendered body byte-identical. Breaks if a line is dropped, duplicated by the render, or the unresolvable value is lost. *Test:* `tests/features/roadmap_item_links`.
- **INV-8** — `amend_body` removing a `Dependencies:` value removes its `relates-to` row in the same commit. Breaks if the rows are derived only at migration. *Test:* `tests/features/roadmap_item_links`.
- **INV-9** — A body write declaring `Blocked-by:` at a line start refuses `body_shadowed`. Breaks if the line is accepted and later overwritten by the render. *Test:* `tests/features/roadmap_item_links`.
- **INV-10** — `feedback_query` on a file citing a parent with an open part reports that part in `mapped_id_parts`, and `mapped_id_status` still reports the parent's own status. Breaks if parts are absent or the status meaning changes. *Test:* `tests/features/roadmap_item_links`.
- **INV-11** — `roadmap_log`'s published op enum lists `link` and `unlink` (checked by `tests/features/roadmap_log_op_enum`). Breaks if either is dispatched but unpublished. *Test:* `tests/features/roadmap_log_op_enum`.

## 4. RAM / build cost

No new build target and no new library. The tests join an existing bundle, chosen with `build_target_for` when written. Link lookups use the existing `idx_rel_src` and `idx_rel_dst` indexes, one query per fetched id. The cycle check walks one type's edges and is bounded by that type's row count; nothing is cached, so nothing grows.

## 5. Out of scope

- Links on a markdown-backed (unmigrated) project — permanent: `op:"link"` is store-only, like every structural op, because the file there has no row to hold.
- Harvesting any link from prose ("blocked by X" in a sentence) — permanent: `roadmap-data-model.md` § 6 forbids it (its INV-5), and `rcExtractGateNote()` keeps serving `mode:"bundles"`.
- `links` on list and section queries — permanent for now: they would widen every survey row, and one id fetch answers the question.

## 6. Tests

Feature test: `tests/features/roadmap_item_links/`, driving `roadmap_log` and `roadmap_query` through a migrated temp store. Covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9, INV-10. `tests/features/roadmap_log_op_enum` covers INV-11. Label `features;fast`. Verify each test fails against pre-change source first.

## 7. Hot reload

The verbs run in `ants-mcpd`: a rebuild of that target plus `/mcp` in a client makes them live, with no terminal relaunch. The store needs no migration step, since the schema is unchanged.

## 8. Cross-doc impact

- `docs/standards/roadmap-data-model.md` § 6: its statement that `relateItems()` is the sole writer is false today (`RoadmapExport::rebuildProject()` also inserts). Amend it to name both, and to name `relateDocument()`.
- `docs/standards/roadmap-format.md`: document the four link trailer lines and that they are written by `op:"link"`.
- CHANGELOG, and the `roadmap_log` / `roadmap_query` / `feedback_query` descriptions.

## Cold-eyes loop log

Rows: [`docs/reviews/ANTS-4079-item-links-loop-log.md`](../reviews/ANTS-4079-item-links-loop-log.md).
