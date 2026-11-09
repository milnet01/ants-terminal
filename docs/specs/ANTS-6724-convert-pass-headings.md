<!-- ants-spec-format: 1 -->
# ANTS-6724 — Convert a pass-headings roadmap to ants-v1, keeping its Pass ids

**Status:** accepted (2026-11-09).
**Kind:** feature.
**Source:** ROADMAP ANTS-6724 (feedback-RetroDB-2026-11-09).

**Amends:** ANTS-4491 (`docs/specs/ANTS-4491-dialect-convert.md`) — § 4.4's
accepted set and INV-7.
**Blocker for:** ANTS-6725.

**Layman:** Lets a project whose roadmap is grouped by review pass switch to
the standard version-grouped format without losing its item numbers, with a
full backup taken first so the switch can be undone.

## 1. Goal

`roadmap_log op:"convert"` takes a store-backed `pass-headings` roadmap to
`ants-v1`. Every item keeps its `PASS-N-M` id, status, headline and body.
Every real convert, of any source dialect, snapshots the store before it
writes. Afterwards the project can use the ants-v1 section ops, such as
`create_section`, which refuses a pass-headings roadmap today.

## 2. Problem

RetroDB wants its roadmap regrouped into `roadmap-format.md` § 3.2 version
blocks. On its pass-headings roadmap `create_section` refuses
`format_mismatch` (ANTS-2031), and `op:"convert"` refuses
`dialect_out_of_scope` (`RemoteControl::cmdRoadmapLogConvert`, ANTS-4491
INV-7). A hand edit is reverted at the next render.

The project is already store-backed in this dialect (ANTS-4803):

```
sqlite3 -readonly ~/.local/share/ants-terminal/roadmap.sqlite \
  "SELECT count(*), sum(status='shipped') FROM item WHERE project_id=
   (SELECT project_id FROM project WHERE export_slug='retrodb');"   # 439|363
```

Every one of those ids is `synthesised` from a `#### Pass N.M` heading
(same query grouped by `id_origin`, 2026-11-09).

**Measured by prototype, 2026-11-09.** A scratch copy of the store (`.backup`)
had RetroDB's `source_format` set to `ants-v1`, and `ants-mcpd --call
roadmap_log op:"render"` published a clone. Findings:

1. All 439 items rendered as ants-v1 bullets carrying their `PASS-N-M` ids. A
   second render wrote zero bytes. A line-by-line comparison against the
   pass-headings file found every line present except the 439 `#### Pass`
   heading lines, whose headline text is now the bullet's.
2. **New ids are wrong without a declared prefix.** An `append` dry run
   proposed `PASS-53-0014`: `RoadmapFoldIn::sniffIdPrefix()` takes `PASS-53`
   for a prefix.
3. **Four ids fail the ants-v1 grammar** on re-read and are noted
   `quarantined_id`: `PASS-47-6-A PASS-41-5-B PASS-41-6-D PASS-41-13-C`
   (`... AND id GLOB 'PASS-*-[A-Z]'` on the query above).
4. Declaring `id_format` in `.ants/project.json` (ANTS-3771) with
   `prefix: "RDB"` and `pattern: "PASS-\d+(?:-\d+)*(?:-[A-Z])?"` fixed both:
   the append proposed `RDB-0001`, and a re-migration dry run carried no
   `quarantined_id` note.

**And the convert has no backup.** `RoadmapMigrateVerb::run()` snapshots the
store before its transaction (ANTS-3855 § 2.4). The convert reaches the load
through `RoadmapMigrateVerb::loadInOpenTransaction()`, which takes none. A
convert rewrites a whole project in a store every project shares.

## 3. Scope decisions (agreed with the user)

1. **Ids, not "Pass N.M" wording.** Titles stay as stored; the id carries the
   pass number. The user (2026-11-09): the ids do not matter, as long as items
   are linked to versions. That linking is ANTS-6725's.
2. **The project declares its own `id_format` before converting.** The convert
   refuses until it does, and never writes `.ants/project.json` itself. The
   user, 2026-11-09. RetroDB's session chose prefix `PASS-59`, and appends
   with `id_strategy:"stable_prefix"` and an explicit id such as
   `PASS-59-273`. A counter append under that prefix pads to `PASS-59-0273`
   (scratch-copy dry run, 2026-11-09), so RetroDB does not use one.
3. **Every real convert backs the store up first**, `github-task-list` sources
   included. The user (2026-11-09) left the scope to this session and asked
   that a conversion be easy to revert.

