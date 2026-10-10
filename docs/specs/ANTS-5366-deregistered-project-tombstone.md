# ANTS-5366 — keep a deregistered project's row, so its mail still arrives and its id is never reused

**Status:** accepted (2026-10-10).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-5366 (in-session-2026-09-25; route decided by the
user 2026-09-27; bundle slimmed by the user 2026-10-10).
**Covers:** ANTS-5366, ANTS-5483 — one surface: keeping the `project` row is
both what keeps the mailbox alive and what stops SQLite reusing the id.
**Pairs with:** ANTS-3781 (the upgrade ladder this adds a rung to),
ANTS-4622 (the mailbox this keeps reachable).

**Layman:** When a project leaves the shared roadmap database, the database
now keeps a marked placeholder for it instead of deleting it. Other sessions
can still leave it notes, and its number is never handed to a new project.

## 1. Problem

*Present tense describes the code before this change.*

`RoadmapStore::deregisterProject()` deletes every row the project owns, its
`message` rows at both ends, and finally its `project` row.

1. **Mail to a departed project is refused.** `session_message op:"send"`
   resolves the recipient with `RoadmapStore::projectIdForSlug()`. With the row
   gone it refuses `unknown_project`. A project usually deregisters while it
   waits for a fix, which is exactly when it wants news (seen 2026-09-25:
   RetroDB, ANTS-5334).
2. **Its id is reused.** `project.project_id` is `INTEGER PRIMARY KEY` without
   `AUTOINCREMENT`, so SQLite gives the next new project the highest deleted
   rowid (ANTS-5483). Nothing caches a `project_id` across a deregister today,
   so this is harmless now and wrong in principle.

