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

**Section-reference convention.** A bare `§ N` is **this document's** section.
Every reference to another document names that document — `roadmap-data-model.md
§ 7.2`, `roadmap-format.md § 3.5.1`. Both standards number their sections in the
same ranges this spec does, so an unqualified cross-doc reference resolves to
the wrong document on a first read.

**Contents.** 1 Problem (1.1 the corpus) · 2 Surface (2.1 the plan · 2.1.1 field
disposition · 2.2 discovery · 2.3 format dispatch and the reader · 2.4 what
counts as an item · 2.5 identity · 2.6 quarantine · 2.7 status · 2.8 kind ·
2.9 id allocation · 2.10 the report · 2.11 the structural walk) · 3 Invariants
(INV-1..13) · 4 RAM / build cost · 5 Out of scope · 6 Tests · 7 Cross-doc
impact · 8 Open questions · Cold-eyes loop log.

---

## 1. Problem

The store ([ANTS-3756](ANTS-3756-roadmap-store-schema.md)) and the export with
its rebuild half ([ANTS-3761](ANTS-3761-roadmap-export-format.md) § 2.7) both
ship. Nothing populates them. `RoadmapExport::rebuildProject()` reads a file
`RoadmapExport::writeProject()` produced, so the ten hand-written markdown
roadmaps this machine actually tracks work in have no path into the store at
all.

### 1.1 The corpus, and the one place its figures live

Every survey figure in this spec comes from one command, and **this section is
the only place any of them is quoted.** Later sections refer to a population by
name, never by number: the totals move whenever anyone edits a roadmap — the
item count read 3,955, then 3,956, then 3,957 across three runs in a single
day, and each drift was previously a finding against a section that had copied
the figure. A measurement that is **not** the survey's — § 5's archive count,
§ 2.5's declaration table, § 2.7's Status-word breakdown — states its own
provenance where it is made and is the single home for that figure.

```bash
tools/roadmap-corpus-survey.py     # run from /mnt/Games/Scripts/Linux
```

| Population | 2026-07-31 run | Referred to below as |
|---|---|---|
| Projects | 10 | — |
| Bullet-form + checkbox items | 3,957 | *the bullet corpus* |
| `#### Pass N.M` headings | 144 | *the pass corpus* |
| `- **Status**:` lines | 154 | — |
| …whose value is outside `roadmap-data-model.md` § 7.3's enum | 136 | — |
| Items with an id matching `roadmap-format.md` § 3.5.1 | 2,334 | — |
| Items with an id that does **not** match it | 3 | *the off-grammar ids* |
| Items with no id | 1,620 (1,021 closed / 599 open) | *the id-less* |
| No `Kind:` / no `Source:` / no `Layman:` | 51% / 50% / 56% | — |
| Sub-bullets | 1,376 | — |
| Status-marked detail lines | 88 | *the detail lines* |
| Status-legend lines | 8 | *the legend lines* |
| Table data rows | 166 (+16 separator rows, not rows) | — |
| Fenced code blocks | 20 | — |

Three properties of that corpus make this more than a parse, and each drives a
decision in § 2:

1. **Three source formats, not one.** The bullet corpus is emoji-bullet or GFM
   checkbox; the pass corpus carries no bullet and no status emoji, so every
   bullet-shaped rule is blind to it.
2. **Roughly 40% of items carry no id.** They cannot be filed under an id they
   do not have, and `roadmap-data-model.md` § 3.3 forbids rejecting them for it.
3. **About half the items carry no `Kind:` and half no `Source:`.** A
   write-time-only reading of the model would refuse every project in the
   corpus.

`roadmap-data-model.md` § 9 hands one policy question here explicitly — how the
pass-headings status vocabulary normalises — on the stated grounds that
`deferred` and `partial` have no target in the five-status enum. § 2.7 answers
it, and the premise turns out to be false.

## 2. Surface

### 2.1 The plan, and the single statement of its shape

**The declarations below are the contract.** Every later section states a
*decision* — why a rule is what it is, and what in the corpus forced it — and
refers to these types rather than restating them. § 3's invariants do the same.
Where the two could disagree, the declaration wins.

**Two functions, and the split between them is the impurity.** Discovery
touches the filesystem; the transformation does not. New files
`src/roadmapmigrate.{h,cpp}` join **`ants_core_lib`** — `Qt6::Core` only, no
`Qt6::Sql` — so the whole read half is testable without a database. ANTS-3765
consumes the plan and owns every write.

```cpp
namespace RoadmapMigrate {

// One item as migration will file it. Field names follow
// RoadmapStore::ItemWrite (ANTS-3756); § 2.1.1 accounts for every ItemWrite
// field and every `item` column, including the ones left empty here.
struct PlannedItem {
    QString     id;
    QString     idOrigin;      // "parsed" | "synthesised" | "quarantined"
    QString     status;        // one of the store's five; never a source string
    QString     headline, kind, source, layman, body;
    QStringList lanes, evidence;   // `Lanes:` / `Evidence:` — the reader already
                                   // parses both, and `item` has a column for each
    QString     sectionSlug;   // the section this is filed under; § 2.1.1 on sectionId
    int         position = 0;  // § 2.11 — the ONE per-section sequence, shared
                               // with that section's PlannedElements
    QJsonObject extras;        // source_status, source_kind — verbatim (§ 2.7, § 2.8)
    QJsonObject provenance;    // per field: asserted | defaulted | migrated (roadmap-data-model.md § 7.7)
    // § 2.9 — the allocation obligation rides on the ITEM, not only on a note:
    // ANTS-3765 allocates, and a note keyed on a line number cannot be
    // correlated back to the item it belongs to.
    bool        idAllocationOwed = false;
    bool        closed = false;         // roadmap-data-model.md § 3.4's sense: shipped or dropped
    int         firstLine = 0, lastLine = 0;   // 1-based, inclusive
};

// A `##` / `###` heading. `section` is a table of its own in ANTS-3756, not an
// element kind, so a heading line is carried here and nowhere else — without
// this the plan can name a `sectionSlug` it gives ANTS-3765 no way to create,
// and INV-11 has no carrier for the heading line itself. Content preceding the
// first heading belongs to a synthetic section with an empty slug and title,
// which the store already accepts. § 2.11 owns detection, `parentSlug`, slug
// uniquing and the intro boundary.
struct PlannedSection {
    QString slug, title;
    QString intro;                 // § 2.11 — prose and fences between the
                                   // heading and the section's first element
    int     level = 0;             // 2 or 3; 0 for the synthetic root
    QString parentSlug;            // "" at top level
    // Spans the HEADING LINE AND ITS INTRO ONLY, never the section's items and
    // elements — those carry their own spans and INV-11 is a partition.
    int     firstLine = 0, lastLine = 0;
};

// Everything inside a section that is NOT an item and NOT the intro. `kind` is
// the store's own element vocabulary minus `item`, and nothing else:
// `element.kind` CHECKs exactly ('item','narration','table').
//
// roadmap-data-model.md § 5 is explicit that a fenced block is NOT a section
// element — "fenced code blocks belong to a `body` or an `intro`. Neither is a
// section element" — so a fence inside an item's span is that item's `body`
// and a fence above the first element is the section's `intro`. `narration` is
// reserved for what roadmap-data-model.md § 5.2 calls section-summary prose: text at section level,
// after the first element. The `<!-- ants-roadmap-format: 1 -->` marker and any
// other stray line are narration too, since the store has no other bucket and
// INV-11 forbids dropping them.
struct PlannedElement {
    QString kind;              // "narration" | "table"
    QString payload;           // narration: verbatim source text.
                               // table: § 2.11's {header, rows} JSON — the
                               // separator row is delimiter, not content
                               // (roadmap-data-model.md § 5.2), so it is never stored.
    QString sectionSlug;
    // § 2.11 — ONE 0-based sequence per section shared with that section's
    // items, because `element` CHECKs UNIQUE (section_id, position) across
    // item and non-item rows alike. Numbering the two independently produces a
    // plan that dies on ANTS-3765's insert.
    int     position = 0;
    int     firstLine = 0, lastLine = 0;
};

// The roadmap-data-model.md § 5.1 status legend, which belongs to the PROJECT and not to any section.
// Two of the ten projects carry one. Without a carrier the legend lines are
// either lost or — if § 2.4's headline half were dropped — promoted to items.
// § 2.11 owns recognition and how the lines become `entries`.
struct PlannedLegend {
    QJsonObject entries;       // status value -> that project's wording
    int         firstLine = 0, lastLine = 0;
};

// Anything a human must see. Never a silent drop, never a stderr line:
// the report is a value, so a test can assert on it.
struct Note {
    QString code;              // § 2.10's closed set
    QString detail;
    int     line = 0;          // 1-based in the source file; 0 = whole-file
};

struct MigrationPlan {
    QString                 projectName, exportSlug, sourcePath;
    QString                 format;    // detectRoadmapFormat()'s own vocabulary:
                                       // "ants-v1" | "github-task-list" | "pass-headings"
    QVector<PlannedSection> sections;
    QVector<PlannedItem>    items;
    QVector<PlannedElement> elements;
    std::optional<PlannedLegend> legend;
    QVector<Note>           notes;
};

// The IMPURE half — the only function here that touches the filesystem, and
// the only one that reads. Resolves the roadmap under `projectRoot`, decodes
// it, and returns both; nullopt with `error` set on any § 2.2 refusal.
struct Source { QString path, markdown; };
std::optional<Source> findRoadmap(const QString &projectRoot, QString *error);

