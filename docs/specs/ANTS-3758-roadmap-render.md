# ANTS-3758 — generate ROADMAP.md from the store at full fidelity

**Status:** spec draft (2026-08-03).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3758 (ANTS-3753 split, spec seam 3a of 5; render
fidelity decided by the user 2026-08-01/08-03).
**Blocked by:** ANTS-3796 (section document order), ANTS-3797 (`source_path`
survives the export), ANTS-3795 (the standard describes a full-fidelity
render) — all shipped.
**Blocker for:** ANTS-3793 (consumer cutover), ANTS-3794 (publish + health
checks).
**Pairs with:** ANTS-3761 (the export, whose serialiser this spec reuses as its
equality oracle).

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 `listElements()`](#21-roadmapstorelistelements--the-reader-that-does-not-exist) ·
[2.2 Ordering](#22-ordering) · [2.3 File routing](#23-file-routing--where-each-section-is-written) ·
[2.4 Membership](#24-membership) · [2.5 The publish gate](#25-the-publish-gate) ·
[2.6 The equality oracle](#26-the-equality-oracle--how-lossless-is-proved) ·
[2.7 Entry point](#27-entry-point) · [2.8 The file preamble](#28-the-file-preamble)) ·
[3. Invariants](#3-invariants) · [4. RAM / build cost](#4-ram--build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

The store is primary after cutover (`roadmap-data-model.md` INV-3), but nothing
writes markdown back out of it. Three consequences, and the third is the one
that makes this a blocker rather than a nicety:

1. **A migrated project has two roadmaps and no rule about which wins.** The
   store holds the items; `ROADMAP.md` still holds the bytes every consumer
   reads. `roadmap_query`, `roadmap_log` and `RoadmapDialog` all still parse
   the file (ANTS-3793 owns moving them), so until the file is generated,
   cutover cannot happen at all.
2. **Nothing exercises `roadmap-data-model.md` INV-2.** That document's *What
   checks this* table says so in as many words — the row reads `nothing yet —
   there is no render before ANTS-3758`. (Its INV-3 row also reads `nothing
   yet`, for the different reason that hand-edit detection is unbuilt; this
   spec does not close that one.)
3. **The store already carries two columns that exist only to serve this, and
   both are currently write-only in effect.** `section.position` (ANTS-3796)
   is the only record of a project's document order, and
   `section.source_path` (ANTS-3797) is the only record of which file a
   section came from. Its DDL comment in `src/roadmapstore.cpp` names this
   spec by id: *"without it ANTS-3758 re-emits a rotated archive back into
   ROADMAP.md"*.

There is no renderer today — `grep -rl "RoadmapRender\|renderRoadmap" src/
tests/` returns nothing.

## 2. Surface

New: `src/roadmaprender.{h,cpp}` in `ants_roadmapstore_lib`, beside
`src/roadmapexport.cpp` in that lib's source list. Qt6::Core only — no Widgets,
because ANTS-3794 will call it from a headless publish path.

### 2.1 `RoadmapStore::listElements()` — the reader that does not exist

The store has **no element enumerator**: `grep -n "listElements\|ElementRow"
src/roadmapstore.h src/roadmapstore.cpp` returns nothing. `RoadmapExport`
consequently reaches past its own reader with raw SQL —
`RoadmapExport::writeElements()` runs `SELECT e.position, e.kind, e.payload,
i.id_fold FROM element e …` directly, precisely the pattern
`RoadmapStore::listSections()`'s own header comment says that surface exists to
prevent.

The render cannot be written without this, so it is added here and
`RoadmapExport` is refitted onto it in the same change — a new reader whose
first caller leaves the old hand-rolled query in place ships two readers that
will disagree, so the refit is part of this work rather than a follow-up.

```cpp
// Ordered contents of one section — the render's per-section input, and the
// reader RoadmapExport::writeElements() stops hand-rolling in SQL.
struct ElementRow {
    int     position = 0;
    QString kind;                    // 'item' | 'narration' | 'table'
    // std::optional and not QString: the DDL makes payload NULL exactly when
    // kind is 'item', and a bare QString collapses NULL with '' — the same
    // distinction SectionRow::sourcePath uses std::optional to keep.
    std::optional<QString> payload;
    qint64  itemPk = 0;              // 'item' only; 0 otherwise
    // The export emits the case-folded ref, and ItemWrite does not carry it
    // (`id_fold` is on ItemRef, not ItemWrite). Without this field the refit
    // below would owe a readItem() per element to recover what the existing
    // single LEFT JOIN already returns.
    QString itemIdFold;              // 'item' only; empty otherwise
};
std::optional<QVector<ElementRow>> listElements(qint64 sectionId,
                                                QString *error = nullptr) const;
```

`SectionRow` carries no `section_id`, so a caller that enumerates via
`listSections()` cannot then call `listElements()`. Rather than widen
`SectionRow` — whose shape ANTS-3765 § 2.4 fixed for the migration's
compare-before-write — the render resolves each slug through the existing
`findSection(projectId, slug)`. One point lookup per section against an indexed
`UNIQUE (project_id, slug)` — on the order of 200 for this project, counting
the live roadmap's 204 headings plus the two archives'.

### 2.2 Ordering

Sections: `RoadmapStore::listSections()` sorted by the shipped free function
`sectionOrderLess()`, whose key is `(position, slug)`. Items and
the other elements: `element.position` ascending, which
`UNIQUE (section_id, position)` already makes total.

Nesting is **not** re-derived from `parent_id`. `position` is project-wide
document order (ANTS-3796), so emitting sections in that order already places a
child after its parent, and **the emitted heading level is `section.level`,
read directly**. `parent_id` is not consulted to compute it. It is still read,
for one purpose only: when the depth it implies disagrees with the stored
`level`, the render **refuses** (INV-9) rather than silently picking one. A
store that disagrees with itself is corrupt, and a renderer that quietly
resolves the disagreement renumbers headings on the next reparent.

### 2.3 File routing — where each section is written

`section.source_path` decides, and nothing else does:

| `source_path` | Rendered into |
|---|---|
| SQL `NULL` | the project's live roadmap (`ROADMAP.md`, or `project.json`'s `roadmap` override) |
| a relative path | that path **resolved against `projectRoot`** (`docs/roadmap/0.5.md`, `docs/roadmap/0.6.md`) |
| `''`, or a path resolving outside `projectRoot` | **refusal** — see below |

A render pass therefore writes **a set of files**, not one. This project has
two archives today (`ls docs/roadmap/`), so the first live render writes three
files.

**`source_path` is stored data, so it is untrusted input to a file write.**
Taking it "verbatim" would let a `../../..`-shaped value write outside the
project entirely. Every path is canonicalised and required to resolve under the
canonicalised `projectRoot`; one that does not aborts the whole pass (INV-13),
and an empty string is a refusal rather than a synonym for `NULL` — ANTS-3782
made that distinction load-bearing on the way in and it holds on the way out.
Missing parent directories are created; an existing path that is not a regular
file is a refusal.

### 2.4 Membership

`roadmap-data-model.md` § 7.5 fixes the eligible set: `visibility = 'public'`,
and never `status = 'dropped'`. Inside that set the curation question § 9 of
that standard left open is **decided here, by the user (2026-08-03): the render
lists everything, closed items included.** The generated file is a faithful
replacement for the file that exists today; nothing disappears on the day a
project switches over, and no commit or CHANGELOG link to a shipped item
breaks.

An excluded item is excluded silently in the file and **counted in the
outcome** (§ 2.7), because a render that quietly drops items and reports
success is the failure this project's audit history keeps re-learning.

### 2.5 The publish gate

`roadmap-data-model.md` § 3.2 gates publish on `layman`, for open items only.
The standard flagged, and this spec settles, what happens at cutover when
§ 3.3 has left `layman` empty on every migrated item.

**Decided by the user (2026-08-03): the gate is strict and has no
migrated-item exemption.** A project with any *public, open* item lacking
`layman` renders **nothing** — not a partial file, not a file with gaps.

**Open** means `roadmap-data-model.md` § 3.4's open: `planned`, `in-progress`,
`considered`. `shipped` and `dropped` are closed and are never gated, which is
why a project with published history can satisfy the gate at all.

**A gate failure is a result, not an error.** The call returns a populated
`RenderOutcome` with `filesWritten` empty and `gateFailures` naming every
offending id; `std::nullopt` is reserved for I/O and SQL failures, where there
is nothing to report. If a refusal returned `nullopt`, the ids this gate exists
to name would be unreachable through the declared API. Turning either into a
process exit code is ANTS-3794's job, not this library's.

**The curation backlog is an output, not a figure in this document.**
`--dry-run` (§ 2.7) reports `gateFailures` without writing anything, so the cost
of cutting a project over is one command away and can never go stale here.
Drafting this spec produced two hand-rolled counts for the same question that
disagreed by 70% — which is the argument for the dry run rather than for a
third count. Its shape is stable and worth stating: most projects owe a handful
of lines, a few owe none and can cut over today, and this project owes the bulk.

**The gate itself parses nothing.** It reads `item.layman` from the store, so
it is `layman.isEmpty()` over the open public items and no markdown is
involved — whether a bullet's `Layman:` line was recognised was settled once,
by the migration, through `RoadmapParse::parseBullets()`. That is why the two
draft counts above disagreed and why neither belongs in this document: both
were hand-rolled markdown scans standing in for a migration that had not been
run, and they diverged on how a bullet body continues across a blank line. The
authoritative count is the store's, after migration, and `--dry-run` is how you
ask for it.

### 2.6 The equality oracle — how "lossless" is proved

Byte-identity against a hand-written file is the wrong contract and would fail
on trivia: the corpus is hand-wrapped, and a renderer that reproduced every
author's wrapping choice would be reproducing accidents, not content.

So fidelity is proved through the **shipped export** (ANTS-3761) instead:

```
  render(store)  ─▶ files on disk under a scratch root
        │            │
        │            └─ RoadmapMigrate::findRoadmaps() ─▶ planFrom()
        │                       ─▶ RoadmapMigrateLoad::load() ─▶ scratch store
        │                                                            │
  store ──RoadmapExport─▶ export A ─▶ PROJECT ─┐      export B ◀────┘
                                            └── byte-compare ──┘
```

The render is written to a **scratch project root**, archives at their
`source_path`s, because `findRoadmaps()` discovers the file set from disk — the
test has to reproduce that layout, not just hand the loader one string.

**The comparison is against a *projection* of export A, not export A itself,
and saying which is the whole contract.** Export A is a complete copy of the
store; the render is deliberately not, so a raw A == B is unsatisfiable on any
real store. Three families of difference are expected and are excluded from the
comparison:

1. **Items the render excludes by design** — `visibility = 'internal'` and
   `status = 'dropped'` (INV-4). Project A to the same predicate.
2. **Record kinds markdown does not carry** — `history`, and any
   `relationship` / `citation` / `feedback_ref` row, none of which has a
   markdown serialisation. `RoadmapMigrateLoad::load()` also *writes* history
   with a caller-supplied `Options::changedAt`, so those rows differ even in
   principle.
3. **Store-populated identity fields whose value is a fact about the write,
   not about the item** — `created` / `last_modified`, which the second load
   stamps afresh.

What survives the projection is exactly the set of facts markdown is supposed
to carry, and INV-1 is the claim that *that* set round-trips. The excluded
families are excluded by an enumerated predicate written once in the test
helper, so a future record kind is a compile-or-fail rather than a silent
widening — the same discipline ANTS-3797 had to retrofit onto ANTS-3761's
column diff after a column went uncarried and the check passed anyway.

**This oracle proves losslessness and NOT format conformance**, and the two are
different claims. `roadmap-format.md` § 3.5.3 defaults an absent `Kind:` to
`implement`, so a render that omitted `Kind:` on every `implement` item would
re-parse to the same store and pass this comparison perfectly. That is the
"render silently missing a required piece" `roadmap-data-model.md` § 9 assigns
to this spec, and it needs its own check — INV-12.

**Consequence that must not surprise anyone: the first render of a
hand-written roadmap produces a large diff.** It is a one-time normalisation
of wrapping and blank lines, reviewable as a diff before it is committed, and
`--dry-run` (§ 2.7) exists so it can be reviewed. It is not data loss, and
§ 2.6's comparison is what distinguishes the two.

### 2.7 Entry point

```cpp
struct RenderOptions {
    // Computes everything and writes nothing. filesWritten lists the files a
    // real run WOULD have written, so a caller can review the set before
    // committing to it; gateFailures is populated identically either way.
    bool dryRun = false;
};
struct RenderOutcome {
    QStringList filesWritten;
    int  itemsRendered = 0, itemsExcluded = 0, sectionsRendered = 0;
    QStringList gateFailures;   // ids lacking `layman`; non-empty ⇒ nothing written
};
std::optional<RenderOutcome> render(RoadmapStore &store, qint64 projectId,
                                    const QString &projectRoot,
                                    const RenderOptions &opts = {},
                                    QString *error = nullptr);
```

All-or-nothing across the file set, **with one honest limit**: every file is
staged with `QSaveFile` and none is committed until all of them serialise, so
no failure in rendering, gating or serialising can leave a half-updated
project. `QSaveFile::commit()` is nevertheless per file, so a failure *during
the commit phase* — the disk filling between commit one and commit two — can
still land some files. That window is not closable without a journal, so it is
reported rather than hidden: the outcome's `filesWritten` names exactly what
landed, and the call reports failure. Staging removes every failure mode except
this one; claiming it removed them all would be the more comfortable sentence
and the false one.

### 2.8 The file preamble

Everything above the first section heading, which §§ 2.1–2.2 do not reach
because it belongs to no section's element list. Each piece names the store
column it comes from, because a preamble sourced from nothing is how INV-8's
format marker would come to be hardcoded:

| Emitted | Source |
|---|---|
| `roadmap-format.md` § 3.1's format marker, first five lines | a constant — it declares the format this renderer emits, so it is the renderer's own claim, not stored data |
| `# <name> — Roadmap` H1 | `project.name` |
| § 5.1's status legend | `project.legend`, written by `RoadmapStore::setLegend()` and rendered back in the project's own wording |
| a section's intro prose | `SectionRow::intro`, emitted between the heading and the section's first element |

A project with no stored legend emits none, rather than emitting this
project's. The legend is per project precisely so one renderer can serve every
project's vocabulary (`roadmap-data-model.md` § 5.1), and substituting a
default would quietly undo that.

## 3. Invariants

- **INV-1** — **The render loses nothing and invents nothing, over the facts
  markdown carries.** Rendering a store, re-loading the render from disk into a
  scratch store and exporting both produces byte-identical exports **once § 2.6's
  three excluded families are projected out of each**. *Breaks when:* a
  non-defaultable field — `layman`, `body`, `resolution`, `lanes`, `evidence`,
  an `extras` key — is dropped from the bullet, or an element is emitted out of
  order. *Test:* `roadmap_render/` case `Inv1ExportsMatch`.
- **INV-12** — **Every emitted bullet literally carries all four pieces
  `roadmap-format.md` § 3.5 makes required** — status emoji, `[PROJ-NNNN]` id,
  bold headline ending in a period, `Kind:`. INV-1 cannot see this: § 3.5.3
  defaults an absent `Kind:`, so omitting it round-trips clean. *Breaks when:*
  the renderer skips `Kind:` for items whose kind is `implement`, on the
  reasoning that the default restores it. *Test:* `Inv12RequiredPiecesPresent`,
  which asserts against the rendered TEXT and not against a re-parse.
- **INV-13** — **No file is written outside `projectRoot`.** A `source_path`
  that canonicalises outside it, or is the empty string, aborts the pass before
  anything is staged. *Breaks when:* `source_path` is joined to the root and
  used without canonicalising, so `../../x.md` escapes. *Test:*
  `Inv13PathContainment`.
- **INV-14** — **`dryRun` writes nothing and reports everything.**
  `filesWritten` names the files a real pass would have written and no file on
  disk changes, including mtime. *Breaks when:* dry-run is implemented as
  write-then-delete. *Test:* `Inv14DryRunWritesNothing`.
- **INV-2** — **Sections are emitted in `sectionOrderLess()` order**, i.e.
  `(position, slug)`. *Breaks when:* the renderer sorts by `slug`, by
  `section_id`, or walks `parent_id` recursively. *Test:* `Inv2SectionOrder`,
  which stores three sections whose slug order and position order disagree.
- **INV-3** — **A section renders into the file its `source_path` names**, and
  a `NULL` `source_path` renders into the live roadmap. *Breaks when:* a
  rotated archive's sections are folded back into `ROADMAP.md` — the exact
  outcome that column's DDL comment names this spec for. *Test:*
  `Inv3ArchiveRouting`.
- **INV-4** — **`internal` and `dropped` items never appear in any rendered
  file**, and every other item does, `shipped` included. *Breaks when:* the
  filter is written as "open items only", or `visibility` is ignored. *Test:*
  `Inv4Membership`.
- **INV-5** — **A public open item with an empty `layman` makes the whole
  project render nothing.** No file is written, `gateFailures` names the ids,
  and the call reports failure. *Breaks when:* the gate is applied per item
  (skipping the offender) rather than per project. *Test:* `Inv5PublishGate`.
- **INV-6** — **A render pass writes every file or none.** *Breaks when:*
  files are written in a loop with no staging, so a mid-loop failure leaves an
  updated archive beside a stale roadmap. *Test:* `Inv6AllOrNothing`, which
  makes the second file's write fail and asserts the first is unchanged.
- **INV-7** — **Rendering is idempotent.** Rendering twice with no store
  change writes byte-identical files the second time, so a scheduled render
  produces no spurious diff. *Breaks when:* any ordering falls back to an
  unstable comparison, or a timestamp is emitted. *Test:* `Inv7Idempotent`.
- **INV-8** — **Every rendered live roadmap carries
  `roadmap-format.md` § 3.1's format marker in its first five lines.** *Breaks
  when:* the marker is treated as prose belonging to the first section's intro
  and is lost with it. *Test:* `Inv8FormatMarker`.
- **INV-9** — **A section's emitted heading level is its stored `level`**, and
  a stored `level` that disagrees with the depth implied by `parent_id` is a
  refusal, not a silent choice between them. *Breaks when:* the renderer
  derives depth by walking parents, which reorders nothing but renumbers
  headings after any reparent. *Test:* `Inv9LevelAgreesWithParent`.
- **INV-10** — **`narration` and `table` elements survive at their stored
  position**, interleaved with items. *Breaks when:* the renderer emits all
  items and then all other elements. *Test:* `Inv10ElementInterleaving`.
- **INV-11** — **`listElements()` is the only element reader in production
  code.** No `FROM element` SQL exists under `src/` outside
  `src/roadmapstore.cpp`. **Tests are out of scope and that is not a
  loophole**: ten such queries already exist under `tests/features/`, and they
  are there to assert the schema behaves as specified — routing them through
  the reader under test would make them assert the reader agrees with itself.
  *Breaks when:* `RoadmapExport::writeElements()` keeps its raw query and the
  two readers drift on ordering. *Test:* source grep scoped to `src/`,
  `Inv11SingleElementReader`.

## 4. RAM / build cost

The live roadmap is **~3 MB over 33,542 lines** (`wc -c -l < ROADMAP.md`), with
**204 headings in the live file** (18 `## ` + 186 `### `) plus the archives'
sections, which the same pass renders. The byte figure is quoted to one
significant figure on purpose: an exact count measured while drafting was
already stale by the end of the draft, because appending roadmap notes changed
it.

**Peak is bounded by the SUM of the output files, not the largest**, because
§ 2.7 stages every file and commits none until all have serialised — so all
three buffers are live at once. For this project: ~3 MB of text held as
`QString` is ~6 MB in UTF-16, plus the UTF-8 bytes handed to each `QSaveFile`,
plus allocator headroom. Budget **~16 MiB**, and an implementation that wants
less should stream each section into its `QSaveFile` as it is rendered rather
than build a per-file `QString` — which is available precisely because staging
separates writing from committing.

This is deliberately **unlike** the export, whose ANTS-3761 INV-12 caps a
*streaming* writer at a 4 MiB delta independent of corpus size. That bound does
not transfer: this render buffers whole files, so its budget scales with the
project.

No new dependency; one new TU in an existing lib, so build cost is one
`cc1plus` invocation plus the relink of a lib that already exists.

## 5. Out of scope

- **Rendering the GFM task-list and pass-heading formats.** The render emits
  `roadmap-format.md` § 3.5's emoji-bullet form. **Two of the thirteen corpus
  projects use another format** — 3D_Engine GFM task lists, RetroDB pass
  headings (`scratchpad/cold-eyes-3795/gate2.py`, 2026-08-03) — and emitting
  § 3.5 for them would silently convert their roadmap's format at cutover, a
  user-visible change nobody asked for. The
  seam already exists on the write side (`src/passheadingwrite.{h,cpp}`,
  ANTS-2126), so this is deferred work rather than a wall.

  **The store does not record a section's source format, so the render cannot
  refuse on it — and an earlier draft of this spec promised exactly that
  refusal.** `SectionRow` carries slug, title, intro, level, `parent_id`,
  `source_path` and `position`, and `RoadmapParse::detectRoadmapFormat()`
  classifies *lines*, not stored rows. The guard has to come from the caller:
  **ANTS-3794 must not schedule a render for a project whose roadmap
  `detectRoadmapFormat()` does not classify as the emoji-bullet form**, and
  that obligation is recorded here because this spec is where the hazard is
  visible. Adding the column instead is a schema change with a migration, which
  is not this id's.
- **When the render runs, and pushing its output anywhere** — ANTS-3794.
- **Moving `roadmap_query` / `roadmap_log` / `RoadmapDialog` onto the store** —
  ANTS-3793. This spec makes the file they parse generated; it does not change
  who parses it.
- **Archive rotation as an ongoing operation.** This spec preserves the
  rotation that already happened, via `source_path`. Whether the store takes
  over *performing* rotations is `roadmap-data-model.md` § 9's open item.
- **Curating the `layman` lines** INV-5 gates this project on — a `dryRun`
  render reports how many, which is § 2.5's whole point. It is content work,
  not code.

## 6. Tests

`tests/features/roadmap_render/`, label `features;fast`, compiled into an
existing bundle per `tests/features/README.md` (no `add_executable`).

| Case | Invariants |
|---|---|
| `Inv1ExportsMatch` | INV-1 |
| `Inv2SectionOrder` | INV-2 |
| `Inv3ArchiveRouting` | INV-3 |
| `Inv4Membership` | INV-4 |
| `Inv5PublishGate` | INV-5 |
| `Inv6AllOrNothing` | INV-6 |
| `Inv7Idempotent` | INV-7 |
| `Inv8FormatMarker` | INV-8 |
| `Inv9LevelAgreesWithParent` | INV-9 |
| `Inv10ElementInterleaving` | INV-10 |
| `Inv11SingleElementReader` | INV-11 |
| `Inv12RequiredPiecesPresent` | INV-12 |
| `Inv13PathContainment` | INV-13 |
| `Inv14DryRunWritesNothing` | INV-14 |

Per this project's convention, **every case is verified RED against its
*Breaks when* mutation before the implementation is restored.** Where that is
scripted, files are restored with `write_text` and never `shutil.copy2` —
`copy2` preserves mtime, ninja then skips the rebuild, and the mutation
accumulates silently in a binary that still links green.

## 7. Cross-doc impact

- `roadmap-data-model.md` *What checks this*: the INV-2 render-fidelity row
  reads `nothing yet — there is no render before ANTS-3758`. It gains this
  spec's test directory when this ships.
- `roadmap-data-model.md` § 3.2 records the publish-gate question as open and
  hands it here; § 2.5 answers it, so that paragraph is amended on ship.
- `roadmap-data-model.md` records the **closed-items curation** question as
  open in two further places — § 8's ID-floor bullet ("§ 9 has yet to decide
  whether the render lists closed items at all") and § 9's own bullet. § 2.4
  answers it; both are amended on ship, or the standard contradicts itself
  about a question that is settled.
- `CLAUDE.md`'s module map and `docs/subsystems.md` gain `roadmaprender`.
- `roadmap-format.md` § 3.5.1's counter definition still needs its cut-over
  amendment — that is ANTS-3793's, not this one's.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-03 | 2 (single doc, cold; genre pinned `spec`) | 4 / 6 / 8 / 7 / 1 | 25 verified, all fixed. **Both lanes independently named the same three CRITICALs, and the first of them gutted the draft's central design.** § 2.6 proved fidelity by comparing a full export of the store against an export of the re-loaded render — unsatisfiable on any real store, because INV-4 deliberately excludes `internal` and `dropped` items, and `RoadmapMigrateLoad::load()` stamps history rows with a caller-supplied `changedAt`. The oracle now compares a **projection**, with the three excluded families enumerated. A second CRITICAL came at the same claim from the other side: the oracle proves losslessness and NOT § 3.5 conformance, because `roadmap-format.md` § 3.5.3 defaults an absent `Kind:` — so a render omitting it would round-trip clean, which is precisely the "silently missing a required piece" check `roadmap-data-model.md` § 9 assigns to this spec. INV-12 now asserts against the rendered text rather than a re-parse. Third: § 2.2 said `parent_id` drove the heading level while INV-9 said `level` did, so the two readings produced opposite implementations of the same line. Also fixed: **INV-11 was already false when written** (ten `FROM element` queries exist under `tests/features/`), now scoped to `src/`; `ElementRow` could not carry the export refit it promised, since `id_fold` is on `ItemRef` and not `ItemWrite`; the gate had three incompatible failure channels (`std::optional`, `gateFailures`, "exits non-zero"); § 2 never said where the format marker, H1, legend or section intro come from, so INV-8 asserted a marker nothing sourced (new § 2.8); `source_path` was written "verbatim" with no containment check, so a `../../` value escaped the project (new INV-13); and the RAM budget contradicted its own staging design, bounding peak by the largest file when all files are buffered at once. Two self-inflicted: § 5 froze one of the two discredited `layman` counts three sections after § 2.5 disowned both, and the exact byte count went stale during drafting because appending roadmap notes changed it. |
