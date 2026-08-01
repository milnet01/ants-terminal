# ANTS-3766 — Roadmap migration: rotated archives as additional sources

**Status:** spec draft, cold-eyes loops 1–3 folded, converged by cap
(2026-08-01) — 78 verified findings fixed, 0 deferred. **Sign-off given
(user, 2026-08-01): amend both shipped specs.** § 2.6's column is written into
ANTS-3756's `CREATE TABLE section` and its write into ANTS-3765 § 2.6; the
amendment shrank to one DDL line and one write once it was established that no
store exists yet to migrate. **Re-gate loop 4 folded** — 27 findings, including
two CRITICAL contract defects; a further loop is owed before implementation.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3766 (ANTS-3757 § 5 exclusion, 2026-07-31; promoted
to a prerequisite by the ANTS-3758 decision of 2026-08-01).
**Blocked by:** ANTS-3757 (read half), ANTS-3765 (load half) — both shipped.
**Blocker for:** ANTS-3758 (publish + consumer cutover).
**Pairs with:** `docs/standards/roadmap-format.md` § 3.9 (archive rotation).

**Layman:** Old finished work that was moved out of the roadmap into archive
files is invisible to the new database. This reads those files back in — and it
has to land before the roadmap file starts being written by machine, or the
first regeneration would quietly delete them.

## 1. Problem

`RoadmapMigrate::findRoadmap()` (`src/roadmapmigrate.cpp`) resolves **exactly
one** file per project root: the file directly in that root whose name
case-folds to `roadmap.md`. `roadmap-format.md` § 3.9 moves closed minors *out*
of that file into `docs/roadmap/<MAJOR>.<MINOR>.md` and deletes them from the
live file. So archived items are not deferred by the current migration — they
are **lost**, and `ANTS-3757` § 5 states that exclusion rather than leaving it
implicit.

Three things make this urgent rather than tidy-up work.

1. **The loss is now destructive, not merely incomplete.** The 2026-08-01
   decision recorded against ANTS-3758 makes a migrated project's `ROADMAP.md` a
   generated artifact, regenerated from the store. Anything the store never
   modelled is gone from the working tree at the first regeneration. That
   decision's own caveat 2 names this spec as the thing that must land first.
2. **Section identity across sources is unspecified, and the collision is
   real today.** Measured — `python3 tools/roadmap-archive-survey.py` (§ 6.3),
   section C: of `docs/roadmap/0.6.md`'s 6 headings, **4** slugify to a slug the
   live file already uses (`features`, `performance`, `security`,
   `dev-experience`). **A second source emitting `performance` does not abort —
   it silently merges into the live file's section**, overwriting its title and
   intro and filing the archive's items there. A crash would be the benign
   outcome; the real one is a corrupted section nobody is told about. § 2.2
   states the mechanism.
3. **`MigrationPlan` cannot describe more than one file**, in two ways rather
   than one. It carries a single `sourcePath`, and every carrier's
   `firstLine`/`lastLine` is 1-based *in that file* — ANTS-3757 INV-11 asserts
   those spans form a partition of the source, so across two files span `42` is
   ambiguous and the invariant is unstatable. It also carries a single
   `format`, which `detectRoadmapFormat()` derives **per file**; left there, an
   archive is parsed under the live file's grammar and its bullets vanish
   without a note (§ 2.1.1).

### 1.1 What is actually at stake

Measured — same script, sections A and B:

| Figure | Value |
|---|---|
| Project roots holding a live roadmap | 11 |
| …of those, holding a `docs/roadmap/` directory at all | 1 (this project) |
| …holding ≥ 1 conforming archive | 1 |
| Archive files here | 2 — `0.5.md` (651 B), `0.6.md` (3,992 B); 4,643 B total |
| Emoji bullets in them | 0 and 20 |
| …of those 20, shipped (✅) / still-open (💭) | 18 / 2 |
| Sections (`##`/`###`) in them | 1 and 6 |
| Non-conforming entries under any root's `docs/roadmap/` | 0 |

**The root count moves, and that is the point of sourcing it.** It was 10
earlier the same day and is 11 now — a project was added mid-session. Read the
proportion, not the integer: **one** project in the corpus has archives, and
every other figure in this spec is derived from that one project's two files.

Eighteen of the twenty are shipped and summarised in `CHANGELOG.md`. **The
other two are not** — `ANTS-1001` (Kitty Unicode-placeholder graphics, "Moved
to 0.7 backlog") and `ANTS-1002` (`EGL_EXT_swap_buffers_with_damage`,
"Deferred") are 💭 items, so a changelog of shipped work does not carry them and
the archive is their **only** record. That is the sharp end of § 1: the volume
is small, but two of these items are live work whose sole copy is a file the
migration currently cannot see, and the first regeneration would delete it.

## 2. Surface

### 2.1 Types — what changes, and what § 5 of ANTS-3757 got wrong

ANTS-3757 § 5 predicted that adding archives changes `findRoadmap()`'s return
and `MigrationPlan`'s `sourcePath`, and asserted that "the item, section,
element, legend and note carriers all stand". **That second half is wrong**, for
the reason in § 1 item 3: a line span is only meaningful with the file it
indexes. Every carrier that holds a line number gains a source discriminator.
This spec corrects that claim; § 7 folds the correction back.

```cpp
namespace RoadmapMigrate {

// `format` MOVES HERE from MigrationPlan. It is detectRoadmapFormat()'s
// per-FILE vocabulary, exactly as sourcePath was, so a plan-level `format`
// would parse an archive under the live file's grammar — see § 2.1.1.
struct Source {
    QString path, markdown;
    QString format;        // "ants-v1" | "github-task-list" | "pass-headings"
};

// Discovery's result. The notes are why this is a struct and not a bare
// QVector: a REJECTED file never becomes a Source, so nothing downstream can
// report it. ANTS-3757 § 2.2 is explicit that a file which does not resolve
// "produces no markdown, so there is no plan for a note to ride on" — which is
// true of its three whole-root refusals and NOT true here, where the call
// succeeds and one entry was dropped.
struct Discovery {
    QVector<Source> sources;   // element 0 is always the live roadmap
    QVector<Note>   notes;     // discovery-time notes; see § 2.2
};

// NEW — replaces findRoadmap(). nullopt + a refusal code on a whole-root
// refusal; otherwise a Discovery whose notes may be non-empty.
std::optional<Discovery> findRoadmaps(const QString &projectRoot,
                                      QString *error);

struct PlannedSection {
    // …every existing field unchanged…
    int sourceIndex = 0;   // indexes MigrationPlan::sources; 0 = live roadmap
};
// The same `sourceIndex` field is added to PlannedItem, PlannedElement,
// PlannedLegend and Note — every carrier that holds a line number, because a
// line number is meaningless without the file it indexes.

struct MigrationPlan {
    QVector<Source> sources;   // REPLACES `QString sourcePath` AND `format`
    // …every other field unchanged…
};

// The pure half. All sources in, ONE plan out — and the discovery notes, so
// they reach the plan's `notes` without planFrom() touching the filesystem
// (ANTS-3757 INV-9 purity is preserved: they arrive as an argument).
// REPLACES the single-source form; no forwarding overload is kept. Measured —
// `grep -n 'planFrom' tests/features/roadmap_migrate_read/test_roadmap_migrate_read.cpp`
// returns 7 lines of which 4 are calls, and 2 of those 4 are the test's own
// `planFixture()` helpers, so every downstream assertion moves with them.
// `grep -n 'findRoadmap' <same file>` returns 9 lines, 8 of them calls. At
// that size a second entry point costs more in ambiguity than it saves in
// churn, and coding.md § 2 prefers the single surface.
MigrationPlan planFrom(const Discovery &discovery,
                       const QString &projectName, const QString &exportSlug);

}  // namespace RoadmapMigrate
```

A single-source plan is **field-for-field equivalent** to what the previous
shape produced, apart from the carriers that necessarily moved: `sourcePath`
and `format` become `sources[0].path` / `sources[0].format`, and every
carrier gains `sourceIndex == 0`. It is not *byte-identical* — the struct
changed — and INV-2 is worded against the equivalence, not the bytes.

#### 2.1.1 `format` is per source, and a mixed-format project is a refusal

`detectRoadmapFormat()` classifies **one file**. Left on the plan, an archive
written in a different format from the live roadmap is parsed under the wrong
grammar and its bullets are silently unrecognised — the exact loss class this
spec exists to prevent, reintroduced by the field nobody moved.