// The PURE half: no filesystem, no clock, no id counter (INV-9).
// `projectName` and `exportSlug` are supplied by the caller, not derived —
// `exportSlug` is ANTS-3756's `project.export_slug`, whose charset the store
// constrains and which nothing in a markdown file carries. `sourcePath` is
// recorded into the plan and never read.
MigrationPlan planFrom(const QString &markdown, const QString &sourcePath,
                       const QString &projectName, const QString &exportSlug);

}  // namespace RoadmapMigrate
```

#### 2.1.1 Where every store field comes from

The table is exhaustive over `RoadmapStore::ItemWrite` and the `item` DDL, so
"left empty" is a stated decision rather than an omission a reader has to
notice. Rows marked **owed** are ANTS-3765's to add before it can accept a
plan; § 7 records that obligation.

| Store field | Source in this half |
|---|---|
| `projectId` | not knowable here — a plan names a project, it does not create one. **Owed**: ANTS-3765 inserts the `project` row (from `projectName` / `exportSlug`, and the root it was given) and fills this in |
| `id`, `idOrigin` | § 2.5 / § 2.6, or synthesised (§ 2.9) |
| `status` | § 2.7 |
| `headline`, `body` | the reader's `headline` / `body` |
| `kind`, `source` | § 2.8, defaulted when absent |
| `layman` | the `Layman:` line; empty when absent, no default |
| `provenance` | per field, § 2.7 / § 2.8 / § 2.9 |
| `position` | ordinal within the section's element list |
| `visibility` | not set — the store's `public` default stands |
| `priority`, `resolution` | left empty; `roadmap-data-model.md` § 4.1 makes each write- or close-time, and no source shape carries them |
| `milestone` | left empty; `roadmap-data-model.md` § 4.1 marks it `optional` and no source shape carries it — an absent optional, not a deferred obligation |
| `created`, `lastModified`, `shipped` | left empty; `roadmap-data-model.md` § 4.2 bounds what is derivable and § 5 excludes recovering dates from git |
| `lanes`, `evidence` | the `Lanes:` / `Evidence:` lines — **owed** (ANTS-3767) |
| `extras` | § 2.7 / § 2.8 — **owed** (ANTS-3767) |
| `sectionId` | not knowable here: a plan carries `sectionSlug`, the store row not existing yet. **Owed** — ANTS-3765 resolves slug to id inside its transaction |

Until those land, "the load half copies rather than translates" is true of every
field except them.

### 2.2 Discovery

Migration takes **explicit project roots**, never a glob over a parent
directory. A glob's membership changes when an unrelated directory appears
beside the projects, and a migration whose input set moves silently is one
nobody can re-run and compare.

Within a root the roadmap is the file **directly in that directory**, not
recursively, whose name case-folds to `roadmap.md` — so a `docs/ROADMAP.md` is
not a candidate and cannot silently outrank the real one.
This is not a nicety: RetroDB names its file `roadmap.md`, and an uppercase-only
glob excluded a 4,800-line project from the first ANTS-3753 survey, which then
reported both a corpus size and a "no project uses pass headings" claim that
were wrong — about the one project that owns the entire pass corpus.

Three refusals, each an **error return and never a plan note**: a root with no
roadmap; two names differing only in case (reachable on a case-sensitive
filesystem, and either choice silently discards a whole project's roadmap); and
a file that is not valid UTF-8. The last is a refusal rather than a lossy decode
because substituting U+FFFD would leave the line carried, the byte count intact
and INV-11 green over content that had already been corrupted — a fixable file
is worth naming.

A root that will not resolve produces no markdown, so there is no plan for a
note to ride on. That is why § 2.10's codes are the transformation's alone.

### 2.3 Format dispatch, and the reader this does not rewrite

The three formats are `roadmap-format.md` § 3.5 (emoji bullet), § 3.10.1 (GFM
task list) and § 3.10.5 (pass headings). The project already owns a complete,
shipped reader for all three — `detectRoadmapFormat()`, `parsePassHeadingBullets()`
and the GFM-adapter branch of `RoadmapDialog::parseBullets()`, in
`src/roadmapdialog.cpp` (ANTS-1530 / 2035 / 2039).

**This spec does not write a second one.** ANTS-3764 lifts that reader into
`ants_core_lib` and `planFrom()` calls it. The alternative is two parsers whose
disagreements would be silent and would be about the corpus itself — and the
project has already made this exact call once, in the other direction:
ANTS-2126 extracted the pass-headings *writer* to `src/passheadingwrite.{h,cpp}`
in `ants_core_lib` so "the remotecontrol handlers and the feature test share one
implementation".

**The shipped reader is a BULLET parser, and the seam is exactly there.** It
answers one question — *given these lines, what bullets are items and what are
their fields* — because that is all `RoadmapDialog` ever needed. It does not
walk the document: it emits nothing for narration, tables, fences, section
bodies or the legend, and it records no line numbers for anything. So this half
is **two jobs against one reader**, and saying which is which is what keeps
§ 2.3's one-parser rule honest:

| Job | Owner |
|---|---|
| Classify a bullet, extract its fields, synthesise a pass id | the shipped reader, via ANTS-3764 |
| Walk the document once for structure — headings, non-bullet lines, table and fence extents, the legend block, and a line span for every carrier | **`planFrom()`'s own scan (§ 2.11)** |

That second job is not a second parser of the *bullet* grammar, which is the
thing § 2.3 forbids and the thing whose disagreements would be silent. It is
the structural layer the reader never had, it never re-decides item-hood or a
status, and § 2.11 specifies it.

**ANTS-3764 must widen `BulletRecord`, and the list is longer than an earlier
draft of this section claimed.** Verified field-by-field against
`src/roadmapdialog.h` and the reader body; each entry is something § 2 needs
that the record does not carry, and every one of them would otherwise be
re-derived by hand from `body` — which *is* the forbidden second bullet parser:

| Field | Why | Verified absence |
|---|---|---|
| the verbatim `- **Status**:` **value** | § 2.7 / INV-7 | `status` is an emoji; the pass regex captures only the first `[A-Za-z0-9_-]` run, lowercases it, and discards it after mapping |
| `source` | § 2.1.1 and § 2.8 read a `Source:` line | the record has `kind`, `lanes`, `evidence`, `layman` — and **no `source`**; nothing in the reader extracts it |
| `firstLine`, `lastLine` | every § 2.1 carrier declares them; INV-11's partition is built on them | the record's only `int` is `sectionLevel` |
| the pass **designator** (`"43.5"`) | § 2.9 calls `passIdFromDesignator()` | the reader has already synthesised `rec.id` from the heading; the designator is consumed and dropped, so the call has no available input (§ 2.9 resolves this without the field — see there) |
| the **id token as written**, before the reader's own acceptance test | § 2.5's dash-optional grammar, § 2.6's quarantine | `id` is **positionless** — it takes the first `[<PREFIX>-NNNN]` found anywhere in the body — and it **conflates** a conforming id with an off-grammar one, since ANTS-1987's leading-bracket rule fills it from both |

**The last row changes an outcome rather than an implementation, and building
it corrected *which* outcome** (2026-07-31, ANTS-3764; the loop log carries the
row). An earlier statement of this row said `[Cl9]` "reaches migration as *no id
at all*". It does not, and § 3's INV-3 has always said otherwise: ANTS-1987
added a leading-bracket rule to the reader for exactly that shape, so `rec.id`
is `Cl9` and § 2.6 could key on it unaided. Three measurements keep the row —
two properties of `id`, and the token the old claim was really describing:

- **`id` is positionless.** A bullet whose leading slot reads `[Cl9]` while its
  prose cites `[ANTS-9999]` reports the **citation** — measured in
  `tests/features/roadmap_parse_widening/`, not inferred. Quarantining that
  value files the wrong id under the right item, which is worse than not
  quarantining at all.
- **`id` cannot separate `id_origin = "parsed"` from `"quarantined"`**, because
  ANTS-1987 fills it from both shapes.
- **`[ANTS-119&]` is the shape the old sentence described, and it is real.**
  Seven bullets of this project's own `ROADMAP.md` carry it; the `&` is refused
  by the strict body-wide matcher *and* by ANTS-1987's rule alike, so those
  items reach migration **id-less** and `roadmap-data-model.md` § 7.2 would
  issue each a second identity for an item that visibly carries one. For them
  the original sentence holds exactly: without the raw token § 2.6 is
  unreachable and INV-4 cannot pass.

All five are additive — no existing caller reads a field that does not yet
exist — so `RoadmapDialog` and `roadmap_log` are unaffected, and § 7 carries
the list.

**A file that yields no items is reported, not accepted quietly.**
`detectRoadmapFormat()` answers `ants-v1` for input it does not recognise —
including an empty file — so "a format was detected" is no evidence that
anything was understood. `empty_source` therefore turns on **items**, not on the
whole plan: a prose-only file legitimately yields narration elements, and a
condition reading "zero items *and* zero elements" would be unreachable for
exactly the file a discovery or parse regression produces. Zero items is the
signal, and a roadmap with none is worth a human's attention whatever else it
holds.

### 2.4 What counts as an item

`roadmap-data-model.md` § 7.2 fixes the rule: a bullet is an item when it
carries **both** a status marker and the bold headline `roadmap-format.md` § 3.5
requires. This spec refines the second half — **an id token in the leading slot
satisfies it in place of the bold headline**, since `roadmap-data-model.md` § 7.1 puts the id there and
the corpus writes `- ✅ [ANTS-1234] **…**` with both. That refinement is this
spec's, not `roadmap-data-model.md` § 7.2's, which states only the conjunction.

Both halves are load-bearing, and the corpus says which population each holds
back. Dropping the **headline** half promotes the detail lines and the legend
lines — status-marked bullets carrying neither an id nor a bold headline, a
low-single-digit percentage of the bullet corpus (§ 1.1 carries both counts),
which corroborates `roadmap-data-model.md` § 7.2's own "roughly 90
status-marked bullets". Dropping the **marker** half instead is the larger error,
since the corpus holds far more markerless sub-bullets — but those carry no
marker, so they are not what the headline rule is holding back, and an earlier
draft of this section wrongly summed the two populations.

Two rules the corpus forced that the model does not state:

- **A pass block's status is the FIRST `- **Status**:` line that classifies.**
  Not merely the first such line: the shipped reader scans forward from the
  heading, bounded by the next heading of level ≤ 4 or 50 lines, and a
  content-free `- **Status**:` line (no emoji and no keyword) does not stop the
  scan. First-match-wins is not a new convention — `PassHeadingWrite::
  flipPassStatus()` already works that way ("rewrite its FIRST `- **Status**:`
  line", "First match wins") — but the two are **not the same rule** and citing
  the writer as precedent for the reader would be wrong: the writer rewrites the
  first such line whatever it holds, the reader takes the first that
  *classifies*. On a block whose first Status line is content-free — INV-10's
  fixture — they pick different lines. Migration takes the **reader's** answer,
  including its 50-line bound, because the reader is what decided the status
  this plan is filing.

  § 1.1 records more Status lines than pass headings, and the command below is
  the single home for how that gap decomposes — every heading has at least one,
  nine blocks carry a second, and one Status line precedes the first heading
  entirely (measured 2026-07-31; the five numbers it prints are, in order,
  144 · 154 · 0 · 1 · 9):

  ```bash
  # run from /mnt/Games/Scripts/Linux — prints: headings, Status lines,
  # headings with none, lines before the first heading, extra lines in a block.
  # The Status pattern is the shipped reader's own (roadmapdialog.cpp
  # rxStatusLine): `[-*]`, optional space before `**`, case-insensitive.
  python3 - RetroDB/roadmap.md <<'EOF'
  import re,sys
  L=open(sys.argv[1],encoding='utf-8',errors='replace').read().split('\n')
  H=[i for i,l in enumerate(L) if re.match(r'^#### Pass \d+\.\d+',l)]
  S=[i for i,l in enumerate(L) if re.match(r'^\s*[-*]\s*\*\*Status\*\*\s*:',l,re.I)]
  B=[(H[k], H[k+1] if k+1<len(H) else len(L)) for k in range(len(H))]
  n=[sum(1 for s in S if a<s<b) for a,b in B]
  print(len(H), len(S), n.count(0), sum(1 for s in S if s<H[0]), sum(x-1 for x in n if x>1))
  EOF
  ```

- **A Status line belonging to no pass block is not an item.** The one such
  line in the corpus is reported (`orphan_status_line`, § 2.10), not imported
  and not dropped.

Narration bullets, tables, fenced blocks, sub-bullets, detail lines and the
legend are modelled as `roadmap-data-model.md` § 5.2 already requires; § 2.1's
types carry one field per home that section names, and this spec adds nothing
to it.

### 2.5 Identity: id-shaped, and the position that decides it

`roadmap-data-model.md` § 7.1 already requires an id be recognised **only at an
item's leading position**, and gives the slot per source shape. What it does not
say — and what the ANTS-3757 bullet asks this spec for — is what that rule
buys, because "id-shaped" cannot be decided by shape.

**"Id-shaped" is a second, deliberately looser grammar — and saying which one is
meant is the whole point, because the two differ on exactly the tokens that
matter.** § 3.5.1's grammar requires a dash before the digits
(`…[A-Za-z0-9_-]*-\d+`); the id-shaped detector makes that dash **optional**:

```
§ 3.5.1 (acceptance)  \[(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+\]
id-shaped (detection) \[(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*?-?\d+\]
```

`[Cl9]` matches the second and not the first — which is precisely what makes it
off-grammar in § 2.6, and why a detector built on § 3.5.1 alone would not see it
to quarantine it. `tools/roadmap-corpus-survey.py` already carries both, as
`ID_DASHED` and `ID_ANY`.

**Neither grammar can decide the question on its own, and the corpus proves it.**
Running the id-shaped detector anywhere on the line finds 6 distinct tokens
across 4 projects. Of those, 3 are declarations and the rest are references:

| Token | Declarations | What the rest are |
|---|---|---|
| `Cl9` | 1 (3D_Engine) | references in Ants' own ROADMAP prose |
| `CE18` | 1 (3D_Engine) | references, same |
| `Cl10` | 1 (3D_Engine) | a reference |
| `milnet01` | 0 | a GitHub username in link labels |
| `x86_64` | 0 | an architecture |
| `FOO123` | 0 | an example inside a bullet discussing this issue |

The **occurrence** count moves and the **declaration** count does not: every
time anyone writes about this problem — including the ROADMAP bullet that
commissioned this spec, and this table — the reference count goes up. That is
the argument for declarations being the unit, made by the measurement itself,
and it is why the occurrence figures are not quoted here (§ 1.1's rule).

Under the id-shaped grammar `[milnet01]` and `[x86_64]` are indistinguishable
from an id — a letter prefix followed by digits is exactly what an id is — and
tightening the detector to § 3.5.1 to exclude them would lose `[Cl9]` with them.
**Position is therefore the whole discriminator**, and it is sufficient: applied,
it yields exactly the three declarations, with zero false positives.
`roadmap-corpus-survey.py` already implements it and already reports 3.

One gap position alone does not close. A markdown link can occupy the leading
slot — `- 📋 [Some Doc](path.md) — do the thing` — and `[CVE-2017-1000117]`
matches even the strict § 3.5.1 grammar. So a token in the leading slot that is
**immediately followed by `(` or `:`** is a markdown link, not an id:

```bash
# run from /mnt/Games/Scripts/Linux. Leading-slot tokens that are markdown
# links, then bullets opening with a link one status marker away from that slot.
# `-h` suppresses filenames, `awk` sums the per-file counts into one total.
grep -rhcE '^\s*[-*] +([✅🚧📋💭]|\[[ xX]\]) *\[[^]]+\][(:]' */[Rr][Oo][Aa][Dd][Mm][Aa][Pp].md | awk '{s+=$1} END{print s}'
grep -rhcE '^\s*[-*] +\[[^]]+\][(:]'                          */[Rr][Oo][Aa][Dd][Mm][Aa][Pp].md | awk '{s+=$1} END{print s}'
```

Measured 2026-07-31, those two commands print **0** and **43**: no leading-slot
token in the corpus is currently a markdown link, while 43 bullets open with a
link one status marker away from that slot. The clause is therefore guarding a
shape the corpus is one edit away from, not one it already contains — which is
the honest reason to keep it: it costs one character check, and the failure it
prevents is filing a CVE number as an item id.

**Two items whose ids fold to the same value are one item's worth of identity
and two items' worth of content.** `roadmap-data-model.md` § 7.1 compares ids
case-insensitively within a project and ANTS-3756's `UNIQUE (project_id,
id_fold)` enforces it, so a source carrying both `Sh-1` and `SH-1` — or the same
id twice — fails at ANTS-3765's insert, in the half that cannot see the source
line that caused it. Migration detects the collision where the lines are still
visible: both items are kept, both are reported as `duplicate_id` with their
line numbers, and neither is silently merged or renamed.

### 2.6 Quarantine, and when it clears

The off-grammar ids are quarantined exactly as `roadmap-data-model.md` § 7.1
requires: imported with the id **verbatim** — `Cl9`, the token's text, **without
the surrounding brackets**, which are the markdown that delimits it and not part
of any id in the store — with `id_origin = "quarantined"`, the project's
migration completing rather than blocking. Migration never invents a dash
(rewriting an id breaks § 3.5.1's append-only rule and every citation of it) and
never treats them as id-less (`roadmap-data-model.md` § 7.2's bulk allocation would issue a second
identity for an item that already has one).

**It clears when the item stops being off-grammar in source, and by no other
route.** A re-run reads `[Cl9]` again and quarantines it again; a re-run that
reads `[CL-9]` files it as parsed. Nothing in the store or in this migration
promotes a quarantined id, because the two available decisions — amend
§ 3.5.1's grammar, or accept the id as opaque — both have consequences beyond
one project, which is why the model declines to pick one and why this spec does
not pick it either. The report names them on every run so the choice stays
visible rather than decaying into silence.

### 2.7 Status normalisation

**Each of the three formats carries status differently, and only one of them is
hard.** Two are a direct transcription of `roadmap-format.md` § 3.5's own
markers:

| Format | Source | Status |
|---|---|---|
| Emoji bullet (§ 3.5) | ✅ / 🚧 / 📋 / 💭 | `shipped` / `in-progress` / `planned` / `considered` |
| GFM task list (§ 3.10.1) | `- [x]` / `- [ ]` | `shipped` / `planned` |
| Pass heading (§ 3.10.5) | the `- **Status**:` value | the word table below |

**`dropped` is never produced by migration, and that is a property of the source
rather than an omission here.** `roadmap-data-model.md` § 7.3 gives it **no
markdown serialisation** — § 3.11 makes a fifth status emoji an anti-pattern —
so nothing in any of the three formats can express it. It enters the store only
through a later curation edit.

Only the pass-headings vocabulary needs a decision, and the framing
`roadmap-data-model.md` § 9 hands over — "`deferred` and `partial` have none in
§ 7.3's enum, so the choice is between adding statuses and losing information"
(the ANTS-3757 bullet carries the same words) — **is false, and twice over.**
The ROADMAP annotation's re-measurement killed the first half: the great
majority of Status values are `done` (the breakdown is below). Shipped code
kills the second — the reader's vocabulary has always been **total**:

| Source word | Status | `passStatusKeyword()` writes back |
|---|---|---|
| `done`, `shipped`, `completed` | `shipped` | `shipped` → `done` |
| `in-progress`, `in_progress`, `inprogress`, `doing`, `wip` | `in-progress` | `in-progress` → `in-progress` |
| `deferred`, `considered`, `parked` | `considered` | `considered` → `deferred` |
| `todo`, `planned`, absent, **anything else** | `planned` | `planned` → `todo` |

Migration uses **this table and no other**, and it applies the shipped reader's
own matching rather than a second one. That matters because an earlier draft
proposed adding a case-fold and a leading-`*` strip on the grounds the reader
lacked both — **it has both already**, verified in `src/roadmapdialog.cpp`:
`rxStatusLine` is built with `QRegularExpression::CaseInsensitiveOption`, its
optional leading group `([^\sA-Za-z0-9_-]+)?` absorbs the `**` in the corpus's
`**un-gated (2026-07-05).**`, and the captured keyword is `.toLower()`-ed before
comparison. The proposed divergence did not exist, so there is none to declare
and none for § 7 to cost.

That `deferred` → `considered` is load-bearing rather than arbitrary: mapping it
to `planned` instead — the reading a fresh author would reach for, and the one
this spec's first draft reached for — breaks the round trip, because a later
flip through `roadmap_log` writes `deferred` back for `considered` and the file
would then re-read as a different status than it was written from. The
right-hand column is a **right**-inverse only: three source words collapse to
`shipped`, so `done → shipped → done` closes but `completed` does not survive a
round trip, which is one more reason § 2.7 keeps the author's string.

Applied to the corpus — first word of each Status value, and this table is the
single home for that breakdown. **It does not contradict § 1.1's count of values
outside `roadmap-data-model.md` § 7.3's enum**, which is much larger: that figure counts values whose
*word is not one of the five status names*, and the reader names far more words
than five. What matters here is how many fall outside the **reader's**
vocabulary, and that is the two rows marked below:

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
the fact that work started.

**The two cases are not equally trustworthy, so they do not share a provenance
value.** `roadmap-data-model.md` § 7.7 calls `provenance` "the model's honesty
mechanism: without it a defaulted `kind` and an author's considered `kind` are
indistinguishable, and every later reader over-trusts the corpus." Collapsing a
faithful transcription and a lossy guess into one value destroys exactly that:

| Source | Stored | `provenance.status` | `extras.source_status` | Reported |
|---|---|---|---|---|
| An emoji or a checkbox | its status | `asserted` | — | no |
| A value whose word the reader **names** (`done`, `completed`, `deferred`, `parked`, `todo`, …) | its status | `asserted` | the value | no |
| A value whose word the reader does **not** name (`partial`, `un-gated`, …) | `planned` | `defaulted` | the value | **yes** |
| No Status line at all | `planned` | `defaulted` | — | **yes** |

**A named word is a faithful transcription, so it is `asserted`.** An author
writing `done` chose that status exactly as one writing ✅ did — only the
notation differs, and § 3.5 makes the emoji this project's own notation for the
same five values. Recording that as anything else would say migration invented a
value the author in fact supplied.

**An unnamed word is a guess, so it is `defaulted`** — the same value `roadmap-data-model.md` § 3.3
gives an absent field, because that is what has happened: the source carried
nothing this model can use and a default was applied. A later reader has to be
able to see that `planned` was migration's choice and not the author's.
Inheriting a GUI's display default *silently* is right for a dialog and wrong
for a primary store.

**`extras.source_status` holds the author's verbatim Status *value*, not the
matched word**, and holds it for both word rows rather than only the lossy one.
The value is the whole remainder of the line after `**Status**:` — so
`done (v3.20.0, 2026-07-05). Adds catalogs for …` is stored entire. Storing the
matched word instead would discard the qualifier tail that made preservation
worth doing, and storing the *normalised* word would discard the distinction
between `done` and `completed`, which `passStatusKeyword()` being a right-inverse
makes unrecoverable. Matching still strips a leading `*` and case-folds, per the
reader; **storage strips nothing**, so `**un-gated (2026-07-05).**` is stored
with its asterisks.

**`migrated` is not used for status at all**, and an earlier draft of this
section using it was the reason this spec briefly proposed amending the
standard. `roadmap-data-model.md` § 7.7 defines `migrated` as "generated by migration itself, with no
source-side counterpart" — written for `roadmap-data-model.md` § 7.2's allocated ids, where nothing
existed before. No status fits that: every one of them has a source-side
counterpart, whether or not the model could use it.

Migration never refuses on a status. The vocabulary is total by construction, so
a refusal path here would be unreachable code guarding an impossible state.

### 2.8 Kind, and the fields the corpus does not carry

`roadmap-data-model.md` § 7.4's mapping table is **normative** and generated
from the same survey; this spec applies it rather than restating it. Absent
`Kind:` defaults to `implement` and absent `Source:` to `planned` per `roadmap-data-model.md` § 3.3,
each recording `provenance.<field> = "defaulted"`. § 2.1.1 states what happens
to every remaining field.

A non-canonical value that is **not** in `roadmap-data-model.md` § 7.4's table is reported and defaulted
to `implement` — not refused. The table was generated from a corpus that grows,
so an unknown value is the expected consequence of a project joining later, and
`roadmap-data-model.md` § 7.4 already says such a project "re-runs it and extends the table by
amendment".

**How a `Source:` line is read is the shared reader's rule, not this spec's**
(ANTS-3764, 2026-07-31). It differs from `Kind:`'s in three ways, each measured
against the corpus rather than inherited from whichever sibling key looked
closest: the value runs to end of line, because 61 of 1282 carry an internal
period; it stops at a following trailer key, because 10 write two keys on one
line; and the label may be bold or inline. `tests/features/roadmap_parse_widening/spec.md`
is the contract, and re-deriving it here would be the second parser § 2.3
forbids in miniature.

`extras.source_kind` follows the same rule as `extras.source_status` and for the
same reason: it holds the author's verbatim value whenever the canonical `kind`
differs from what the source said — both a mapped value (`bugfix` → `fix`) and
an unmapped one defaulted to `implement`. A canonical value written by the
author sets neither `extras.source_kind` nor a `provenance.kind` of anything but
`asserted`.

### 2.9 Id allocation

Policy is fixed by `roadmap-data-model.md` § 7.2; this spec executes it and adds
the one thing a plan needs that a policy does not — that **allocation is not
this half's job to perform**. `planFrom()` is pure and an id counter is shared
mutable state, so ANTS-3765 allocates inside the same transaction as the write.

The obligation therefore rides on `PlannedItem` itself — `idAllocationOwed`
plus `closed` — and **not** on the note alone. A `Note` is keyed on a source
line; ANTS-3765 writes items, not lines, so a load half handed only notes would
have to re-derive which item each one meant. The `id_allocation_owed` note
remains as the *report* view of the same fact, for a human reading § 2.10.

**What separates a closed id-less item from an open one is
`roadmap-data-model.md` § 7.2's rule and is not restated here** — an earlier
draft reproduced it as a table, which is one more copy to drift. `closed` is on
`PlannedItem` precisely so ANTS-3765 can apply that rule without re-deriving it,
and both cases carry `provenance.id = "migrated"`: they differ in what is owed
afterwards, not in where the id came from.

The standard's "~1,600 id-less, ~1,020 closed /
~600 open" is **confirmed** by the 2026-07-31 run (§ 1.1), not — as the
ANTS-3757 bullet's annotation suspected — a transposition of Ants' own
id-bearing count. The rival figures in that annotation (2,818 / 3,794) counted
*lines*, a different population.

Pass-heading items are **not** among the id-less: their ids are synthesised from
the heading, and migration **takes the reader's synthesised id as-is** rather
than calling `PassHeadingWrite::passIdFromDesignator()` itself. An earlier draft
of this section named that call, which cannot be made: it takes the *designator*
(`"43.5"`), and by the time a record exists the reader has consumed the heading
and emitted `PASS-43-5` already — obtaining the designator back would mean
re-matching the heading regex, the second parser § 2.3 forbids. The property
that mattered is unaffected, because the two derivations are the same code path
in the same release: the reader synthesises the id the writer's function would
have produced, so reader, writer and migration still agree on `PASS-43-5-B` and
a flip through `roadmap_log` still finds the item it wrote. INV-10 asserts that
agreement directly rather than assuming it from a shared call. Pass items carry
`id_origin = "synthesised"`.

**A reader-synthesised id that is NOT a pass id is discarded.** The GFM adapter
sets `BulletRecord::synthetic` when it derives an id from a content hash rather
than from a token — an identity the *dialog* invented so it could address a
bullet, which is precisely what `roadmap-data-model.md` § 7.2 must not see: it
is neither an author's id nor an absence, and filing it would silently remove
that item from the id-less population the standard's bulk allocation exists to
serve. So a `synthetic` id is dropped and the item is planned id-less, with
`idAllocationOwed` set. Pass ids are synthesised too and are kept, because they
are derived from the author's own heading and round-trip back to it.

### 2.10 The report

`MigrationPlan::notes` is a value, not a log line, so tests assert on it. Codes
are a closed set — an open one becomes prose nobody can grep:

| Code | Raised when |
|---|---|
| `quarantined_id` | § 2.6 — a declared id outside § 3.5.1's grammar. |
| `duplicate_id` | § 2.5 — two items whose ids fold to the same value. |
| `status_defaulted` | § 2.7 — an unnamed word, **or no Status line at all**. |
| `kind_unmapped` | § 2.8 — non-canonical and absent from `roadmap-data-model.md` § 7.4's table. |
| `orphan_status_line` | § 2.4 — a `- **Status**:` line belonging to no pass block. |
| `id_allocation_owed` | § 2.9 — an id-less item, carrying open/closed. |
| `empty_source` | § 2.3 — a file yielding zero items. |

Discovery's failures are **not** in this set. `findRoadmap()` returns an error
before any plan exists (§ 2.2), so a missing, case-ambiguous or undecodable
roadmap has no `MigrationPlan` to be reported on. They are still a closed set:
`findRoadmap()` reports **`not_found` | `case_ambiguous` | `not_utf8`** in a
machine-readable code beside the human message, because INV-1 asserts *which*
refusal happened and free text is not assertable.

### 2.11 Sections, elements and the legend — the structural walk

§ 2.3 splits this half into the reader's job and `planFrom()`'s own scan. This
section is that scan, and it exists because the shipped reader answers only
"which bullets are items"; every carrier in § 2.1 other than `PlannedItem`
comes from here, as do the line spans INV-11 partitions.

**One pass over the lines, in document order.** It never re-decides item-hood or
a status — those are the reader's, and a disagreement between the two would be
exactly the silent kind § 2.3 rules out.

- **Sections are `##` and `###`, and no deeper.** Not an arbitrary depth limit:
  the reader is the authority on which section an item belongs to, it tracks
  levels 2 and 3 only, and a section set larger than the one it tracks would
  file items under slugs it never assigns. A `####` heading (outside the
  pass-headings format, where § 2.4 makes it an item) is therefore ordinary
  content, carried as narration. ANTS-3761's export can represent deeper
  sections for stores built other ways; migration does not produce them.
- **Slugs come from `RoadmapIndex::uniqueSlug()`** — the same function, with the
  same running `seen` set, that the shipped reader uses on both the pass path
  and the ants-v1 / GFM path. So a section's slug equals the `sectionSlug` the
  reader put on every item inside it *by construction* rather than by
  coincidence, and the store's `UNIQUE (project_id, slug)` is satisfied by the
  uniquing that function already does for duplicate headings.
- **`parentSlug`** is the most recent `##` for a `###`, and empty for a `##`.
- **Content before the first heading** belongs to a synthetic section: empty
  slug, empty title, level 0, no parent.
- **`intro` versus `narration` is a position rule, not a content rule.** Lines
  between a heading and that section's **first element** are its `intro`;
  section-level text after the first element is a `narration` element. That is
  `roadmap-data-model.md` § 5's own distinction — a section has "optional intro
  prose" *and* an ordered element list — and it is why `PlannedSection`'s span
  covers the heading and its intro and stops there.
- **A fenced block is never an element.** § 5 is explicit: "fenced code blocks
  belong to a `body` or an `intro`. Neither is a section element." So a fence
  inside an item's span is that item's `body`; a fence at section level before
  the first element joins the `intro`; a fence at section level after one is
  appended to the **preceding item's** `body`, since § 5's rule is that prose
  belongs to what it is subordinate to. Fence extents are matched by their own
  delimiters, so a `##` line *inside* a fence is not a heading — the one place
  this walk must not read a line at face value.
- **A table** is a maximal run of contiguous lines whose trimmed form starts and
  ends with `|`. Its first row is the header, its second is the separator and is
  **dropped** (`roadmap-data-model.md` § 5.2: "delimiter, not content… would have the store
  round-tripping a line that means nothing"), and the rest are rows. The
  `payload` is `{"header": [...], "rows": [[...], ...]}`, matching the shape
  ANTS-3761 § 2.3 exports for `kind = "table"`.
- **The legend** is recognised exactly as `tools/roadmap-corpus-survey.py`
  already counts it, so the spec and its own oracle cannot disagree: a
  status-marked bullet that § 2.4 rejects (no id, no bold headline) whose text
  begins with a status word and is under 160 characters. A maximal run of such
  lines is the legend block; each becomes an `entries` key of the status
  § 2.7 maps its marker to, with the line's remaining text as the wording. A
  project with no such run plans no legend, which is eight of the ten.
- **`position` is ONE 0-based, contiguous, gapless sequence per section**,
  covering that section's items and elements together in document order. The
  store CHECKs `UNIQUE (section_id, position)` over `element` rows and an item
  is filed by an element row, so numbering items and elements independently
  yields a plan that dies at ANTS-3765's insert — in the half that can no longer
  see the source. This is the cheapest mistake in the section to make and the
  most expensive to diagnose.
- **Every carrier records a 1-based inclusive line span**, and INV-11 is what
  holds the walk to it.

## 3. Invariants

Each invariant asserts the section named in its first clause; it does not
restate the rule, and where the two could differ § 2 wins. Every *Breaks when*
clause is a mutation an implementer must apply and observe RED (§ 6).

- **INV-1** — Discovery behaves per § 2.2. *Test:* `tests/features/roadmap_migrate_read/` — fixture roots holding `ROADMAP.md` only, `roadmap.md` only, both, neither, and one holding invalid UTF-8; the first two resolve and return the file's text, the last three return `nullopt` with the § 2.10 refusal code `not_found` / `case_ambiguous` / `not_utf8` respectively — asserted on the **code**, since a human message is not assertable. *Breaks when:* discovery globs `ROADMAP.md` — which silently drops RetroDB, the only project using the pass-headings format, and with it the **whole** pass corpus plus RetroDB's own bullet-form items, a loss half again as large as the pass corpus alone (§ 1.1).
- **INV-2** — A bullet becomes an item only under § 2.4's conjunction. *Test:* `roadmap_migrate_read` asserts the per-fixture item count equals a **committed expectation file** generated out-of-band by `tools/roadmap-corpus-survey.py` over the same fixtures — the survey is not invoked from the test, so no interpreter joins the `features;fast` path, and the oracle applies § 2.4's id-in-leading-slot refinement, which the survey already implements. Regenerating that file is a reviewable diff and must never be done to make the test pass. *Breaks when:* the bold-headline half is dropped — so **the fixtures must carry the populations that move**: at least one status-marked detail line beneath an item and one status-legend line, both of which promote to items under the mutation and neither of which any other fixture here needs. Parity against an independently-written parser is the point: a test written from this spec's own rules can only confirm what the rules say.
- **INV-3** — An id-shaped token is an id only under § 2.5's position and link rules, and an item with no id carries `idAllocationOwed` with `closed` set per § 2.9. *Test:* `roadmap_migrate_read` — a fixture whose bullets are **all status-marked with bold headlines**, so § 2.4 admits every one of them and only identity is in question: `[milnet01]`, `[x86_64]` and `[FOO123]` appear mid-line, `[CVE-2017-1000117]` as a link label in the leading slot, `[Cl9]` in the leading slot. Only `[Cl9]` yields a non-empty `id`; the rest plan with `id` empty, `idAllocationOwed == true`, and `closed` matching their status. A second fixture in the GFM format carries an id-less checkbox item, for which the shipped reader sets a content-hash id with `synthetic == true`: § 2.9 discards it, so it too plans `id` empty with `idAllocationOwed == true`. *Also breaks when:* a `synthetic` reader id is taken at face value — which passes every other invariant here while silently removing that item from the id-less population `roadmap-data-model.md` § 7.2's bulk allocation exists to serve. *Breaks when:* the **leading-slot restriction is dropped** from the id-shaped detector, which promotes the mid-line tokens to ids; or the link clause is dropped, which files the CVE number as an id. **Not** "the detector uses § 3.5.1's grammar anywhere on the line" — an earlier draft named that mutation and it cannot redden this invariant, because none of the six tokens contains the dash § 3.5.1 requires, so a strict detector never matches them wherever it runs.
- **INV-4** — Quarantine behaves per § 2.6. *Test:* `roadmap_migrate_read` asserts the planned id equals the literal `Cl9` — bracket-free and unrepaired — that `id_origin` is `quarantined`, that `idAllocationOwed` is false, and that one `quarantined_id` note names its line. **The fixture must also carry `[ANTS-119&]`**, which the reader refuses outright where it accepts `Cl9`: it is the only shape in the corpus for which the raw token is load-bearing (§ 2.3), so a fixture holding `Cl9` alone leaves the field's whole justification untested. *Breaks when:* quarantine is implemented as "repair to `CL-9`", which breaks § 3.5.1's append-only rule; or as "treat as id-less", which issues a second identity for an item that already has one. Asserting only that two runs agree would catch neither: a repairing implementation is perfectly deterministic, so that assertion tests INV-9 and not this.
- **INV-5** — Every source shape maps to the status § 2.7's tables give it, and `dropped` is never produced. *Test:* `roadmap_migrate_read` drives all four emoji, both checkbox states and every word in § 2.7's word table through `planFrom()`, asserting **per source shape against an expected status** rather than only in aggregate — a table of (source, expected) pairs, one row per emoji, per checkbox state and per named word, plus a mixed-case (`Done`) and an asterisk-wrapped (`**deferred**`) leg proving migration inherits the reader's matching and adds none of its own; asserts no plan anywhere yields `dropped`; and asserts that for the four statuses `PassHeadingWrite::passStatusKeyword()` covers, `status → keyword → status` returns the original. *Breaks when:* migration introduces its own word mapping — `deferred` → `planned` is the natural one and it breaks the round trip, since a later `roadmap_log` flip writes `deferred` back for `considered`; or the emoji rows are omitted, which leaves almost the whole corpus with no defined status and is what an earlier draft of § 2.7 did. The per-shape table is what makes the second mutation redden *here*: against an aggregate assertion it passes, because no plan yields `dropped` either way.
- **INV-6** — § 2.7's `asserted` / `defaulted` split holds, and `status_defaulted` is raised for exactly the two rows that carry it. *Test:* `roadmap_migrate_read` asserts `partial` and `un-gated` are `defaulted` and reported, that a pass block with **no Status line** is likewise `defaulted` and reported, and that `done`, `todo`, `deferred`, ✅ and `- [x]` are all `asserted` and silent. *Breaks when:* the else-branch is inherited wholesale, so every value becomes `asserted` — every other invariant here still passes, the statuses are all still legal, and the store quietly gains items whose `planned` is a guess indistinguishable from an author's choice. That is the precise confusion `roadmap-data-model.md` § 7.7 exists to prevent, and no test that only checks the stored status can see it.
- **INV-7** — `extras.source_status` holds the verbatim Status **value** per § 2.7 for every pass block that **carries a Status line**, and is unset for a pass block that carries none as well as for every emoji or checkbox status. *Test:* `roadmap_migrate_read` asserts the exact source string survives for a value with a qualifier tail (`done (v3.20.0, 2026-07-05). Adds catalogs for …`), that `completed` survives as `completed` and not as the `done` its round trip would write back, that `**un-gated (2026-07-05).**` survives with its asterisks, and that both an emoji-format item **and a pass block with no Status line** set it not at all — the second leg being the one § 2.7's table row makes explicit and an earlier statement of this invariant contradicted. *Breaks when:* the normaliser stores its own normalised word, or the matched word rather than the whole value — either of which passes INV-5 and INV-6 while discarding the qualifier that made preservation worth doing.
- **INV-8** — `kind` and `source` behave per § 2.8. *Test:* `roadmap_migrate_read` — a fixture item with no `Kind:` plans as `implement`/`defaulted`; one with no `Source:` plans as `planned`/`defaulted`; one with `bugfix` plans as `fix`; one with `frobnicate` plans as `implement` and raises `kind_unmapped`. *Breaks when:* `planFrom()` refuses an item carrying no `Kind:` — which refuses roughly half of every project in the corpus.
- **INV-9** — `planFrom()` is pure per § 2.1: identical input yields an identical plan, and it touches no filesystem, clock or id counter. *Test:* `roadmap_migrate_read` calls it twice on the same fixture and compares field-wise, including `provenance` and note order; and asserts that no file under the fixture root has a changed mtime across the two calls, since a counter persisted to disk is the shape allocation would actually take. *Breaks when:* allocation is done here rather than deferred to ANTS-3765 — the mutation must **read and increment**, because a counter that is only read returns the same value twice and leaves the comparison green. That is the cheapest possible detector for exactly the mistake § 2.9 exists to prevent.
- **INV-10** — Pass blocks behave per § 2.4 and § 2.9: one item per block, the id the reader synthesised, status from the block's first *classifying* `- **Status**:` line, and a Status line in no block reported rather than imported. *Test:* `roadmap_migrate_read` — a fixture with `Pass 43.5`, `Pass 43.5.B`, a block whose first Status line is content-free and whose second carries a different word from its third, and a Status line before the first heading; asserts ids `PASS-43-5` / `PASS-43-5-B`, one item per block, the status of the first *classifying* line, and one `orphan_status_line` note. **The id leg is asserted against `PassHeadingWrite::passIdFromDesignator("43.5")` rather than against a literal** — the two derivations must agree or `roadmap_log` stops finding what it wrote, and § 2.9 explains why migration cannot simply *call* it. *Breaks when:* two separate mutations, one per clause — **(a)** the letter-led sub-designator is dropped from the synthesised id, collapsing `Pass 43.5` and `Pass 43.5.B` to `PASS-43-5`, the regression ANTS-2035 already fixed once in the reader; **(b)** the **last** classifying Status line in a block wins instead of the first, which changes *at most* the nine corpus blocks carrying a second Status line — how many of those nine actually classify differently is unmeasured, so the fixture and not the corpus is what makes this mutation redden. "The derivation is reimplemented" is not a mutation — a correct reimplementation stays green — and an earlier draft named it as one.
- **INV-11** — No source content is silently discarded. Every non-blank line of the source falls inside **exactly one** `PlannedItem`, `PlannedElement`, `PlannedSection` or `PlannedLegend` span — a partition, so a gap is a dropped line and an overlap is a double-filed one — and every `Note` with a non-zero line names a line **inside** one of those spans. It also asserts § 2.11's ordering: within each section, the `position` values of its items and elements together form a **contiguous 0-based sequence with no gap and no repeat**. *Test:* `roadmap_migrate_read` asserts the partition over the committed fixtures, that each note's line resolves into a span, and the per-section position sequence. *Breaks when:* **(a)** a parser branch falls through — the failure mode migration exists to prevent, and the only clause here that fires on a format nobody anticipated; **(b)** items and elements are numbered in **separate** sequences, the natural implementation, which every other invariant here passes and which then dies on ANTS-3765's `UNIQUE (section_id, position)` insert, in the half that can no longer see the source. Notes are deliberately **not** part of the union: every § 2.10 code except `empty_source` names a line that already sits inside one of the four spans — `orphan_status_line` sits in an element rather than an item, which is why this exclusion is stated over the spans and not over items — so adding notes to the union would make every conforming plan overlap and the invariant would fail on the fixtures § 6 requires. It also requires each of the four carriers: against a plan holding only `items` and `notes`, as an earlier draft declared, the invariant is unsatisfiable rather than merely unmet.
- **INV-12** — Folded-id collisions behave per § 2.5. *Test:* `roadmap_migrate_read` — a fixture declaring `Sh-1` and `SH-1`; asserts two `PlannedItem`s, both ids verbatim, and one `duplicate_id` note per item naming its line. *Breaks when:* the parser keys items on the folded id — the natural implementation, since ANTS-3756 keys the store that way — which silently drops the second item here and would instead fail ANTS-3765's `UNIQUE (project_id, id_fold)` insert, in the half that can no longer see the source line.
- **INV-13** — A source yielding zero **items** raises `empty_source`, per § 2.3. *Test:* `roadmap_migrate_read` plans an empty file and a prose file with no bullets; both raise it, and the prose file additionally plans narration elements — which is what makes this invariant and INV-11 consistent rather than opposed. *Breaks when:* the condition is written over the whole plan (`zero items and zero elements`), which the prose fixture then leaves silent while INV-11 stays green — the two together being the only way to see it.

## 4. RAM / build cost

`planFrom()` holds one project's plan in memory. The largest roadmap in the
corpus is Ants' own at 2.95 MB on disk — measured 2026-07-31 by `wc -c` over
every `*/[Rr][Oo][Aa][Dd][Mm][Aa][Pp].md`, and larger than the runner-up by a
factor of five, so the budget is pinned to a clear worst case rather than a
close one. **Bytes, not lines, is the metric that matters here** and the two do
not rank the same way: § 2.2 calls RetroDB "a 4,800-line project", which is a
line count from the ANTS-3753 survey and a much smaller file. The working set
starts at roughly **twice** that before any plan exists, because `QString` is
UTF-16 and the corpus is almost entirely ASCII. On top of that a plan carries
the item bodies and the element payloads, each a second copy of a slice of the
source. Budget **under 4× the source file's byte size** — under 12 MB for the
worst project — and never the whole corpus at once, since ANTS-3765 loads one
project per transaction and discards each plan after it commits. That
per-project bound is the eviction policy; there is no cache and nothing
accumulates across projects.

No new external dependency, and the test path adds no interpreter: INV-2's
parity expectation file is generated out-of-band and committed, so
`tools/roadmap-corpus-survey.py` never runs from `test_core`.
`src/roadmapmigrate.{h,cpp}` joins the existing `ants_core_lib` (`Qt6::Core`)
and the test joins the existing `test_core` bundle rather than adding a target.

ANTS-3764 is the larger build-side move and this spec should not undersell it.
It is not a file rename: `detectRoadmapFormat()` and `parsePassHeadingBullets()`
have internal linkage in an anonymous namespace in `src/roadmapdialog.cpp`, the
GFM-adapter branch is inline in `RoadmapDialog::parseBullets()`, and
`BulletRecord` is a struct nested inside a `QDialog` subclass — so the move is a
dispatcher, three per-format readers and a record type, out of
`ants_dialogs_lib` and into `ants_core_lib`, plus the § 2.3 widening. Its cost
is that spec's to state.

## 5. Out of scope

- **Writing anything to the store** — ANTS-3765. That includes per-project
  atomicity, the `Access::Bulk` connection, re-run matching, items deleted from
  source, and the cutover interim.
- **Extracting the reader** — ANTS-3764, this spec's blocker.
- **The published render, and the fate of `roadmap_query` / `roadmap_log` /
  `RoadmapDialog`** — ANTS-3758.
- **Rotated archives.** `roadmap-format.md` § 3.9 moves closed minors out of
  `ROADMAP.md` into `docs/roadmap/<major>.<minor>.md` and deletes them from the
  live file, so excluding them loses those items rather than deferring them —
  which is why this exclusion is stated rather than left implicit. Measured
  2026-07-31: one project of the ten has archives at all (this one), across two
  files, holding 20 emoji bullets, every one of them shipped and every one
  already summarised in `CHANGELOG.md`. Including them is not a discovery tweak
  but a second design — `findRoadmap()` would return a set rather than a file,
  and the same `## 0.6.0` heading exists in both the archive and the live file's
  history, so section identity across sources needs a rule nothing here has. The
  standard already treats archives as a separate tier (§ 3.9: "the
  `roadmap-query` IPC verb reads only the current `ROADMAP.md`. Archives are
  dialog-only by contract"). A later pass can add them as additional sources
  without changing a **plan** type in § 2.1 — the item, section, element,
  legend and note carriers all stand. What it does change is `findRoadmap()`'s
  return and `MigrationPlan`'s single `sourcePath`, both of which become
  per-source; ANTS-3766 carries that, and an earlier statement here claiming
  nothing in § 2.1 changed was contradicted by this same bullet's own
  "would return a set rather than a file".
- **Deciding whether `[Cl9]`'s shape becomes legal.** A permanent exclusion, not
  deferred work: it is an amendment to `roadmap-format.md` § 3.5.1 affecting
  every project, and § 2.6 makes migration correct under either outcome.
- **Recovering dates from git history.** `roadmap-data-model.md` § 4.2 already
  bounds what is derivable; nothing here needs a date the source does not carry.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_read/`, covering INV-1..13, added
to the existing `test_core` bundle's `SOURCES` (never `add_executable` — see
`tests/features/README.md`). Label `features;fast`; no database, no network, no
interpreter.

Fixtures are **committed roadmaps, not the machine's corpus**, which is not
present in CI — one per format, plus a pathological fixture carrying the
reference-vs-declaration tokens, a pass block whose first Status line is
content-free, the orphan Status line, the markdown-link-in-leading-slot case,
the folded-id collision, a status-marked detail line, a status-legend line, an
item with no `Kind:` and one with no `Source:`, a prose-only file, an empty
file, and a file that is not valid UTF-8.

INV-2's parity leg compares against `expected-counts.json`, committed beside the
fixtures and regenerated by running `tools/roadmap-corpus-survey.py` over them
**by hand**. The indirection is what keeps the cross-check honest *and* keeps
Python off the test path: the expectation still comes from an independently
written parser rather than from this spec's own rules, but the C++ test only
reads a file.

Per the project convention, each invariant must be shown **RED under the exact
mutation its own *Breaks when* clause names** before it is accepted green. Where
a named break does not redden, that is a finding about the invariant and is
recorded rather than dropped — ANTS-3761's row 6-impl found one such clause that
was self-refuting.

## 7. Cross-doc impact

- `CLAUDE.md` — module map gains `src/roadmapmigrate.{h,cpp}`; ANTS-3764 moves
  the reader entry in the same release.
- **ANTS-3764 has shipped the § 2.3 widening** (2026-07-31) — `BulletRecord`
  carries all five: `sourceStatus`, `source`, `idToken`, `passDesignator` and a
  1-based inclusive `firstLine`/`lastLine` span. Additive, as predicted: the
  full suite stayed green with no call-site change, so `RoadmapDialog`,
  `roadmap_query` and `roadmap_log` see no behaviour change — and neither does
  the IPC envelope, which does not emit the new fields because `planFrom()`
  links the reader directly. `passDesignator` was kept although § 2.9 resolved
  not to need it: it is the only one of the five that cannot be recovered after
  the fact, and it makes INV-10's agreement assertable as
  `passIdFromDesignator(passDesignator) == id`. Contract and the corpus
  measurements behind the `Source:` rules:
  `tests/features/roadmap_parse_widening/spec.md`.
- **`extras`, `lanes` and `evidence` have no write path at all today** — a
  defect in ANTS-3756's shipped surface rather than something ANTS-3765
  introduces, and this spec is only the first caller to need them. `item` has a
  column for each, `ItemWrite` has a field for none, and `setItemField()`'s
  allowlist excludes all three, so through the public API they can only hold
  their DDL defaults. **ANTS-3767** carries it and blocks ANTS-3765. The
  `sectionSlug` → `sectionId` resolution is separate and is ANTS-3765's own.
  § 2.1.1 is the list.
- `docs/standards/roadmap-data-model.md` — two changes, and they are not equal.
  **Neither is an amendment to `roadmap-data-model.md` § 7.7**: § 2.7 uses `asserted` and `defaulted`
  exactly as that section defines them, and an earlier draft's proposal to widen
  `migrated`'s gloss is withdrawn.
  - `roadmap-data-model.md` § 9's open policy question ("how the pass-headings status vocabulary
    normalises") is answered by § 2.7 and should point here. **Scope the
    correction precisely:** the shipped reader falsifies the premise for
    `deferred`, which maps to `considered`. It does *not* give `partial` a
    target — `partial` reaches `planned` through the else-branch, which § 2.7
    concedes discards the fact that work started, and is why the source value
    is preserved rather than mapped. An edit claiming both were falsified
    replaces one wrong sentence with another.
  - **§ 7.2's item rule needs an amendment, and this spec must not just assert
    past it.** That section is normative and says a bullet is an item when it
    carries **both** a status marker and a bold headline; § 2.4 admits an id in
    the leading slot *in place of* the headline. Today both documents claim
    authority and they disagree about a real bullet — `- ✅ [ANTS-1234] plain
    text`. The refinement is the right rule (§ 7.1 puts the id in that slot, and
    the corpus writes both together), so the fix is an amendment to § 7.2
    rather than a silent local override. Until it lands, § 2.4's refinement is
    this spec's and is flagged as such wherever it is used.
  - `roadmap-data-model.md` § 3.3 quotes "**57%** carry no `Layman:`"; the 2026-07-31 run says 56%.
    Noticed, not edited here — this run did not review that document, and its
    § 2 already says its figures are re-derived by re-running the survey.
- `tools/roadmap-corpus-survey.py` — the stronger rung for § 2.4's and § 2.5's
  inline commands is to fold them into the survey so the figures become output
  rather than prose, exactly as the standard's § 3.3 did for its own. § 1.1
  reduces the blast radius of the drift in the meantime; the fold-in is worth
  doing at implementation, when the parser those counters describe exists.
- `docs/subsystems.md` — the roadmap lane gains the migration files.
- `ROADMAP.md` — **ANTS-3766** filed 2026-07-31 for § 5's archive exclusion;
  **ANTS-3764** annotated with the § 2.3 widening and with two corrections its
  own body carried, both produced by this spec's verification against source.
- `CHANGELOG.md` — on ship.

## 8. Open questions

- **Whether `partial` deserves `in-progress` rather than the else-branch's
  `planned`.** It is more truthful — the two corpus items say "Phase A landed" —
  but it diverges from the shipped reader, and § 2.3's whole argument is one
  reader. § 2.7 keeps the reader's answer, marks it `defaulted` and reports it,
  which leaves the question open in the store rather than deciding it silently:
  promoting the item is then an ordinary curation edit, and setting
  `provenance.status = asserted` is exactly what `roadmap-data-model.md` § 7.7
  already says an edit through the store does. No new mechanism is needed
  either way, so this can stay open without blocking implementation.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-split | 2026-07-31 | none — no reviewer dispatched | — | **Provenance row, not a review.** Split from ANTS-3757, which owned parse + normalise + allocate + load + cutover in one document — the shape ANTS-3756 carried when it converged by cap at 3 loops and had to split into ANTS-3761. This id keeps the read half; ANTS-3765 takes the load half. The seam is pure transformation vs. transactional load, and both halves are independently testable, which is the evidence it is a seam rather than a filing convenience. Invariants are numbered from 1: this is a scope reduction of a spec that was never written, not a split of a reviewed document, so there is nothing to avoid reflowing. **No review has been run on this document** — the gate starts at loop 1 on these bytes. |
| 1 | 2026-07-31 | 2 (both cold, same shared packet) | 4 / 8 / 9 / 5 / 0 | 26 verified, 25 fixed, 1 surfaced, 1 dismissed, 1 out-of-scope. **The highest-value finding is one only a mutation could produce: INV-3's own *Breaks when* could not redden it.** The clause named "the detector matches the grammar anywhere on the line", but § 3.5.1's grammar requires a dash before the digits and not one of the six measured tokens has one — so a strict detector never matches them wherever it runs, and the invariant passed under its own break. § 2.5 now states the dash-optional id-shaped grammar explicitly as a *second, looser* detector, and the mutation is restated as dropping the leading-slot restriction. Three further contract gaps: **§ 2.7 defined status for the pass-headings word only**, leaving almost the whole corpus — every emoji bullet and checkbox — with no rule, and no account of why `dropped` is unreachable; **INV-11 was unsatisfiable against its own types**, since `MigrationPlan` carried only `items` and `notes` with no line spans, so narration, tables and fences had no carrier and nothing to count; and **discovery was specified with no entry point**, its refusal reported as a plan note that a pure `planFrom()` could never produce. `findRoadmap()` is now declared as the impure half and discovery failures are errors, not notes. Also corrected: a **wrong measurement** — "~37% over-count" summed markerless sub-bullets that the rule cannot promote; the real population is the status-marked bullets lacking id and headline, ~2.4%, which corroborates the standard's own "roughly 90". INV-4 asserted determinism where it meant verbatimness, and INV-10's break ("the derivation is reimplemented") was not a mutation at all; both restated, and INV-12/INV-13 added for folded-id collisions and empty sources. **Surfaced, not fixed:** `provenance.status = "migrated"` does not fit `roadmap-data-model.md` § 7.7's "with no source-side counterpart" — an authored `done` has one — but `asserted` would make migration's guesses indistinguishable from an author's choices. |
| 2-decision | 2026-07-31 | none — a decision, not a review | — | **Decision row, written by the author; no reviewer was dispatched.** Loop 1's one surfaced finding is closed, and **without** the amendment it asked for. Re-examined, the defect was not that `migrated` fit `roadmap-data-model.md` § 7.7 badly — it was that § 2.7 gave **one** provenance value to two cases that differ in exactly the way `provenance` exists to record. `done` → `shipped` is a faithful transcription: the author chose that status, and only the notation differs from an author writing ✅. `partial` → `planned` is a lossy guess that discards "Phase A landed". `roadmap-data-model.md` § 7.7's own rationale — "without it a defaulted `kind` and an author's considered `kind` are indistinguishable, and every later reader over-trusts the corpus" — is precisely the distinction the single value destroyed. So a named word (and every emoji and checkbox) is now **`asserted`**, an unnamed word or an absent Status line is **`defaulted`** and reported, and `migrated` is not used for status at all. **The proposed amendment to `roadmap-data-model.md` § 7.7 is withdrawn.** A fix that removes the need to change a published standard is a better fix than the one that changes it. |
| 2 | 2026-07-31 | 2 (both cold, same shared packet) | 5 / 12 / 11 / ~10 / 0 | **~38 verified, none fixed in-loop — the run was stopped deliberately and the findings carried to a consolidation pass (row 3-consolidate).** Findings rose against loop 1 and roughly half were **fix collateral**: defects loop 1's own fixes introduced, not defects in the draft. `/cold-eyes` Phase 5 names that pattern — "collateral rising while draft defects fall means the sweep is under-running, and looping harder makes it worse". The diagnosis was structural and specific: § 2.1's declarations, § 2's prose and § 3's invariants each stated the same contract, so every loop found disagreements *between the three copies* rather than defects in the design. Continuing to loop would have kept reconciling N copies instead of deleting N−1. |
| 3-consolidate | 2026-07-31 | none — a restructure, not a review | — | **Consolidation row, written by the author.** § 2.1's declarations are now the **single statement of shape**, § 2's prose states only decisions and their evidence, and every invariant asserts a section by reference instead of restating it. Three of loop 2's five CRITICALs dissolved rather than being fixed: they were disagreements between copies that no longer exist. The rest were folded in, and the restructure surfaced **five defects no cold read had reached, all from checking § 2.1's types against ANTS-3756's shipped DDL rather than against the prose**. (1) `PlannedElement::kind` included `fence`, which `element.kind`'s CHECK constraint — `('item','narration','table')` — would refuse; `roadmap-data-model.md` § 5.2 puts fenced blocks in a `body` or an `intro`, so the kind set is now the store's own minus `item`. (2) **The plan had no carrier for a section**, although `section` is a table with `title`, `level`, `intro` and `parent_id` and every item names a `sectionSlug` — ANTS-3765 could not have created the rows the plan referred to. `PlannedSection` added. (3) **No carrier for the `roadmap-data-model.md` § 5.1 status legend**, which `project.legend` holds and `roadmap-data-model.md` § 5.2 assigns a home; without it the legend lines are lost, or promoted to items under INV-2's own mutation. `PlannedLegend` added. (4) `ItemWrite` lacks `lanes` and `evidence` as well as `extras` — three owed additions, not two — although `item` has a column for each and the shipped reader already parses both lines; § 2.1.1 is now an exhaustive field-disposition table, so "left empty" is a decision rather than an omission. (5) **§ 2.7's proposed case-fold and leading-`*` strip were already in the shipped reader** — `rxStatusLine` carries `CaseInsensitiveOption`, its optional leading group absorbs the `**`, and the keyword is `.toLower()`-ed — so the divergence loop 2 asked § 7 to cost did not exist. The claim had been carried since loop 1 on recall rather than on a read of the regex. Two further corrections from the same source: the winning Status line is the first that **classifies** (a content-free `- **Status**:` line does not stop the reader's 50-line scan), and § 4 had ANTS-3764 as an anonymous-namespace pair when `parseBullets()`'s GFM branch is inline in a member function. Structural fixes: `empty_source` now turns on zero **items**, which removes its contradiction with INV-11; INV-11 is a **partition** over four carriers with notes excluded from the union, which removes the double-cover falsification; § 1.1 is the single home for every corpus figure, since the same drifting item count had been a finding in both loops; and archives are now an explicit § 5 exclusion with the measurement behind it. |
| 3 | 2026-07-31 | 2 (both cold, same shared packet, `--max-loops 1`) | 2 / 5 / 7 / 12 / 0 | **26 verified, 24 fixed, 2 dismissed. NOT converged — see the recommendation below.** The consolidation held: **not one of loop 2's ~38 findings was re-raised**, which is the proof the restructure worked rather than merely moved text. Everything here is new, and it is new for a reason worth recording — making § 2.1 the single statement of the contract gave the lanes one place to check it against the shipped DDL, and both lanes independently returned the same top finding. **§ 2.3's claim that "nothing else in this spec needs a field the shipped reader does not already produce" was false five times over.** `BulletRecord` has no `source` (§ 2.8 reads a `Source:` line), no line spans (every carrier declares them and INV-11 partitions on them), no pass designator (so § 2.9's `passIdFromDesignator()` call had no available input), and an `id` that is empty unless a strict `[PREFIX-NNNN]` token matched — which means `[Cl9]` reached migration as *no id at all* and would have been bulk-allocated a second identity instead of quarantined, making § 2.6 unreachable and INV-4 unpassable. Worse, the reader emits **bullets only**: no narration, tables, sections or legend. § 2.3 now states the seam explicitly — the reader classifies bullets, `planFrom()` owns a structural walk — and § 2.11 specifies that walk, which is the section three of the five plan types were declared without. Second CRITICAL: routing fences and heading-trailing prose to `narration` contradicted `roadmap-data-model.md` § 5's "Neither is a section element", orphaned `PlannedSection::intro`, and created an INV-11 overlap on the very fixtures § 6 requires; § 2.11 now derives `intro` from position and keeps fences subordinate. Also fixed: items and elements must share **one** per-section `position` sequence or the plan dies on `UNIQUE (section_id, position)` (INV-11 now asserts it); a reader-`synthetic` GFM hash id is discarded so the item stays id-less (INV-3); § 2.1.1 gained the `projectId` row it claimed exhaustiveness without; § 7 now proposes the § 7.2 amendment § 2.4's refinement actually needs rather than asserting past a normative rule; discovery's refusals became a closed code set so INV-1 can assert on them; and 29 bare cross-doc section references were qualified. **Two lane findings dismissed on measurement, not judgement:** that `uniqueSlug()` runs only on the pass path (it is called at `roadmapdialog.cpp:1069` on the ants-v1 / GFM path too, which is what makes § 2.11's slug rule sound), and that the survey's 3,957 includes the detail and legend lines (`roadmap-corpus-survey.py:176-181` `continue`s before any item counter; 2,334 + 3 + 1,620 = 3,957 exactly, so the survey *is* § 2.4's oracle). **Recommendation, made rather than acted on:** this is loop 3, a new *structural* draft defect appeared (three declared types with no derivation rules), and the document is now ~900 lines. Phase 5's own trigger says that is a size signal, not a thoroughness shortfall. The natural seam is the one § 2.3 just had to name — bullets-to-items versus the structural walk — and it is the user's call, not this run's. |
| impl (ANTS-3764) | 2026-07-31 | none — an implementation, not a review | — | **Implementation row, written by the author; no reviewer was dispatched.** ANTS-3764 shipped § 2.3's five-field widening — `sourceStatus`, `source`, `idToken`, `passDesignator`, `firstLine`/`lastLine` — behind `tests/features/roadmap_parse_widening/`, each field proven RED before it existed. Two things only building it could produce. **One § 2.3 clause was false and is amended:** `[Cl9]` does not reach migration as *no id at all* — ANTS-1987 added a leading-bracket rule to the reader for exactly that shape, so `rec.id` is `Cl9`, which is what § 3's INV-3 has said all along while § 2.3 said the opposite. The row survives on measurement instead: `id` is **positionless** (a bullet whose slot reads `[Cl9]` and whose prose cites `[ANTS-9999]` reports the citation — now asserted, not inferred), it cannot separate `parsed` from `quarantined`, and the shape the old sentence actually described is **`[ANTS-119&]`, on 7 bullets of this project's own ROADMAP**, refused by the strict matcher and by ANTS-1987's rule alike and therefore genuinely id-less. INV-4's fixture now requires it, since a fixture holding `Cl9` alone leaves the field's justification untested. **And § 2.8 gained the `Source:` reading rules**, which no section owned: measured against the corpus, `Kind:`'s stop-at-first-period would truncate 61 of 1282 values mid-value (4.8%), 157 occurrences are inline rather than line-leading, 24 write the label bold, and 10 write a second trailer key on the same line — four shapes a first-period rule inherited by resemblance would have got wrong, found by running the new matcher over the whole corpus rather than over its fixtures. **Not reviewed:** § 2.11 is new — it was written by loop 3's fixes and has never been read cold by anyone. The gate is capped at 3 loops by decision, so this is a note, not a deferral. |
