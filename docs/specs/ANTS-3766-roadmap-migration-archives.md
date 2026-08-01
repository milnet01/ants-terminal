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
3. **`MigrationPlan` cannot describe more than one file.** It carries a single
   `sourcePath`, and every carrier's `firstLine`/`lastLine` is 1-based *in that
   file*. ANTS-3757 INV-11 asserts those spans form a partition of the source;
   across two files, span `42` is ambiguous and the invariant is unstatable.

### 1.1 What is actually at stake

Measured — same script, sections A and B:

| Figure | Value |
|---|---|
| Project roots holding a live roadmap | 10 |
| …of those, holding ≥ 1 conforming archive | 1 (this project) |
| Archive files here | 2 — `0.5.md`, `0.6.md` |
| Emoji bullets in them | 0 and 20 |
| Sections (`##`/`###`) in them | 1 and 6 |
| Non-conforming entries in `docs/roadmap/` | 0 |

Twenty items, all shipped, all already summarised in `CHANGELOG.md`. The
*volume* is small; the *contract* is not, because it is what stops the first
regeneration from silently un-rotating them back into the live file.

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

// Unchanged shape, now one element of an ordered set.
struct Source { QString path, markdown; };

// NEW — replaces findRoadmap(). Element 0 is always the live roadmap.
std::optional<QVector<Source>> findRoadmaps(const QString &projectRoot,
                                            QString *error);

// Every line-bearing carrier gains this. It indexes MigrationPlan::sources.
// 0 is the live roadmap, so a plan built from one source is byte-identical
// to what the single-source form produced (INV-2).
int sourceIndex = 0;   // added to PlannedItem, PlannedSection,
                       // PlannedElement, PlannedLegend and Note

struct MigrationPlan {
    QVector<Source> sources;   // REPLACES `QString sourcePath`
    // …every other field unchanged…
};

// The pure half. All sources in, ONE plan out. REPLACES the single-source
// form; no forwarding overload is kept. Measured —
// `grep -n 'planFrom' tests/features/roadmap_migrate_read/test_roadmap_migrate_read.cpp`
// returns 7 lines of which 4 are calls, and 2 of those 4 are the test's own
// `planFixture()` helpers, so every downstream assertion moves with them.
// `grep -n 'findRoadmap' <same file>` returns 9 lines, 8 of them calls. At
// that size a second entry point costs more in ambiguity than it saves in
// churn, and coding.md § 2 prefers the single surface.
MigrationPlan planFrom(const QVector<Source> &sources,
                       const QString &projectName, const QString &exportSlug);

}  // namespace RoadmapMigrate
```

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
2. **Archives** under `<root>/docs/roadmap/`, each entry whose name matches the
   **case-sensitive** regex `^[0-9]+\.[0-9]+\.md$`.

Both rules are `roadmap-format.md` § 3.9's, quoted rather than invented — that
section already fixes the directory, the regex (no leading `v`, no
`roadmap-` prefix, no zero-padding, no patch suffix) and the sort.

- **Order:** archives follow the live file, sorted by the `(major, minor)`
  integer tuple **descending** — § 3.9's stated contract, adopted rather than
  re-decided. Lexical sort is explicitly wrong there (`0.10` < `0.9`), and this
  project will reach minor 10.
- **No archive directory is not a refusal.** Nine of ten roots have none
  (§ 1.1); `findRoadmaps()` returns the live source alone.
- **A non-conforming entry in `docs/roadmap/` raises `archive_unrecognised`**
  naming the filename, and is not loaded. It is never a silent skip: a
  misnamed archive (`0.7.0.md`, `v0.7.md`) is exactly the shape whose silent
  loss this whole lane exists to prevent, and § 3.9 rejects those names on
  purpose.
- **An archive that is not valid UTF-8 refuses the whole call** with
  `not_utf8`, naming the file. Partial migration of a project is worse than
  none: the load half is one transaction, and a plan silently missing one
  archive would commit as though complete.

### 2.3 Section identity across sources

**Live slugs never move. Archive slugs are namespaced by their source file.**

```
live roadmap  (index 0) : slug = uniqueSlug(seen, heading)          // unchanged
archive <M>.<N>         : slug = "<M>-<N>" , or
                          "<M>-<N>-" + slugifyHeading(heading)
