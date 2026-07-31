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

Most figures below come from one command, re-runnable; every figure that does
**not** carries its own command where it is stated:

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
`deferred` and `partial` have no target in the five-status enum. § 2.7 answers
it, and the premise turns out to be false.

## 2. Surface

### 2.1 What this half produces

**Two functions, and the split between them is the impurity.** Discovery touches
the filesystem; the transformation does not. New files
`src/roadmapmigrate.{h,cpp}` join **`ants_core_lib`** — `Qt6::Core` only, no
`Qt6::Sql` — so the whole read half is testable without a database. ANTS-3765
consumes the plan and owns every write.

```cpp
namespace RoadmapMigrate {

// One item as migration will file it. Field names follow
// RoadmapStore::ItemWrite (ANTS-3756) where a counterpart exists — see the
// two exceptions below the block; they are named rather than papered over.
struct PlannedItem {
    QString     id;
    QString     idOrigin;      // "parsed" | "synthesised" | "quarantined"
    QString     status;        // one of the five; never a source string
    QString     headline, kind, source, layman, body;
    QString     sectionSlug;
    int         position = 0;
    QJsonObject extras;        // source_status, source_kind — verbatim
    QJsonObject provenance;    // per field: asserted | defaulted | migrated
    // § 2.9 — the allocation obligation rides on the ITEM, not only on a note:
    // ANTS-3765 allocates, and a note keyed on a line number cannot be
    // correlated back to the item it belongs to.
    bool        idAllocationOwed = false;
    bool        closed = false;         // § 3.4's sense: shipped or dropped
    int         firstLine = 0, lastLine = 0;   // 1-based, inclusive
};

// Everything in the source that is NOT an item. Without this the plan cannot
// carry what roadmap-data-model.md § 5.2 requires the model to survive, and
// INV-11 has nothing to count.
struct PlannedElement {
    QString kind;              // "narration" | "table" | "fence"
    QString payload;           // verbatim source text
    QString sectionSlug;
    int     position = 0;
    int     firstLine = 0, lastLine = 0;
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
    QVector<PlannedItem>    items;
    QVector<PlannedElement> elements;
    QVector<Note>           notes;
};

// The IMPURE half — the only function here that touches the filesystem.
// Returns the resolved roadmap path, or nullopt with `error` set (§ 2.2).
std::optional<QString> findRoadmap(const QString &projectRoot, QString *error);

// The PURE half: no filesystem, no clock, no id counter (INV-9).
// `projectName` and `exportSlug` are supplied by the caller, not derived —
// `exportSlug` is ANTS-3756's `project.export_slug`, whose charset the store
// constrains and which nothing in a markdown file carries. `sourcePath` is
// recorded into the plan and never read.
MigrationPlan planFrom(const QString &markdown, const QString &sourcePath,
                       const QString &projectName, const QString &exportSlug);

}  // namespace RoadmapMigrate
```

**Two fields have no `ItemWrite` counterpart today, and that is a dependency
rather than a mismatch to hide.** `ItemWrite` carries no `extras`, so § 2.7's
whole preservation mechanism has nowhere to land; and it files by `sectionId`
where a plan can only know `sectionSlug`, the store row not existing yet. Both
are ANTS-3765's to resolve — § 7 records the `ItemWrite` addition it owes — and
until it does, "the load half copies rather than translates" is true of every
field except these two.

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

All of this is `findRoadmap()`, not `planFrom()`, and the refusal is an **error
return, never a plan note**: a root that will not resolve produces no markdown,
so there is no plan for a note to ride on. That is why § 2.10's codes are the
transformation's alone.

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

**A file that yields no items is reported, not accepted quietly.**
`detectRoadmapFormat()` answers `ants-v1` for input it does not recognise —
including an empty file — so "a format was detected" is no evidence that
anything was understood. A plan with zero items and zero elements raises
`empty_source`. Without it, a discovery or parse regression that silently
emptied a project's roadmap would satisfy every other invariant here, INV-11
included, because there would be nothing left to lose.

### 2.4 What counts as an item

`roadmap-data-model.md` § 7.2 fixes the rule: a bullet is an item when it
carries **both** a status marker and the bold headline `roadmap-format.md` § 3.5
requires. This spec refines the second half — **an id token in the leading slot
satisfies it in place of the bold headline**, since § 7.1 puts the id there and
the corpus writes `- ✅ [ANTS-1234] **…**` with both. That refinement is this
spec's, not § 7.2's, which states only the conjunction.

