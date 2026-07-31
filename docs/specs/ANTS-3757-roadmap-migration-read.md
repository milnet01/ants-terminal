# ANTS-3757 — Roadmap migration, read half: parsing the corpus into a plan

**Status:** spec draft (2026-07-31).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3757 (ANTS-3753 split, spec seam 2 of 3; read/load split 2026-07-31).
**Blocked by:** [ANTS-3764](../../ROADMAP.md) (the reader extraction), [ANTS-3756](ANTS-3756-roadmap-store-schema.md) (the field vocabulary a plan is written against).
**Blocker for:** ANTS-3765 (the load half), ANTS-3758 (publish + consumer cutover).
**Pairs with:** [ANTS-3761](ANTS-3761-roadmap-export-format.md) — its § 2.7 rebuild is this job's nearest sibling and deliberately not the same job: that reader consumes a file this project's own writer produced, this one consumes prose a human wrote.

**Layman:** The one-time job that reads every project's roadmap file and works
out exactly what should go into the database — without losing anything, and
without guessing.

---

## 1. Problem

The store ([ANTS-3756](ANTS-3756-roadmap-store-schema.md)) and the export with
its rebuild half ([ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.7) both
ship. Nothing populates them. `RoadmapExport::rebuildProject()` reads a file
`RoadmapExport::writeProject()` produced, so the ten hand-written markdown
roadmaps this machine actually tracks work in have no path into the store at
all.

Every figure below comes from one command, re-runnable:

```bash
tools/roadmap-corpus-survey.py        # run 2026-07-31 for the figures here
```

Three properties of the corpus make this more than a parse, and each drives a
decision in § 2:

1. **Three source formats, not one.** Roughly 3,950 items are emoji-bullet or
   GFM checkbox; a further **144** are `#### Pass N.M` headings, which carry no
   bullet and no status emoji, so every bullet-shaped rule is blind to them.
2. **About 40% of items carry no id** — some 1,600 of them, about 1,020 closed
   and 600 open. They cannot be filed under an id they do not have, and
   `roadmap-data-model.md` § 3.3 forbids rejecting them for it.
3. **About half the items carry no `Kind:` and half no `Source:`** (51% and 50%
   respectively; 56% carry no `Layman:`). A write-time-only reading of the
   model would refuse every project in the corpus.

`roadmap-data-model.md` § 9 hands one policy question here explicitly — how the
pass-headings status vocabulary normalises — on the stated grounds that
`deferred` and `partial` have no target in the five-status enum. § 2.5 answers
it, and the premise turns out to be false.

## 2. Surface

### 2.1 What this half produces

A pure function from markdown to a plan. New files `src/roadmapmigrate.{h,cpp}`
join **`ants_core_lib`** — `Qt6::Core` only, no `Qt6::Sql` — so the whole read
half is testable without a database. ANTS-3765 consumes the plan and owns every
write.

```cpp
namespace RoadmapMigrate {

// One item as migration will file it. Field names mirror
// RoadmapStore::ItemWrite (ANTS-3756) so the load half copies rather than
// translates; a translation step is a second place for the two to disagree.
struct PlannedItem {
    QString     id;
    QString     idOrigin;      // "parsed" | "synthesised" | "quarantined"
    QString     status;        // one of the five; never a source string
    QString     headline, kind, source, layman, body;
    QString     sectionSlug;
    int         position = 0;
    QJsonObject extras;        // source_status, source_kind — verbatim
    QJsonObject provenance;    // per field: asserted | defaulted | migrated
};

// Anything a human must see. Never a silent drop, never a stderr line:
// the report is a value, so a test can assert on it.
struct Note {
    QString code;              // § 2.9's closed set
    QString detail;
    int     line = 0;          // 1-based in the source file
};

struct MigrationPlan {
    QString              projectName, exportSlug, sourcePath, format;
    QVector<PlannedItem> items;
    QVector<Note>        notes;
};

// Pure: no filesystem, no clock, no id counter. `sourcePath` is recorded,
// never read.
MigrationPlan planFrom(const QString &markdown, const QString &sourcePath);

}  // namespace RoadmapMigrate
```

### 2.2 Discovery

Migration takes **explicit project roots**, never a glob over a parent
directory. A glob's membership changes when an unrelated directory appears
beside the projects, and a migration whose input set moves silently is one
nobody can re-run and compare.

Within a root the roadmap is the file whose name case-folds to `roadmap.md`.
This is not a nicety: RetroDB names its file `roadmap.md`, and an uppercase-only
glob excluded a 4,800-line project from the first ANTS-3753 survey, which then
reported both a corpus size and a "no project uses pass headings" claim that
were wrong — about the one project that uses them for 144 items.

Two names in one directory that differ only in case is **refused, not
resolved**. It is reachable on a case-sensitive filesystem, and either choice
silently discards a whole project's roadmap.

### 2.3 Format dispatch, and the reader this does not rewrite

The three formats are `roadmap-format.md` § 3.5 (emoji bullet), § 3.10.1 (GFM
task list) and § 3.10.5 (pass headings). The project already owns a complete,
shipped reader for all three — `detectRoadmapFormat()` and its per-format
parsers in `src/roadmapdialog.cpp` (ANTS-1530 / 2035 / 2039).

**This spec does not write a second one.** ANTS-3764 lifts the existing reader
into `ants_core_lib` and `planFrom()` calls it. The alternative is two parsers
whose disagreements would be silent and would be about the corpus itself — and
the project has already made this exact call once, in the other direction:
ANTS-2126 extracted the pass-headings *writer* to
`src/passheadingwrite.{h,cpp}` in `ants_core_lib` so "the remotecontrol handlers
and the feature test share one implementation".

### 2.4 What counts as an item

`roadmap-data-model.md` § 7.2 fixes the rule: a bullet is an item when it
carries **both** a status marker and the bold headline `roadmap-format.md` § 3.5
requires — or an id token in the same slot. Both halves are load-bearing, and
the corpus says by how much: sub-bullets and status-marked detail lines would
promote to items if the second half were dropped, **a ~37% over-count**.

Two rules the corpus forced that the model does not state:

- **A pass block's status is its FIRST `- **Status**:` line.** Measured: 144
  pass headings carry 154 Status lines — every heading has at least one, nine
  blocks have a second, and one Status line precedes the first heading
  entirely. First-match-wins is not a new convention; it is what
  `PassHeadingWrite::flipPassStatus()` already does ("rewrite its FIRST
  `- **Status**:` line", "First match wins").
- **A Status line belonging to no pass block is not an item.** The one such
  line in the corpus is reported (§ 2.9), not imported and not dropped.

Narration bullets, tables and fenced blocks are modelled as
`roadmap-data-model.md` § 5.2 already requires; this spec adds nothing to that
and does not restate it.

### 2.5 Identity: id-shaped, and the position that decides it

`roadmap-data-model.md` § 7.1 already requires an id be recognised **only at an
item's leading position**, and gives the slot per source shape. What it does not
say — and what the ANTS-3757 bullet asks this spec for — is what that rule
buys, because "id-shaped" cannot be decided by shape.

**It cannot, and the corpus proves it.** A detector matching the grammar
anywhere on the line finds 6 distinct id-shaped tokens across 4 projects, 29
occurrences in total. Of those, **3 are declarations and 26 are references**:

| Token | Occurrences | Declarations | What the rest are |
|---|---|---|---|
| `Cl9` | 14 | 1 (3D_Engine) | 13 references in Ants' own ROADMAP prose |
| `CE18` | 5 | 1 (3D_Engine) | 4 references, same |
| `Cl10` | 2 | 1 (3D_Engine) | 1 reference |
| `milnet01` | 4 | 0 | a GitHub username in link labels |
| `x86_64` | 2 | 0 | an architecture |
| `FOO123` | 2 | 0 | an example inside a bullet discussing this issue |

The **occurrence** column moves and the **declaration** column does not: every
time anyone writes about this problem — including the ROADMAP bullet that
commissioned this spec, and this table — the reference count goes up. That is
the argument for declarations being the unit, made by the measurement itself.

`[milnet01]` and `[x86_64]` are indistinguishable from an id by shape — a letter
prefix followed by digits is exactly what an id is. **Position is the whole
discriminator**, and it is sufficient: applied, it yields exactly the three
declarations, with zero false positives. `tools/roadmap-corpus-survey.py`
already implements it and already reports 3.

One gap position alone does not close. A markdown link can occupy the leading
slot — `- 📋 [Some Doc](path.md) — do the thing` — and `[CVE-2017-1000117]`
matches § 3.5.1's grammar exactly. So a token in the leading slot that is
**immediately followed by `(` or `:`** is a markdown link, not an id. Measured:
**0** such bullets today, against **43** bullets corpus-wide that open with a
markdown link one status marker away from the slot. The clause is cheap, and the
failure it prevents is filing a CVE number as an item id.

### 2.6 Quarantine, and when it clears

The three declared off-grammar ids are quarantined exactly as
`roadmap-data-model.md` § 7.1 requires: imported with the id **verbatim**,
`id_origin = 'quarantined'`, the project's migration completing rather than
blocking. Migration never invents a dash (rewriting an id breaks § 3.5.1's
append-only rule and every citation of it) and never treats them as id-less
(§ 7.2's bulk allocation would issue a second identity for an item that already
has one).

**It clears when the item stops being off-grammar in source, and by no other
route.** A re-run reads `[Cl9]` again and quarantines it again; a re-run that
reads `[CL-9]` files it as parsed. Nothing in the store or in this migration
promotes a quarantined id, because the two available decisions — amend
§ 3.5.1's grammar, or accept the id as opaque — both have consequences beyond
one project, which is why the model declines to pick one and why this spec does
not pick it either. The report names them on every run so the choice stays
visible rather than decaying into silence.

### 2.7 Status normalisation

**The bullet's framing — "no canonical target, so the choice is add statuses or
lose information" — is false, and twice over.** The corpus re-measurement in the
ROADMAP annotation killed the first half: of the 154 `- **Status**:` values,
122 are `done`. Shipped code kills the second: the reader's vocabulary has
always been **total**.

`src/roadmapdialog.cpp`'s pass reader maps, and
`PassHeadingWrite::passStatusKeyword()` is its exact inverse:

| Source word | Status | Inverse (`passStatusKeyword`) |
|---|---|---|
| `done`, `shipped`, `completed` | `shipped` | `shipped` → `done` |
| `in-progress`, `in_progress`, `inprogress`, `doing`, `wip` | `in-progress` | `in-progress` → `in-progress` |
| `deferred`, `considered`, `parked` | `considered` | `considered` → `deferred` |
| `todo`, `planned`, absent, **anything else** | `planned` | `planned` → `todo` |

Migration uses **this table and no other**. That `deferred` → `considered` is
load-bearing rather than arbitrary: mapping it to `planned` instead — the
reading a fresh author would reach for, and the one this spec's first draft
reached for — breaks the round trip, because a later flip through `roadmap_log`
writes `deferred` back for `considered` and the file would then re-read as a
different status than it was written from.

Applied to the corpus (first word of each value, 154 lines):

| Source word | Count | → | Reported? |
|---|---|---|---|
| `done` | 122 | `shipped` | no |
| `planned` | 15 | `planned` | no |
| `deferred` | 11 | `considered` | no |
| `partial` | 2 | `planned` | **yes** |
| `in-progress`, `considered`, `shipped` | 1 each | themselves | no |
| `un-gated` | 1 | `planned` | **yes** |

**The residual loss is that the else-branch is silent, and that is what this
spec fixes.** `partial (v3.6.20). Phase A landed` becoming `planned` discards
the fact that work started. So:

- Whenever normalisation was **not an identity**, the author's verbatim string
  goes to `extras.source_status` and `provenance.status = "migrated"`. Nothing
  is lost, the five-status enum does not grow, and ANTS-3756's `CHECK`
  constraint is untouched.
- Whenever a value reaches the **else-branch by a word the reader does not name
  explicitly** — i.e. not `todo`, `planned`, or absent — it is **reported**.
  Inheriting a GUI's display default silently is right for a dialog and wrong
  for a primary store: `partial` and `un-gated` today, and whatever a project
  invents next.

Migration never refuses on a status. The vocabulary is total by construction, so
a refusal path here would be unreachable code guarding an impossible state.

### 2.8 Kind, and the fields the corpus does not carry

`roadmap-data-model.md` § 7.4's mapping table is **normative** and generated
from the same survey; this spec applies it rather than restating it. Absent
`Kind:` defaults to `implement` and absent `Source:` to `planned` per § 3.3,
each recording `provenance.<field> = "defaulted"`. `layman`, `priority`,
`resolution` and any non-derivable date have no default and are left empty.

A non-canonical value that is **not** in § 7.4's table is reported and defaulted
— not refused. The table was generated from a corpus that grows, so an unknown
value is the expected consequence of a project joining later, and § 7.4 already
says such a project "re-runs it and extends the table by amendment".

### 2.9 Id allocation

Policy is fixed by `roadmap-data-model.md` § 7.2; this spec executes it and adds
the one thing a plan needs that a policy does not — that **allocation is not
this half's job to perform**. `planFrom()` is pure and an id counter is shared
mutable state, so the plan marks each id-less item with the obligation and
ANTS-3765 allocates inside the same transaction as the write.

| | Corpus | Rule |
|---|---|---|
| Closed | 1,021 | Bulk, in document order. Nobody cites a finished item. |
| Open | 599 | Into the project's normal sequence; § 3.2's publish gate then applies. |

Both carry `provenance.id = "migrated"`; they differ in what is owed afterwards,
not in where the id came from. The standard's "~1,600 id-less, ~1,020 closed /
~600 open" is **confirmed** by the 2026-07-31 run, not — as the ANTS-3757
bullet's annotation suspected — a transposition of Ants' own id-bearing count.
The rival figures in that annotation (2,818 / 3,794) counted *lines*, a
different population.

Pass-heading items are **not** among the id-less: their ids are synthesised from
the heading. Migration calls
`PassHeadingWrite::passIdFromDesignator()` rather than reimplementing the
derivation, because the reader, the writer and now migration must agree on
`PASS-43-5-B` or a flip through `roadmap_log` stops finding the item it wrote.
They carry `id_origin = "synthesised"`.

### 2.10 The report

`MigrationPlan::notes` is a value, not a log line, so tests assert on it. Codes
are a closed set — an open one becomes prose nobody can grep:

| Code | Raised when |
|---|---|
| `quarantined_id` | § 2.6 — a declared id outside § 3.5.1's grammar. |
| `status_defaulted` | § 2.7 — an else-branch word the reader does not name. |
| `kind_unmapped` | § 2.8 — non-canonical and absent from § 7.4's table. |
| `orphan_status_line` | § 2.4 — a `- **Status**:` line belonging to no pass block. |
| `id_allocation_owed` | § 2.9 — an id-less item, carrying open/closed. |
| `ambiguous_roadmap` | § 2.2 — two filenames differing only in case. |

## 3. Invariants

- **INV-1** — Roadmap discovery case-folds the filename, and refuses two names in one root that differ only in case. *Test:* `tests/features/roadmap_migrate_read/` — fixture roots holding `ROADMAP.md` only, `roadmap.md` only, and both; the first two resolve, the third returns an error naming both. *Breaks when:* discovery globs `ROADMAP.md` — which silently drops RetroDB, the only project in the corpus using the pass-headings format, and with it all 144 of its items.
- **INV-2** — A bullet becomes an item only with **both** a status marker and an id-or-bold-headline in the leading slot. *Test:* `roadmap_migrate_read` asserts the per-project item count equals `tools/roadmap-corpus-survey.py`'s for the same file, over committed fixture roadmaps in all three formats. *Breaks when:* the bold-headline half is dropped — the corpus's sub-bullets and status-marked detail lines promote to items, a ~37% over-count. Parity against an independently-written parser is the point: a test written from this spec's own rules can only confirm what the rules say.
- **INV-3** — An id-shaped token is an id **only** in the leading slot, and never when immediately followed by `(` or `:`. *Test:* `roadmap_migrate_read` — a fixture carrying `[milnet01]`, `[x86_64]` and `[FOO123]` in prose, `[CVE-2017-1000117]` as a link label in the leading slot, and `[Cl9]` in the leading slot; only `[Cl9]` becomes an item. *Breaks when:* the detector matches the grammar anywhere on the line — measured over the corpus that admits 6 tokens across 4 projects, 26 of whose 29 occurrences are references, not declarations.
- **INV-4** — A declared off-grammar id is imported **verbatim** with `id_origin = "quarantined"`, and no run ever rewrites it or allocates it a second id. *Test:* `roadmap_migrate_read` plans a `[Cl9]` fixture twice and asserts the id is byte-identical both times, `id_origin` is `quarantined`, and no `id_allocation_owed` note is raised for it. *Breaks when:* quarantine is implemented as "repair to `CL-9`", which breaks § 3.5.1's append-only rule; or as "treat as id-less", which issues a second identity for an item that already has one.
- **INV-5** — Status normalisation is exactly the shipped reader's table, and `deferred` maps to `considered`. *Test:* `roadmap_migrate_read` drives every source word in § 2.7's first table through `planFrom()`, and asserts round-trip closure against `PassHeadingWrite::passStatusKeyword()` for the four statuses it covers. *Breaks when:* migration introduces its own mapping — `deferred` → `planned` is the natural one and it breaks the round trip, since a later `roadmap_log` flip writes `deferred` back for `considered`.
- **INV-6** — Every status value reaching the else-branch by a word the reader does not name explicitly raises `status_defaulted`. *Test:* `roadmap_migrate_read` asserts `partial` and `un-gated` each raise it and that `todo`, `planned` and an absent Status line do not. *Breaks when:* the else-branch is inherited wholesale — every invariant here still passes, the statuses are all still legal, and the store quietly gains items whose `planned` is a guess indistinguishable from an author's.
- **INV-7** — Whenever normalisation was not an identity, `extras.source_status` holds the author's verbatim string and `provenance.status = "migrated"`. *Test:* `roadmap_migrate_read` asserts the exact source string survives for a value with a qualifier tail (`done (v3.20.0, 2026-07-05). Adds catalogs for …`), and that a bare `planned` sets neither. *Breaks when:* the normaliser writes back its own normalised word, or stores the first token rather than the line — either of which passes INV-5 while discarding the qualifier that made preservation worth doing.
- **INV-8** — Absent `Kind:` / `Source:` default per § 3.3 with `provenance` recording `defaulted`, and a non-canonical value absent from § 7.4's table raises `kind_unmapped` rather than refusing. *Test:* `roadmap_migrate_read` — a fixture item with no `Kind:` plans as `implement`/`defaulted`; one with `bugfix` plans as `fix`; one with `frobnicate` plans as `implement` and raises the note. *Breaks when:* migration requires `Kind:` at write time — which refuses roughly half of every project in the corpus.
- **INV-9** — `planFrom()` is pure: identical input yields an identical plan, and it touches no filesystem, clock or id counter. *Test:* `roadmap_migrate_read` calls it twice on the same fixture and compares field-wise, including `provenance` and note order. *Breaks when:* allocation is done here rather than deferred to ANTS-3765 — a counter read makes the second call differ from the first, which is the cheapest possible detector for exactly the mistake § 2.9 exists to prevent.
- **INV-10** — Pass ids come from `PassHeadingWrite::passIdFromDesignator()`, and every pass block yields exactly one item taking its status from the block's **first** `- **Status**:` line. *Test:* `roadmap_migrate_read` — a fixture with `Pass 43.5`, `Pass 43.5.B` and a block carrying two Status lines; asserts ids `PASS-43-5` / `PASS-43-5-B` and that the second Status line is ignored. *Breaks when:* the derivation is reimplemented — `Pass 41.5` and `Pass 41.5.B` collapsing to one id is the regression ANTS-2035 already fixed once, in the reader.
- **INV-11** — No source line is silently discarded: every item, narration bullet, table row and fenced block in the source is either in the plan or named by a note. *Test:* `roadmap_migrate_read` asserts, over the committed fixtures, that the count of non-blank source lines attributed to some plan element or note equals the file's. *Breaks when:* a parser branch falls through — the failure mode migration exists to prevent, and the only invariant here that fires on a format nobody anticipated.

## 4. RAM / build cost

`planFrom()` holds one project's plan in memory. The largest roadmap in the
corpus is Ants' own at 2.9 MB / ~1,790 items (`wc -c ROADMAP.md`); a plan carries
the item bodies, so budget **under 3× the source file** — under 10 MB for the
worst project, and never the whole corpus at once, since ANTS-3765 loads one
project per transaction and discards each plan after it commits. That per-project
bound is the eviction policy; there is no cache and nothing accumulates across
projects.

No new external dependency. `src/roadmapmigrate.{h,cpp}` joins the existing
`ants_core_lib` (`Qt6::Core`), and the test joins the existing `test_core`
bundle rather than adding a target. ANTS-3764 moves source between two libraries
already in the graph.

## 5. Out of scope

- **Writing anything to the store** — ANTS-3765. That includes per-project
  atomicity, the `Access::Bulk` connection, re-run matching, items deleted from
  source, and the cutover interim.
- **Extracting the reader** — ANTS-3764, this spec's blocker.
- **The published render, and the fate of `roadmap_query` / `roadmap_log` /
  `RoadmapDialog`** — ANTS-3758.
- **Deciding whether `[Cl9]`'s shape becomes legal.** A permanent exclusion, not
  deferred work: it is an amendment to `roadmap-format.md` § 3.5.1 affecting
  every project, and § 2.6 makes migration correct under either outcome.
- **Recovering dates from git history.** `roadmap-data-model.md` § 4.2 already
  bounds what is derivable; nothing here needs a date the source does not carry.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_read/`, covering INV-1..11, added
to the existing `test_core` bundle's `SOURCES` (never `add_executable` — see
`tests/features/README.md`). Label `features;fast`; no database, no network.

Fixtures are **committed roadmaps, not the machine's corpus**, which is not
present in CI — one per format, plus a pathological fixture carrying the
reference-vs-declaration tokens, the two-Status-line pass block, the orphan
Status line, and the markdown-link-in-leading-slot case.

INV-2's survey-parity leg is the exception: it runs `roadmap-corpus-survey.py`
against those same committed fixtures, so the cross-check is against an
independently written parser rather than against this spec's own rules.

Per the project convention, each invariant must be shown **RED under the exact
mutation its own *Breaks when* clause names** before it is accepted green. Where
a named break does not redden, that is a finding about the invariant and is
recorded rather than dropped — ANTS-3761's row 6-impl found one such clause that
was self-refuting.

## 7. Cross-doc impact

- `CLAUDE.md` — module map gains `src/roadmapmigrate.{h,cpp}`; ANTS-3764 moves
  the reader entry in the same release.
- `docs/standards/roadmap-data-model.md` — § 9's open policy question ("how the
  pass-headings status vocabulary normalises") is answered by § 2.7 and should
  point here. Its premise, that `deferred` and `partial` have no target, is
  falsified by the shipped reader and needs correcting in place.
- `docs/subsystems.md` — the roadmap lane gains the migration files.
- `CHANGELOG.md` — on ship.

## 8. Open questions

- **Whether `partial` deserves `in-progress` rather than the else-branch's
  `planned`.** It is more truthful — the two corpus items say "Phase A landed" —
  but it diverges from the shipped reader, and § 2.3's whole argument is one
  reader. § 2.7 keeps the reader's answer and reports the case; promoting it is
  then a curation edit that sets `provenance.status = asserted`, which is the
  mechanism the model already has for exactly this.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-split | 2026-07-31 | none — no reviewer dispatched | — | **Provenance row, not a review.** Split from ANTS-3757, which owned parse + normalise + allocate + load + cutover in one document — the shape ANTS-3756 carried when it converged by cap at 3 loops and had to split into ANTS-3761. This id keeps the read half; ANTS-3765 takes the load half. The seam is pure transformation vs. transactional load, and both halves are independently testable, which is the evidence it is a seam rather than a filing convenience. Invariants are numbered from 1: this is a scope reduction of a spec that was never written, not a split of a reviewed document, so there is nothing to avoid reflowing. **No review has been run on this document** — the gate starts at loop 1 on these bytes. |