```

The archive's synthetic root section — every source produces one, and its slug
is empty today — takes the bare `"<M>-<N>"` form (`0-6`). Its `##`/`###`
headings take the prefixed form (`0-6-features`).

Three properties, and each is the reason for a rule:

- **Live slugs are byte-identical to what a live-only migration produced.**
  Eight of the ten projects migrate today (ANTS-3765, corpus run 2026-08-01);
  none is yet loaded into a production store, so the exposure is prospective
  rather than actual — but it lands the moment one is. Any scheme that shifted
  a live slug would fail to re-match every item filed under it (ANTS-3765
  § 2.6.1 keys id-less items on *the same section*), orphaning and re-inserting
  the corpus on the next run.
- **An archive slug does not depend on the live file's contents.** This is the
  rule that rules out the obvious implementation, and it is worth stating why
  at length in § 2.3.1.
- **`uniqueSlug()` still runs, as a backstop, over a set shared by all
  sources** — so the store's `UNIQUE (project_id, slug)` can never be violated
  even by a pathological live heading (`## 0 6 features`). If the backstop ever
  fires for an archive slug it raises `archive_slug_collision` naming both. It
  must never fire in practice: when it does, a slug moved, and § 2.3.1 is the
  reason that matters.

#### 2.3.1 Why not simply share the uniquing counter

The mechanism already exists — `RoadmapIndex::uniqueSlug(QSet<QString> &seen,
…)` takes its accumulator by reference, so threading one `seen` set across
sources in a fixed order is a two-line change and produces legal, unique,
deterministic slugs. **It is also unsafe, and the corpus shows exactly how.**

`performance` already appears **3** times in the live file (survey section D),
so the live pass emits `performance`, `performance-2`, `performance-3`, and the
archive's would become `performance-4`. Add one more `### ⚡ Performance` to the
live roadmap — an ordinary week's edit — and the archive's slug shifts to
`performance-5` on the next run. Under ANTS-3765 § 2.6.1 that section is a
different section: its 20 id-less items no longer match, all 20 are re-inserted
with freshly allocated ids, and the old rows are orphaned. The store grows
without bound against a source nobody edited.

That is the same defect class ANTS-3765's own § 2.6.1 was amended to fix on
2026-08-01, arriving through a different door. A counter is positional; identity
must not be. Namespacing by the source file is derived from the filename alone,
so it is stable under every edit to every other source.

### 2.4 Positions, partitions and the merged plan

- `position` stays **per section**, contiguous and 0-based (ANTS-3757 § 2.11 /
  INV-11). Because no section spans two sources, merging cannot perturb it —
  each section's sequence is built from one file, exactly as today.
- INV-11's partition becomes **per source**: every non-blank line of source
  *i* falls inside exactly one carrier whose `sourceIndex == i`. This is the
  amendment § 1 item 3 forces, and it is a strict generalisation — for a
  single-source plan it is the current statement verbatim.
- `MigrationPlan::sources` is ordered and index-stable within a run; a
  carrier's `sourceIndex` is only meaningful against the plan that produced it.

### 2.5 `empty_source` becomes per-source

ANTS-3757 § 2.3 raises `empty_source` when a source yields zero **items**.
`docs/roadmap/0.5.md` yields zero items legitimately — it is 651 bytes of prose
pointing at `CHANGELOG.md` (§ 1.1). Merged naively, one prose-only archive would
raise a whole-project `empty_source` beside a live file holding thousands of
items, which reads as a failed migration.

The rule: **`empty_source` is raised per source and its `detail` names that
source's path.** No suppression, no archive-specific exemption. A prose-only
archive genuinely contributed no items and a human should see that said out
loud — the note is informative, and notes have never been failures. What was
missing was only *which file* it was about.

### 2.6 What the render needs (ANTS-3758's dependency on this)

`MigrationPlan::sources` plus each carrier's `sourceIndex` is exactly enough for
ANTS-3758 to re-emit the split: a section whose source path matches
`docs/roadmap/<M>.<N>.md` belongs in that archive, everything else in the live
file. **No `isArchive` flag is added** — the path is already the discriminator,
and § 3.9's naming regex is already the test. Without this, the first
regeneration would write all 20 archived bullets back into `ROADMAP.md`: a
silent un-rotation, and the same loss class as the ANTS-3758 caveat this spec
exists to clear.