Both halves are load-bearing, and the corpus says by how much. Dropping the
headline half promotes the status-marked bullets that carry neither an id nor a
bold headline — the survey's `status_no_id_no_headline`, **88 detail lines plus
8 status-legend lines, ~2.4% of 3,955 items**. That corroborates § 7.2's own
"roughly 90 status-marked bullets". Dropping the *marker* half instead is the
larger error, since the corpus holds some 1,370 markerless sub-bullets — but
those carry no marker, so they are not what the headline rule is holding back,
and an earlier draft of this section wrongly summed the two populations.

Two rules the corpus forced that the model does not state:

- **A pass block's status is its FIRST `- **Status**:` line.** The survey prints
  144 pass headings against 154 Status lines; the 10-line gap decomposes as
  every heading having at least one, nine blocks carrying a second, and one
  Status line preceding the first heading entirely:

  ```bash
  # 144 / 154 / 0 headings with none / 1 before the first heading / 9 extra
  python3 - RetroDB/roadmap.md <<'EOF'
  import re,sys
  L=open(sys.argv[1],encoding='utf-8',errors='replace').read().split('\n')
  H=[i for i,l in enumerate(L) if re.match(r'^#### Pass \d+\.\d+',l)]
  S=[i for i,l in enumerate(L) if re.match(r'^\s*-\s+\*\*Status\*\*\s*:',l)]
  B=[(H[k], H[k+1] if k+1<len(H) else len(L)) for k in range(len(H))]
  n=[sum(1 for s in S if a<s<b) for a,b in B]
  print(len(H), len(S), n.count(0), sum(1 for s in S if s<H[0]), sum(x-1 for x in n if x>1))
  EOF
  ```

  First-match-wins is not a new convention; it is what
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
across 4 projects, 29 occurrences in total. Of those, **3 are declarations and
26 are references**:

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

Under the id-shaped grammar `[milnet01]` and `[x86_64]` are indistinguishable
from an id — a letter prefix followed by digits is exactly what an id is, and
tightening the detector to § 3.5.1 to exclude them would lose `[Cl9]` with them.
**Position is therefore the whole discriminator**, and it is sufficient: applied,
it yields exactly the three declarations, with zero false positives.
`roadmap-corpus-survey.py` already implements it and already reports 3.

One gap position alone does not close. A markdown link can occupy the leading
slot — `- 📋 [Some Doc](path.md) — do the thing` — and `[CVE-2017-1000117]`
matches even the strict § 3.5.1 grammar. So a token in the leading slot that is
**immediately followed by `(` or `:`** is a markdown link, not an id:

```bash
# leading-slot tokens that are markdown links (0), vs bullets opening with a
# link one status marker away from that slot (43):
grep -rhcE '^\s*[-*] +([✅🚧📋💭]|\[[ xX]\]) *\[[^]]+\][(:]' */[Rr][Oo][Aa][Dd][Mm][Aa][Pp].md
grep -rhcE '^\s*[-*] +\[[^]]+\][(:]'                          */[Rr][Oo][Aa][Dd][Mm][Aa][Pp].md
```

The clause costs one character check, and the failure it prevents is filing a
CVE number as an item id.

