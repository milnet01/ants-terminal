# ANTS-3766 — Roadmap migration: rotated archives as additional sources

**Status:** spec draft (2026-08-01).
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
   `dev-experience`). `RoadmapStore` declares `UNIQUE (project_id, slug)` on
   `section` (`src/roadmapstore.cpp`, the `section` DDL; see also
   `RoadmapStore::findSection()`'s contract comment), so a second source
   emitting `performance` aborts the load.
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
| Project roots holding a live roadmap | 10 |
| …of those, holding ≥ 1 conforming archive | 1 (this project) |
| Archive files here | 2 — `0.5.md`, `0.6.md` |
| Emoji bullets in them | 0 and 20 |
| …of those 20, shipped (✅) / still-open (💭) | 18 / 2 |
| Sections (`##`/`###`) in them | 1 and 6 |
| Non-conforming entries in `docs/roadmap/` | 0 |

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
`ants-v1` on both sides. When it does **not**
— an archive whose detected format differs from `sources[0]`'s —
`findRoadmaps()` refuses the whole call with `archive_format_mismatch` rather
than guessing. A per-source parse would be defensible; a refusal is chosen
because a format difference between a file and its own archive means one of
them is mis-detected, and migrating half a project under a mis-detection is
the failure that is hardest to notice afterwards.

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
   matches the **case-sensitive** regex `^[0-9]+\.[0-9]+\.md$`.

Both rules are `roadmap-format.md` § 3.9's, quoted rather than invented — that
section already fixes the directory, the regex and the sort.

- **Regular files only; symlinks are not followed.** A *directory* named
  `0.7.md` matches the regex, and a symlink under `docs/roadmap/` can point
  outside the project root — the filesystem boundary `specs.md` § 5.4 requires
  be stated rather than assumed. Both are skipped with the same
  `archive_unrecognised` note as a misnamed file.
- **Order:** archives follow the live file, sorted by the `(major, minor)`
  integer tuple **descending** — § 3.9's stated contract, adopted rather than
  re-decided. Lexical sort is explicitly wrong there (`0.10` < `0.9`), and this
  project will reach minor 10.
- **Zero-padding is NOT excluded by the regex, whatever § 3.9's prose says.**
  `00.07.md` matches `^[0-9]+\.[0-9]+\.md$` and parses to the same `(0, 7)`
  tuple as `0.7.md`, so the two would be indistinguishable after sorting. Two
  entries parsing to the same tuple refuse the call with `archive_duplicate_minor`
  rather than picking one. (§ 3.9's parenthetical claims the naming rule
  excludes zero-padding; its own regex does not. That contradiction is in the
  standard, not here — § 7 surfaces it.)
- **No archive directory is not a refusal.** Nine of ten roots have none
  (§ 1.1); `findRoadmaps()` returns the live source alone, notes empty.
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
| `archive_duplicate_minor` (above) | `findRoadmaps()` | `nullopt`, no plan |
| `archive_unrecognised` | `findRoadmaps()` | **note**; that entry is skipped, the call succeeds |
| `archive_slug_collision` (§ 2.3) | `planFrom()` | **note**; the load half refuses the plan |

The last row is shaped by the types rather than by preference: `planFrom()`
returns a `MigrationPlan`, not an optional, and ANTS-3757 INV-9 makes it pure
and total — so it has no way to refuse. It therefore records the note and
**does not rename**, leaving the collision visible in the plan; `load()` refuses
a plan carrying that note before it opens a transaction. Detecting it at the
store's `UNIQUE (project_id, slug)` instead would abort mid-transaction with no
line number to report, which is the failure ANTS-3757 INV-12 already exists to
avoid one level down.

### 2.3 Section identity across sources

**Live slugs never move. Archive slugs are namespaced by their source file.**

```
each source, independently : slug = uniqueSlug(seenForThisSource, heading)
then, for an archive <M>.<N> only, the result is prefixed:
    root section (empty slug) -> "<M>-<N>"
    any other heading         -> "<M>-<N>-" + <that source's unique slug>
```

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
is empty today — takes the bare `"<M>-<N>"` form (`0-6`). A heading that
slugifies to the empty string takes the bare form too rather than a dangling
`0-6-`, matching `slugifyHeading()`'s own rule that a slug never ends in a dash.

Two properties this buys:

- **Live slugs do not move.** Eight of the ten projects migrate today
  (ANTS-3765, corpus run 2026-08-01); none is yet loaded into a production
  store, so the exposure is prospective rather than actual — but it lands the
  moment one is. Any scheme that shifted a live slug would fail to re-match
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
bullets today. But nothing bounds it to one section — every slug the live file
can collide with is exposed, and of the archive's 20 bullets **18 are id-less**
(2 carry ids: `ANTS-1001`, `ANTS-1002`), so 18 is the ceiling for one archive.
The defect is unbounded in *time* rather than large in one pass, which is what
makes it worth a design rather than a note.

That is the same defect class ANTS-3765's own § 2.6.1 was amended to fix on
2026-08-01, arriving through a different door. A counter is positional; identity
must not be. Namespacing by the source file is derived from the filename alone,
so it is stable under every edit to every other source.

### 2.4 Positions, partitions and the merged plan

- `position` stays **per section**, contiguous and 0-based (ANTS-3757 § 2.11 /
  ANTS-3757 INV-11). Because no section spans two sources, merging cannot
  perturb it — each section's sequence is built from one file, exactly as today.
- **ANTS-3757 INV-11's partition becomes per source**: every non-blank line of
  source *i* falls inside exactly one carrier whose `sourceIndex == i`. This is
  the amendment § 1 item 3 forces, and it is a strict generalisation — for a
  single-source plan it is the current statement verbatim.
- `MigrationPlan::sources` is ordered and index-stable within a run; a
  carrier's `sourceIndex` is only meaningful against the plan that produced it.
- **A note about a file that is not a source carries `sourceIndex == -1`.**
  `archive_unrecognised` names an entry that was deliberately never read, so it
  indexes nothing and has no line; its filename lives in `detail`. Without this,
  such a note would default to `0` and claim to be about the live roadmap, and
  the per-source partition above would have a carrier it cannot place.

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
of which file it concerns; `detail` echoes the path for a human reader and
nothing parses it.

### 2.6 What the render needs — and the store column it requires

ANTS-3758 regenerates `ROADMAP.md` **from the store**, not from a plan. The plan
is discarded at commit, so `sourceIndex` alone does not reach the render: it is
a plan-lifetime index into a `sources` vector nothing persists. Verified —
`CREATE TABLE section (section_id, project_id, slug, title, level, intro,
parent_id, UNIQUE (project_id, slug))` in `src/roadmapstore.cpp`, and
`RoadmapStore::SectionRow` carries `slug, title, intro, level, parentId`. **No
table in the schema records which file a section came from.**

So the discriminator has to be persisted, and this spec's requirement on the
store is one nullable column:

```sql
ALTER TABLE section ADD COLUMN source_path TEXT;   -- NULL = the live roadmap
```

`NULL` for the live file rather than a literal path, so every already-migrated
project's rows are correct without a backfill. The load half writes it from
`sources[sourceIndex].path`, relative to the project root. ANTS-3758 then
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

Three choices here are judgement, not deduction, and all are mine (Claude,
2026-08-01, drafting) — recorded, per `specs.md` § 4's *Open questions* bullet,
so a reviewer can overturn them rather than reverse-engineer them:

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
  which is real and is why this is a judgement call rather than a deduction.
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
  every `docs/roadmap/` entry matching the case-sensitive `^[0-9]+\.[0-9]+\.md$`,
  ordered by the `(major, minor)` integer tuple descending. *Test:*
  `tests/features/roadmap_migrate_read/` — a fixture root carrying
  `ROADMAP.md` plus `docs/roadmap/{0.5.md,0.6.md,0.10.md}`; asserts the returned
  paths in order. *Breaks when:* the sort is lexical, which puts `0.10` before
  `0.9` — hence the `0.10.md` fixture, without which every ordering
  implementation passes.
- **INV-2** — a root with no `docs/roadmap/` directory plans exactly as it did
  before this change: one source, every carrier's `sourceIndex == 0`, and every
  ANTS-3757 assertion still true. *Test:* same suite — **ANTS-3757's INV-1..13
  are re-run against their existing fixtures** (`antsv1`, `gfm`, `identity`,
  `passes`, `prose`, `empty`, `malformed`, and the `discovery/*` roots) **with
  call sites mechanically retargeted to `findRoadmaps()` / the new `planFrom()`
  and no assertion changed** — § 2.1 removes the old entry points, so
  "unmodified" would be impossible; what must not move is what they assert.
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
  *Test:* same suite — plans the archive fixture twice, once with
  `docs/roadmap/` present and once absent, and compares the index-0 slug list.
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
  per source over the archive fixture. *Breaks when:* `sourceIndex` defaults to
  0 on carriers built from an archive, which makes the archive's lines read as
  live-file lines and leaves both files' partitions false while every count
  stays right.
- **INV-6** — `empty_source` is raised per source with its `detail` naming that
  source's path, and a prose-only archive does not raise it for a live file
  that yielded items. *Test:* same suite — the archive fixture's `0.5.md` leg
  raises exactly one `empty_source` naming `0.5.md`, and none naming the live
  file. *Breaks when:* the condition is evaluated over the merged plan, which
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
  § 3.9's regex raises an `archive_unrecognised` note carrying its filename in
  `detail` and `sourceIndex == -1`, and is not loaded. *Test:* same suite — a
  fixture carrying `0.7.0.md` (a patch suffix, which § 3.9 rejects), `README.md`,
  a **directory** named `0.8.md`, and a **symlink** `0.9.md` pointing outside
  the root; asserts four notes, `sourceIndex == -1` on each, and that no item
  from any of them appears. **The two regular files must each carry a
  uniquely-identifiable item** — otherwise "its items do not appear" is true of
  an empty file whatever the code does, and that leg tests nothing. *Breaks
  when:* the filter is a `*.md` glob, which loads `0.7.0.md` as an archive; a
  silent `continue`, which is the drop this lane exists to prevent; or an
  `entryInfoList()` filter that does not exclude directories, for which the
  directory leg is the only detector.
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
  that refuses the plan. *Test:* `tests/features/roadmap_migrate_read/` (not
  the load suite INV-9 names) — a fixture whose live file carries a heading
  slugifying to `0-6-features`; asserts the note, that the archive section's
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
  being "refuse every archive".
- **INV-12** — two entries in `docs/roadmap/` parsing to the same
  `(major, minor)` tuple refuse the call with `archive_duplicate_minor`.
  *Test:* same suite — a fixture carrying both `0.7.md` and `00.07.md`;
  asserts `nullopt` and the code. *Breaks when:* the tuple is used as a sort
  key without a uniqueness check, which silently keeps whichever the directory
  enumeration returned second — a coin-flip between two files claiming the same
  minor, and § 3.9's prose wrongly implies the regex already prevents it
  (§ 2.2).

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
one whose live roadmap alone is far past this skill's design point. No cache and
no eviction policy, because nothing here outlives the call — but the growth is
linear in released minors and is written down rather than assumed away.

`sourceIndex` adds one `int` per carrier. No new build target, no new library.

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
- **The other nine projects.** None has an archive directory (§ 1.1), so this
  changes nothing for them — which INV-2 is the proof of, not an assumption.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_read/` (INV-1..8, INV-11, INV-12)
and `tests/features/roadmap_migrate_load/` (INV-9). **INV-10 spans both** — the
note and the absence of a rename are asserted in the read suite, the plan's
refusal in the load suite. Label `features;fast`. Each invariant is verified RED
against the mutation its clause names, before the implementation is restored.

### 6.1 Fixtures are copies of the real archives, not hand-written ones

`fixtures/archives/` gains one root per case. The baseline root's live
`ROADMAP.md` is a trimmed file that **must carry the four headings that actually
collide** — `### 🎨 Features`, `### ⚡ Performance`, `### 🔒 Security`,
`### 🧰 Dev experience` — plus the two extra `### ⚡ Performance` repeats that
make the live count 3, because INV-4's mutation only reddens when the counter
has somewhere to move. Its `docs/roadmap/0.5.md` and `0.6.md` are
**byte-identical copies of this project's own archives**, H1 title and all.

Four further roots, each carrying exactly what one invariant's mutation needs
and nothing else: a `0.10.md` alongside `0.5.md`/`0.6.md` (INV-1's numeric
sort); an invalid-UTF-8 archive (INV-7); a root holding `0.7.0.md`,
`README.md`, a directory named `0.8.md` and an outward-pointing symlink
`0.9.md` (INV-8); and one whose archive is in a different format from its live
file (§ 2.1.1's refusal). The collision root — a live heading slugifying to
`0-6-features` — is INV-10's.

This is the lesson from ANTS-3765 applied. Sixteen green invariant tests
coexisted with a loader that could not migrate three of ten real projects,
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
output rather than a transcription. Before the item is closed, run the real
migration over this project's actual root and record in the loop log: the
archive item count reaching the store, the second run's `Outcome`
(INV-9 end-to-end on real data), and the slug list assigned to the two real
archives.

**Both checks run, and each catches what the other cannot.** On ANTS-3765 the
corpus caught what the fixtures missed — sixteen green invariant tests beside a
loader that could not migrate three of ten real projects (§ 6.1). The same pass
also went the other way: a case that only a committed fixture held was absent
from all ten real files. A run of real data is not a superset of a designed
one, so neither check licenses skipping the other.

## 7. Cross-doc impact

**Two of these are amendments to already-shipped specs and are the reason this
document must be signed off before it is implemented, not after.**

- **`docs/specs/ANTS-3756-roadmap-store-schema.md`** — **schema change.**
  `section` gains a nullable `source_path TEXT` (§ 2.6), with a migration for
  existing stores. Required: without it ANTS-3758 cannot re-split the render
  and the first regeneration un-rotates the archive.
- **`docs/specs/ANTS-3765-roadmap-migration-load.md`** — **write side of the
  same column**: `load()` writes `source_path` from
  `sources[sourceIndex].path`, and refuses a plan carrying an
  `archive_slug_collision` note (§ 2.2). Its § 2.6.1 section-identity rule is
  now load-bearing across sources; a pointer to § 2.3.1 here, which is the
  second way that rule can be broken.
- **`docs/specs/ANTS-3757-roadmap-migration-read.md`** — four amendments, each
  annotated `amended by ANTS-3766` per `specs.md` § 5.5 — annotated, never
  renumbered:
  - § 5's archive bullet: its claim that the five plan carriers "all stand" is
    wrong (§ 2.1), and its "every one shipped" claim about the 20 archive
    bullets is wrong (§ 1.1 — 2 are 💭).
  - § 2.2 (discovery) gains a pointer to § 2.2 here; § 2.10's **closed** note-
    code set gains `archive_unrecognised`, `archive_slug_collision`,
    `archive_format_mismatch` and `archive_duplicate_minor`; § 2.11's rule that
    a section's slug comes from one running `seen` set — so a section's slug
    equals the reader's own `sectionSlug` by construction — is false by design
    for every archive section (§ 2.3).
  - INV-1 (discovery), INV-11 (partition) and INV-13 (`empty_source`) are
    annotated as amended.
- **`docs/standards/roadmap-format.md`** — no change *required*, but § 3.9
  carries an internal contradiction this spec had to work around and should not
  silently inherit: its prose says the naming rule forbids zero-padding while
  its own stated regex `^[0-9]+\.[0-9]+\.md$` accepts `00.07.md` (§ 2.2).
  Surfaced for the standard's owner rather than amended here.
- **`CLAUDE.md`** — no change (the migration lane is not in the module map).
- **`docs/subsystems.md`** — the roadmap-migration lane entry names
  `findRoadmap`; update to `findRoadmaps`.
- **`CHANGELOG.md`** — one `Added` entry when this ships.

## Cold-eyes loop log

| Loop | Reviewer | Findings | Resolution |
|---|---|---|---|
| 1 (2026-08-01) | 3 cold `general-purpose` lanes, shared context packet | C 2 · H 7 · M 12 · L 9 · I 2 — 30 verified, 0 unverified | All 30 fixed. **C1:** `archive_unrecognised` had no carrier — `findRoadmaps()` now returns `Discovery{sources, notes}` and `planFrom()` takes it, notes about a rejected file carrying `sourceIndex == -1`. **C2:** the source discriminator died at the load boundary (verified: `section` has no source column), so § 2.6 now requires a nullable `section.source_path`, and ANTS-3756 + ANTS-3765 join § 7. **H1:** `format` is per-file and was left on the plan — moved onto `Source` (§ 2.1.1) with `archive_format_mismatch`. **H6/H7:** the shared-`seen` backstop could rename an archive slug on a live edit — uniquing is now per source and a residual cross-source collision is a refusal, never a rename. Also: "twenty items, all shipped" was false (18 ✅ + 2 💭, the 💭 pair recorded nowhere else); "20 id-less items" overstated (that section holds 3; 18 of 20 are id-less); ANTS-3757 § 2.10 and § 2.11 added to § 7. Two invariants added for the new refusal codes (INV-11, INV-12). Deferred: no TOC at 694 lines (INFO). |
|---|---|---|---|
