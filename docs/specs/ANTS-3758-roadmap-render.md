# ANTS-3758 — generate ROADMAP.md from the store at full fidelity

**Status:** accepted (2026-08-03) — cold-eyes loops 1–3 folded, converged at the cap.
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
([2.1 `listElements()`](#21-roadmapstorelistelements--the-reader-that-did-not-exist) ·
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

There was no renderer when this was drafted — `grep -rl
"RoadmapRender\|renderRoadmap" src/ tests/` returned nothing. It ships now
(`src/roadmaprender.{h,cpp}`); the absence is recorded as the starting state,
not as a live claim.

## 2. Surface

New: `src/roadmaprender.{h,cpp}` in `ants_roadmapstore_lib`, beside
`src/roadmapexport.cpp` in that lib's source list — so `Qt6::Core` and
`Qt6::Sql`, the two that lib links, and **no Widgets**, because ANTS-3794 will
call it from a headless publish path.

That lib does **not** link `src/projectsettings.cpp`, so the render cannot read
`.ants/project.json` to find a project's roadmap path. It therefore takes the
live roadmap's path as a parameter and resolving `project.json` is the caller's
job (ANTS-3794's).

### 2.1 `RoadmapStore::listElements()` — the reader that did not exist

The store had **no element enumerator** when this was drafted: `grep -n
"listElements\|ElementRow" src/roadmapstore.h src/roadmapstore.cpp` returned
nothing. It ships now; what follows is the contract it was built to. `RoadmapExport`
consequently reaches past its own reader with raw SQL — `writeElements()`, a
free function in `src/roadmapexport.cpp`'s anonymous namespace, runs `SELECT
e.position, e.kind, e.payload, i.id_fold FROM element e …` directly, precisely
the pattern `RoadmapStore::listSections()`'s own header comment says that
surface exists to prevent.

**The same gap exists for the project row**, and § 2.8's preamble cannot be
built without closing it: there is no reader for `project.name` or
`project.legend` either, and `roadmapexport.cpp` hand-rolls `SELECT project_id,
name, legend FROM project WHERE export_slug = ?` for them. So this spec adds
**two** readers:

```cpp
struct ProjectRow {
    qint64  projectId = 0;
    QString name, exportSlug;
    // The RAW stored text, not a parsed QJsonObject. The export reads it as a
    // string (`*legendText = q.value(2).toString()`) inside a byte-identity
    // contract, so a reader that parsed and re-serialised it would put a
    // round-trip through the middle of INV-1 for no reason.
    QString legendText;
};
std::optional<ProjectRow> readProject(qint64 projectId, QString *error = nullptr) const;
std::optional<ProjectRow> readProjectBySlug(const QString &exportSlug,
                                            QString *error = nullptr) const;
```

**Two lookups, because the two callers key differently and one of them is the
refit.** The render holds a `projectId` already; the export does not — it
resolves `WHERE export_slug = ?` and *obtains* the id as an output. A
`projectId`-only reader could not serve the call site it is meant to replace,
which would leave INV-11's `FROM project` clause unsatisfiable.

The render cannot be written without either, so both are added here and
`RoadmapExport` is refitted onto them in the same change — a new reader whose
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
`UNIQUE (project_id, slug)` — on the order of 300 for this project: the live
roadmap's 204 headings plus the archives' and the synthetic roots.

### 2.2 Ordering

Sections: `RoadmapStore::listSections()` sorted by the shipped free function
`sectionOrderLess()`, whose key is `(position, slug)`. Items and
the other elements: `element.position` ascending, which
`UNIQUE (section_id, position)` already makes total.

**Whitespace is part of the contract, because INV-1 and INV-7 both rest on
it.** A `narration` element's `payload` is emitted **verbatim** — never
re-wrapped, never re-canonicalised; the store holds the author's bytes and the
render is not the place to have opinions about them. Exactly one blank line
separates a heading from what follows it, and each element from the next; each
file ends with exactly one newline. Without a stated policy two conforming
implementations would differ on every blank line, and INV-7 would be asserting
nothing but agreement with its own predecessor.

**A `table` element is not covered by that rule at all: its payload is not
markdown.** `roadmap-data-model.md` § 5.2 stores a table as canonical
`{"header": […], "rows": [[…]]}` JSON, so a render that replayed it would write
that JSON into the file where the rows belong — and the JSON is not
`isTableRow()`, so a re-load files it as `narration` and INV-1 fails. A table
is therefore **serialised** (ANTS-3832), by the inverse of the migration's
reader:

- the header cells, then a separator row of `---` per column — the migration
  drops the separator as delimiter rather than content, so it has none to
  replay and one is synthesised;
- each row's cells, in stored order;
- **each row is spelled `| a | b |`** — leading `"| "`, `" | "` between cells,
  trailing `" |"`. The row syntax is part of the contract because the
  migration's reader is what has to invert it;
- every cell with a literal `|` spelled `\|`, which the migration's
  `tableCells()` inverts. **The escaping must be invertible or INV-1 fails on
  the first pipe-bearing cell**, which is also why a newline is *not* escaped
  here: `<br>` is not invertible (a re-load reads it as literal text), so a
  newline is folded at the write boundary — `roadmap_log`'s `bundle_row`,
  ANTS-3809 § 2.2 — and never reaches the render.

**Invertibility is exact for pipes and deliberately not claimed for surrounding
whitespace, which is a third hazard and not a gap.** `tableCells()` trims every
cell it reads, and `bundle_row` trims every cell it writes, so leading and
trailing whitespace is **not part of a cell's value** anywhere in the system:
it cannot be stored through a supported write path, and a hand-inserted one is
normalised on the next load rather than round-tripping. The render therefore
emits cells as stored and does not pad or strip them. Read "inverse of the
migration's reader" as exact for the pipe escaping and as *agreement about what
a cell's value is* for whitespace — a render that tried to preserve surrounding
whitespace would break INV-1 against a reader that discards it.

**Three payload faults are refused, not rendered malformed:** a payload that is
not a JSON object at all (including one that fails to parse), a payload whose
`header` is absent *or empty*, and a row whose cell count disagrees with the
header's. A refusal here is a **render error** in § 2.7's sense — `std::nullopt`
with `*error` set, raised before anything is staged, so no partial file exists.
The posture is § 2.2's own, stated once below for the level/`parent_id` check
and applying identically here: a store that disagrees with itself is corrupt,
and a render that quietly resolved it would emit a broken table.

Nesting is **not** re-derived from `parent_id`. `position` is project-wide
document order (ANTS-3796), so emitting sections in that order already places a
child after its parent, and **the emitted heading level is `section.level`,
read directly**. `parent_id` is not consulted to compute it. It is still read,
for one purpose only: when the depth it implies disagrees with the stored
`level`, the render **refuses** (INV-9) rather than silently picking one. The
rule is exact: a section with no parent has `level == 2`, and a section with a
parent has `level == parent.level + 1`. The synthetic root (§ 2.8) has no
parent and `level == 0`, and is exempt. A store that disagrees with itself is
corrupt, and a renderer that quietly resolves the disagreement renumbers
headings on the next reparent.

### 2.3 File routing — where each section is written

`section.source_path` decides, and nothing else does:

| `source_path` | Rendered into |
|---|---|
| SQL `NULL` | the live roadmap, at `RenderOptions::liveRoadmapPath` (the caller resolves `project.json`'s `roadmap` override) |
| a relative path | that path **resolved against `projectRoot`** (`docs/roadmap/0.5.md`, `docs/roadmap/0.6.md`) |
| an absolute path | accepted **only if** it canonicalises under `projectRoot`; the column is relative by convention and nothing enforces it, so the containment check is what decides |
| `''`, or any path resolving outside `projectRoot` | **refusal** — see below |

A render pass therefore writes **a set of files**, not one. This project has
two archives today (`ls docs/roadmap/`), so the first live render writes three
files.

**`source_path` is stored data, so it is untrusted input to a file write** —
and so is `liveRoadmapPath`, which a caller supplies. Taking either "verbatim"
would let a `../../..`-shaped value write outside the project entirely.
**Both** are canonicalised and required to resolve under the canonicalised
`projectRoot`; INV-13 covers them equally, because the live roadmap is the one
path every project uses and exempting it would hollow the invariant out.

An empty `source_path` is a refusal rather than a synonym for `NULL` —
ANTS-3782 made that distinction load-bearing on the way in and it holds on the
way out — and an empty `liveRoadmapPath` is a refusal too, since § 2.7 makes it
required. Missing parent directories are created; an existing path that is not
a regular file is a refusal.

**Every refusal in this section returns `std::nullopt` with `*error` set**, not
a `gateFailures` entry: that field means "an item is missing its `layman` line"
and widening it to carry path faults would make a caller's check for a curation
backlog fire on a filesystem error. Path refusals happen before anything is
staged, which is the pre-commit case § 2.7 reserves `nullopt` for.

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

**A section always renders, whatever survives the filter** — heading and intro
included, even when every one of its items was excluded and even when it had
none to begin with. Suppressing an empty section is the other defensible
choice, and it would make the rendered document's section set depend on
item visibility, which § 2.2's ordering and INV-1's round-trip both assume it
does not.

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
offending id; `std::nullopt` is reserved for failures **before the commit
phase** — SQL errors, a render error, a path refusal — where there is nothing
to report. **A commit-phase I/O failure is the exception and does *not* return
`nullopt`** (§ 2.7): it returns an engaged outcome naming exactly what landed,
because that list is the one thing the caller needs and `nullopt` throws it
away. If a refusal returned `nullopt`, the ids this gate exists
to name would be unreachable through the declared API. Turning either into a
process exit code is ANTS-3794's job, not this library's.

**The curation backlog is an output, not a figure in this document.** A
`RenderOptions::dryRun` pass (§ 2.7) reports `gateFailures` without writing
anything, so the cost of cutting a project over is one call away and cannot go
stale here.

**The gate itself parses nothing.** It reads `item.layman` from the store, so
it is `layman.isEmpty()` over the open public items — whether a bullet's
`Layman:` line was recognised was settled once, by the migration, through
`RoadmapParse::parseBullets()`. Any count taken by scanning markdown directly
is answering a different question from the one the gate asks, and will disagree
with it.

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

**The comparison is between *projections of both exports*, taken with the same
predicate — and saying which predicate is the whole contract.** Export A is a
complete copy of the store; the render is deliberately not, so a raw A == B is
unsatisfiable on any real store. Projecting only A would not help: families 2
and 3 below are present in **B** as well, since the re-load writes its own
history and reconstructs its own `id_origin` and `provenance`. One predicate,
applied to each side. Three families of difference are expected and are
excluded:

1. **Items the render excludes by design** — `visibility = 'internal'` and
   `status = 'dropped'` (INV-4). Project A to the same predicate.
2. **Record kinds markdown does not carry** — `history`, `relationship`,
   `citation`, `feedback_ref`, and the `id_prefix` high-water record
   `writeIdPrefixes()` emits. None has a markdown serialisation.
   `RoadmapMigrateLoad::load()` also *writes* history with a caller-supplied
   `Options::changedAt`, so those rows differ even in principle; and the
   high-water drops whenever the highest id belongs to an item family 1
   excluded.
3. **Per-item fields the export emits and markdown has no carrier for** —
   `id_origin` (`roadmapexport.cpp` writes it beside `id`), `provenance` (one
   of its four JSON columns, with `lanes`, `evidence` and `extras`), and the
   `created` / `last_modified` / `shipped` dates. A re-load reconstructs
   `id_origin` as `parsed` and `provenance` from its own defaulting rules, so
   these differ for a reason that is about the *format*, not about the render.
   `milestone` joins them: `roadmap-format.md` defines no bullet line for it
   (a case-insensitive search of that standard returns nothing), so a store
   holding one has no way to render it and INV-1 would fail on any project that
   set one.

**The third family is excluded because markdown has no carrier for those
fields, not because anything re-stamps them.** `putItem()` binds
caller-supplied values and writes NULL when they are empty, and neither
migration TU touches `created` or `last_modified` — so the difference is the
format's, not the loader's.

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
    // Where a NULL source_path routes. Required: this lib cannot read
    // .ants/project.json (§ 2), so the caller resolves it.
    QString liveRoadmapPath;
    // Computes everything and writes nothing. filesWritten lists the files a
    // real run WOULD have written — and is EMPTY when the gate fails, because
    // a real run would have written nothing either. gateFailures is populated
    // identically either way.
    bool dryRun = false;
};
struct RenderOutcome {
    QStringList filesWritten;   // what landed (or, under dryRun, what would have)
    bool committed = false;     // false + non-empty filesWritten = partial commit
    int  itemsRendered = 0, itemsExcluded = 0, sectionsRendered = 0;
    QStringList gateFailures;   // ids lacking `layman`; non-empty ⇒ nothing written
};
// The counters are populated on every engaged return, gate failures included:
// knowing how many items WOULD have rendered is exactly what a caller staring
// at a gate failure wants.
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
reported rather than hidden — and reporting it needs a channel that survives,
which `std::nullopt` is not:

**A commit-phase failure returns an engaged `RenderOutcome` with `committed =
false`, `filesWritten` naming exactly what landed, and `*error` set.** This is
the same rule as the gate's, for the same reason: a refusal that returns
`nullopt` throws away the one field the caller needs. `std::nullopt` is
therefore reserved for failures **before** the commit phase — SQL errors, a
render error, a path refusal — where there is genuinely nothing to report.

Staging removes every failure mode except this one; claiming it removed them
all would be the more comfortable sentence and the false one.

### 2.8 The file preamble

**The preamble is not outside the section model — it is a section.** Content
above the first heading is filed into the **synthetic root**: the empty-slug,
`level = 0` row ANTS-3757 § 2.1 creates and `RoadmapMigrateLoad` documents as
"the same row the plan uses for content above the first heading". It is
returned by `listSections()` like any other, and `sectionOrderLess()` puts it
first.

So §§ 2.1–2.2 **do** reach it, and two rules follow that an implementer would
otherwise have to invent:

- **A `level == 0` section emits no heading.** There is no markdown form for
  it, and § 2.2's "the emitted heading level is `section.level`" would
  otherwise have to produce one. Its intro and elements render normally.
- **INV-9's level/parent agreement check does not apply to it.** A root has no
  parent and level 0 by construction.

**The marker and the H1 are NOT synthesised — they are already in that
section's intro, and emitting them as constants would duplicate both.**
`RoadmapMigrate` builds a section's intro verbatim from its source lines
(`intro.append(lines.at(k - 1))`) and applies no format-marker or H1 filter
anywhere, so the root's stored intro opens with `<!-- ants-roadmap-format: 1
-->` and `# <name> — Roadmap` exactly as the source file did. A render that
prepended its own copies would emit each twice and fail INV-1 on the first live
render of any real project.

So the preamble is **stored data replayed**, with one exception:

| Emitted | Source |
|---|---|
| format marker, H1, and any prose above the first heading | the synthetic root's `SectionRow::intro`, emitted **verbatim** |
| `roadmap-data-model.md` § 5.1's status legend | `project.legendText` — the **exception**, because the migration lifts the legend *out* of the intro into its own record (`looksLikeLegendLine()` closes the intro at the first legend run in source 0), so replaying the intro alone would lose it. **It is stored STRUCTURED, so rendering it is the inverse of that parse and not a verbatim replay** (amended at implementation): the column holds a JSON object of status → wording, and emitting its text would publish JSON into the roadmap. The render emits one `- <emoji> <wording>` line per entry, in the same order as the export's `kStatusOrder`, skipping `dropped` because `roadmap-format.md` § 3.11 gives it no glyph |
| a non-root section's intro prose | that `SectionRow::intro`, between its heading and its first element |

**Emission order for the live roadmap**, which is load-bearing because the
migration only recognises a legend as the first legend-like run following the
intro — emit it elsewhere and a re-load files it as narration, breaking INV-1:

1. the synthetic root's intro, verbatim (marker, H1, any leading prose);
2. the status legend, if the project has one;
3. the synthetic root's own elements, in `position` order;
4. every other section, per §§ 2.2–2.3.

**An archive file gets the same steps for the sections routed to it, but no
legend** — the legend belongs to the project, not to a file, and the migration
only ever reads one from source 0.

**It gets step 1 exactly when it has a root section of its own, which is why
the marker is not purely a replay.** Each source's root takes ANTS-3766 § 2.3's
per-source slug — `""` for the live roadmap, `"<M>-<N>"` for an archive — so an
archive with pre-heading content stores its own root row, carrying its own
marker, H1 and prose, and this render replays it like any other. `walkSource()`
drops a root its source put nothing into, so a file that opens directly on a
heading reaches the store with no root at all, and rendering it from stored data
alone would ship with no format marker and stop being parsed as a conforming
file. The rule is therefore: **replay the marker where a root intro carries it,
emit the constant where no root section routes to this file, and never both** —
which is what INV-8's "exactly once" tests in each direction.

*(Amended twice. At implementation, 2026-08-03, this said an archive **never**
gets step 1, because the empty-slug root was read as one row per project under
`UNIQUE (project_id, slug)` — `RoadmapMigrateLoad` resolves it with
`findSection(projectId, "")` — and a consequence was drawn that an archive's
pre-heading content had nowhere to live. Corrected the same day under
**ANTS-3806**, which ran it: the per-source prefix already gives each source its
own root, the archive's preamble does round-trip, and the constant is for a file
with no preamble rather than for every archive. The standing proof is
`tests/features/roadmap_migrate_archive_root/`.)*

A project with no stored legend emits none, rather than emitting this
project's. The legend is per project precisely so one renderer can serve every
project's vocabulary, and substituting a default would quietly undo that.

## 3. Invariants

- **INV-1** — **The render loses nothing and invents nothing, over the facts
  markdown carries.** Rendering a store, re-loading the render from disk into a
  scratch store and exporting both produces byte-identical exports **once § 2.6's
  three excluded families are projected out of each**. *Breaks when:* a
  non-defaultable field — `layman`, `body`, `resolution`, `lanes`, `evidence`,
  an `extras` key — is dropped from the bullet, an element is emitted out of
  order, or a `table` element is emitted verbatim instead of serialised so its
  payload re-loads as `narration` (INV-15 owns that contract; it is named here
  because it is an INV-1 loss). *Test:* `roadmap_render/` case
  `Inv1ExportsMatch`, plus `Inv1TableRendersAsGfm` for the table leg —
  `Inv1ExportsMatch` alone stayed green through the ANTS-3832 defect, which is
  the blindness that made a second case necessary.
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
  file**, and every other item does, `shipped` included — **except an unfiled
  one**, which has no element row (`ItemRef::sectionId == 0`) and so no place in
  any section. That is a store fault rather than a render decision, so the pass
  **refuses**: rendered nowhere and counted nowhere is exactly the silent loss
  § 2.4 refuses to permit. *Breaks when:* the filter is written as "open items
  only", `visibility` is ignored, or an unfiled item is skipped without comment.
  *Test:* `Inv4Membership`.
- **INV-5** — **A public open item with an empty `layman` makes the whole
  project render nothing.** No file is written, and the call returns an
  **engaged** `RenderOutcome` whose non-empty `gateFailures` names every
  offending id — never `std::nullopt`, which § 2.5 reserves for failures before
  the commit phase. *Breaks when:* the gate is applied per item (skipping the offender)
  rather than per project, or a refusal is signalled by returning `nullopt`,
  which makes the ids unreachable. *Test:* `Inv5PublishGate`.
- **INV-6** — **No failure in rendering, gating or serialising leaves a partial
  write.** Every file is staged before any is committed. The commit phase
  itself is the documented exception (§ 2.7): it cannot be made atomic without
  a journal, so a failure there reports which files landed rather than
  pretending none did. *Breaks when:* files are written in a loop with no
  staging, so a mid-loop *render* failure leaves an updated archive beside a
  stale roadmap. *Test:* `Inv6AllOrNothing`, which fails the second file's
  serialisation and asserts the first is unchanged on disk.
- **INV-7** — **Rendering is idempotent.** Rendering twice with no store
  change writes byte-identical files the second time, so a scheduled render
  produces no spurious diff. *Breaks when:* any ordering falls back to an
  unstable comparison, or a timestamp is emitted. *Test:* `Inv7Idempotent`.
- **INV-8** — **Every rendered file carries `roadmap-format.md` § 3.1's format
  marker within its first five lines, exactly once.** The render **replays** it
  from the synthetic root's intro rather than emitting its own (§ 2.8) and then
  **checks** the emitted bytes, refusing a file that would ship without one —
  so a store whose root intro lost the marker fails loudly instead of
  publishing an unparseable roadmap. *Breaks when:* the render prepends a
  constant marker on top of the one the intro already carries, emitting it
  twice and failing INV-1; or it replays the intro without checking, and a
  markerless store publishes silently. *Test:* `Inv8FormatMarker`, which covers
  both directions.
- **INV-9** — **A section's emitted heading level is its stored `level`**, and
  a stored `level` that disagrees with the depth implied by `parent_id` is a
  refusal, not a silent choice between them. *Breaks when:* the renderer
  derives depth by walking parents, which reorders nothing but renumbers
  headings after any reparent. *Test:* `Inv9LevelAgreesWithParent`.
- **INV-10** — **`narration` and `table` elements survive at their stored
  position**, interleaved with items. *Breaks when:* the renderer emits all
  items and then all other elements. *Test:* `Inv10ElementInterleaving`.
- **INV-11** — **`listElements()` is the only element reader in production
  code**, and `readProject()` / `readProjectBySlug()` (§ 2.1 requires both) the
  only project readers. No `FROM element` or
  `FROM project` SQL exists under `src/` outside `src/roadmapstore.cpp`. **Tests are out of scope and that is not a
  loophole**: such queries already exist under `tests/features/` for both
  patterns, and they
  are there to assert the schema behaves as specified — routing them through
  the reader under test would make them assert the reader agrees with itself.
  *Breaks when:* `roadmapexport.cpp`'s `writeElements()` keeps its raw query,
  or its `writeMeta()` project query survives, and the two readers drift. *Test:* a case-insensitive source grep for
  `FROM\s+element` and `FROM\s+project` over `src/` **excluding
  `src/roadmapstore.cpp`**, which legitimately holds both and is the exemption
  the invariant names, `Inv11SingleElementReader`.

- **INV-12** — **Every emitted bullet literally carries all four pieces
  `roadmap-format.md` § 3.5 makes required** — status emoji, `[PROJ-NNNN]` id,
  bold headline ending in a period, `Kind:`. INV-1 cannot see this: § 3.5.3
  defaults an absent `Kind:`, so omitting it round-trips clean. *Breaks when:*
  the renderer skips `Kind:` for items whose kind is `implement`, on the
  reasoning that the default restores it. *Test:* `Inv12RequiredPiecesPresent`,
  which asserts against the rendered TEXT and not against a re-parse.
- **INV-13** — **No file is written outside `projectRoot`.** A `source_path`
  **or a `liveRoadmapPath`** that canonicalises outside it, or is empty, aborts
  the pass before anything is staged, returning `std::nullopt` with `*error`
  set. *Breaks when:* either path is joined to the root and used without
  canonicalising, so `../../x.md` escapes; or the check is applied to
  `source_path` alone, leaving the one path every project uses unchecked. *Test:*
  `Inv13PathContainment`.
- **INV-14** — **`dryRun` writes nothing and reports everything.**
  `filesWritten` names the files a real pass would have written and no file on
  disk changes, including mtime. *Breaks when:* dry-run is implemented as
  write-then-delete. *Test:* `Inv14DryRunWritesNothing`.
- **INV-15** — **A `table` element is serialised to GFM, never replayed, and a
  shapeless payload is refused rather than emitted.** § 2.2's contract in full:
  the header row, a synthesised `---` separator, each stored row in order, rows
  spelled `| a | b |`, a literal `|` in a cell spelled `\|`; and a refusal
  (`std::nullopt` + `*error`, nothing staged) for a non-object payload, an
  absent-or-empty `header`, or a row whose cell count disagrees with the
  header's. **This invariant exists because the contract it states was added to
  § 2.2 after acceptance and initially sat outside the INV ↔ case scheme every
  other rule in this document is verified by** — prose alone is what let the
  original defect ship. *Breaks when:* a `table` payload is emitted verbatim
  (the ANTS-3832 defect: the canonical JSON lands where the rows belong and
  re-loads as `narration`), the separator row is omitted, a pipe-bearing cell
  is left unescaped, or a malformed payload renders a broken table instead of
  refusing. *Test:* `Inv1TableRendersAsGfm` (serialisation + escaping) and
  `TableRefusesShapelessPayload` (the refusal), both in `roadmap_render/`.

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

**Latency budget: under 2 s for a full render of this project**, cold,
single-threaded — roughly 300 indexed point lookups plus one element query per
section over a ~3 MB corpus, which is I/O-trivial beside the export that
already runs over the same data. It is stated because ANTS-3794 will schedule
this, and a scheduled pass with no budget has nothing to regress against.
`Inv7Idempotent` already renders twice and is the natural place to assert it.

No new dependency; one new TU in an existing lib, so build cost is one
`cc1plus` invocation plus the relink of a lib that already exists.

## 5. Out of scope

- **Rendering the GFM task-list and pass-heading formats.** The render emits
  `roadmap-format.md` § 3.5's emoji-bullet form. **Two of the thirteen corpus
  projects use another format** — 3D_Engine GFM task lists, RetroDB pass
  headings (measured 2026-08-03) — and emitting
  § 3.5 for them would silently convert their roadmap's format at cutover, a
  user-visible change nobody asked for. (Committed tool:
  `tools/roadmap-corpus-survey.py`, which reports the pass-headings count
  directly; the GFM half is a dominant-bullet-shape classification over the
  same corpus, measured 2026-08-03.) The
  seam already exists on the write side (`src/passheadingwrite.{h,cpp}`,
  ANTS-2126), so this is deferred work rather than a wall.

  **The store does not record a section's source format, so the render cannot
  refuse on it.** `SectionRow` carries slug, title, intro, level, `parent_id`,
  `source_path` and `position`, and `RoadmapParse::detectRoadmapFormat()`
  classifies *lines*, not stored rows. The guard has to come from the caller:
  **ANTS-3794 must not schedule a render for a project unless
  `RoadmapParse::detectRoadmapFormat(lines, &sawSignal)` returns `"ants-v1"`
  *and* sets `sawSignal`.** Both halves are required: that function's own
  header records that it "answers `ants-v1` for input it does not recognise,
  including an empty file — so a returned format is no evidence that anything
  was understood", and `sawSignal` exists for exactly this. A guard testing the
  return value alone would pass every unrecognised file it is meant to catch.
  The obligation is recorded here because this spec is where the hazard is
  visible, and repeated in § 7 so it reaches the id that must honour it. Adding the column instead is a schema change with a migration, which
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
| `Inv1TableRendersAsGfm` | INV-15 (with INV-1) |
| `TableRefusesShapelessPayload` | INV-15 |

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
- **ANTS-3794 inherits § 5's scheduling guard** — it must not schedule a render
  unless `detectRoadmapFormat(lines, &sawSignal)` returns `"ants-v1"` *and*
  sets `sawSignal`. Recorded here because the id that must honour it is not the
  id that discovered it.
- `roadmap-data-model.md` § 9's bullet asking for "the check that catches a
  render silently missing a required piece" is answered by INV-12, and is
  amended on ship.
- `CLAUDE.md`'s module map and `docs/subsystems.md` gain `roadmaprender`.
- `roadmap-format.md` § 3.5.1's counter definition still needs its cut-over
  amendment — that is ANTS-3793's, not this one's.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 3 | 2026-08-06 | 2 (single doc, cold; genre pinned `spec`) | 0 / 5 / 6 / 9 / 1 | **The rule-14 gate ANTS-3832's post-acceptance edit skipped (ANTS-3834).** It found what an unrun gate is supposed to: the edit added a whole normative contract — table serialisation, `\|` escaping, a synthesised separator, three refusals — to a document whose entire verification scheme is INV-N ↔ test case, and left it **outside that scheme**, guarded by prose. Both lanes led on it independently. `Inv1TableRendersAsGfm` and `TableRefusesShapelessPayload` had shipped and were listed in the test bundle's own spec.md, but appeared in neither § 6 nor any invariant here. Now **INV-15**, with both cases named, and INV-1's *Breaks when* gains the table leg plus the note that `Inv1ExportsMatch` stayed green through the original defect. Second shared find: § 2.2 presented pipe and newline as a closed hazard list, but `tableCells()` trims every cell it reads — settled by checking the write side, where `bundle_row` trims too, so surrounding whitespace is not part of a cell's value anywhere and the spec now says so rather than claiming an invertibility it does not have. Third: § 2.5 and INV-5 reserved `std::nullopt` for "I/O and SQL failures" while § 2.7 reserves it for failures *before the commit phase* and makes a commit-phase I/O failure return an **engaged** outcome — an implementer following § 2.5 returns `nullopt` on a commit failure and throws away the `filesWritten` list INV-6 needs. Also fixed: the refusal set omitted a non-object payload; "the one exception" was residue of the rule the edit removed; INV-11 named one project reader where § 2.1 requires two (`readProjectBySlug` ships); two absence claims ("there is no renderer today") still read as live against a shipped `roadmap_render.cpp`; § 3.11 was attributed to the wrong standard; an uncommitted scratchpad path was cited as evidence. Dimension tally: dim 5×5, dim 4×4, dim 7×3, dim 2×3, dim 12×2, dim 8×2, dim 15×1, dim 10×1, dim 1×1. **Not converged — stopped after one loop by budget**, with a verified tail (INV-8's constant-emission branch, the legend emoji's unnamed source, `committed` under a successful dry run, and § 2.2's title no longer covering its contents) filed on ANTS-3834 rather than carried in a head. |
| 5-correct | 2026-08-03 | none — a verification, not a review | — | **Correction row, written by the implementer.** Row 4-impl's clause (1) is **wrong on its second half**, and § 2.8 carried the error until now: the synthetic root is not one row per project. `walkSource()` gives each source's root `ctx.prefix` as its slug (ANTS-3766 § 2.3 — `""` for the live roadmap, `"<M>-<N>"` for an archive), so an archive with pre-heading content **does** store its own root row, with its own `source_path`, and this render already replays it into that file. The constant marker is for a file with **no preamble** — one that opens directly on a heading, whose empty root `walkSource()` drops — not for every archive. Established by **running** a two-source fixture end to end (`findRoadmaps()` → `planFrom()` → `load()` → `render()`) under **ANTS-3806**, which was filed on the same misreading and is closed by this row; the standing proof is `tests/features/roadmap_migrate_archive_root/`, two cases, each shown red against its own defect. The mutation that reproduces the reported shape (`root.slug = QString()`) does not lose data either — `planFrom()` raises `archive_slug_collision` and `load()` refuses. Surfaced by the same fixture and **not** fixed here: **ANTS-3808**, the migration and the render disagreeing about what `item.body` holds, so a rendered bullet repeats its own headline and fields. |
| 4-impl | 2026-08-03 | none — implementation, not a review | — | **Implementation row, written by the implementer** (`/cold-eyes` writes review rows only). `src/roadmaprender.{h,cpp}` in `ants_roadmapstore_lib`, plus `RoadmapStore::listElements()` / `readProject()` / `readProjectBySlug()` and the `roadmapexport.cpp` refit off its own SQL. 14 cases in `tests/features/roadmap_render/`, **every one shown RED against a mutation of its own *Breaks when* before the implementation was restored** — two batches of seven, since the mutations sit in distinct code paths. **Three clauses implementation disproved, each amended above.** (1) § 2.8's marker rule was still half wrong: the synthetic root has an EMPTY slug under `UNIQUE (project_id, slug)`, so there is one per PROJECT and an archive has no root section at all — the marker is replayed where a root intro carries it and emitted as a constant where none routes to the file, never both. (**The "one per PROJECT" half is wrong — see row 5-correct.** The marker rule itself survives.) (2) § 2.8 called the legend a source to replay; it is stored **structured** (a JSON object of status → wording), so rendering it is the inverse of the migration's parse and emitting its text would publish JSON into the roadmap. (3) INV-1's full oracle needs the migration loader, a second store and its `Options`, so the shipped case asserts the half that stands alone — every field survives into the text — and the render → load → export comparison moves to ANTS-3793, which has the loader in hand. Two test-side defects caught by running rather than reading: the fixture default-constructed `RoadmapStore`, which resolves `defaultPath()` — the developer's REAL store — and would have had every case writing into it; and INV-11's scrape was case-insensitive over raw text, so it matched **English prose** in three unrelated files ("re-walking from project root") and failed on day one exactly as loop 3 predicted. It now strips comments and matches case-sensitively. Filed separately: **ANTS-3806**, the migration limitation this surfaced — one root row per project means an archive's own preamble has nowhere to live. (**Not a limitation — see row 5-correct**, which ran it.) |
| 3 | 2026-08-03 | 2 (same partition, cold; no prior-loop briefing) | 5 / 4 / 11 / 12 / 0 | **Converged by cap.** 32 verified, all fixed. Both lanes again led on the same three, and the sharpest was loop 2's own fix turned inside out: § 2.8 had just been rewritten to say the preamble IS the synthetic root section — correct — and then emitted the format marker and H1 as renderer **constants** on top of it. `RoadmapMigrate` builds a section's intro verbatim from its source lines and applies no marker or H1 filter anywhere, so the root's stored intro already carries both: the render would have emitted each **twice** and failed INV-1 on the first live render of any real project. The preamble is now stored data replayed, with the legend the single exception (the migration lifts it out of the intro into its own record), and an ordered emission list, because the migration only recognises a legend as the first legend-like run after the intro. Second: `readProject(projectId)` could not serve the call site it was added to replace — `writeMeta()` resolves `WHERE export_slug = ?` and *obtains* the id — so INV-11's `FROM project` clause was unsatisfiable; there are now two lookups, and `ProjectRow` carries the legend as raw text because the export reads it as a string inside a byte-identity contract. Third: a commit-phase failure returned `std::nullopt`, discarding the `filesWritten` list that names what landed — the identical defect § 2.5 explicitly forbids for the gate; `RenderOutcome` gains `committed`. Also fixed: the projection was one-sided (families 2 and 3 are in export B too, so the compare could never pass); `liveRoadmapPath` was uncontained while INV-13 promised no file outside the root; path refusals had no signalling channel; `milestone` has no markdown carrier and belonged in family 3; INV-11's own grep did not exempt the file the invariant exempts, so its test failed on day one; the whitespace and payload policy INV-1 and INV-7 both rest on was never stated; an unfiled item was rendered nowhere and counted nowhere. Invariants reordered ascending; drafting narration removed from the contract body. |
| 2 | 2026-08-03 | 2 (same partition, cold; no prior-loop briefing) | 6 / 6 / 10 / 12 / 0 | 34 verified, all fixed. **Stopped here at the user's explicit instruction to limit review token spend, one loop short of the 3-loop cap — recorded so nobody reads this as convergence.** Both lanes again led on the same defects and both had opened real source, which is what makes the tail credible. Three where the draft asserted behaviour that does not exist: § 2.8 claimed the preamble sits outside the section model, but content above the first heading is filed into the **synthetic root** (empty slug, `level = 0`) which `listSections()` returns — so §§ 2.1–2.2 do reach it, and a level-0 section has no markdown heading form; the equality projection omitted `id_origin`, `provenance` and the `id_prefix` high-water, all of which the export emits, so INV-1's test could not have passed; and the third exclusion family was justified by the loader re-stamping `created`/`last_modified`, which **nothing does** — `putItem()` binds caller values, and neither migration TU touches either field. The exclusion was right and its stated reason was invented. Also fixed: § 2.8's legend and H1 had no reader (`readProject()` added — `roadmapexport.cpp` hand-rolls `SELECT … FROM project`, the same anti-pattern § 2.1 exists to end); the `detectRoadmapFormat()` guard was unsound, since that function answers `ants-v1` for input it does not recognise and `sawSignal` is what distinguishes them; the lib does not link `projectsettings.cpp`, so the live-roadmap path became a `RenderOptions` field rather than a `project.json` read the render cannot perform; INV-5 and INV-6 each contradicted the prose three sections away; and `writeElements()` was cited as a member when it is a free function in an anonymous namespace. Two collateral from loop 1's own fixes were caught in the post-fix sweep rather than by a lane: INV-8 still said "live roadmap" after § 2.8 extended the marker to archives, and INV-11's grep covered `FROM element` after `readProject()` had made `FROM project` equally load-bearing. |
| 1 | 2026-08-03 | 2 (single doc, cold; genre pinned `spec`) | 4 / 6 / 8 / 7 / 1 | 25 verified, all fixed. **Both lanes independently named the same three CRITICALs, and the first of them gutted the draft's central design.** § 2.6 proved fidelity by comparing a full export of the store against an export of the re-loaded render — unsatisfiable on any real store, because INV-4 deliberately excludes `internal` and `dropped` items, and `RoadmapMigrateLoad::load()` stamps history rows with a caller-supplied `changedAt`. The oracle now compares a **projection**, with the three excluded families enumerated. A second CRITICAL came at the same claim from the other side: the oracle proves losslessness and NOT § 3.5 conformance, because `roadmap-format.md` § 3.5.3 defaults an absent `Kind:` — so a render omitting it would round-trip clean, which is precisely the "silently missing a required piece" check `roadmap-data-model.md` § 9 assigns to this spec. INV-12 now asserts against the rendered text rather than a re-parse. Third: § 2.2 said `parent_id` drove the heading level while INV-9 said `level` did, so the two readings produced opposite implementations of the same line. Also fixed: **INV-11 was already false when written** (ten `FROM element` queries exist under `tests/features/`), now scoped to `src/`; `ElementRow` could not carry the export refit it promised, since `id_fold` is on `ItemRef` and not `ItemWrite`; the gate had three incompatible failure channels (`std::optional`, `gateFailures`, "exits non-zero"); § 2 never said where the format marker, H1, legend or section intro come from, so INV-8 asserted a marker nothing sourced (new § 2.8); `source_path` was written "verbatim" with no containment check, so a `../../` value escaped the project (new INV-13); and the RAM budget contradicted its own staging design, bounding peak by the largest file when all files are buffered at once. Two self-inflicted: § 5 froze one of the two discredited `layman` counts three sections after § 2.5 disowned both, and the exact byte count went stale during drafting because appending roadmap notes changed it. |