### 2.7 Preference calls and a rejected alternative

Two choices here are judgement, not deduction, and both are mine (Claude,
2026-08-01, drafting) — recorded per `specs.md` § 3.5 so a reviewer can
overturn them rather than reverse-engineer them:

- **Scoping per-source provenance into this spec rather than ANTS-3758.** It
  could live with the render that consumes it. It is here because the plan is
  where the information exists — once merged, nothing downstream can recover
  which file a section came from — and because § 1 item 3 shows INV-11 is
  unstatable across sources without it. Surfaced to the user at drafting time;
  the alternative is to let ANTS-3758 re-derive it from a second parse, which
  is a second reader and ANTS-3757 § 2.3 forbids that.
- **Adopting § 3.9's descending sort rather than choosing oldest-first.**
  Ordering does not affect identity under § 2.3, so this is free choice; the
  standard already states a sort, and a second one would be a second rule.

**Rejected: derive a carrier's source from its section instead of storing
`sourceIndex`.** Sections are namespaced per source (§ 2.3), so an item's
source *is* recoverable from its `sectionSlug` by string-matching the prefix,
and that would add no field to four of the five carriers. It loses on two
counts: `Note` carries no `sectionSlug` and the legend belongs to the project
rather than any section, so the derivation does not cover the carriers that
most need it; and recovering structure by parsing a slug re-introduces exactly
the string-coupling the store's typed columns exist to remove. One `int` per
carrier is cheaper than a parse that can be wrong.

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
  are re-run unmodified against their existing fixtures** (`antsv1`, `gfm`,
  `identity`, `passes`, `prose`, `empty`), which is what makes this invariant
  more than self-report; plus an added leg asserting `sources.size() == 1`,
  `sourceIndex == 0` on every carrier, and — load-bearing — **the full ordered
  section-slug list of each fixture against a committed golden** captured from
  the shipped single-source implementation. *Breaks when:* the merge path
  namespaces or re-numbers unconditionally rather than only for indices ≥ 1,
  which re-slugs every project's live sections and orphans its corpus on the
  next run. **The golden list is what makes that mutation redden here, and
  measuring this was worth it** (2026-08-01, drafting): the shipped suite
  mentions `slug` three times —
  `grep -c slug tests/features/roadmap_migrate_read/test_roadmap_migrate_read.cpp`
  → 3 — of which one is a debug label and one a field-wise dump compared only
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
  leaves every archive slug unchanged. *Test:* same suite — plans the archive
  fixture, then re-plans it with `### ⚡ Performance` appended to the live file,
  and asserts the archive slug list is identical. *Breaks when:* slugs come
  from a counter shared across sources — the § 2.3.1 design, which passes
  INV-1, INV-2, INV-3 and every ANTS-3757 invariant, and then grows the store
  without bound on a live-file edit.
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
- **INV-8** — an entry in `docs/roadmap/` not matching § 3.9's regex raises
  `archive_unrecognised` naming it, and is not loaded. *Test:* same suite — a
  fixture carrying `0.7.0.md` (a patch suffix, which § 3.9 rejects) and
  `README.md`; asserts two notes and that neither file's items appear.
  **Both files must carry a uniquely-identifiable item** — otherwise "its items
  do not appear" is true of an empty file whatever the code does, and the leg
  tests nothing. *Breaks when:* the filter is a `*.md` glob, which loads
  `0.7.0.md` as though it were an archive; or a silent `continue`, which is the
  drop this lane exists to prevent.
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
  on live-file content (§ 2.3.1), which re-files all 20 archive items under a
  shifted slug and orphans the originals. This is the end-to-end detector for
  INV-4, and the shape that would have caught the ANTS-3765 § 2.6.1 defect
  measured on 2026-08-01.
- **INV-10** — the `uniqueSlug()` backstop never fires for an archive slug on
  the real corpus; when it fires it raises `archive_slug_collision` naming both
  slugs. *Test:* `tests/features/roadmap_migrate_read/` (not the load suite
  INV-9 names) — a fixture whose live file carries a heading slugifying to
  `0-6-features`; asserts the note and two distinct slugs.
  *Breaks when:* the backstop is dropped, which lets two sources emit one slug
  and aborts the load on `UNIQUE (project_id, slug)` — in the half that can no
  longer see the source line.