Two routes without a schema change were examined and rejected on 2026-09-27
(ANTS-5366's body records why). `AUTOINCREMENT` cannot be added: a shipped
`CREATE TABLE` may only gain a column appended last (the comment above
`createSchema()`'s `ddl[]`).

## 2. Surface

### 2.1 The column and the rung

Append one nullable column to `project`, last, in `createSchema()`'s DDL:

```sql
  deregistered_at TEXT
                 CHECK (deregistered_at IS NULL OR deregistered_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z')
```

The GLOB is `message.created_at`'s, character for character. NULL means
registered. The column's rationale goes in the comment block above `ddl[]`,
never as a `--` comment beside it (that block's rule 1).

`RoadmapStore::kSchemaVersion` goes from 3 to 4. `upgradeLadder()` gains
`Upgrade{4, {"ALTER TABLE project ADD COLUMN deregistered_at TEXT CHECK (…)"}}`
with the same column text. The column is nullable with no default, so the ALTER
succeeds on a table that has rows.

### 2.2 Deregistering keeps the row

`deregisterProject()` keeps doing what it does, with two changes:

- It deletes **no `message` rows**. Mail to the project stays in its inbox;
  mail it sent stays in other inboxes, and its sender row still exists.
- It does **not delete the `project` row**. It sets `deregistered_at` instead,
  to a timestamp the caller passes. The store takes timestamps from the caller
  (`appendHistory()`'s convention), so the signature gains a `deregisteredAt`
  argument. `DeregisterCounts::messages` is removed, since it is always 0.

Everything else it deletes today (elements, history, feedback refs,
relationships, citations, items, sections, id prefixes) it still deletes.

No project row is ever deleted after this change, so SQLite never reuses a
`project_id` (ANTS-5483).

### 2.3 Who sees a deregistered row

Readers split into two groups. **The default hides the row.** A row only
appears where a caller asks for it, so a reader added later fails closed.

| Sees a deregistered row | Does not see it |
|---|---|
| `projectIdForRoot()`, `projectIdContaining()`, `projectIdForSlug()` — the mailbox's sender and recipient lookups | `readProject()`, `readProjectBySlug()`, `readProjectByRoot()`, `listProjects()` |
| `slugCandidates()` — mail may be sent to a departed slug | everything built on those: `migratedProject()`, the roadmap verbs, the export of all projects |
| the identity checks of § 2.4 and § 2.5 | |

`ProjectRow` gains `deregisteredAt`. `readProject()`, `readProjectBySlug()`,
`readProjectByRoot()` and `listProjects()` each gain a last parameter
`RoadmapStore::Visibility visibility = Visibility::Registered`;
`Visibility::IncludeDeregistered` returns deregistered rows too.
`slugCandidates()` calls `listProjects()` with `IncludeDeregistered`. The three
id lookups take no parameter: they never filtered on registration.

So a departed project has a mailbox and no roadmap. Its inbox works from its
own root, because the inbox resolves the caller by root.

### 2.4 Migrating a deregistered root revives it

`registerProject()` on a root whose row is deregistered clears
`deregistered_at` and returns the same `project_id`. It does not insert.

`roadmap_migrate`'s two identity checks in `RoadmapMigrateVerb::run()` call
`readProjectBySlug()` and `readProjectByRoot()` with
`Visibility::IncludeDeregistered`. A revived
project keeps its stored slug and name under the same rules a live one does;
each refusal carries `deregistered:true` when the row it names is deregistered.
For a deregistered owner, the fields `withOwner()` adds say `store_backed:false`,
and the message says the root is deregistered rather than already migrated.

### 2.5 A departed project's slug stays taken

A new root asking for a deregistered project's `export_slug` is refused
`slug_collision`, as for a live holder, with `deregistered:true`. The slug is
the mailbox address, so handing it on would deliver the departed project's mail
to a stranger. Freeing a slug is out of scope (§ 5).

### 2.6 Restoring onto a deregistered row

`RoadmapExport::runImportCommand()` refuses when the root or slug is already
held, asking the id-only `projectIdForRoot()` and `projectIdForSlug()`. It asks
`readProjectByRoot()` and `readProjectBySlug()` with
`Visibility::IncludeDeregistered` instead, so it learns `deregisteredAt`. A
deregistered row holding **both** the root and the slug being restored is the
same project returning: the restore writes into that row (clears
`deregistered_at`, sets name and legend from the export) instead of inserting.
Any other overlap refuses with today's message, plus ` (deregistered)` after
the holder when its row is deregistered. Its "remove it first with
roadmap_migrate op:"deregister"" remedy is offered only where the holder holds
both the root and the slug being restored, the one case where deregistering
then restoring revives the row. Elsewhere the remedy is "restore into another
store" alone, since deregistering frees neither root nor slug (§ 2.2).

### 2.7 Reaching a running terminal

The verbs reach sessions through a rebuilt `ants-mcpd` plus `/mcp`, with no
terminal relaunch. The first process built at version 4 to open the store
climbs it. A process built at version 3 then refuses the store
(`createSchema()`'s `version > kSchemaVersion` arm). The user accepted locking older builds out once, for this
bump, on 2026-09-27 (ANTS-5483's body).

## 3. Invariants

- **INV-1** — Deregistering keeps the `project` row, sets `deregistered_at` to
  the passed timestamp, and deletes the project's items, sections, history and
  id prefixes. *Test:* register, add an item and a section, deregister; the row
  is present with the timestamp, and the item and section counts are 0. *Breaks
  when:* the `DELETE FROM project` step is kept.
- **INV-2** — Deregistering deletes no `message` row. *Test:* A sends to B and B
  sends to A; deregister B; both rows are still present. *Breaks when:* the
  message step is kept.
- **INV-3** — Mail reaches a deregistered project. *Test:* deregister B, then
  `sendMessage()` from A to B's slug succeeds, B's inbox, resolved from B's
  root, lists it, and `slugCandidates()` offers B's slug for a near miss.
  *Breaks when:* `projectIdForSlug()` or `projectIdForRoot()` filters on
  registration, or `slugCandidates()` reads `listProjects()` with the default
  visibility.
- **INV-4** — The roadmap readers do not return a deregistered row. *Test:*
  after deregistering B, `readProjectByRoot()`, `readProjectBySlug()`,
  `readProject()` return nothing for B and `listProjects()` omits it, while A is
  still returned. *Breaks when:* the default visibility includes deregistered
  rows.
- **INV-5** — A `project_id` is never reused (ANTS-5483). *Test:* register A
  and B, deregister B (the highest id), register C; C's id is greater than B's.
  *Breaks when:* the row is deleted, which is the only way SQLite reuses the
  id.
- **INV-6** — Registering a deregistered root revives the same row. *Test:*
  deregister B, `registerProject()` B's root; the same `project_id` comes back
  and `deregistered_at` is NULL. *Breaks when:* registration inserts, which the
  UNIQUE root refuses.
- **INV-7** — A deregistered project's slug stays taken. *Test:*
  `roadmap_migrate` a new root asking for B's slug after B deregistered;
  refusal `slug_collision` with `deregistered:true`, nothing written. *Breaks
  when:* the identity check reads with the default visibility, so the INSERT
  fails later as `migrate_failed`.
- **INV-8** — A restore onto its own deregistered row succeeds. *Test:*
  export B, deregister B, import the export at B's root; it succeeds, B's
  `project_id` is unchanged and its items are back. *Breaks when:* the import's
  held-root check sees the deregistered row as a live holder.
- **INV-9** — A store climbed to version 4 matches a DDL-built one. *Test:*
  `RoadmapStoreUpgrade.Inv8DdlBuiltAndClimbedStoresMatch`, which seeds a
  version-1 store, climbs it to `kSchemaVersion` and compares both schemas
  under its `normaliseDdl` rule. *Breaks when:* the rung's column definition
  and the DDL's differ in a way that rule does not fold.

## 4. Migration / compatibility

- The rung adds a NULL column; every existing project reads as registered.
- `RoadmapStore::contentDigest()` hashes `SELECT * FROM project`, so each
  project's digest changes once at the climb. `drift_cause` reports `store`
  for that project until its next publish. Nothing else reads the digest.
- The three scripts that read `project` directly (`tools/check-shipped-coverage.sh`,
  `tools/roadmap-roundtrip-diff.py`, `tools/roadmap-import-verify.py`) look a
  project up by root or slug. A deregistered row has no items, so each sees an
  empty project rather than none. Each is a developer check, not a user path.

## 5. Out of scope

- Freeing a departed project's slug, or deleting its row for good.
- Keeping a departed project's roadmap readable. It has a mailbox and nothing
  else.
- ANTS-4426's render fingerprint: it needs no schema change (user decision
  2026-10-10).

## 6. Tests

New feature directory `tests/features/project_tombstone/` (`spec.md` plus
`test_project_tombstone.cpp`), driving `RoadmapStore` against a temporary store
for INV-1, INV-2, INV-3, INV-4, INV-5 and INV-6, and `RoadmapMigrateVerb` and
`RoadmapExport` for INV-7 and INV-8. INV-9 is
`tests/features/roadmap_store_upgrade/`.

| Invariant | Checked by |
|---|---|
| INV-1 | `project_tombstone` |
| INV-2 | `project_tombstone` |
| INV-3 | `project_tombstone` |
| INV-4 | `project_tombstone` |
| INV-5 | `project_tombstone` |
| INV-6 | `project_tombstone` |
| INV-7 | `project_tombstone` |
| INV-8 | `project_tombstone` |
| INV-9 | `roadmap_store_upgrade` |

## 7. Cross-doc impact

- `docs/specs/ANTS-4622-cross-session-mailbox.md` § 2.5 says deregistering
  clears mail at both ends; INV-5 there is withdrawn by INV-2 here.
- `tests/features/session_message/spec.md` INV-5 and its test change with it.
- `.claude/rules/roadmap-store.md` describes deleting a project as deleting
  every table down to `project`; it then describes deregistering instead.

## Cold-eyes loop log

The rows are in [`docs/reviews/ANTS-5366-deregistered-project-tombstone-loop-log.md`](../reviews/ANTS-5366-deregistered-project-tombstone-loop-log.md).