## 4. Surface

### 4.1 The accepted set

`cmdRoadmapLogConvert` accepts all three answers of
`RoadmapParse::detectRoadmapFormat()`: `github-task-list`, `ants-v1` and,
now, `pass-headings`. The `dialect_out_of_scope` branch stays for a dialect
the detector may name later; no current input reaches it.

A `pass-headings` source is converted as the store-served project it is.
`fileIsTruth` stays `github-task-list` only, so the orphan refusal (ANTS-5326)
does not apply. The rest of ANTS-4491 § 4.3's sequence is unchanged: the load
through `loadInOpenTransaction()`, then `setProjectSourceFormat(…, "ants-v1")`,
inside one `RoadmapWrite::commitAndRender()` with `LaymanGate::Exempt`.

Ids are taken as the load gives them. The pass-headings reader already
synthesises `PASS-N-M` from each heading, and the render writes each in
brackets. No id is allocated.

### 4.2 The id_format precondition

Only on a `pass-headings` source, before any store write:

```
id_format = ProjectSettings::idFormatFor(root)
refuse id_format_required when:
  id_format.prefix  is empty, or
  id_format.pattern is empty, or
  some id of the project, stored or read from the file's Pass headings,
  fails isGrammaticalId(id, id_format)
```

`isGrammaticalId()` passes an id matching the universal grammar or the
declared pattern. It moves out of `src/roadmapmigrate.cpp`'s anonymous
namespace into `roadmapmigrate.h`; the convert calls it, never a copy. The
refusal carries:

| Field | Meaning |
|---|---|
| `missing` | `["prefix"]`, `["pattern"]`, both, or absent |
| `unmatched_ids` | ids failing that test, capped at 200 |
| `unmatched_ids_shown` | present only when the cap bit |

The message names `project_settings` and the `id_format` key. It suggests no
prefix: the project chooses it.

### 4.3 The snapshot

Every convert that is not a `dry_run` snapshots the store after the checks
made before `commitAndRender()`, the rehearsal included, and before it opens
its transaction. ANTS-3855 § 2.4's reasons hold: `VACUUM INTO` cannot run
inside a transaction, and a backup after the write protects nothing. A call
refused before that point takes none. A refusal raised inside the real
`commitAndRender()`, such as `orphans_present`, rolls the store back and
leaves its snapshot; its envelope carries `backup_path` and
`backup_path_source`.

- **Where.** `backup_to` (a new `op:"convert"` argument) when given: the
  snapshot's file path, used as given, as `roadmap_migrate`'s is. Otherwise
  ANTS-3855 § 2.4's folder ladder: config key, backup record, beside the
  store. The convert's handler picks the folder with
  `RoadmapBackupHealth::snapshotDest()`, as the migrate handler does. The
  snapshot-and-fallback block moves out of `RoadmapMigrateVerb::run()` into
  a function both verbs call, taking the file name.
- **Name.** On the ladder,
  `pre-convert-<export_slug>-<UTC YYYYMMDDTHHMMSSZ>.sqlite`. Dated, so a later
  convert or migrate never overwrites it. Outside the weekly job's
  `roadmap-*.sqlite` prune glob.
- **Failure** refuses `backup_failed` and converts nothing. There is no
  `backup:false`.
- **Envelope.** `backup_path` and `backup_path_source`, as `roadmap_migrate`
  reports them; `backup_fallback` after a fallback.

### 4.4 Undoing a convert

Two routes, cheapest first.