## 4. RAM / build cost

No new build target, no new library, no new external dependency —
`src/roadmapmigrate.{h,cpp}` in `ants_core_lib`, Qt6::Core only, as today.

Memory: the plan now holds every source's markdown for the life of the call
rather than one file's. Bounded by the archive corpus, which § 3.9 caps by
construction — archives exist *because* `ROADMAP.md` passed ~150 KiB, and this
project's two total 4,643 bytes (§ 1.1). Worst case is one live roadmap plus its
archives, all already read into memory one at a time by the current code; the
peak rises by the archive bytes alone. `sourceIndex` adds one `int` per carrier.
No cache, no eviction policy needed — every allocation is scoped to the call.

## 5. Out of scope

- **Writing archives back out / re-splitting the render** — ANTS-3758. This
  spec supplies the discriminator (§ 2.6) and stops there.
- **Teaching `roadmap_query` to read archives.** A permanent exclusion, not
  deferred: `roadmap-format.md` § 3.9 states that verb reads only the current
  `ROADMAP.md` and archives are dialog-only by contract. Changing that is an
  amendment to the standard affecting every project, and nothing here needs it.
- **Extending `tools/roadmap-corpus-survey.py` to archives.** Deferred, and
  § 6.2 states what the archive fixtures do instead and what that costs.
- **Rotating new archives.** `/bump` owns rotation (§ 3.9); this spec only
  reads what rotation already produced.
- **The other nine projects.** None has an archive directory (§ 1.1), so this
  changes nothing for them — which INV-2 is the proof of, not an assumption.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_read/` (INV-1..8, INV-10) and
`tests/features/roadmap_migrate_load/` (INV-9). Label `features;fast`. Each
invariant is verified RED against the mutation its clause names, before the
implementation is restored.

### 6.1 Fixtures are copies of the real archives, not hand-written ones

`fixtures/archives/` gains one root per case. Its live `ROADMAP.md` is a trimmed
file that **must carry the four headings that actually collide** — `### 🎨
Features`, `### ⚡ Performance`, `### 🔒 Security`, `### 🧰 Dev experience` — plus
the two extra `### ⚡ Performance` repeats that make the live count 3, because
INV-4's mutation only reddens when the counter has somewhere to move. Its
`docs/roadmap/0.5.md` and `0.6.md` are **byte-identical copies of this project's
own archives**, H1 title and all.

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
parser, so INV-2's parity property does not extend to them. § 6.3 is what
covers the gap instead.

### 6.3 The corpus run is part of the work, not a follow-up

`tools/roadmap-archive-survey.py` — the script that produced every figure in
§ 1.1 and § 2.3.1 — is committed with this change, so the spec's numbers are an
output rather than a transcription. Before the item is closed, run the real
migration over this project's actual root and record in the loop log: the
archive item count reaching the store, the second run's `Outcome`
(INV-9 end-to-end on real data), and the slug list assigned to the two real
archives.

Ten real files missed a case a committed fixture held, on the last pass. Both
checks run; neither substitutes for the other.

## 7. Cross-doc impact

- **`docs/specs/ANTS-3757-roadmap-migration-read.md`** — § 5's archive bullet
  is amended: its claim that the five plan carriers "all stand" is wrong
  (§ 2.1). INV-1 (discovery), INV-11 (partition) and INV-13 (`empty_source`)
  are annotated `amended by ANTS-3766` per `specs.md` § 5.5 — annotated, never
  renumbered. Its § 2.2 gains a pointer to § 2.2 here.
- **`docs/specs/ANTS-3765-roadmap-migration-load.md`** — § 2.6.1's
  section-identity dependency is now load-bearing across sources; a pointer to
  § 2.3.1 here, which is the second way that rule can be broken.
- **`docs/standards/roadmap-format.md`** — no change. § 3.9 is quoted, not
  amended; this spec deliberately adopts its regex and sort rather than
  restating them.
- **`CLAUDE.md`** — no change (the migration lane is not in the module map).
- **`docs/subsystems.md`** — the roadmap-migration lane entry names
  `findRoadmap`; update to `findRoadmaps`.
- **`CHANGELOG.md`** — one `Added` entry when this ships.

## Cold-eyes loop log

| Loop | Reviewer | Findings | Resolution |
|---|---|---|---|