So `format` rides on `Source`. Rotation (§ 3.9) is content-preserving — it
moves bullets byte-identically — so an archive normally inherits the format of
the file it was cut from, and the only project with archives today is
`ants-v1` on both sides. When it does **not** — an archive whose detected
format differs from `sources[0]`'s — `findRoadmaps()` refuses the whole call
with `archive_format_mismatch` rather than guessing. A per-source parse would
be defensible; a refusal is chosen because a format difference between a file
and its own archive means one of them is mis-detected, and migrating half a
project under a mis-detection is the failure that is hardest to notice
afterwards.

**A source with no format signal inherits `sources[0]`'s format and never
triggers that refusal.** The predicate has to be computable **inside
`findRoadmaps()`**, which is where the refusal is raised (§ 2.2's table) and
which has no plan: **a source has no format signal when the detector's scan
matched no bullet of any grammar.** So `detectRoadmapFormat()` gains a way to
say so — an out-parameter reporting whether its classification rested on
evidence — and this spec requires that addition of `src/roadmapparse.cpp`.

**Not "yields zero items", which an earlier draft of this section used and
which cannot work here.** Item yield is a `planFrom()` result, and § 2.5's
`empty_source` is computed on the plan, so a discovery-time refusal defined on
it is unevaluable where it is raised. The two predicates are also genuinely
different: a source can carry bullets that `isItem()` rejects, which is a
format signal and zero items. Defining one in terms of the other would have
made a bullet-less-but-mismatched archive turn on which half ran first.

That predicate is needed precisely because the detector's return value is
ambiguous. `detectRoadmapFormat()` (`src/roadmapparse.cpp`) classifies from
bullet grammar and **falls back to `ants-v1` when it finds none** — both on an
empty line list and at the end of its scan — so a genuine `ants-v1` file and a
file with no evidence at all return the identical string. A bullet-less
archive therefore reports `ants-v1` on no evidence, and `docs/roadmap/0.5.md`
is exactly that file: 651 bytes of prose, zero bullets (§ 1.1). On this
project the fallback happens to agree with the live file and nothing fires.
On a `github-task-list` project it would not, and a prose-only archive would
refuse the migration of the one thing this spec exists to migrate. The
inheritance rule removes a refusal that would be triggered by the *absence* of
evidence rather than by a conflict.

**One plan per project is forced, not chosen.** `RoadmapMigrateLoad::load()`
(`src/roadmapmigrateload.h`) is declared "one plan, one project, one
transaction". Per-source plans would mean N transactions per project and would
break that spec's § 2.5 atomicity, so merging is the only shape the load half
accepts. The compiler enforces it — there is no invariant here for a signature.

### 2.2 Discovery

`findRoadmaps()` resolves, in this order:

1. **The live roadmap** — exactly ANTS-3757 § 2.2's rule, unchanged: the file
   directly in the root whose name case-folds to `roadmap.md`. Same three
   refusals (`not_found`, `case_ambiguous`, `not_utf8`). Always index 0.
2. **Archives** under `<root>/docs/roadmap/`, each **regular file** whose name
   matches the case-sensitive regex **`^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$`**
   — stated here once and cited, never restated, everywhere else in this spec.

**Neither rule is invented here, and they have different owners.** Rule 1 is
ANTS-3757 § 2.2's, unchanged. Rule 2's *directory* and *sort* are
`roadmap-format.md` § 3.9's, adopted as they stand; its **name regex is
deliberately tighter** than § 3.9's stated one, for the reason two bullets
down.

- **Regular files only; symlinks are not followed — the predicate is
  `isFile() && !isSymLink()`.** Both halves have to be written out because Qt
  will not give them for free: `QFileInfo::isFile()` and a `QDir::Files` filter
  **follow** symlinks and report a symlink-to-file as a file, so the natural
  implementation loads the symlink. A *directory* named `0.7.md` matches the
  regex, and a symlink under `docs/roadmap/` can point outside the project root
  — the filesystem boundary `specs.md` § 5.4 requires to be stated rather than
  assumed. Both are skipped with the same `archive_unrecognised` note as a
  misnamed file.
- **Order:** archives follow the live file, sorted by the `(major, minor)`
  integer tuple **descending** — § 3.9's stated contract, adopted rather than
  re-decided. Lexical sort is explicitly wrong there (`0.10` < `0.9`), and this
  project will reach minor 10.
- **Why that regex is tighter than § 3.9's.** The standard's prose already says
  "no zero-padding" while its own stated regex `^[0-9]+\.[0-9]+\.md$` accepts
  `00.07.md` — which parses to the same `(0, 7)` tuple as `0.7.md`, making the
  two indistinguishable after sorting. Tightening to match the prose removes
  that case by construction rather than adding a rule to detect it: every
  conforming name now maps to a distinct tuple, so there is no duplicate-minor
  condition to refuse. A zero-padded name is simply non-conforming and takes
  the `archive_unrecognised` branch below. (The contradiction is in the
  standard; § 7 surfaces it rather than amending it here.)
- **A missing archive directory is not a refusal, and it is the only silent
  case.** Every project in the corpus but this one has none (§ 1.1);
  `findRoadmaps()` returns the live source alone, notes empty.
- **A `docs/roadmap` that exists but cannot be enumerated — it is a file, or
  the directory is unreadable — raises `archive_unrecognised` naming
  `docs/roadmap` itself**, and the call succeeds with the live source alone. An
  earlier draft folded this into the missing-directory branch, which made the
  one case that loses *every* archive at once the quietest thing this lane can
  do — strictly worse than the misnamed single file two bullets down, which
  does get a note. Absent and unreadable are not the same fact and must not
  share an outcome.
- **An archive matching the regex that cannot be opened** (permissions, a
  broken symlink target) takes the same `archive_unrecognised` note, named by
  its own filename. `findRoadmap()` maps an open failure on the *live* file to
  `not_found`; an archive is skippable where the live file is not, so it takes
  the note rather than the refusal.
- **A non-conforming entry raises `archive_unrecognised`** naming the filename
  in the note's `detail`, and is not loaded. It is never a silent skip: a
  misnamed archive (`0.7.0.md`, `v0.7.md`) is exactly the shape whose silent
  loss this whole lane exists to prevent, and § 3.9 rejects those names on
  purpose.
- **An archive that is not valid UTF-8 refuses the whole call** with
  `not_utf8`. Partial migration of a project is worse than none: the load half
  is one transaction, and a plan silently missing one archive would commit as
  though complete.

**Refusals carry a code and nothing else; notes carry the filename.** ANTS-3757
fixes `*error` as "the REFUSAL CODE alone — and no prose"
(`src/roadmapmigrate.h`), and this spec does not widen it. That is exactly why
`Discovery` carries `notes`: a caller that must know *which* archive failed
reads the note, not the error string.

| Code | Raised by | Effect |
|---|---|---|
| `not_found`, `case_ambiguous`, `not_utf8` | `findRoadmaps()` | inherited; `nullopt`, no plan |
| `archive_format_mismatch` (§ 2.1.1) | `findRoadmaps()` | `nullopt`, no plan |
| `archive_unrecognised` | `findRoadmaps()` | **note**; that entry is skipped, the call succeeds |
| `archive_slug_collision` (§ 2.3) | `planFrom()` | **note**; the load half refuses the plan |

**Every refusal returns `nullopt` and a code, and names no file.** That is
ANTS-3757's error contract inherited unchanged, and it is a deliberate
limitation rather than an oversight: a refusal aborts the whole project's
migration, so the operator's next step is to inspect `docs/roadmap/`, which
holds a handful of files. Naming the file matters only where the call
*succeeds* and one entry was dropped — and that case, `archive_unrecognised`,
does name it, because it rides on a `Discovery` that exists.

The last row is shaped by the types rather than by preference: `planFrom()`
returns a `MigrationPlan`, not an optional, and ANTS-3757 INV-9 makes it pure —
totality follows from that return type — so it has no way to refuse. It
therefore records the note and **does not rename**, leaving the collision
visible in the plan; `load()` refuses a plan carrying that note before it opens
a transaction. That refusal is what the note is *for*, because the store does
**not** catch this: ANTS-3765 § 2.6.1 resolves every section with
`findSection()` and calls `addSection()` only for a genuinely-new slug, so a
duplicate slug is silently **merged** into the existing row — title and intro
overwritten, the archive's items filed into the live section — rather than
rejected by `UNIQUE (project_id, slug)`.

### 2.3 Section identity across sources

**Live slugs never move. Archive slugs are namespaced by their source file.**

```
each source, independently:
    base = slugifyHeading(heading)
    if base is empty : base = "h" + <ordinal>      # 1-based, over ##/### headings
    slug = uniqueSlug(seenForThisSource, base)     # UNPREFIXED, always
then, for an archive <M>.<N> only, the result is prefixed:
    root section (empty slug) -> "<M>-<N>"
    any other heading         -> "<M>-<N>-" + <that source's unique slug>
```

**Uniquing runs on the UNPREFIXED name, before the prefix is applied** — every
heading, including the synthesised `h<ordinal>`. That ordering is the whole
mechanism and reversing it breaks it: a `seen` set holding unprefixed slugs
never compares against an already-prefixed `0-6-h3`, so a synthesised `0-6-h3`
inserted post-prefix would sit beside a real `### H3` (which slugifies to `h3`
and prefixes to `0-6-h3`) without either seeing the other, and the two would
collide exactly as INV-13 forbids.

**Uniquing runs per source, over that source's own `seen` set** — never one set
shared across sources. Two consequences, and both are the point:

- The live file's slugs are computed exactly as they are today, from a set
  containing only its own headings, so they are unchanged by the presence or
  absence of any archive.
- Two identical headings *within one archive* unique against each other
  (`0-6-performance`, `0-6-performance-2`) deterministically and silently. This
  is ordinary and expected — the live file already carries `### ⚡ Performance`
  three times (survey section D) and archives are cut from it — so it is **not**
  a collision and raises no note.

The archive's synthetic root section — every source produces one, and its slug
is empty today — takes the bare `"<M>-<N>"` form (`0-6`), and **only the root
may take that form.**

**A heading that slugifies to the empty string takes `"<M>-<N>-h<ordinal>"`**,
where `ordinal` is that heading's 1-based position among the source's
`##`/`###` headings — the third such heading in `0.6.md` gives `0-6-h3`. The
block above is the statement of how; what matters is that `h<ordinal>` is
substituted for the empty base **before** `uniqueSlug()`, so it uniques against
every other heading of that source like any ordinary name. Minting a name
outside `seen` would reproduce the very bypass this rule exists to close.

It cannot take the bare form and it cannot take a
dangling `0-6-`: `RoadmapIndex::uniqueSlug()` **returns an empty base
immediately without uniquing it and without inserting it into `seen`** —
`if (base.isEmpty()) return base;` — so every empty-slugifying heading yields
`""`, and the prefix would map all of them, plus the synthetic root, onto the
single slug `0-6`. Under § 2.2's silent-merge behaviour those become one
section, not a refusal. An emoji-only `### 🎨` is one edit away from this
project's own `### 🎨 Features`, so this is a live shape rather than a
hypothetical. Ordinal position is a safe key here precisely because an archive
is immutable by contract — § 3.9 writes it once at rotation and nothing edits
it afterwards — so INV-4's "function of its own file and heading alone" still
holds.

> **A pre-existing defect this exposes, in ANTS-3757 rather than here.** The
> live file has the same hole: two headings that both slugify to empty produce
> two sections with slug `""`, which merge. Nothing in this spec makes it worse
> and nothing here fixes it; § 7 surfaces it for ANTS-3757's owner.

Two properties this buys:

- **Live slugs do not move** (§ 2.3's rule, not a second one). Eight of the ten
  projects then in the corpus
  migrated cleanly at ANTS-3765's run of 2026-08-01, the remaining two blocked
  on colliding ids (ANTS-3772); none is yet loaded into a production store, so
  the exposure is prospective rather than actual — but it lands the moment one
  is. Any scheme that shifted a live slug would fail to re-match
  every item filed under it (ANTS-3765 § 2.6.1 keys id-less items on *the same
  section*), orphaning and re-inserting the corpus on the next run.
- **An archive slug is a function of its own file and heading alone**, so it is
  stable under every edit to every other source. § 2.3.1 is why that has to be
  true rather than merely nice.

**A prefixed archive slug that still equals a live slug is a REFUSAL, never a
rename.** It requires a live heading slugifying to something like
`0-6-features`, which no corpus file has; but the remedy matters more than the
likelihood, because renaming the archive's slug to `0-6-features-2` would shift
an archive slug in response to a live-file edit — reintroducing precisely the
orphan cascade § 2.3.1 rules out, just more rarely. So `planFrom()` records
`archive_slug_collision` naming both sections and does not rename, and the load
half refuses the plan (§ 2.2's table).

#### 2.3.1 Why not simply share the uniquing counter

The mechanism already exists — `RoadmapIndex::uniqueSlug(QSet<QString> &seen,
…)` takes its accumulator by reference, so threading one `seen` set across
sources in a fixed order is a two-line change and produces legal, unique,
deterministic slugs. **It is also unsafe, and the corpus shows exactly how.**

`performance` already appears **3** times in the live file (survey section D),
so the live pass emits `performance`, `performance-2`, `performance-3`, and the
archive's would become `performance-4`. Add one more `### ⚡ Performance` to the
live roadmap — an ordinary week's edit — and the archive's slug shifts to
`performance-5` on the next run. Under ANTS-3765 § 2.6.1 that is a *different
section*, so its id-less items no longer match: they are re-inserted with
freshly allocated ids and the old rows are orphaned, on every subsequent edit,
against a source nobody touched.

The blast radius is the shifted section, not the archive: that section holds 3
bullets today, of which **2 are id-less** — `ANTS-1002` sits in it and matches
by id regardless of section, so it is unaffected. But nothing bounds the defect
to one section: every slug the live file can collide with is exposed, and of
the archive's 20 bullets 18 are id-less, so 18 is the ceiling for one archive.
The defect is unbounded in *time* rather than large in one pass, which is what
makes it worth a design rather than a note.

That is the same defect class ANTS-3765's own § 2.6.1 was amended to fix on
2026-08-01, arriving through a different door. A counter is positional; identity
must not be. Namespacing by the source file is derived from the filename alone,
so it is stable under every edit to every other source.

### 2.4 Positions, partitions and the merged plan

- `position` stays **per section**, contiguous and 0-based (ANTS-3757 § 2.11).
  Cited to that section alone: ANTS-3757 INV-11 is the **line-span partition**,
  which the next bullet is about, and citing it here too would put one id
  behind two different properties. Because no section spans two sources, merging cannot
  perturb it — each section's sequence is built from one file, exactly as today.
- **ANTS-3757 INV-11's partition becomes per source**: every non-blank line of
  source *i* falls inside exactly one carrier whose `sourceIndex == i`. This is
  the amendment § 1 item 3 forces, and it is a strict generalisation — for a
  single-source plan it is the current statement verbatim.
- `MigrationPlan::sources` is ordered and index-stable within a run; a
  carrier's `sourceIndex` is only meaningful against the plan that produced it.
- **Only `sources[0]` may plan a legend.** `MigrationPlan` holds one
  `std::optional<PlannedLegend>` and the legend belongs to the **project**, not
  to a file (ANTS-3757 § 2.11), so N sources cannot each contribute one. An
  archive's status-legend run is planned as `narration` in that archive's own
  section instead, which costs nothing: every line is still carried, so INV-5's
  per-source partition holds. `PlannedLegend` carries a `sourceIndex` for the
  same reason every other line-bearing carrier does (§ 2.1) — its span needs a
  file — and not because the value is informative; under this rule it is always
  `0`. Dropping the losing legend instead would leave its lines in
  no carrier and make INV-5 unsatisfiable rather than merely unmet.
- **An item's or element's `sectionSlug` is the section's final slug**,
  prefixed form included — `0-6-features`, not `features`. It is the same
  string § 2.3 assigns; stated because ANTS-3757 § 2.11 guarantees the two
  agree "by construction" via one running `seen` set, and that construction no
  longer produces it.
- **A duplicate id across sources is not a new case.** The store keys items on
  `(project_id, id_fold)`, which is source-blind, so an id appearing in both the
  live file and an archive collides — and that is exactly ANTS-3757 § 2.5's
  existing `duplicate_id` rule, which keeps both items and reports each. Nothing
  new is required. Measured: the archive's only two id-bearing bullets,
  `ANTS-1001` and `ANTS-1002`, appear nowhere else —
  `workspace_search 'ANTS-1001|ANTS-1002'` matches only `docs/roadmap/0.6.md`
  and unrelated test fixtures.
- **A note about a file that is not a source carries `sourceIndex == -1`.**
  `archive_unrecognised` names an entry that was deliberately never read, so it
  indexes nothing and has no line; its filename lives in `detail`. Without this
  it would default to `0` and claim to be about the live roadmap. It does not
  disturb the partition above — ANTS-3757 INV-11 excludes notes from the union
  by design — so the defect is purely that the note would name the wrong file.

### 2.5 `empty_source` becomes per-source

ANTS-3757 § 2.3 raises `empty_source` when a source yields zero **items**.
`docs/roadmap/0.5.md` yields zero items legitimately — it is 651 bytes of prose
pointing at `CHANGELOG.md` (§ 1.1).

Evaluated over the **merged plan**, that condition silently stops firing: the
plan holds the live file's items, so it is not empty, and the fact that one
archive parsed to nothing is never reported at all. **The failure is a lost
signal, not a spurious one** — which is the harder direction, because a
prose-only archive and an archive the parser failed on are then
indistinguishable, and the second is a real defect this note is the only
detector for.

The rule: **`empty_source` is raised per source.** No suppression, no
archive-specific exemption. A prose-only archive genuinely contributed no items
and a human should see that said out loud — the note is informative, and notes
have never been failures. The note's `sourceIndex` is the canonical identifier
of which file it concerns, and a test asserts on that rather than on prose.
`detail` carries the **filename** — § 2.2's wording and INV-8's, not a path —
and **is** assertable for the one code that has
no index to assert on, `archive_unrecognised`, whose `sourceIndex` is `-1`
(§ 2.4) because the file was never a source. For every other code `detail` is a
human-readable echo and nothing depends on its wording.

### 2.6 What the render needs — and the store column it requires

ANTS-3758 regenerates `ROADMAP.md` **from the store**, not from a plan. The plan
is discarded at commit, so `sourceIndex` alone does not reach the render: it is
a plan-lifetime index into a `sources` vector nothing persists. Verified —
`CREATE TABLE section (section_id, project_id, slug, title, level, intro,
parent_id, UNIQUE (project_id, slug))` in `src/roadmapstore.cpp`, and
`RoadmapStore::SectionRow` carries `slug, title, intro, level, parentId`. **No
table in the schema records which file a section came from.**

So the discriminator has to be persisted, and this spec's requirement on the
store is one nullable column, added to ANTS-3756's `CREATE TABLE section`:

```sql
  source_path TEXT,          -- NULL = the live roadmap (ANTS-3766)
```

`NULL` for the live file rather than a literal path, because `NULL` is what the
live roadmap honestly means *and* it is what any row written before the column
reads back as. The two arguments coincide, so no backfill is ever needed or
attempted.

**Added to the DDL in place, at `kSchemaVersion` 1 — not by `ALTER TABLE`, and
not with a version bump.** There is no store to migrate: every translation unit
including `roadmapstore.h` is under `src/roadmap*` or in the test suite, so
nothing user-facing can open one and none exists outside a test's temp
directory. There is also nothing to hang an `ALTER` on —
`RoadmapStore::createSchema()` (`src/roadmapstore.cpp`) refuses a version above
its own, no-ops on an equal one and runs the DDL at 0, with no branch between;
a store in between falls through to that same DDL, which is deliberately
written **without** `IF NOT EXISTS` — so a second creator fails loudly instead
of succeeding silently, which is why ANTS-3756 INV-15 makes the in-transaction
`user_version` read the discriminator and says `IF NOT EXISTS` cannot be one. And `tests/features/roadmap_export_roundtrip/golden/*.jsonl`
each carry `"schema":1`, which `RoadmapExport` refuses on mismatch. A bump
would therefore manufacture the upgrade case nothing implements, and
regenerate three goldens, in order to migrate zero stores.

**The freedom expires at ANTS-3758's cutover**, which is what first makes the
store reachable. ANTS-3781 carries the missing upgrade path and the
misattribution in ANTS-3756 that this argument exposed; a schema change landing
after the cutover gets no such shortcut.

**The column needs a reader, not only a writer.** `RoadmapStore::SectionRow`
(declared by ANTS-3765 § 2.4) carries `slug, title, intro, level, parentId`, and
`readSection()` returns it — so INV-14's read-back has nothing to read through
the typed surface, and ANTS-3758 would have to reach past it into raw SQL. So
`SectionRow` gains `std::optional<QString> sourcePath` and `readSection()`
populates it. `std::optional` rather than an empty `QString`, matching
`parentId` in the same struct: here the `NULL`/`''` distinction *is* the
semantics — `NULL` is the live roadmap, while `''` would be a root-relative
path naming nothing.

**The stored value is project-root-relative, and something must relativise it.**
`Source::path` is **absolute** — `findRoadmap()` builds it from
`fi.absoluteFilePath()` (`src/roadmapmigrate.cpp`) and § 2.1 does not redefine
it — so writing `sources[sourceIndex].path` verbatim would store
`/home/…/docs/roadmap/0.6.md`, which makes the store machine-specific and never
matches the membership test below. `load()` performs the conversion, against the
`projectRoot` it already holds in `Options`, and **both sides are canonicalised
first**:

```cpp
QDir(QFileInfo(projectRoot).canonicalFilePath())
    .relativeFilePath(QFileInfo(source.path).canonicalFilePath())
```

`Source::path` itself stays absolute so ANTS-3757's discovery invariants are
untouched. **Canonicalising both sides is what makes INV-14's symlink leg
satisfiable, and neither side alone would do.** `absoluteFilePath()` does not
resolve symlinks, so a root reached through one yields
`/home/…/link/docs/roadmap/0.6.md`; canonicalising only the root then computes
a relative path *out of* the project (`../link/docs/…`), and canonicalising
only the source has the mirror defect. Both, and the spelling of the root a
caller happened to pass stops mattering — which is also what keeps this column
consistent with `project.root`, keyed on `canonicalFilePath()` by ANTS-3756
INV-8.

ANTS-3758 then
re-emits the split by the rule § 3.9 already fixes: a section whose
`source_path` matches `docs/roadmap/<M>.<N>.md` belongs in that archive,
`NULL` belongs in the live file. **No `isArchive` flag** — the path is the
discriminator and the naming regex is the test.

**This is a change to two already-shipped specs and is surfaced, not assumed:**
ANTS-3756 owns the schema and ANTS-3765 owns the write. § 7 lists both. Without
the column, the first regeneration writes the archived bullets back into
`ROADMAP.md` — a silent un-rotation, and the same loss class as the ANTS-3758
caveat this spec exists to clear.

### 2.7 Preference calls and a rejected alternative

Three choices here are judgement, not deduction, and all three are drafting
calls recorded on 2026-08-01 rather than consequences of anything above. They
are written down — rather than left implicit in the design — so a reviewer can
overturn them instead of reverse-engineering them.
They are **not** open questions in `specs.md` § 4's sense: each is resolved, and
this section says who resolved it and why.

- **Scoping per-source provenance into this spec rather than ANTS-3758.** It
  could live with the render that consumes it. It is here because the plan is
  where the information exists — once merged, nothing downstream can recover
  which file a section came from — and because § 1 item 3 shows ANTS-3757
  INV-11 is unstatable across sources without it. Surfaced to the user at
  drafting time; the alternative is to let ANTS-3758 re-derive it from a second
  parse, which is a second reader and ANTS-3757 § 2.3 forbids that.
- **Requiring a `section.source_path` column rather than letting ANTS-3758
  infer archive membership.** § 2.6 shows inference has nothing to work from
  once the plan is discarded. The cost is an amendment to two shipped specs,
  which is real and is why this is a judgement call rather than a deduction —
  though smaller than it looks: one DDL line and one write, because there is no
  store yet to migrate (§ 2.6).
- **Adopting § 3.9's descending sort rather than choosing oldest-first.**
  Ordering does not affect identity under § 2.3, so this is free choice; the
  standard already states a sort, and a second one would be a second rule.

**Rejected: recover a section's source by string-matching its slug prefix,
storing nothing.** Sections are namespaced per source (§ 2.3), so `0-6-features`
does encode its origin, and this would need neither `sourceIndex` nor the
`source_path` column. It loses on three counts. `Note` carries no section and
the legend belongs to the project rather than any section, so the derivation
does not cover the carriers that most need it. The encoding is ambiguous: a
*live* section legitimately titled `## 0.6 features` slugifies into the archive
namespace, and no parse can tell the two apart. And recovering structure by
parsing an identifier re-introduces exactly the string-coupling the store's
typed columns exist to remove — a rule the store already applies to itself,
since it keys items on `(project_id, id_fold)` rather than on parsed text.

## 3. Invariants

- **INV-1** — `findRoadmaps()` returns the live roadmap at index 0 followed by
  every `docs/roadmap/` entry matching **§ 2.2's regex** (which forbids leading
  zeros — cited, not restated, so this invariant cannot drift from it),
  ordered by the `(major, minor)` integer tuple descending. *Test:*
  `tests/features/roadmap_migrate_read/` — a fixture root carrying
  `ROADMAP.md` plus `docs/roadmap/{0.5.md,0.6.md,0.10.md}`; asserts the returned
  paths in order — numeric descending gives `0.10, 0.6, 0.5`. *Breaks when:* the
  sort is lexical, which orders `0.10` **last** rather than first, since
  `"0.10" < "0.6"` as strings. The `0.10.md` fixture is what makes the two
  orderings differ at all; against `{0.5, 0.6}` alone every implementation
  passes.
- **INV-2** — a root with no `docs/roadmap/` directory plans exactly as it did
  before this change: one source, every carrier's `sourceIndex == 0`, and every
  ANTS-3757 assertion still true. *Test:* same suite — **ANTS-3757's INV-1..13
  are re-run against their existing fixtures** (`antsv1`, `gfm`, `identity`,
  `passes`, `prose`, `empty`, `malformed`, and the `discovery/*` roots) **with
  call sites mechanically retargeted to `findRoadmaps()` / the new `planFrom()`
  and no assertion changed** — § 2.1 removes the old entry points, so
  "unmodified" would be impossible; what must not move is what they assert.
  **§ 7 annotates three of them as amended and that is consistent with this**:
  ANTS-3757's INV-1, INV-11 and INV-13 are each *generalised* to N sources, and
  each generalisation degenerates to its original statement when N is 1 — a
  root with no archives is exactly that case, so their assertions hold
  unchanged here while their wording moves.
  That is what makes this invariant
  more than self-report; plus an added leg asserting `sources.size() == 1`,
  `sourceIndex == 0` on every carrier, and — load-bearing — **the full ordered
  section-slug list of each fixture against a committed golden**,
  `tests/features/roadmap_migrate_read/expected-section-slugs.json`, captured
  from the shipped single-source implementation before this change lands.
  *Breaks when:* the merge path namespaces or re-numbers unconditionally rather
  than only for indices ≥ 1, which re-slugs every project's live sections and
  orphans its corpus on the next run. **The golden list is what makes that
  mutation redden here, and measuring this was worth it** (2026-08-01,
  drafting): `grep -c slug
  tests/features/roadmap_migrate_read/test_roadmap_migrate_read.cpp` → **3
  lines**, of which one is a debug label and one a field-wise dump compared only
  against *another run of the same code*, so a systematic re-slug is invisible
  to it. Only the root-section check at `sawRoot` would have caught the
  mutation, and only incidentally. **Not** "the single-source overload is
  dropped": there is no overload (§ 2.1), so a mutation removing one cannot
  redden anything.
- **INV-3** — with archives present, every live-source section slug is
  byte-identical to the slug the same root produces with the archives removed.
  *Test:* same suite — plans the **baseline** root and the `noarchivedir/`
  root, which is a copy of it with `docs/roadmap/` removed (§ 6.1), and
  compares the index-0 slug list. **Two committed roots rather than a deletion
  performed at test time**: a test that removes a directory from a committed
  fixture leaves the tree dirty and every later assertion in the run reading a
  root that no longer holds what the table says it holds.
  *Breaks when:* the uniquing set is shared with archives processed first, or
  archives are merged before the live file.
- **INV-4** — an archive section's slug is a function of that archive's
  filename and its own heading alone: appending a section to the live roadmap
  leaves every archive slug unchanged. Absolute — there is no case in which a
  live-file edit renames an archive slug, because § 2.3 makes the one residual
  collision a refusal rather than a rename. *Test:* same suite — plans the
  archive fixture, then re-plans it with `### ⚡ Performance` appended to the
  live file, and asserts the archive slug list is identical. *Breaks when:*
  slugs come from a counter shared across sources (§ 2.3.1), which passes
  INV-1, INV-2, INV-3 and every ANTS-3757 invariant and then grows the store
  without bound on a live-file edit; **or** when a cross-source collision is
  resolved by renaming the archive's slug, which is the same defect reached
  through a rarer door.
- **INV-5** — every carrier holding a line number carries the `sourceIndex` of
  the file that line is in, and ANTS-3757 INV-11's partition holds **within
  each source**: every non-blank line of source *i* lies inside exactly one
  carrier with `sourceIndex == i`. *Test:* same suite — asserts the partition
  per source over the archive fixture, **including a leg whose archive carries
  its own status-legend run**, which § 2.4 demotes to `narration`. *Breaks
  when:* `sourceIndex` defaults to 0 on carriers built from an archive, which
  makes the archive's lines read as live-file lines and leaves both files'
  partitions false while every count stays right; **or** the archive's legend
  is dropped rather than demoted, which puts its lines in no carrier at all —
  the legend leg is the only one that can see that, because every other fixture
  has no legend to lose.
- **INV-6** — `empty_source` is raised per source, carrying the `sourceIndex`
  of the source that yielded no items, and a prose-only archive does not raise
  it for a live file that yielded items. *Test:* same suite — the baseline
  archive fixture raises exactly one `empty_source`, whose `sourceIndex`
  resolves through `plan.sources` to `docs/roadmap/0.5.md`, and none resolving
  to the live file. **Asserted on `sourceIndex`, not on `detail`** (§ 2.5): a
  human message is not assertable, which is ANTS-3757 INV-1's own rule. *Breaks when:* the condition is evaluated over the merged plan, which
  raises nothing at all here (the live file has items) and so **silently drops
  the signal** that an archive parsed to nothing — the inverse failure to the
  one § 2.5 describes, and the reason this invariant asserts the note's
  presence rather than its absence.
- **INV-7** — an archive whose bytes are not valid UTF-8 refuses the whole
  call with `not_utf8`, and no plan is produced. *Test:* same suite — a fixture
  whose `docs/roadmap/0.6.md` holds an invalid sequence; asserts `nullopt` and
  the code. *Breaks when:* the archive is skipped with a note instead, which
  commits a transaction that looks complete and is not.
- **INV-8** — an entry in `docs/roadmap/` that is not a regular file matching
  **§ 2.2's regex** raises an `archive_unrecognised` note carrying its filename
  in `detail` and `sourceIndex == -1`, and is not loaded. *Test:* same suite —
  the `unrecognised/` fixture (§ 6.1), carrying `0.7.0.md` (a patch suffix),
  `00.07.md` (zero-padded — the case withdrawn INV-12 hands to this invariant),
  `README.md`, a **directory** named `0.8.md`, and a **symlink** `0.9.md`
  pointing outside the root; asserts **five** notes, `sourceIndex == -1` on
  each, and that no item from any of them appears. **The three regular `.md`
  files must each carry a uniquely-identifiable item** — otherwise "its items
  do not appear" is true of an empty file whatever the code does, and that leg
  tests nothing. *Breaks when:* the filter is a `*.md` glob, which loads
  `0.7.0.md` as an archive; the loose `^[0-9]+\.[0-9]+\.md$` is used, which
  loads `00.07.md`; a silent `continue`, which is the drop this lane exists to
  prevent; an `entryInfoList()` filter that does not exclude directories, for
  which the directory leg is the only detector; or one that tests `isFile()`
  alone, which **follows** the symlink and loads it (§ 2.2) — the symlink leg
  is the only detector for that, and it is the mutation a correct-looking Qt
  filter falls into.
- **INV-9** — archive items survive an edit to the **live** file: after a first
  load, appending a section to the live roadmap and loading again leaves every
  archive item `itemsUnchanged`, with zero archive items inserted or orphaned.
  *Test:* `tests/features/roadmap_migrate_load/` — loads the archive fixture,
  appends `### ⚡ Performance` to its live file, loads again against the same
  store, and asserts the second `Outcome` restricted to archive-sourced items.
  **The live-file edit between the two loads is the whole invariant**
  (2026-08-01, drafting): stated as "load the same input twice" this clause is
  vacuous, because a counter-based slug scheme is perfectly deterministic and
  reproduces its own slugs exactly on an unchanged re-run — it would pass
  against the very design § 2.3.1 rejects. *Breaks when:* archive slugs depend
  on live-file content (§ 2.3.1), which re-files the affected section's items
  under a shifted slug and orphans the originals. This is the end-to-end
  detector for INV-4, and the shape that would have caught the ANTS-3765
  § 2.6.1 defect measured on 2026-08-01.
- **INV-10** — a prefixed archive slug equal to a live slug produces an
  `archive_slug_collision` note naming both sections, no rename, and a load
  that refuses the plan. *Test:* **both suites** —
  `tests/features/roadmap_migrate_read/` for the note and the absent rename,
  `tests/features/roadmap_migrate_load/` for the refusal. A fixture whose live
  file carries a heading slugifying to `0-6-features`; asserts the note, that
  the archive section's
  slug is still `0-6-features`, and that `tests/features/roadmap_migrate_load/`
  refuses that plan. *Breaks when:* the collision is resolved by renaming
  (INV-4's second mutation); or the note is dropped and the duplicate reaches
  the store, which aborts on `UNIQUE (project_id, slug)` mid-transaction with
  no line number to report. **Its "never fires on the real corpus" half is
  covered by § 6.3's corpus run, not by this fixture** — a fixture built to
  make it fire cannot also show it does not.
- **INV-11** — `format` is detected per source, and an archive whose detected
  format differs from `sources[0]`'s refuses the whole call with
  `archive_format_mismatch`. *Test:* `tests/features/roadmap_migrate_read/` — a
  fixture whose live file is `ants-v1` and whose `0.6.md` is a GitHub task
  list; asserts `nullopt` and the code, and a sibling leg asserts that an
  archive matching its live file's format plans normally. *Breaks when:*
  `format` stays on the plan and is detected from the live file alone — under
  which this fixture's archive parses to **zero items with no note at all**,
  which every other invariant here passes, because a plan that never saw those
  bullets cannot report them missing. The sibling leg is what stops the fix
  being "refuse every archive". **A third leg covers the inheritance rule**, on
  the `inherit/` root (§ 6.1) — `mixedformat/` cannot host it, since neither of
  its legs has a `github-task-list` live file: a *bullet-less* archive under a
  `github-task-list` live file plans normally, inheriting `github-task-list`,
  and raises no `archive_format_mismatch`. **It is also the leg that tests the
  evidence flag § 2.1.1 adds**, since the no-signal predicate is the only thing
  distinguishing this archive from a genuinely mismatched one.
  *That leg breaks when:* inheritance is omitted — the archive then reports
  `detectRoadmapFormat()`'s evidence-free `ants-v1` default, mismatches, and
  refuses the migration of a project whose archive is merely prose. The real
  corpus cannot catch this, because its live file is `ants-v1` and the default
  agrees with it by luck.
- **INV-12** — *withdrawn — 2026-08-01, never implemented and never referenced.*
  It required an `archive_duplicate_minor` refusal for two entries
  parsing to the same `(major, minor)` tuple. § 2.2 now forbids leading zeros
  in the name regex, which makes the name→tuple map injective and removes the
  condition by construction rather than detecting it. The id is tombstoned
  rather than reused, per `specs.md` § 5.5. The zero-padding case it covered is
  now INV-8's, as an `archive_unrecognised` entry.
- **INV-13** — a section whose heading slugifies to the empty string takes
  `<M>-<N>-h<ordinal>` and never the bare `<M>-<N>` the synthetic root holds,
  so no two sections of one archive share a slug. *Test:*
  `tests/features/roadmap_migrate_read/` — a fixture archive carrying an
  emoji-only `### 🎨` heading alongside its H1 title and prose intro; asserts
  the root section's slug is `0-6` and the emoji heading's is `0-6-h<n>`, both
  present and distinct. *Breaks when:* the empty-slug case is routed through
  `uniqueSlug()` and then prefixed — which is the natural implementation and is
  silently wrong, because that function returns an empty base **without
  inserting it into `seen`**, so it never uniques and every such heading
  collapses onto the root's slug. INV-10 cannot catch this: the collision is
  *within* one source and INV-10's note fires only archive-against-live.
- **INV-14** — the store's persisted discriminator is correct and
  machine-independent: after a load, `section.source_path` is SQL `NULL` for
  every section from the live roadmap and the **project-root-relative** path
  (`docs/roadmap/0.6.md`) for every section from an archive. *Test:*
  `tests/features/roadmap_migrate_load/` — loads the baseline archive fixture
  and reads back every section row; asserts `NULL` for live sections, the exact
  relative string for archive sections, and that loading the same fixture again
  **through a differently-spelled but equivalent root** (a trailing slash, and
  a path through a symlink) stores byte-identical values. *Breaks when:*
  `load()` stores `sources[sourceIndex].path` verbatim, which is absolute
  (`fi.absoluteFilePath()`) — the store then works on the machine that wrote it
  and ANTS-3758's membership test never matches anywhere else; **or either side
  of § 2.6's relative computation is left un-canonicalised**, which the
  symlinked-root leg is the only detector for — the trailing-slash leg passes
  against it, because `QDir` normalises that much on its own. This is the only
  invariant covering § 2.6, which is the whole reason this spec blocks
  ANTS-3758; without it the column ships untested.

## 4. RAM / build cost

No new build target, no new library, no new external dependency —
`src/roadmapmigrate.{h,cpp}` in `ants_core_lib`, Qt6::Core only, as today.

Memory, stated as the true delta rather than a rough one. `MigrationPlan`
previously held **no markdown at all** — it carried a `QString sourcePath`, and
the markdown was a `planFrom()` parameter owned by the caller. It now holds
`sources`: N paths *and* N markdown bodies, and it retains them for the plan's
whole lifetime, which extends past `planFrom()` to the end of
`RoadmapMigrateLoad::load()`. That is a real increase, not a re-labelling.

The bound is **not** "§ 3.9 caps it" — § 3.9 caps nothing. One archive
accumulates per closed minor, indefinitely: today 2 files / 4,643 bytes
(§ 1.1), and the honest worst case is *N* minors each up to the ~150 KiB
rotation threshold that produced them. At the current release cadence that is
single-digit megabytes after years, held transiently during one migration and
freed at its end; a project that ever approached a problematic figure would be
one whose live roadmap alone is far past this migration's design point. No cache and
no eviction policy, because nothing here outlives the call — but the growth is
linear in released minors and is written down rather than assumed away.

`sourceIndex` adds one `int` per carrier.

### 4.1 Migration / compatibility

The only on-disk change is § 2.6's `section.source_path`, which lands in
ANTS-3756's DDL at `kSchemaVersion` 1 — no migration, no backfill, no version
bump. § 2.6 carries the argument and is the only statement of it in this
document, because a second statement of an upgrade procedure is a second
procedure.

- **The `findRoadmap()` → `findRoadmaps()` rename is source-level only.** It is
  not a released public API: `find_caller` reports its callers as the test
  suite and this lane's own headers, and `remotecontrol.cpp`'s similarly-named
  `findRoadmapUnder()` (ANTS-1459) is an unrelated function that is not touched.
- **No behaviour changes for a project with no archives** — INV-2 is the proof,
  and it is why that invariant re-runs ANTS-3757's whole suite rather than
  asserting something weaker.

## 5. Out of scope

- **Writing archives back out / re-splitting the render** — ANTS-3758. This
  spec supplies the discriminator (§ 2.6) and stops there.
- **Teaching `roadmap_query` to read archives.** A permanent exclusion, not
  deferred: `roadmap-format.md` § 3.9 states that verb reads only the current
  `ROADMAP.md` and archives are dialog-only by contract. Changing that is an
  amendment to the standard affecting every project, and nothing here needs it.
- **Extending `tools/roadmap-corpus-survey.py` to archives.** A permanent
  exclusion, not deferred work, so it carries a reason rather than a follow-up
  id. The survey exists to be an *independently written* oracle for ANTS-3757
  INV-2; teaching it this spec's rules would make it a second implementation of
  them and destroy the independence that is its whole value. § 6.2 states what
  the archive fixtures do instead and what that costs.
- **Rotating new archives.** `/bump` owns rotation (§ 3.9); this spec only
  reads what rotation already produced.
- **Every other project in the corpus.** None has a `docs/roadmap/` directory
  at all — not merely no *conforming* archive, so none even reaches the
  `archive_unrecognised` branch (§ 1.1, survey section B, which reports the
  directory's presence and the non-conforming count per root for exactly this
  claim). This spec changes nothing for them, and INV-2 is the proof rather
  than an assumption.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_read/` (INV-1..8, INV-11, INV-13)
and `tests/features/roadmap_migrate_load/` (INV-9, **INV-14**). **INV-10 spans
both** — the note and the absence of a rename are asserted in the read suite,
the plan's refusal in the load suite. INV-12 is withdrawn and has no surface.
INV-14 is named explicitly because it is the only invariant covering § 2.6,
which is what makes this spec block ANTS-3758; omitted from this list, the
store column ships untested. Label
`features;fast`. Each invariant is verified RED
against the mutation its clause names, before the implementation is restored.

### 6.1 Fixtures are copies of the real archives, not hand-written ones

`fixtures/archives/` gains one root per case. The baseline root's live
`ROADMAP.md` is a trimmed file that **must carry the four headings that actually
collide** — `### 🎨 Features`, `### ⚡ Performance`, `### 🔒 Security`,
`### 🧰 Dev experience` — plus the two extra `### ⚡ Performance` repeats that
make the live count 3, because INV-4's mutation only reddens when the counter
has somewhere to move. Its `docs/roadmap/0.5.md` and `0.6.md` are
**byte-identical copies of this project's own archives**, H1 title and all.

Eight further roots, each carrying exactly what one invariant's mutation needs
and nothing else:

| Root | For | Carries |
|---|---|---|
| `sort/` | INV-1 | `0.10.md` alongside `0.5.md` / `0.6.md` |
| `badutf8/` | INV-7 | an archive holding an invalid byte sequence |
| `unrecognised/` | INV-8 | `0.7.0.md`, `00.07.md`, `README.md`, a **directory** `0.8.md`, an outward symlink `0.9.md` — **all three regular files** each carrying a findable item, since a leg whose file is empty asserts nothing |
| `mixedformat/` | INV-11 | an `ants-v1` live file and a `github-task-list` archive, plus a sibling leg whose archive matches |
| `inherit/` | INV-11 leg 3 | a **`github-task-list`** live file and a bullet-less prose archive — the inheritance leg, which `mixedformat/` cannot host because neither of its legs has a `github-task-list` live file |
| `noarchivedir/` | INV-3 | a copy of the baseline root with `docs/roadmap/` **removed** — the with/without pair INV-3 compares, as a committed root rather than a deletion performed at test time |
| `collision/` | INV-10 | a live heading slugifying to `0-6-features` |
| `emptyslug/` | INV-13 | an archive with an emoji-only `### 🎨` heading beside its H1 title and prose intro |

**`INV-4` and `INV-9` mutate their fixture** — both append a section to the live
file between two plans. Each copies its root to a temp directory first and
operates there; a test that edits a committed fixture in place corrupts every
later assertion in the same run and shows up as a dirty working tree.

This is the lesson from ANTS-3765 applied. Sixteen green invariant tests
coexisted with a loader that, **on that spec's first corpus pass**, could not
migrate three of ten real projects — two after the fixes that pass triggered
(ANTS-3773, ANTS-3769), which is the figure § 2.3.1 quotes —
because every fixture named its own sections and headlines and so never built
the shapes real files contain. A hand-written archive fixture would have a
tidy `## Features` heading and would never produce the collision this spec is
about.

### 6.2 What the fixtures do NOT cover

`tools/roadmap-corpus-survey.py` is ANTS-3757 INV-2's independent oracle, and it
is blind to archives. The archive fixture roots therefore sit one level deeper
so the survey never walks them — the same exclusion `fixtures/discovery` already
carries, recorded in `expected-counts.json`'s `_excluded` map for the same
reason. **The cost is stated rather than hidden:** archive item counts are
checked against this spec's own rules, not against an independently written
parser, so **ANTS-3757 INV-2**'s parity property does not extend to them (that
spec's INV-2, not this one's). § 6.3 is what covers the gap instead.

### 6.3 The corpus run is part of the work, not a follow-up

`tools/roadmap-archive-survey.py` — the script that produced every figure in
§ 1.1 and § 2.3.1 — is committed with this change, so the spec's numbers are an
output rather than a transcription. **It is a measuring instrument, not an
oracle, which is why committing it does not reopen § 5's exclusion of
`tools/roadmap-corpus-survey.py`:** that script is ANTS-3757 INV-2's
independent second implementation and its value is being written without
reference to this spec's rules, whereas this one only reports what is in
`docs/roadmap/` and no invariant is asserted against its output. Before the item is closed, run the real
migration over this project's actual root and record in the loop log: the
archive item count reaching the store, the second run's `Outcome`
(INV-9 end-to-end on real data), and the slug list assigned to the two real
archives.

**Both checks run, and each catches what the other cannot.** On ANTS-3765 the
corpus caught what the fixtures missed — sixteen green invariant tests beside a
loader that could not migrate three of ten real projects (§ 6.1). The same pass
also went the other way (the three-of-ten figure is that pass's *first* run;
§ 6.1): a case that only a committed fixture held was absent
from all ten real files. A run of real data is not a superset of a designed
one, so neither check licenses skipping the other.

## 7. Cross-doc impact

**Three of these are amendments to already-shipped specs — ANTS-3756,
ANTS-3765 and ANTS-3757 — which is the reason this document had to be signed
off before it was implemented, not after.** The first two are **applied
(2026-08-01)**, after the user's sign-off on § 2.6; the ANTS-3757 annotations
and every non-spec carrier below are still outstanding. Each bullet says which
it is, because a § 7 read as a to-do list is how a landed amendment gets
applied twice.

- **`docs/specs/ANTS-3756-roadmap-store-schema.md`** — **schema change,
  APPLIED 2026-08-01.**
  `section` gains a nullable `source_path TEXT` **in the DDL, at
  `kSchemaVersion` 1** — no migration, no version bump, for the reasons § 2.6
  gives, and `SectionRow` / `readSection()` gain the matching
  `std::optional<QString> sourcePath` so the column has a reader. Required:
  without the column ANTS-3758 cannot re-split the render and the first
  regeneration un-rotates the archive. Its upgrade-ownership
  sentence — *"a lower `user_version` is an upgrade, which ANTS-3757 owns"* —
  is corrected in the same pass to name **ANTS-3781**, which is where that
  still-unbuilt path now lives.
- **`docs/specs/ANTS-3765-roadmap-migration-load.md`** — **write side of the
  same column, APPLIED 2026-08-01**, exactly as § 2.6 specifies it and not
  restated here:
  `load()` writes `source_path` **`NULL` for `sourceIndex == 0`** and the
  **root-relative** conversion of `sources[sourceIndex].path` otherwise. It
  also refuses a plan carrying an `archive_slug_collision` note (§ 2.2). Its § 2.6.1 section-identity rule is
  now load-bearing across sources; a pointer to § 2.3.1 here, which is the
  second way that rule can be broken.
- **`docs/specs/ANTS-3757-roadmap-migration-read.md`** — the bullets below,
  each annotated `amended by ANTS-3766` per `specs.md` § 5.5 — annotated, never
  renumbered. The last two are **surfaced only**: this spec neither causes nor
  fixes them, and they are recorded so they are not lost.
  - § 5's archive bullet: its claim that the five plan carriers "all stand" is
    wrong (§ 2.1), and its "every one shipped" claim about the 20 archive
    bullets is wrong (§ 1.1 — 2 are 💭).
  - § 2.2 (discovery) gains a pointer to § 2.2 here, **and its refusal-code
    enumeration** (`not_found` | `case_ambiguous` | `not_utf8`, also stated in
    `src/roadmapmigrate.h`) gains `archive_format_mismatch`. Its § 2.10
    **note**-code set — a different closed set — gains `archive_unrecognised`
    and `archive_slug_collision`. The split matters: of the **three** new codes
    (§ 2.2's table) **one is a refusal and two are notes**, and filing all three
    into § 2.10 would leave the refusal vocabulary unamended. Three, not four —
    `archive_duplicate_minor` died with INV-12's withdrawal.
  - § 2.11's rule that a section's slug comes from one running `seen` set — so
    a section's slug equals the reader's own `sectionSlug` by construction — is
    false by design for every archive section (§ 2.3).
  - **A pre-existing defect, surfaced not fixed:** `uniqueSlug()` returns an
    empty base without inserting it into `seen`, so two live headings that both
    slugify to empty already produce two sections with slug `""`, which
    ANTS-3765 § 2.6.1 merges silently. This spec avoids it for archives (§ 2.3,
    INV-13) and does not touch the live path.
  - `src/roadmapmigrate.h`'s comment that "`sourcePath` is recorded into the
    plan and never read" becomes false — § 2.6 reads it.
  - **ANTS-3757's** INV-1 (discovery), INV-11 (partition) and INV-13
    (`empty_source`) are annotated as amended — that spec's numbers, written
    out because this document has an INV-11 and an INV-13 of its own meaning
    something else entirely. Each generalises to N sources and degenerates to
    its current statement at N = 1 (INV-2).
- **`src/roadmapparse.cpp` / `.h`** — **a source change this spec requires, and
  the only one outside the migration lane.** `detectRoadmapFormat()` gains a way
  to report whether its classification rested on evidence (§ 2.1.1); without it
  the no-format-signal predicate is not computable in `findRoadmaps()`, which is
  where the refusal is raised. The existing signature keeps working — the
  evidence flag is an out-parameter, so `RoadmapDialog` and every other caller
  is untouched.
- **`docs/standards/roadmap-format.md`** — no change *required*, but § 3.9
  carries an internal contradiction this spec had to work around and should not
  silently inherit: its prose says the naming rule forbids zero-padding while
  its own stated regex `^[0-9]+\.[0-9]+\.md$` accepts `00.07.md`. § 2.2 adopts
  the tightened `^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$`, which is what the
  prose already describes; the standard should adopt the same. Surfaced for its
  owner rather than amended here.
- **`tests/features/roadmap_migrate_read/spec.md`** — its opening line is a
  test contract "for `RoadmapMigrate::findRoadmap()` and
  `RoadmapMigrate::planFrom()`"; both signatures change, and its Fixtures
  section gains the nine archive roots (§ 6.1 — one baseline plus eight).
- **`tests/features/roadmap_migrate_read/expected-counts.json`** — its
  `_excluded` map gains the archive roots and the reason (§ 6.2), and a new
  `expected-section-slugs.json` lands beside it (INV-2's golden).
- **`CLAUDE.md`** — no change (the migration lane is not in the module map).
- **`docs/subsystems.md`** — the roadmap-migration lane entry names
  `findRoadmap`; update to `findRoadmaps`.
- **`CHANGELOG.md`** — one `Added` entry when this ships.

## Cold-eyes loop log

| Loop | Reviewer | Findings | Resolution |
|---|---|---|---|
| 4 (2026-08-01) — re-gate after the § 2.6 sign-off | 3 cold `general-purpose` lanes, one shared byte-identical context packet, no prior-loop context | C 2 · H 7 · M 8 · L 10 — 27 verified, 2 dismissed | All 27 fixed. **Origin split: 25 draft defects vs 2 fix collateral** — draft defects dominate by an order of magnitude, so the loop was the right remedy and no ratio trigger fires. The two collateral are from this session's § 2.6 amendment pass: § 7 still stating the ANTS-3756 / ANTS-3765 amendments as pending after they landed, and § 2.6 misattributing INV-15's discriminator to the absence of `IF NOT EXISTS` when ANTS-3756 says the in-transaction `user_version` read is the discriminator and `IF NOT EXISTS` cannot be one. **C1 — the format-mismatch predicate was unimplementable, and loop 3 introduced it**: that loop resolved an undefined inheritance rule by defining "no format signal" as *yields zero items*, which is a `planFrom()` result, while the refusal is raised in `findRoadmaps()`, which has no plan. All three lanes reported it independently. Now an evidence out-parameter on `detectRoadmapFormat()` (a source change § 7 now carries), computable where the refusal lives; the false identity with § 2.5's `empty_source` is deleted, since bullets `isItem()` rejects are a format signal and zero items. **C2 — § 2.3's `h<ordinal>` rule reproduced the bypass it exists to close**: the prose put the *prefixed* name through `seen`, which never compares against the unprefixed slugs already in it, so a real `### H3` and a synthesised `h3` both reached `0-6-h3`. Uniquing now runs on the unprefixed base for every heading, stated once in the pseudo-code block. **A count conflict that verification saved from a wrong fix:** "three of ten real projects" against "the remaining two" are both true — three at ANTS-3765's first corpus pass, two after the fixes that pass triggered — so both are now moment-qualified rather than reconciled to one number. Also: INV-14 absent from § 6's test mapping (the one invariant covering § 2.6); an unreadable `docs/roadmap` silently dropping every archive in a spec whose § 2.2 promises "never a silent skip"; § 6.1's fixture table two roots short of what INV-3 and INV-11's third leg need; § 2.6's relativisation un-canonicalised on both sides, which INV-14's own symlink leg would have failed. |
| 3 (2026-08-01) — **converged by cap** | 3 cold `general-purpose` lanes, same brief, no prior-loop context | C 2 · H 5 · M 5 · L 8 · I 3 — 20 verified, 0 dismissed | All 20 fixed. **Origin split: ~18 of 20 were fix collateral from loop 2, and all three lanes independently reported the same top two** — which is Phase 5's ratio trigger on its second consecutive loop, so the run stops here rather than taking a fourth pass to repair its own third. Both criticals were one fact stated twice and disagreeing: the loop-2 paragraph making a refusal return a populated `Discovery` contradicted the table, INV-7 and INV-11 (resolved to `nullopt` + code everywhere); and INV-1 still carried the loose name regex loop 2 had tightened in § 2.2, which re-admitted the exact `00.07.md` case withdrawn INV-12 existed for. Fixed by **deleting N−1** — the regex, the silent-merge mechanism and the relativisation rule are each now stated once and cited. Also: INV-8 asserted four notes against a five-entry fixture; § 7 instructed the ANTS-3765 amendment to write the absolute path § 2.6 forbids; the `h<ordinal>` form bypassed `seen` exactly as the defect it fixed did; the format-inheritance predicate was undefined and untested (now "yields zero items", with an INV-11 leg); and § 2.6 — the spec's only persisted output — had no invariant at all, now **INV-14**. `tools/roadmap-archive-survey.py` was tightened in step with the regex and prints its own pattern so the two cannot drift. |
| 2 (2026-08-01) | 3 cold `general-purpose` lanes, same brief, no prior-loop context | C 1 · H 6 · M 11 · L 10 · I 2 — 28 verified, 2 dismissed | All 28 fixed. Origin split: **~12 fix collateral vs ~8 draft defects** — loop 1's own fixes generated the larger share, recorded here because the trend is the signal. **Dismissed on verification:** lane A's Critical (cross-source id collision) — `ANTS-1001`/`ANTS-1002` appear only in `docs/roadmap/0.6.md`, and the case is already ANTS-3757 § 2.5's `duplicate_id` rule; and lane B's claim that `0.5.md` would refuse today — `detectRoadmapFormat()` falls back to `ants-v1`, so it agrees by luck, though the underlying gap was real and is fixed. **Biggest correction: "a second source emitting `performance` aborts the load" was wrong** — ANTS-3765 § 2.6.1 resolves sections with `findSection()`, so a duplicate slug **silently merges**, overwriting title/intro and refiling items. Worse than the claimed abort, and § 1 and § 2.2 now say so. Also: the empty-slug case escaped uniquing entirely (`uniqueSlug()` returns an empty base without inserting it) — new INV-13 and an `<M>-<N>-h<ordinal>` form; the legend had no merge rule against one `optional<PlannedLegend>`; `Source::path` is absolute so `source_path` needed an explicit relativisation; a format-less source now inherits `sources[0]`'s format; the name regex is tightened to forbid leading zeros, which **withdrew INV-12** by removing its condition rather than detecting it. Corpus moved mid-session (10 → 11 roots), so § 1.1 now leads with the proportion. Deferred: no TOC at 864 lines (INFO). |
| 1 (2026-08-01) | 3 cold `general-purpose` lanes, shared context packet | C 2 · H 7 · M 12 · L 9 · I 2 — 30 verified, 0 unverified | All 30 fixed. **C1:** `archive_unrecognised` had no carrier — `findRoadmaps()` now returns `Discovery{sources, notes}` and `planFrom()` takes it, notes about a rejected file carrying `sourceIndex == -1`. **C2:** the source discriminator died at the load boundary (verified: `section` has no source column), so § 2.6 now requires a nullable `section.source_path`, and ANTS-3756 + ANTS-3765 join § 7. **H1:** `format` is per-file and was left on the plan — moved onto `Source` (§ 2.1.1) with `archive_format_mismatch`. **H6/H7:** the shared-`seen` backstop could rename an archive slug on a live edit — uniquing is now per source and a residual cross-source collision is a refusal, never a rename. Also: "twenty items, all shipped" was false (18 ✅ + 2 💭, the 💭 pair recorded nowhere else); "20 id-less items" overstated (that section holds 3; 18 of 20 are id-less); ANTS-3757 § 2.10 and § 2.11 added to § 7. Two invariants added for the new refusal codes (INV-11, INV-12). Deferred: no TOC at 694 lines (INFO). |
|---|---|---|---|