**Two items whose ids fold to the same value are one item's worth of identity
and two items' worth of content.** `roadmap-data-model.md` § 7.1 compares ids
case-insensitively within a project and ANTS-3756's `UNIQUE (project_id,
id_fold)` enforces it, so a source carrying both `Sh-1` and `SH-1` — or the same
id twice — fails at ANTS-3765's insert, in the half that cannot see the source
line that caused it. Migration detects the collision where the lines are still
visible: both items are kept, both are reported as `duplicate_id` with their
line numbers, and neither is silently merged or renamed.

### 2.6 Quarantine, and when it clears

The three declared off-grammar ids are quarantined exactly as
`roadmap-data-model.md` § 7.1 requires: imported with the id **verbatim** —
`Cl9`, the token's text, **without the surrounding brackets**, which are the
markdown that delimits it and not part of any id in the store — with
`id_origin = "quarantined"`, the project's migration completing rather than
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

**Each of the three formats carries status differently, and only one of them is
hard.** Two are a direct transcription of `roadmap-format.md` § 3.5's own
markers:

| Format | Source | Status |
|---|---|---|
| Emoji bullet (§ 3.5) | ✅ / 🚧 / 📋 / 💭 | `shipped` / `in-progress` / `planned` / `considered` |
| GFM task list (§ 3.10.1) | `- [x]` / `- [ ]` | `shipped` / `planned` |
| Pass heading (§ 3.10.5) | the `- **Status**:` word | the word table below |

**`dropped` is never produced by migration, and that is a property of the source
rather than an omission here.** `roadmap-data-model.md` § 7.3 gives it **no
markdown serialisation** — § 3.11 makes a fifth status emoji an anti-pattern —
so nothing in any of the three formats can express it. It enters the store only
through a later curation edit.

Only the pass-headings word vocabulary needs a decision, and the framing
`roadmap-data-model.md` § 9 hands over — "`deferred` and `partial` have none in
§ 7.3's enum, so the choice is between adding statuses and losing information"
(the ANTS-3757 bullet carries the same words) — **is false, and twice over.**
The ROADMAP annotation's re-measurement killed the first half: of the 154
`- **Status**:` values, 122 are `done`. Shipped code kills the second — the
reader's vocabulary has always been **total**:

| Source word | Status | `passStatusKeyword()` writes back |
|---|---|---|
| `done`, `shipped`, `completed` | `shipped` | `shipped` → `done` |
| `in-progress`, `in_progress`, `inprogress`, `doing`, `wip` | `in-progress` | `in-progress` → `in-progress` |
| `deferred`, `considered`, `parked` | `considered` | `considered` → `deferred` |
| `todo`, `planned`, absent, **anything else** | `planned` | `planned` → `todo` |

Migration uses **this table and no other** for the pass-headings word. Matching
is **case-insensitive on the first whitespace-delimited word**, after stripping
leading `*` — the corpus contains `**un-gated (2026-07-05).**`, where the bold
marker is part of the value — and the reader compares against lowercase
literals, so an author's `Done` would otherwise fall to the else-branch and be
filed `planned`.

That `deferred` → `considered` is load-bearing rather than arbitrary: mapping it
to `planned` instead — the reading a fresh author would reach for, and the one
this spec's first draft reached for — breaks the round trip, because a later
flip through `roadmap_log` writes `deferred` back for `considered` and the file
would then re-read as a different status than it was written from. The
right-hand column is a **right**-inverse only: three source words collapse to
`shipped`, so `done → shipped → done` closes but `completed` does not survive a
round trip, which is one more reason § 2.7 keeps the author's string.

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

**And the two cases are not equally trustworthy, so they do not share a
provenance value.** `roadmap-data-model.md` § 7.7 calls `provenance` "the
model's honesty mechanism: without it a defaulted `kind` and an author's
considered `kind` are indistinguishable, and every later reader over-trusts the
corpus." Collapsing a faithful transcription and a lossy guess into one value
destroys exactly that:

| Source | Stored | `provenance.status` | `extras.source_status` | Reported |
|---|---|---|---|---|
| An emoji or a checkbox | its status | `asserted` | — | no |
| A word the reader **names** (`done`, `completed`, `deferred`, `parked`, `todo`, …) | its status | `asserted` | the word | no |
| A word the reader does **not** name (`partial`, `un-gated`, …) | `planned` | `defaulted` | the word | **yes** |
| No Status line at all | `planned` | `defaulted` | — | **yes** |

**A named word is a faithful transcription, so it is `asserted`.** An author
writing `done` chose that status exactly as one writing ✅ did — only the
notation differs, and § 3.5 makes the emoji this project's own notation for the
same five values. Recording that as anything else would say migration invented a
value the author in fact supplied.

**An unnamed word is a guess, so it is `defaulted`** — the same value § 3.3
gives an absent field, because that is what has happened: the source carried
nothing this model can use and a default was applied. `partial (v3.6.20). Phase
A landed` becoming `planned` really does discard that work had started, and a
later reader has to be able to see that `planned` was migration's choice and not
the author's. Inheriting a GUI's display default *silently* is right for a
dialog and wrong for a primary store.

`extras.source_status` carries the author's word in **both** word rows, not only
the lossy one, because `passStatusKeyword()` is a right-inverse: `completed`
maps to `shipped` and writes back as `done`, so the original is the only record
of how the file actually read.

**`migrated` is not used for status at all**, and an earlier draft of this
section using it was the reason this spec briefly proposed amending the
standard. § 7.7 defines `migrated` as "generated by migration itself, with no
source-side counterpart" — written for § 7.2's allocated ids, where nothing
existed before. No status fits that: every one of them has a source-side
counterpart, whether or not the model could use it. Splitting the two cases
removes the need for the amendment and states something truer than the amendment
would have.

Migration never refuses on a status. The vocabulary is total by construction, so
a refusal path here would be unreachable code guarding an impossible state.

### 2.8 Kind, and the fields the corpus does not carry

`roadmap-data-model.md` § 7.4's mapping table is **normative** and generated
from the same survey; this spec applies it rather than restating it. Absent
`Kind:` defaults to `implement` and absent `Source:` to `planned` per § 3.3,
each recording `provenance.<field> = "defaulted"`. `layman`, `priority`,
`resolution` and any non-derivable date have no default and are left empty.

A non-canonical value that is **not** in § 7.4's table is reported and defaulted
to `implement` — not refused. The table was generated from a corpus that grows,
so an unknown value is the expected consequence of a project joining later, and
§ 7.4 already says such a project "re-runs it and extends the table by
amendment".

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
| `duplicate_id` | § 2.5 — two items whose ids fold to the same value. |
| `status_defaulted` | § 2.7 — an else-branch word the reader does not name. |
| `kind_unmapped` | § 2.8 — non-canonical and absent from § 7.4's table. |
| `orphan_status_line` | § 2.4 — a `- **Status**:` line belonging to no pass block. |
| `id_allocation_owed` | § 2.9 — an id-less item, carrying open/closed. |
| `empty_source` | § 2.3 — a file yielding zero items and zero elements. |

Discovery's failures are **not** in this set. `findRoadmap()` returns an error
before any plan exists (§ 2.2), so a missing or case-ambiguous roadmap has no
`MigrationPlan` to be reported on.

## 3. Invariants

- **INV-1** — `findRoadmap()` case-folds the filename, and returns an **error** — never a plan — for two names in one root differing only in case. *Test:* `tests/features/roadmap_migrate_read/` — fixture roots holding `ROADMAP.md` only, `roadmap.md` only, and both; the first two resolve, the third returns `nullopt` with an error naming both files. *Breaks when:* discovery globs `ROADMAP.md` — which silently drops RetroDB, the only project in the corpus using the pass-headings format, and with it all 144 of its items.
- **INV-2** — A bullet becomes an item only with **both** a status marker and an id-or-bold-headline in the leading slot. *Test:* `roadmap_migrate_read` asserts the per-fixture item count equals a **committed expectation file** generated out-of-band by `tools/roadmap-corpus-survey.py` over the same fixtures — the survey is not invoked from the test, so no interpreter joins the `features;fast` path. Regenerating that file is a reviewable diff and must never be done to make the test pass. *Breaks when:* the bold-headline half is dropped — the 96 status-marked bullets carrying neither an id nor a bold headline promote to items. Parity against an independently-written parser is the point: a test written from this spec's own rules can only confirm what the rules say.
- **INV-3** — An **id-shaped** token (§ 2.5's dash-optional grammar, *not* § 3.5.1's) is an id only in the leading slot, and never when immediately followed by `(` or `:`. *Test:* `roadmap_migrate_read` — a fixture carrying `[milnet01]`, `[x86_64]` and `[FOO123]` in prose, `[CVE-2017-1000117]` as a link label in the leading slot, and `[Cl9]` in the leading slot; only `[Cl9]` becomes an item. *Breaks when:* the **leading-slot restriction is dropped** from the id-shaped detector, which promotes all six tokens; or the link clause is dropped, which files the CVE number as an id. **Not** "the detector uses § 3.5.1's grammar anywhere on the line" — an earlier draft named that mutation and it cannot redden this invariant, because none of the six tokens contains the dash § 3.5.1 requires, so a strict detector never matches them wherever it runs. The looser grammar is what makes position load-bearing, and stating which grammar is meant is the fix.
- **INV-4** — A declared off-grammar id is imported **verbatim** with `id_origin = "quarantined"`, and no run rewrites it or allocates it a second id. *Test:* `roadmap_migrate_read` asserts the planned id equals the literal `Cl9` — bracket-free and unrepaired — that `id_origin` is `quarantined`, and that `idAllocationOwed` is false. *Breaks when:* quarantine is implemented as "repair to `CL-9`", which breaks § 3.5.1's append-only rule; or as "treat as id-less", which issues a second identity for an item that already has one. Asserting only that two runs agree would catch neither: a repairing implementation is perfectly deterministic, so that assertion tests INV-9 and not this.
- **INV-5** — Every format's status maps per § 2.7 — emoji and checkbox by direct transcription, the pass word by the reader's table with `deferred` → `considered` — and `dropped` is never produced. *Test:* `roadmap_migrate_read` drives all four emoji, both checkbox states, and every source word in § 2.7's word table through `planFrom()`; asserts no plan anywhere yields `dropped`; and asserts round-trip closure against `PassHeadingWrite::passStatusKeyword()` for the four statuses it covers. *Breaks when:* migration introduces its own word mapping — `deferred` → `planned` is the natural one and it breaks the round trip, since a later `roadmap_log` flip writes `deferred` back for `considered`; or the emoji rows are omitted, which leaves ~99% of the corpus with no defined status and is what an earlier draft of § 2.7 did.
- **INV-6** — A status the reader's vocabulary does **not** name — an unnamed word, or no Status line at all — is stored `planned` with `provenance.status = "defaulted"` and raises `status_defaulted`; a status it **does** name is `asserted` and raises nothing. *Test:* `roadmap_migrate_read` asserts `partial` and `un-gated` are `defaulted` and reported, that a pass block with no Status line is too, and that `done`, `todo`, `deferred`, ✅ and `- [x]` are all `asserted` and silent. *Breaks when:* the else-branch is inherited wholesale, so every value becomes `asserted` — every other invariant here still passes, the statuses are all still legal, and the store quietly gains items whose `planned` is a guess indistinguishable from an author's choice. That is the precise confusion `roadmap-data-model.md` § 7.7 exists to prevent, and no test that only checks the stored status can see it.
- **INV-7** — `extras.source_status` holds the author's verbatim word for every pass-headings status, and for no emoji or checkbox status. *Test:* `roadmap_migrate_read` asserts the exact source string survives for a value with a qualifier tail (`done (v3.20.0, 2026-07-05). Adds catalogs for …`), that `completed` survives as `completed` and not as the `done` its round trip would write back, and that an emoji-format item sets it not at all. *Breaks when:* the normaliser stores its own normalised word, or the first token rather than the whole value — either of which passes INV-5 and INV-6 while discarding the qualifier that made preservation worth doing.
- **INV-8** — Absent `Kind:` / `Source:` default per § 3.3 with `provenance` recording `defaulted`, and a non-canonical value absent from § 7.4's table raises `kind_unmapped` rather than refusing. *Test:* `roadmap_migrate_read` — a fixture item with no `Kind:` plans as `implement`/`defaulted`; one with `bugfix` plans as `fix`; one with `frobnicate` plans as `implement` and raises the note. *Breaks when:* migration requires `Kind:` at write time — which refuses roughly half of every project in the corpus.
- **INV-9** — `planFrom()` is pure: identical input yields an identical plan, and it touches no filesystem, clock or id counter. *Test:* `roadmap_migrate_read` calls it twice on the same fixture and compares field-wise, including `provenance` and note order. *Breaks when:* allocation is done here rather than deferred to ANTS-3765 — a counter read makes the second call differ from the first, which is the cheapest possible detector for exactly the mistake § 2.9 exists to prevent.
- **INV-10** — Pass ids come from `PassHeadingWrite::passIdFromDesignator()`, and every pass block yields exactly one item taking its status from the block's **first** `- **Status**:` line. *Test:* `roadmap_migrate_read` — a fixture with `Pass 43.5`, `Pass 43.5.B` and a block carrying two Status lines whose words differ; asserts ids `PASS-43-5` / `PASS-43-5-B`, one item per block, and the status of the *first* line. *Breaks when:* two separate mutations, one per clause — **(a)** the letter-led sub-designator is dropped from the synthesised id, collapsing `Pass 43.5` and `Pass 43.5.B` to `PASS-43-5`, the regression ANTS-2035 already fixed once in the reader; **(b)** the **last** Status line in a block wins instead of the first, which changes nine blocks in the corpus. "The derivation is reimplemented" is not a mutation — a correct reimplementation stays green — and an earlier draft named it as one.
- **INV-11** — No source content is silently discarded: every item, narration bullet, table row and fenced block is carried by a `PlannedItem` or a `PlannedElement`, and every line not so carried is covered by a `Note`. *Test:* `roadmap_migrate_read` asserts, over the committed fixtures, that the union of every `PlannedItem` and `PlannedElement` line span plus every noted line covers exactly the file's non-blank lines — no gaps, no double-cover. *Breaks when:* a parser branch falls through — the failure mode migration exists to prevent, and the only invariant here that fires on a format nobody anticipated. It requires `MigrationPlan` to carry `elements` and both records to carry line spans: against a plan holding only `items` and `notes`, as an earlier draft declared, the invariant is unsatisfiable rather than merely unmet.
- **INV-12** — Two items whose ids fold to the same value are both kept, both reported as `duplicate_id`, and neither is merged or renamed. *Test:* `roadmap_migrate_read` — a fixture declaring `Sh-1` and `SH-1`; asserts two `PlannedItem`s, both ids verbatim, and one `duplicate_id` note per item naming its line. *Breaks when:* the parser keys items on the folded id — the natural implementation, since ANTS-3756 keys the store that way — which silently drops the second item here and would instead fail ANTS-3765's `UNIQUE (project_id, id_fold)` insert, in the half that can no longer see the source line.
- **INV-13** — A source yielding zero items and zero elements raises `empty_source`. *Test:* `roadmap_migrate_read` plans an empty file and a prose file with no bullets; both raise it. *Breaks when:* the plan is returned empty and silent — which every other invariant here passes, INV-11 included, because an empty plan trivially covers an empty set of carried lines.

## 4. RAM / build cost

`planFrom()` holds one project's plan in memory. The largest roadmap in the
corpus is Ants' own at 2.9 MB / ~1,790 items (`wc -c ROADMAP.md`); a plan carries
the item bodies, so budget **under 3× the source file** — under 10 MB for the
worst project, and never the whole corpus at once, since ANTS-3765 loads one
project per transaction and discards each plan after it commits. That per-project
bound is the eviction policy; there is no cache and nothing accumulates across
projects.

No new external dependency, and the test path adds no interpreter: INV-2's
parity expectation file is generated out-of-band and committed, so
`tools/roadmap-corpus-survey.py` never runs from `test_core`.
`src/roadmapmigrate.{h,cpp}` joins the existing `ants_core_lib` (`Qt6::Core`)
and the test joins the existing `test_core` bundle rather than adding a target.

ANTS-3764 is the larger build-side move and this spec should not undersell it:
it is not a file rename but an extraction of two functions out of an anonymous
namespace in `src/roadmapdialog.cpp` and of their record type out of a `QDialog`
subclass, into `ants_core_lib`. Its cost is that spec's to state.

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

Feature test: `tests/features/roadmap_migrate_read/`, covering INV-1..13, added
to the existing `test_core` bundle's `SOURCES` (never `add_executable` — see
`tests/features/README.md`). Label `features;fast`; no database, no network, no
interpreter.

Fixtures are **committed roadmaps, not the machine's corpus**, which is not
present in CI — one per format, plus a pathological fixture carrying the
reference-vs-declaration tokens, the two-Status-line pass block, the orphan
Status line, the markdown-link-in-leading-slot case, the folded-id collision,
and an empty file.

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
- `docs/standards/roadmap-data-model.md` — two changes, and they are not equal.
  **Neither is an amendment to § 7.7**: § 2.7 uses `asserted` and `defaulted`
  exactly as that section defines them, and an earlier draft's proposal to widen
  `migrated`'s gloss is withdrawn.
  - § 9's open policy question ("how the pass-headings status vocabulary
    normalises") is answered by § 2.7 and should point here. **Scope the
    correction precisely:** the shipped reader falsifies the premise for
    `deferred`, which maps to `considered`. It does *not* give `partial` a
    target — `partial` reaches `planned` through the else-branch, which § 2.7
    concedes discards the fact that work started, and is why the source string
    is preserved rather than mapped. An edit claiming both were falsified
    replaces one wrong sentence with another.
  - § 3.3 quotes "**57%** carry no `Layman:`"; the 2026-07-31 run says 56%.
    Noticed, not edited here — this run did not review that document, and its
    § 2 already says its figures are re-derived by re-running the survey.
- `tools/roadmap-corpus-survey.py` — the stronger rung for § 2.4's and § 2.5's
  inline commands is to fold them into the survey so the figures become output
  rather than prose, exactly as the standard's § 3.3 did for its own. Worth
  doing at implementation, when the parser those counters describe exists.
- `docs/subsystems.md` — the roadmap lane gains the migration files.
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
| 1 | 2026-07-31 | 2 (both cold, same shared packet) | 4 / 8 / 9 / 5 / 0 | 26 verified, 25 fixed, 1 surfaced, 1 dismissed, 1 out-of-scope. **The highest-value finding is one only a mutation could produce: INV-3's own *Breaks when* could not redden it.** The clause named "the detector matches the grammar anywhere on the line", but § 3.5.1's grammar requires a dash before the digits and not one of the six measured tokens has one — so a strict detector never matches them wherever it runs, and the invariant passed under its own break. § 2.5 now states the dash-optional id-shaped grammar explicitly as a *second, looser* detector, and the mutation is restated as dropping the leading-slot restriction. Three further contract gaps: **§ 2.7 defined status for the pass-headings word only**, leaving ~99% of the corpus — every emoji bullet and checkbox — with no rule, and no account of why `dropped` is unreachable; **INV-11 was unsatisfiable against its own types**, since `MigrationPlan` carried only `items` and `notes` with no line spans, so narration, tables and fences had no carrier and nothing to count; and **discovery was specified with no entry point**, its refusal reported as a plan note that a pure `planFrom()` could never produce. `findRoadmap()` is now declared as the impure half and discovery failures are errors, not notes. Also corrected: a **wrong measurement** — "~37% over-count" summed markerless sub-bullets that the rule cannot promote; the real figure is the 96 status-marked bullets lacking id and headline, ~2.4%, which corroborates the standard's own "roughly 90". INV-4 asserted determinism where it meant verbatimness, and INV-10's break ("the derivation is reimplemented") was not a mutation at all; both restated, and INV-12/INV-13 added for folded-id collisions and empty sources. **Surfaced, not fixed:** `provenance.status = "migrated"` does not fit `roadmap-data-model.md` § 7.7's "with no source-side counterpart" — an authored `done` has one — but `asserted` would make migration's guesses indistinguishable from an author's choices. The spec states its position; the standard's gloss needs amending, which is the standard author's call. |
| 2-decision | 2026-07-31 | none — a decision, not a review | — | **Decision row, written by the author; no reviewer was dispatched.** Loop 1's one surfaced finding is closed, and **without** the amendment it asked for. Re-examined, the defect was not that `migrated` fit § 7.7 badly — it was that § 2.7 gave **one** provenance value to two cases that differ in exactly the way `provenance` exists to record. `done` → `shipped` is a faithful transcription: the author chose that status, and only the notation differs from an author writing ✅, which nobody would call a migration artifact. `partial` → `planned` is a lossy guess that discards "Phase A landed". § 7.7's own rationale — "without it a defaulted `kind` and an author's considered `kind` are indistinguishable, and every later reader over-trusts the corpus" — is precisely the distinction the single value destroyed. So a named word (and every emoji and checkbox) is now **`asserted`**, an unnamed word or an absent Status line is **`defaulted`** and reported, and `migrated` is not used for status at all — it keeps § 7.7's meaning, reserved for § 7.2's allocated ids where nothing existed before. **The proposed amendment to `roadmap-data-model.md` § 7.7 is withdrawn**; § 7 now records only two changes to that standard, neither touching the provenance enum. INV-6 and INV-7 restated: INV-6 now asserts the `asserted`/`defaulted` split rather than only the report, and names the mutation that defeats it — inheriting the else-branch wholesale, which leaves every stored status legal and every other invariant green while making a guess indistinguishable from a choice. A fix that removes the need to change a published standard is a better fix than the one that changes it. |