1. **One project.** Restore the pre-convert roadmap file (`git revert` of the
   convert's commit), then `roadmap_migrate`. ANTS-4491 § 4.3 already names
   this re-run as the remedy for a file and store that disagree: the re-import
   records `pass-headings` again. Items filed after the convert are absent
   from that file, so the migrate deletes them; `mass_deletion` guards a
   large count.
2. **The whole store.** Replace it with the snapshot while no Ants process
   holds it, which means closing the terminal. This also undoes every other
   project's writes since the snapshot, so it is the last resort.

## 5. Invariants

All cases are in `tests/features/roadmap_convert/test_convert.cpp`, on a
throwaway store and project root. The pass-headings fixture carries two
`Pass` sections, items `Pass 1.1`, `Pass 1.2` and `Pass 2.1.A`, and two
`Status` markers per ANTS-4491 INV-7's note, so the detector reads it as
`pass-headings`.

- **INV-1** — A pass-headings convert with a valid `id_format` leaves the file
  detected as `ants-v1` and the stored `source_format` `ants-v1`.
  *Test:* case `passHeadingsFormatsAgree` — migrate the fixture, declare
  `id_format`, convert, assert both.
  *Breaks when:* the accepted set still excludes `pass-headings`.

- **INV-2** — Every item keeps its id, status, headline and body across a
  pass-headings convert.
  *Test:* case `passIdsKept` — read the item rows before and after, assert
  equal on those four fields, and that the file holds `[PASS-2-1-A]`.
  *Breaks when:* the convert treats the source as id-less and allocates, or
  the convert's write changes a body or headline.

- **INV-3** — Without a declared `prefix` and `pattern`, or with an id of the
  project failing `isGrammaticalId()` under them, a pass-headings convert
  refuses `id_format_required`.
  It leaves the file's bytes, the store's rows and `source_format` unchanged,
  and writes no snapshot.
  *Test:* case `idFormatRequired` — three runs: no `id_format`; a pattern
  `RDB-\d+`, which no id matches, so `unmatched_ids` is `["PASS-2-1-A"]`
  because the other two pass the universal grammar; a pattern with no prefix.
  Each asserts the code, the unchanged file hash and `source_format`, and no
  file at `backup_to`.
  *Breaks when:* the check is missing, so the convert proceeds and the next
  append proposes an id under a `PASS-<n>` prefix; or it tests only
  presence, so the letter-suffix id is quarantined at the next migrate.

- **INV-4** — Converting a pass-headings source twice produces the same file
  as converting it once.
  *Test:* case `passConvertIsIdempotent` — ANTS-4491 INV-3's case over this
  fixture.
  *Breaks when:* the second run, now over an ants-v1 file, re-matches by a key
  that the first run's render changed.

- **INV-5** — A real convert writes a snapshot before its transaction, and
  the snapshot holds the store as it was before the convert. A `dry_run`
  writes none.
  *Test:* case `convertSnapshotsFirst` — convert with `backup_to` in the temp
  root; open the snapshot and assert the project's `source_format` is
  `pass-headings` and the envelope's `backup_path` names it. Repeat on the
  github-task-list fixture, and assert an `orphans_present` refusal there carries
  `backup_path`. A `dry_run` writes no file at `backup_to`.
  *Breaks when:* the snapshot is taken after `commitAndRender()`, so it reads
  `ants-v1`; or skipped for one source dialect.

- **INV-6** — A failed snapshot refuses `backup_failed` and converts nothing.
  *Test:* case `snapshotFailureRefuses` — `backup_to` under a path whose
  parent is a regular file; assert the code and the unchanged file hash and
  `source_format`.
  *Breaks when:* the convert warns and proceeds.

- **INV-7** — Restoring the pre-convert file and migrating returns every
  item's id, status, headline and body, and `source_format` `pass-headings`.
  *Test:* case `convertIsReversible` — convert, write the original bytes back,
  run the migrate seam, compare against the rows read before the convert.
  *Breaks when:* the convert changes stored data the pass-headings re-import
  cannot restore — an id, or a body rewritten in the ants-v1 form.

- **INV-8** — A `#### Pass N.M <headline>` line whose item renders as
  `[PASS-N-M] **<headline>**` is not counted as lost text.
  *Test:* case `passHeadingNotTextLost` — convert the fixture without
  `accept_text_loss`; assert `ok:true` and no `text_lost`.
  *Breaks when:* the rehearsal compares lines verbatim, so every pass heading
  refuses as lost.

## 6. Failure modes

- **No `id_format`, or one that misses an id.** INV-3's refusal. The project
  sets it with `project_settings` and re-runs.
- **The text-loss rehearsal finds other text.** ANTS-5286's `text_lost`
  refusal, unchanged. Narration that is not a pass heading is the project's
  to check.
- **The snapshot fails.** INV-6. Pass a writable `backup_to`.
- **Publish fails after commit.** ANTS-4491 § 6's window. The store says
  `ants-v1` and the file is still pass-headings, so reads refuse. Re-run the
  migration, or § 4.4 route 2.
- **The first migrate after a convert updates every item.** Measured in the
  prototype: each id's origin settles from `synthesised` to `parsed`, and
  `extras.source_status`, which only the pass-headings reader fills, reports
  `field_conflict`. No file bytes change. This is ANTS-4491 § 4.5's settling,
  not a fault.

## 7. Tests

| Invariant | Case |
|---|---|
| INV-1 | `passHeadingsFormatsAgree` |
| INV-2 | `passIdsKept` |
| INV-3 | `idFormatRequired` |
| INV-4 | `passConvertIsIdempotent` |
| INV-5 | `convertSnapshotsFirst` |
| INV-6 | `snapshotFailureRefuses` |
| INV-7 | `convertIsReversible` |
| INV-8 | `passHeadingNotTextLost` |

Same file, bundle and label (`features`) as ANTS-4491's cases. Each case is
seen to fail against the pre-change code before the change is restored. Every
real convert in the file, ANTS-4491's cases included, passes `backup_to`
inside its temp root, so no case writes to the user's snapshot folder; the default ladder is ANTS-3855 INV-15's to test.

`thirdDialectRefused` (ANTS-4491 INV-7) is removed: its fixture is now
accepted input.

## 8. Alternatives considered (and rejected)

- **Teach every section op the pass-headings dialect** (the request's route
  b). Rejected: `create_section`, section moves and archive rotation would
  each need a pass-headings writer, and the project would still be in a
  dialect `place_by_release` does not serve.
- **Convert writes `id_format` itself from an argument.** Rejected by the
  user (§ 3 item 2). It writes a second file outside the one transaction, so
  a failure could leave half a change.
- **Re-id every item into the project's new prefix.** Rejected: RetroDB's
  commits and code cite `PASS-N-M` and "Pass N.M"; new ids would break every
  citation.
- **Reuse `pre-migrate.sqlite` as the snapshot name.** Rejected: it is
  overwritten by the next migrate of any project, so the convert's undo would
  vanish.

## 9. Out of scope

- Placing shipped items in version blocks from the evidence RetroDB keeps —
  tracked by ANTS-6725.
- RetroDB's own preamble, which tells a reader to grep `^#### Pass`. The
  project edits it with `op:"set_preamble"`.
- A restore verb for a snapshot — deferred; not yet queued.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `test_convert.cpp::passHeadingsFormatsAgree` |
| INV-2 | `test_convert.cpp::passIdsKept` |
| INV-3 | `test_convert.cpp::idFormatRequired` |
| INV-4 | `test_convert.cpp::passConvertIsIdempotent` |
| INV-5 | `test_convert.cpp::convertSnapshotsFirst` |
| INV-6 | `test_convert.cpp::snapshotFailureRefuses` |
| INV-7 | `test_convert.cpp::convertIsReversible` |
| INV-8 | `test_convert.cpp::passHeadingNotTextLost` |
| One snapshot ladder for both verbs (§ 4.3) | **nothing** — a second copy would pass every case; code review only |
| The dated snapshot name (§ 4.3) | **Partial:** `convertSnapshotsFirst` passes `backup_to`, so the default name is unexercised |

## 11. Cross-doc impact

- `docs/specs/ANTS-4491-dialect-convert.md` — § 4.4 and INV-7 annotated as
  amended by this spec; § 9's third-dialect bullet withdrawn.
- `tests/features/roadmap_convert/spec.md` — the new invariants; INV-7's line
  withdrawn.
- `docs/standards/mcp-error-codes.md` — `id_format_required` added;
  `dialect_out_of_scope` says no current dialect reaches it; `backup_failed`
  names the convert.
- `docs/standards/roadmap-format.md` — the convert route names pass-headings.
- `roadmap_log`'s schema text in `src/claudeintegration_store.cpp` — the
  convert description and `backup_to`. The `McpEagerLoad` test caps each
  description.
- `CHANGELOG.md` — a bullet stating what shipped.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-6724-convert-pass-headings-loop-log.md`.

## 13. Resource cost

Each snapshot is a full copy of the store: 125,427,712 bytes on 2026-11-09
(`ls -l` of that day's `.backup`). Nothing prunes `pre-convert-*` files. A
convert runs once per project, and the user deletes a snapshot once the
convert is accepted.

## 14. Migration / compatibility

No schema change. A caller passing a pass-headings source got
`dialect_out_of_scope` and now gets `id_format_required` or a convert. A
github-task-list convert now writes a snapshot and reports `backup_path`.

## 15. Reload

`roadmap_log` is project-scoped and `ants-mcpd` serves it. A rebuilt
`ants-mcpd` reaches clients on their next reconnect; no terminal relaunch.
The op runs in `ants-mcpd`'s process, like every other `roadmap_log` op. It
holds no running state, so it has no stop/start control.
