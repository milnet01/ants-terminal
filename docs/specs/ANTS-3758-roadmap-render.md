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

## 1. Problem

The store is primary after cutover (`roadmap-data-model.md` INV-3), but nothing
writes markdown back out of it. Three consequences, and the third is the one
that makes this a blocker rather than a nicety:

1. **A migrated project has two roadmaps and no rule about which wins.** The
   store holds the items; `ROADMAP.md` still holds the bytes every consumer
   reads. `roadmap_query`, `roadmap_log` and `RoadmapDialog` all still parse
   the file (ANTS-3793 owns moving them), so until the file is generated,
   cutover cannot happen at all.
2. **Nothing exercises `roadmap-data-model.md` INV-2 or INV-3.** That
   document's *What checks this* table says so in as many words — both rows
   read `nothing yet`, because there is no render to check.
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
`RoadmapExport` is refitted onto it in the same change (global rule 3b: the
bump and its callers ship together).

```cpp
// Ordered contents of one section — the render's per-section input, and the
// reader RoadmapExport::writeElements() stops hand-rolling in SQL.
struct ElementRow {
    int     position = 0;
    QString kind;        // 'item' | 'narration' | 'table'
    QString payload;     // narration/table payload; empty for 'item'
    qint64  itemPk = 0;  // 'item' only; 0 otherwise
};
std::optional<QVector<ElementRow>> listElements(qint64 sectionId,
                                                QString *error = nullptr) const;
```

`SectionRow` carries no `section_id`, so a caller that enumerates via
`listSections()` cannot then call `listElements()`. Rather than widen
`SectionRow` — whose shape ANTS-3765 § 2.4 fixed for the migration's
compare-before-write — the render resolves each slug through the existing
`findSection(projectId, slug)`. One point lookup per section, 204 of them for
this project (`## ` + `### ` heading count in `ROADMAP.md`), against an
indexed `UNIQUE (project_id, slug)`.

### 2.2 Ordering

Sections: `RoadmapStore::listSections()` sorted by the shipped free function
`sectionOrderLess()`, whose key is `(position, slug)`. Items and
the other elements: `element.position` ascending, which
`UNIQUE (section_id, position)` already makes total.

Parent/child nesting is **not** re-derived from `parent_id`; `position` is
project-wide document order (ANTS-3796), so emitting sections in that order
already places a child after its parent. `parent_id` is used only to emit the
heading level, and `level` is stored anyway — the two must agree, which is
INV-9's job rather than the renderer's.

### 2.3 File routing — where each section is written

`section.source_path` decides, and nothing else does:

| `source_path` | Rendered into |
|---|---|
| SQL `NULL` | the project's live roadmap (`ROADMAP.md`, or `project.json`'s `roadmap` override) |
| a relative path | that path, verbatim (`docs/roadmap/0.5.md`, `docs/roadmap/0.6.md`) |

A render pass therefore writes **a set of files**, not one. This project has
two archives today (`ls docs/roadmap/`), so the first live render writes three
files.

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
`layman` renders **nothing** — not a partial file, not a file with gaps. The
render refuses, names the offending ids, and exits non-zero.

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
  render(store)  ──parse──▶  RoadmapMigrate/Load  ──▶  scratch store
                                                          │
  store  ──RoadmapExport──▶ export A          export B ◀──┘
                                  └────── byte-compare ──────┘
```

If A == B, the render dropped nothing and invented nothing, because the export
is already contractually a complete copy of the store (INV-1 of the data
model, tested by `Inv1RoundTripIsByteIdentical`). This reuses a checked
serialiser rather than adding a second definition of equality.

**Consequence that must not surprise anyone: the first render of a
hand-written roadmap produces a large diff.** It is a one-time normalisation
of wrapping and blank lines, reviewable as a diff before it is committed, and
`--dry-run` (§ 2.7) exists so it can be reviewed. It is not data loss, and
§ 2.6's comparison is what distinguishes the two.

### 2.7 Entry point

```cpp
struct RenderOptions {
    bool dryRun = false;   // compute + report, write nothing
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

All-or-nothing across the file set: every file is staged with `QSaveFile` and
committed only once all of them serialise, so an archive can never be updated
while the live roadmap fails.

## 3. Invariants

- **INV-1** — **The render loses nothing and invents nothing.** Exporting the
  store, rendering it, re-loading the render into a scratch store and
  exporting that produces byte-identical exports. *Breaks when:* any field of
  `roadmap-format.md` § 3.5 is dropped from the bullet, or an element is
  emitted out of order. *Test:* `roadmap_render/` case `Inv1ExportsMatch`.
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
- **INV-11** — **`listElements()` is the only element reader.** No `FROM
  element` SQL exists outside `src/roadmapstore.cpp`. *Breaks when:*
  `RoadmapExport` keeps its raw query and the two readers drift on ordering.
  *Test:* source grep, `Inv11SingleElementReader`.

## 4. RAM / build cost

The live roadmap is **3,033,345 bytes over 33,542 lines** (`wc -c -l <
ROADMAP.md`, 2026-08-03) across **204 sections** (18 `## ` + 186 `### `). The
render holds one section's elements at a time and appends to a per-file
`QString`, so peak is bounded by the largest single output file rather than by
the corpus: **under 8 MiB** for this project (3 MB of text, ~2 bytes/char in
`QString`'s UTF-16, plus the staged `QSaveFile` buffer). That is the same
order as the export, which INV-12 of ANTS-3761 already caps at a 4 MiB delta.

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
  ANTS-2126), so this is deferred work rather than a wall. **A project whose
  stored sections came from a non-§ 3.5 source refuses to render**, so the
  conversion cannot happen by accident.
- **When the render runs, and pushing its output anywhere** — ANTS-3794.
- **Moving `roadmap_query` / `roadmap_log` / `RoadmapDialog` onto the store** —
  ANTS-3793. This spec makes the file they parse generated; it does not change
  who parses it.
- **Archive rotation as an ongoing operation.** This spec preserves the
  rotation that already happened, via `source_path`. Whether the store takes
  over *performing* rotations is `roadmap-data-model.md` § 9's open item.
- **Curating the 103 `layman` lines** INV-5 gates this project on. It is
  content work, not code.

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
- `CLAUDE.md`'s module map and `docs/subsystems.md` gain `roadmaprender`.
- `roadmap-format.md` § 3.5.1's counter definition still needs its cut-over
  amendment — that is ANTS-3793's, not this one's.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
