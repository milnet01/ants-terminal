# ANTS-3793 — roadmap consumer cutover: one reader seam, two backends

**Status:** spec draft (2026-08-03).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3793 (ANTS-3758 split, spec seam 3b of 5); the
`item.body` half from ANTS-3808, found while verifying ANTS-3806 (2026-08-03)
and homed here by the user the same day.
**Covers:** ANTS-3793. **NOT ANTS-3808 — split out 2026-08-03** to
[`ANTS-3808-item-body-and-trailer-suppression.md`](ANTS-3808-item-body-and-trailer-suppression.md),
which is now the sole contract for `item.body` and the trailer accessor.
**§ 2.3 below is superseded and must not be implemented from** — it is retained
only until this document is rewritten as the read seam (see its loop log).
**Blocked by:** ANTS-3758 (the render these consumers write through) — shipped.
**Blocker for:** ANTS-3794 (publish + health checks), which would otherwise
publish § 2.3's duplication into every migrated project's `ROADMAP.md`.
**Pairs with:** ANTS-3765 (the load half, whose § 2.10 marker § 2.2 consumes),
ANTS-3757 (the read half, whose § 2.1.1 § 2.3 amends).

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 The seam](#21-the-seam-one-reader-function-two-backends) ·
[2.2 The dispatch marker](#22-the-dispatch-marker) ·
[2.3 What `item.body` holds](#23-what-itembody-holds-ants-3808) ·
[2.4 The write half](#24-the-write-half--roadmap_logs-eight-ops) ·
[2.5 RoadmapDialog](#25-roadmapdialog-and-the-legend) ·
[2.6 The round-trip oracle](#26-the-round-trip-oracle-inv-1s-deferred-half) ·
[2.7 Acyclicity](#27-whole-store-relationship-acyclicity)) ·
[3. Invariants](#3-invariants) · [4. RAM / build cost](#4-ram--build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

The store is primary after cutover (`roadmap-data-model.md` INV-3) and
ANTS-3758 can now write markdown back out of it, but **nothing reads or writes
the store yet**. Every consumer still parses `ROADMAP.md`: `roadmap_query`,
`roadmap_log`'s eight ops, and `RoadmapDialog`. Until they move, the store is a
write-only copy that drifts from the file the moment anyone edits it.

Two further things block on this id, both because it owns the seam they
straddle:

- **ANTS-3808** — the migration files `BulletRecord::body` (the *whole* bullet)
  into `item.body`, and `RoadmapRender::renderBullet()` reads that column as
  *residual prose*. Two contracts, one column, opposite meanings, so a rendered
  bullet repeats its own headline and then every field a second time. Observed
  in the ANTS-3806 fixture, not inferred.
- **ANTS-3758's INV-1 full oracle**, deferred at ship because it needs the
  migration loader and a second store. This id has both in hand, and the oracle
  is what would have caught ANTS-3808.

**The measurement that shapes this spec.** The consumer surface reads as
enormous — `grep -o 'roadmap_log' src/remotecontrol.cpp | wc -l` returns 205
(2026-08-03) — but those are mentions, not read paths. Every consumer read **of
bullet records** goes through **one function**:

```
$ grep -n 'parseBullets(' src/remotecontrol.cpp src/roadmapdialog.cpp \
    | grep -v 'roadmapdialog.cpp:51[489]:' | wc -l
26
```

All 26 call `RoadmapDialog::parseBullets()`, which since ANTS-3764 is a
forwarder whose entire body is `return RoadmapParse::parseBullets(markdownText);`
(`src/roadmapdialog.cpp`). The excluded lines are that forwarder's own comment,
signature and body. `src/roadmapmigrate.cpp`'s call is
**not** a consumer — it is the migration reading source markdown to build the
store, and it stays markdown forever.

So the read cutover is not 26 rewrites. It is **one function with two backends**,
and each of the 26 sites swaps its one call for § 2.1's resolver — a mechanical
edit, because **no site grows backend logic, a dispatch branch or a store
reference**. The distinction matters: the resolver needs project identity and
`parseBullets(markdownText)` is pure text-to-records, so the signature does
change and "untouched" would be false. What is untouched is the *shape* of every
consumer, and the 82 test call sites, which keep parsing markdown directly:

```
$ grep -rn 'parseBullets(' tests/ --include=*.cpp | wc -l
82
```

**"Bullet records" is a real restriction, not a hedge, and the readers it
excludes are named here so § 2.5 cannot quietly inherit the wider claim.**
`src/roadmapdialog.cpp` holds four further readers that do not go through
`parseBullets()` and are **out of scope**. Three read the *rendered*
`ROADMAP.md` — `RoadmapDialog::parseLastTouchDates()`, `extractToc()` and
`loadMarkdown()` — taking headings, raw bullet lines and file mtimes, and that
file keeps existing after cutover because ANTS-3758's render writes it. The
fourth, `collectCurrentBullets()`, **reads no roadmap at all**: it returns
`readUnreleasedBullets(m_changelogPath)` plus `readRecentCommitSubjects()`, so
its inputs are `CHANGELOG.md` and `git log` and the cutover cannot reach it.
It is listed here only because its name invites the opposite assumption. So none
of the four needs a store path, and none gets one. A reader that later needs store-backed data asks § 2.1's resolver;
until then, "the markdown file is still there" is the whole answer, and it is
the answer *because* the render ships, not by accident.

## 2. Surface

### 2.1 The seam: one reader function, two backends

`RoadmapParse::parseBullets(markdownText)` is a pure text-to-records function
and stays exactly that. The cutover adds a **sibling producer of the same
record type**, sourced from the store, and a resolver that picks between them:

```cpp
// src/roadmapsource.h — new TU. § 4 owns which library, and it is not free.
namespace RoadmapSource {
    // The records a consumer would have got from parsing this project's
    // markdown, sourced from the store instead. Document order, one entry per
    // item, in the SAME shape RoadmapParse::parseBullets returns.
    std::optional<QVector<BulletRecord>>
    bulletsFromStore(RoadmapStore &store, qint64 projectId, QString *error);

    // § 2.2's dispatch. Three outcomes, not two — see § 2.2 for why `nullopt`
    // alone cannot carry them:
    //   engaged            → migrated; read the store at this projectId
    //   nullopt, *error empty  → not migrated; parse markdown as today
    //   nullopt, *error set    → REFUSE; never fall back (INV-3)
    //
    // `markdown` is REQUIRED and is the project's live roadmap text: § 2.2's
    // ants-v1 gate runs detectRoadmapFormat() over it, and no store column
    // records a source format. An earlier draft omitted it and the gate had no
    // input at all.
    std::optional<qint64> migratedProject(RoadmapStore &store,
                                          const QString &projectRoot,
                                          const QString &markdown,
                                          QString *error);

    // The resolver every consumer actually calls — the two above are its
    // halves, exposed because the tests drive them separately. `markdown` is
    // the caller's existing text, used on the unmigrated path AND by the
    // ants-v1 gate above; it is never re-read from disk here.
    std::optional<QVector<BulletRecord>>
    bulletsFor(RoadmapStore &store, const QString &projectRoot,
               const QString &markdown, QString *error);
}
```

**`BulletRecord` is the interface, and that is a deliberate constraint rather
than a convenience.** Every consumer, every response field and all 82 test call
sites are already written against it, so a store backend that fills the same
struct changes no caller and no test. It also makes the two backends directly
comparable, which is what INV-2 asserts and what no wider refit would allow.

**Every `BulletRecord` field is accounted for below, and the table is the
comparison set INV-2 tests against** — an unlisted field is an undefined diff,
which is what makes an enumeration-by-exception unusable here.

**One rule generates most of the table, and stating it first is what keeps the
two backends comparable.** `bulletsFromStore()` fills every text-derived field
with **what `RoadmapParse::parseBullets()` would assign if it parsed the bullet
`roadmaprender.cpp` renders for that item**. That bullet has exactly one shape
(`renderBullet()`, verified 2026-08-03):

```
- <emoji> [<id>] **<headline>**
  <residual body>
  **Layman:** …
  Kind: …            ← always emitted (ANTS-3758 INV-12)
  Source: … / Lanes: … / Evidence: …   ← when non-empty
```

The rule is not a convenience: it is the *only* derivation under which INV-2 can
be total, because a migrated project's `ROADMAP.md` **is** that rendered text, so
"what the store backend returns" and "what the markdown backend returns for this
project after cutover" are then the same object by construction rather than by
coincidence.

| Field | On the store path |
|---|---|
| `id`, `kind`, `lanes`, `evidence`, `layman`, `source` | from the item columns, verbatim |
| `status` | **mapped, never copied.** The store holds a lifecycle *word* (`planned` / `in-progress` / `considered` / `shipped` / `dropped`, per `roadmap-data-model.md` § 3.4) and `BulletRecord::status` holds the *emoji* (`src/roadmapparse.h`: `"✅" \| "🚧" \| "📋" \| "💭"`). The store path applies the same word→glyph table `roadmaprender.cpp`'s `emojiFor()` uses, which is `roadmap-format.md` § 3.3's four. `dropped` has no glyph by design (§ 3.11 makes a fifth an anti-pattern) — see the membership note below, which is where that case is decided. Copying this column verbatim would put `"planned"` where every consumer expects `"📋"`, and would fail INV-2 on **every** record |
| `headline`, `headlineFull` | **one rule, not two** — both come from the stored `headline` through the parser's own `assignHeadline()`: `headlineFull` is the stored text unchanged, and `headline` is that text cut to 120 characters **with `"…"` appended** when it is longer (`truncateEllipsis()`, ANTS-1811). An earlier draft said "equal-or-truncated", which does not pin the ellipsis and would let a conforming implementation fail INV-2 |
| `body` | the rendered bullet **minus its leading `"- "` and status emoji**, which is precisely what `parseBullets()` seeds `body` from and then appends continuations to. So it is `[<id>] **<headline>**`, then the stored residual, then the trailer lines the render emits. **It is NOT the stored `item.body`** — § 2.3 makes that column the residual alone, and returning it raw would drop the headline line from every migrated project's `body` and break INV-2 by construction |
| `sectionHeading`, `sectionLevel`, `sectionSlug` | from the item's `SectionRow` via its element |
| `format` | always `"ants-v1"` — the store path serves no other dialect (§ 2.2, § 5) |
| `firstLine`, `lastLine` | **0.** Markdown line numbers do not exist in a store, and no store read can invent them. This is the one field pair with a live consumer: `roadmap_log`'s `line_range` locator. § 2.4 owns what that locator does on a migrated project |
| `sourceStatus`, `passDesignator`, `anchor` | empty — each is an artefact of a dialect the store path does not serve (`src/roadmapparse.h` documents each as such) |
| `synthetic` | `false` — a store id is a real id; nothing is content-hashed on this path |
| `idToken` | the item's `id`. The render puts it in the leading bracket slot, and `rxLeadToken` (`^\[([^\]\s]+)\](?![(:])`) captures exactly that |
| `boldId` | **always empty**, and this corrects an earlier draft that filled it "from the item's `id`". `roadmapparse.h` records the contract — "when `boldId` is non-empty, `id == boldId`" — and `boldId` is set only when `extractBoldId()` finds a `**…**` token *at the head's start*. The rendered head starts with `[`, so it never does. Filling it would make INV-2 fail on the commonest bullet in the corpus, which is the opposite of what the earlier fix intended |

**A store-path record is every item the store holds for the project, filtered
only by § 2.2's dispatch — the render's membership rules do NOT apply here.**
ANTS-3758 § 2.4 excludes `internal` and `dropped` from a *rendered file*, and
INV-4 there **refuses** on an unfiled item. Importing either into a query path
would silently change `roadmap_query`, which returns what the markdown holds and
has no visibility concept at all. So `bulletsFromStore()` returns unfiled items
too (their `sectionSlug` empty), and any filtering stays where it already is: in
the verb's own `filter` / `status` / `section` arguments.

**An unfiled item makes the project readable but not writable, and that
asymmetry is deliberate rather than an oversight.** ANTS-3758's INV-4 **refuses
the whole render** on an item with no element row, so under § 2.4's
mutate-then-render every `roadmap_log` op against such a project fails at the
render step and rolls back (INV-4 here). The project is therefore frozen for
writes until the item is filed — which is the correct outcome, because the
alternative is a write that succeeds while the file silently stops matching the
store. It is *readable* throughout, because a query that hid the unfiled item
would make the fault invisible to the only tool likely to surface it. The
refusal names the unfiled item's id so the caller can file it; this is the one
case where the two specs' rules deliberately disagree about the same item, and
neither is changed here.

### 2.2 The dispatch marker

ANTS-3765 § 2.10 already fixed the marker and its guarantee: **a `project` row
exists exactly when that project's whole plan committed**, because per-project
atomicity makes "half a project" unreachable. That spec states the marker and
explicitly does not build the fallback. This one builds it.

**The store has no reader that can take that marker, and adding one is this
spec's first obligation.** `registerProject(root, name, exportSlug)` writes the
root and ANTS-3756 INV-8 keys a project on its **canonical root**, but the only
readers are `readProject(projectId)` and `readProjectBySlug(exportSlug)`, and
`ProjectRow` carries no `root` at all. A dispatch keyed on the caller's
directory therefore has nothing to call. So:

```cpp
// src/roadmapstore.h — the same gap ANTS-3758 § 2.1 found for listElements().
std::optional<ProjectRow> readProjectByRoot(const QString &canonicalRoot,
                                            QString *error = nullptr) const;
```

`ProjectRow` gains `root` in the same change, because a caller that resolved a
project by root and cannot read the root back cannot confirm the canonicalisation
INV-8 depends on.

**The resolver canonicalises `projectRoot` before it looks anything up, by the
same call `registerProject()` used.** `roadmapstore.h` records INV-8's rule at
the writer: a project is keyed on `QFileInfo::canonicalFilePath()` and a path
that cannot be canonicalised is refused, never stored. A reader that passed the
caller's raw path would miss on every symlinked or non-normalised root and
report "not migrated" — which is the silent fallback INV-3 exists to forbid,
arriving through the one door the invariant does not watch. So: canonicalise
first; an **empty** result (the path does not exist) is an `*error`-set refusal,
not a `nullopt` fallback, because a caller whose own project root does not
resolve has a fault worth naming rather than a roadmap worth parsing.

Five rules follow, and each exists because the obvious reading is wrong:

- **The store not existing is `nullopt` with no error.** Most machines running
  this code have never migrated anything, and a verb that refused there would
  break every unmigrated project on the day the store shipped.
- **The store existing and failing to open IS an error** — `nullopt` *with
  `*error` set*, which is why `migratedProject()` takes an error channel at all.
  A two-valued return cannot express it: `nullopt` already means "not migrated,
  parse markdown", so without the third outcome the spec's own no-silent-fallback
  rule is unimplementable through its own signature. A corrupted store that
  quietly falls back is a store nobody notices is corrupt — the failure class
  `roadmap-data-model.md` § 9 names for silent backup loss.
- **A migrated project whose roadmap is not `ants-v1` is `nullopt`, not
  engaged.** The migration reads all three dialects
  (`src/roadmapmigrate.cpp` branches on `"pass-headings"`), so a project row
  existing does **not** imply this path can serve it — and § 5 scopes the store
  backend to emoji bullets. Without this rule INV-3 ("a migrated project is
  never served markdown") and § 5 contradict each other for every migrated
  RetroDB- or 3D_Engine-shaped project. The test is ANTS-3758 § 5's scheduling
  guard, unchanged and reused: `detectRoadmapFormat(lines, &sawSignal)` returns
  `"ants-v1"` **and** sets `sawSignal`, because that function answers `ants-v1`
  for input it does not recognise and `sawSignal` is what separates the two.
  **It is read off the live file, not the store, because no store column records
  a source format** — `format` lives on the migration's `RoadmapMigrate::Source`
  (`src/roadmapmigrate.h:145`, a `{path, markdown, format}` struct; there is no
  type called `SourceRow`) and nowhere in `roadmapstore.h`. § 7 records the
  column as owed; until it exists, the file is the only witness. The text comes
  from the resolver's `markdown` argument, which the caller already holds — the
  gate re-reads nothing from disk, which is what keeps the per-call dispatch
  below inside § 4's latency budget.
- **The connection profile is `Access::Bulk` for a migration and
  `Access::Interactive` for a consumer**, named at each call site rather than
  defaulted. `RoadmapMigrateLoad::load()` **refuses** a non-Bulk store outright
  (`project_refused`, "a migration load needs an `Access::Bulk` store" —
  ANTS-3765 § 2.2 / INV-12), so the wrong profile fails loudly rather than
  intermittently. A consumer read has no such guard and wants the short
  interactive deadline: `Access` selects a busy timeout and page-cache size, and
  a five-second deadline is right for a verb call and wrong for a corpus load.
- **The `RoadmapStore &` the consumers pass is one long-lived, process-owned
  `Access::Interactive` connection per store path, opened lazily on the first
  migrated read and never per call.** Stating this is not housekeeping: opening
  SQLite, applying pragmas and warming the page cache is the dominant term in
  § 4's p95 budget, and a per-call connection would blow it on its own while
  leaving every measurement in this spec unreproducible. Ownership sits with the
  same object that owns the other per-process integration state
  (`RemoteControl` for the verbs, `RoadmapDialog` for the dialog), and
  `RoadmapSource` takes a reference precisely so it owns no lifetime itself.

**The dispatch is resolved once per call, not once per process — and that is a
statement about the marker, not about the connection.** A verb call that
resolved the marker at startup would serve markdown for the rest of the session
to a project migrated in between, so the `readProjectByRoot()` lookup runs every
call. The *connection* is the opposite: opened once and reused, per the rule
above. Conflating the two is how this design gets implemented as a per-call
`QSqlDatabase::open()`, which is why they are written as separate sentences.

### 2.3 What `item.body` holds (ANTS-3808)

> **SUPERSEDED 2026-08-04 — do not implement from this section.** ANTS-3808 has
> its own spec:
> [`ANTS-3808-item-body-and-trailer-suppression.md`](ANTS-3808-item-body-and-trailer-suppression.md).
> Everything below is the pre-split draft and is **known wrong in two ways** its
> cold-eyes loop 1 fixed: the storage rule here drops the whole first line,
> which destroys the body prose of 241 of 1645 bullets in this project's
> corpus; and the claim that `trailerValuesIn(body)` "equals the column by
> construction" is false, because the column was extracted from the *full* body
> and the stored body is a residual. Read the new spec, not this.

**The disagreement, stated exactly.** `RoadmapParse::parseBullets()` seeds
`QString body = head` where `head` is the bullet line minus its `"- "` and minus
its status emoji (`stripInlineEmoji()` runs first on the native path), then
appends every continuation line trimmed of indentation. So `body`'s first line is
the id-and-headline text and the rest is every continuation line, the
`Layman:` / `Kind:` / `Source:` / `Lanes:` / `Evidence:` trailer included.
`RoadmapMigrate`'s `makeItem()` copies that verbatim into `item.body`, which is
what ANTS-3757 § 2.1.1 tells it to do. `renderBullet()` then emits a synthesised
head line, **then** `body`, **then** the trailer again from the columns.

**A note on that name, because it is not the public symbol earlier drafts
implied.** `renderBullet(const RoadmapStore::ItemWrite &)` is a **free function
in an anonymous namespace** in `src/roadmaprender.cpp` — not
`RoadmapRender::renderBullet()`, which does not exist. This is good news for the
change below: the edit is TU-local and adds no header surface. It is bad news
for the tests, which therefore **cannot call it directly** and must drive INV-1
and INV-6 through the render's public entry point over a store they populated. So the headline is emitted twice (head, then body's first line) and
each trailer key twice (inside body, then from its column) — matching § 1's
"repeats its own headline and then every field a second time".

**Why the obvious repair is wrong.** "Store `body` minus its head line minus its
trailer lines" assumes the trailer *is* a set of lines. `src/roadmapparse.cpp`'s
own regex comments carry the corpus measurements that say otherwise: 157
`Source:` values sit inline in a prose sentence rather than at a line start
(which is why `rxSource` and `rxLanes` are deliberately un-anchored, ANTS-2058 /
ANTS-3764), 10 lines carry two keys (`rxTrailerKey` exists for exactly those),
and 22 are backticked mentions of a key excluded by ANTS-3722's guard. For a
large share of the corpus there is no field *line* to drop — the metadata is a
span inside a sentence, and excising it either deletes prose or leaves half a
sentence. Computing the residual is a new parsing contract, and re-deriving it
outside `RoadmapParse` is the second bullet parser ANTS-3757 § 2.3 forbids.

**The decision: the migration drops one line, and the render asks before it
re-derives.** Two changes, neither of which needs a schema change, a new
provenance value, or a second parser.

1. **`item.body` is the bullet body with its first line removed.** This half is
   well-defined and needs no parsing at all: `body` is seeded from `head` and
   every continuation is appended after a `'\n'`, so the head line is exactly
   the text before the first `'\n'` and the residual is exactly the text after
   it — empty for a bullet with no continuation. ANTS-3757 § 2.1.1's row for
   `body` is amended from "the reader's `body`" to say so.

2. **`renderBullet()` emits a trailer key from its column only when the body
   would not already re-parse to that same value**, asked through
   `RoadmapParse`'s own matchers. The six regexes (`rxKind`, `rxLanes`,
   `rxLayman`, `rxEvidence`, `rxSource` and `rxTrailerKey`, which bounds a value
   at a following key on the ten two-key lines) are **shared, not moved** —
   `parseBullets()` needs their captures and a boolean predicate could not serve
   it — by hoisting them to file scope behind one exported accessor:

   ```cpp
   // src/roadmapparse.h — the matchers, asked rather than duplicated.
   // Values, not booleans: the render compares, and parseBullets() still
   // captures. Default-initialised so an absent key is an empty string
   // rather than an indeterminate read.
   struct TrailerValues {
       QString layman, kind, source;
       QStringList lanes, evidence;
   };
   TrailerValues trailerValuesIn(const QString &body);
   ```

   **The accessor returns each value exactly as `parseBullets()` assigns it to
   the record — post-match, not raw captures — and that contract is the whole
   fix rather than a detail of it.** § 2.3's "equals the column by construction"
   is true only if the two run the same normalisation; against raw captures
   nothing would ever compare equal, no suppression would ever fire, and
   ANTS-3808 would be *documented* as fixed while remaining live. The steps are
   already in `roadmapparse.cpp` and are named here so an implementer cannot
   reasonably re-derive them differently:

   | Key | Normalisation the accessor must reproduce |
   |---|---|
   | `kind`, `layman` | `captured(1).trimmed()` |
   | `lanes` | split on `,` (`SkipEmptyParts`), each part trimmed, empties dropped |
   | `evidence` | trim; drop **one** trailing `.` unless the value ends `..`; then split/trim as `lanes` |
   | `source` | truncate at the first following `rxTrailerKey` match, trim, drop one trailing `.`, trim again |

   INV-6 asserts this equality directly, so a divergence fails a test rather
   than silently disabling the feature.

   This is **not** a second parser. It is the one reader answering a question
   about its own grammar, which is the distinction ANTS-3757 § 2.3 draws.

**Value-equality and not mere presence, because presence alone breaks
ANTS-3758's INV-12.** That invariant requires every emitted bullet to *literally
carry* `Kind:`, and its test asserts against the rendered text. Suppressing on
presence would satisfy it for an ordinary bullet and break it for the corner
below — a body mentioning `Kind:` in prose has the key *present* and the wrong
*value*, so the render would drop the real line and emit nothing. Comparing
values instead means a mismatch always emits from the column, which is canonical.
So INV-12 continues to hold as written: the required piece is in the rendered
text either way, exactly once.

**Why per-key and not a stored flag.** A `verbatim`-versus-`residual` flag on
`provenance.body` was the sketch on the ANTS-3808 bullet, and it is wrong for a
reason worth recording: `provenance` is per-field **write origin** —
`migrated` / `asserted` / `defaulted` (`src/roadmapstore.h`, the `setItemField`
overload comment) — and body *shape* is a different axis. A migrated body edited
by a consumer flips its provenance to `asserted` while staying verbatim, and a
post-cutover body written residual would carry `asserted` too. One key cannot
answer both questions, and the per-key predicate answers the question actually
being asked without storing anything.

**The corner this design has, named rather than papered over — and it is
narrower than it first looks — though not as narrow as "anchored means safe".**
Only `Source:` and `Lanes:` can be mis-extracted from *mid-sentence* prose,
because those two are deliberately un-anchored (ANTS-2058 for `Lanes:`, and
ANTS-3764 extended it to `Source:` on its own measurement of 157 inline
occurrences). `rxKind`, `rxLayman` and `rxEvidence` are `^`-anchored — **but
with `QRegularExpression::MultilineOption`**, so `^` anchors at the start of
*any line within the bullet body*, not at the start of the body. A continuation
line that merely *begins* `Kind:` in prose still matches them. The residual
exposure is therefore "a body whose continuation line starts with a trailer key"
rather than "none", and ANTS-3722's backtick guard is what usually absorbs it.
Stating this narrowly matters because § 2.4's write-refusal is scoped to the
un-anchored pair, and that scope is a judgement about *likelihood*, not a proof
of impossibility.

For a **migrated** item the mismatch cannot arise: the store's `source` column
was populated by this same reader from this same body, so
`trailerValuesIn(body)` equals the column **by construction**, suppression
fires, and nothing is duplicated. The mismatch appears only once a consumer has
written the column without rewriting the body — a `flip` or `annotate` under
§ 2.4. The render then emits the canonical line beside the prose sentence, and
an un-anchored re-parse takes the *earlier* one, so INV-6 fails for that item.

**This is a property of the format's un-anchored keys, not of the render**, and
fixing it means anchoring `rxSource` / `rxLanes` — which would discard the 157
inline values that motivated un-anchoring them. § 5 records it as out of scope
with its own id owed, and INV-6's fixture pins the migrated case (where the
design is sound) and asserts the edited case fails **loudly** rather than
silently: § 2.4's write path refuses a column write whose new value would be
shadowed by an earlier un-anchored match in the same body.

### 2.4 The write half — `roadmap_log`'s eight ops

The dispatcher accepts seven named ops plus `append` as the unnamed fallthrough
(`src/remotecontrol.cpp`, `RemoteControl::cmdRoadmapLog`):

```
$ awk '/^QJsonDocument RemoteControl::cmdRoadmapLog\(/,/^}/' src/remotecontrol.cpp \
    | grep -o 'op == QStringLiteral("[a-z_]*")' | sed 's/.*("//;s/")//' | sort -u
amend_body annotate append_batch bundle_row create_section flip flip_batch
```

Each hand-splices markdown and commits it with `QSaveFile` — ten such *lines* on
a roadmap path (`grep -c 'QSaveFile [a-z]*(roadmapPath)' src/remotecontrol.cpp`
→ 10, 2026-08-03). `grep -c` counts matching lines, not call sites; the two
coincide here only because each construction sits on its own line, and the
figure is a scale indicator rather than a count to implement against.

**On a migrated project every op becomes: mutate the store, then re-render.**
The render already exists and is proved **idempotent** (ANTS-3758 INV-7); its
losslessness is proved only over the standalone half until § 2.6 of this spec
lands the full oracle, which is why that section is a deliverable here and not a
citation. No op grows a markdown writer of its own. The splice paths stay on the
unmigrated branch, unchanged, and are deleted by the id that retires markdown —
not this one.

**Two ops do not map onto item rows, and pretending they do is how this half
goes wrong.** `create_section` writes a `section` row (`addSection()`), which the
store models directly. `bundle_row` appends a Markdown *table* row, and **its
store form is not a rendered row** — an earlier draft said the `kind = 'table'`
element's "payload is the rendered row", which would write markdown into a
column the schema defines as JSON. ANTS-3756 § 2.3 states `element.payload` is
"narration prose when `kind = 'narration'`, **a JSON table when
`kind = 'table'`**", and its INV-24 (ANTS-3765 INV-9) requires
`addElement(kind='table')` to store **canonical** JSON — the
`roadmap_store_schema` test asserts exactly that against
`{"rows":[["x"]],"header":["h"]}`. So `bundle_row` on a migrated project is a
**read-modify-write of one table element**: read the section's `kind='table'`
element, append the row to its canonical-JSON payload, write it back. Not an
append of a new element, which is the other way this goes wrong. Both ops are
element-level writes, not item writes; `roadmap-data-model.md` § 5 carries the
element kinds.

**Two locators have no store equivalent, and each gets a stated answer rather
than an implicit one.**

- **`line_range`** addresses bullets by markdown line number, and § 2.1 fills
  `firstLine` / `lastLine` with 0 on the store path. On a migrated project the
  op **refuses** with `locator_unsupported` naming `line_range` as markdown-only,
  rather than silently matching nothing — a locator that resolves to an empty
  set looks identical to a locator that matched no bullets, and `flip_batch`
  would report those as skipped rows nobody investigates.
- **Id allocation.** `append` reads its high-water from `.roadmap-counter` today;
  on a migrated project it reads `RoadmapStore::idHighWater(projectId, prefix)`.
  **That takes a prefix the caller must source**, and the store answers it:
  `idPrefixFor(projectId)` returns the prefix this project already allocates
  under, or `nullopt` when the project has no id-bearing item yet — an absent
  row, explicitly *not* an error (`roadmapstore.cpp:1445`). On `nullopt` the op
  falls back to the prefix the caller passed or derived exactly as it does
  today, because a project with nothing to be consistent with cannot have drift.

  **The counter's role is narrower than an earlier draft claimed, and the
  correction changes what § 7 has to say.** `roadmap-format.md` § 3.5.1 does
  **not** define `.roadmap-counter` as the sole source — it defines it as *"a
  derived, per-machine cache — NOT source (ANTS-3450)"*, `.gitignore`d, whose
  true value is the highest id across the committed corpus, with every
  allocation *flooring* to that corpus high-water via
  `RoadmapFoldIn::corpusHighWater`. So the risk is not "two allocators, one
  source"; it is that **swapping in `idHighWater()` alone silently drops the
  corpus floor**, which is the mechanism that makes a stale or fresh-clone
  counter unable to reissue a live id. On a migrated project the store's
  high-water is authoritative *and the corpus floor still applies*: the
  allocation is `max(idHighWater(), corpusHighWater())`, and the counter file is
  not written. § 7 amends the standard to say which carrier is authoritative
  during the interim — it does not overturn the ANTS-3450 rule, and an earlier
  draft that read it as doing so is corrected here.

**A column write that § 2.3's un-anchored corner would shadow is refused, and
this is the rule that keeps INV-6 satisfiable.** Any op writing `source` or
`lanes` (`flip`, `annotate`, `amend_body`, `append` on an existing item) first
asks `trailerValuesIn()` what the item's current `body` yields for that key. If
the body yields a value, the new value differs from it, and the body's match is
**un-anchored** — i.e. the render would emit the canonical line *after* a prose
sentence an un-anchored re-parse reaches first — the op refuses with
`body_shadowed`, naming the shadowing sentence.

**Two refusals, two codes, and an earlier draft spent one code on both.**
`locator_unsupported` (above) and `body_shadowed` are unrelated faults with
unrelated remedies — "use a different locator" versus "rewrite or backtick a
sentence" — and `docs/standards/mcp-error-codes.md` requires a caller to be able
to branch on `code` alone. Both are **new** to that taxonomy and § 7 files them.
So, it turns out, is `bad_op_combo` itself: it is used 23 times across
`src/remotecontrol.cpp` and `src/claudeintegration.cpp` and appears **nowhere**
in the standard (verified 2026-08-03), which is a pre-existing documentation gap
this spec inherits rather than causes. § 7 records it; it is not this id's to
fix, and the two codes above are defined here rather than by analogy to it.

The alternative is a bullet that renders correctly and re-parses wrongly, which
is data loss the next migration would silently commit. Refusing is safe because
the case is rare *and* self-clearing: the caller rewrites the body's sentence,
or backticks the key, and the write goes through. It only ever fires on a body
that discusses a trailer key in prose without backticks, which ANTS-3722's guard
already treats as the mistake it usually is.

**`annotate` and `flip` share one path today and must keep sharing one**
(`cmdRoadmapLog` routes both to `cmdRoadmapLogFlip`). On the store side that is
`setItemField()` for the body append plus, for `flip` only, a `status` write —
so the shared path is preserved rather than reinvented, and `annotate` remains
the flip that changes no status.

**Every store write is one transaction, and a failed render rolls it back.** A
committed store change whose render failed leaves the file disagreeing with the
store, which is the divergence ANTS-3794 exists to detect and must never be
caused here. INV-4 pins it.

### 2.5 RoadmapDialog, and the legend

The dialog's three consumer reads go through the same § 2.1 resolver and need no
other change: it renders `BulletRecord`s and will receive `BulletRecord`s. They
sit in `RoadmapDialog::renderCardsHtml()`, `captureScrollAnchor()` and
`restoreScrollAnchor()`. The latter two each re-parse the whole file to resolve
one anchor, which the code accepts because it happens **on dialog close and
reopen, not per scroll event** (`src/roadmapdialog.cpp`: "One parse on close is
cheap relative to the user action"). That cadence is what makes a store read
acceptable in the same place, and INV-2's record-for-record equality is what
keeps the anchor landing where it did before — the anchor key is
`BulletRecord::sectionSlug`, which § 2.1 fills.

**The dialog renders each project's own legend when the store has one, and no
legend when it does not.** ANTS-3793's bullet leaves this open — the data
model's § 5.1 "makes it possible, not mandatory". Deciding it *renders* costs
one `readProjectByRoot()` the dialog already needs for § 2.2's dispatch — not
`readProject()`, which takes a `projectId` the dialog does not have until that
call returns — plus a **parse**: `ProjectRow::legendText` is documented as "the
RAW stored text, not a parsed `QJsonObject`" (`src/roadmapstore.h`), held that
way so the export's byte-identity contract does not run a round-trip through the
middle of INV-1. So the dialog parses it itself. The alternative to rendering it
at all silently shows this project's vocabulary for another project's statuses. A project with no stored legend shows none rather than a default, for
the reason ANTS-3758 § 2.8 gives: the legend is per project precisely so one
renderer serves every project's vocabulary.

### 2.6 The round-trip oracle: INV-1's deferred half

ANTS-3758 § 2.6 fixes the whole contract: render to a scratch project root,
rediscover it with `findRoadmaps()`, load it into a scratch store, export both,
and compare **projections taken with the same predicate**, with three enumerated
families excluded. This spec builds it and adds nothing to that contract.

**What it does change is where the claim lives, and that needs saying because
two documents currently assert it.** ANTS-3758's INV-1 is written as the full
oracle and names a shipped test, `Inv1ExportsMatch`; what that case actually
asserts is the half that stands alone — every field survives into the rendered
text — because the full comparison needs `RoadmapMigrateLoad`, a second store
and its `Options`, none of which that spec had. So INV-1 today over-claims what
its own test covers. § 7 **rewords ANTS-3758's INV-1** to the half it proves and
points the full oracle at this spec's INV-7; annotating it would leave two live
statements of one contract, which is the conflict class this gate exists to
catch.

**It is built before § 2.3's fix and shown red against it.** ANTS-3808 is
exactly the defect this oracle exists to catch, and a fixture that only ever
runs against corrected code proves the oracle compiles, not that it discriminates.

### 2.7 Whole-store relationship acyclicity

Deferred here by ANTS-3760 finding 9, and distinct from the per-row
self-relationship `CHECK` the schema already enforces: that constraint stops
`A → A` and cannot see `A → B → A`.

**It is a check, not a constraint.** SQLite cannot express a graph-reachability
constraint in DDL, and enforcing acyclicity on every `relateItems()` call would
put a traversal in the write path of the migration's hottest loop. It runs as a
health check over the finished store — the same family ANTS-3794 schedules — and
**reports** rather than refuses, because a cycle is a data fault to surface, not
a write to reject after the fact.

It gets a declared surface because INV-8 asserts the check "names the cycle" and
a contract cannot assert that about a function nobody declared. It sits in the
same TU as § 2.1's resolver for one reason only — **that TU is where this spec
puts everything needing a `RoadmapStore &` from outside the store library, so it
is the file that already pays the link edge § 4 argues about**. There is no
deeper relation between a reader seam and a graph check, and claiming one would
be cohesion invented after the fact:

```cpp
// src/roadmapsource.h — reports, never refuses.
struct RelationshipCycle { QString type; QStringList itemIds; };  // in path order
std::optional<QVector<RelationshipCycle>>
findRelationshipCycles(RoadmapStore &store, qint64 projectId, QString *error);
```

An empty vector is a clean store; `nullopt` with `*error` set is a failed check,
which is distinct from a clean one for the reason § 2.2 gives about silent
fallback. Cycles are sought per relationship `type`: two items may legitimately
be linked both `blocks` and `relates-to`, and folding the types together would
report that pair as a cycle.

**It ships with no scheduled caller, deliberately, and that is stated so nobody
reads its absence from the run loop as an oversight.** The scheduling belongs to
ANTS-3794 along with the rest of the health-check family (§ 5). Until then it is
reachable from `Inv8Acyclicity` and from a future check runner, and nothing calls
it in production — a declared, tested function with no caller is the correct
intermediate state when the id that owns the cadence has not landed.

## 3. Invariants

- **INV-1** — **A migrated project's rendered bullet contains its headline
  exactly once and each trailer key it carries exactly once**, `Kind:` included.
  *Rationale, not part of the assertion:* "exactly once" is a two-sided bound,
  and the lower side is already ANTS-3758's INV-12 — which is why § 2.3
  suppresses on value equality rather than on presence, since presence-based
  suppression can reach zero. *Breaks when:* the migration stores the head line
  in `item.body`
  (today's defect), or `renderBullet()` emits a column-sourced trailer line whose
  value the body already carries. *Test:* `roadmap_consumer_cutover/` case
  `Inv1NoDuplication`, over this directory's own bullet fixture, asserting
  exactly one occurrence of the headline and exactly one of each trailer key.
- **INV-2** — **Both backends produce the same `BulletRecord`s for the same
  project**, field-for-field over § 2.1's table, with `firstLine` / `lastLine`
  the only declared difference (0 on the store path). **`body` is NOT a declared
  difference**, because § 2.1 defines the store path's `body` as the rendered
  bullet minus `"- "` and its emoji — not the stored residual column, which
  would differ by its whole first line and fail this invariant by construction.
  *Breaks when:* the store backend orders by `id` rather than document order,
  drops narration elements' effect on position, returns `item.body` raw, fills
  `boldId` on a bracket-id bullet (§ 2.1: it is always empty on this path), omits
  `headline`'s 120-char ellipsis, or fills a field § 2.1's table does not assign.
  *Test:* `Inv2BackendsAgree`, which migrates a fixture's markdown, renders that
  store back to markdown, parses the **rendered** text, and compares
  record-for-record against `bulletsFromStore()` over every field the table
  lists. **The comparison is against the rendered text and not the source
  fixture, and that is the invariant's substance rather than a testing
  convenience:** a migrated project's `ROADMAP.md` *is* the render's output, so
  the rendered text is what the markdown backend will actually be handed. The
  fixture is written already-canonical — byte-identical to what the render emits
  — so the two texts coincide and a drift between them fails INV-7 rather than
  silently weakening this one.
- **INV-3** — **An `ants-v1` project with a store row is served by the store; any
  other project is served markdown; a store that fails to open is served
  neither.** *Breaks when:* the marker is resolved once per process rather than
  per call; a store that fails to open falls back silently; or a migrated
  pass-headings or GFM project is routed to the store, which § 5 scopes it out
  of. *Test:* `Inv3DispatchMarker`, three cases in one process — query a project
  before and after loading it, a loaded pass-headings project (must serve
  markdown), and an unreadable store file (must refuse, not fall back).
- **INV-4** — **A `roadmap_log` op that fails to render leaves the store
  unchanged.** *Breaks when:* the store transaction commits before the render
  runs. *Test:* `Inv4WriteRollsBack`, which fails the render and asserts the
  item's pre-op field values.
- **INV-5** — **`RoadmapParse` remains the only bullet grammar in `src/`,
  outside the enumerated exemptions below.** *Breaks when:* the render or a consumer
  grows its own trailer-key matcher instead of calling `trailerValuesIn()`.
  *Test:* `Inv5SingleGrammar`, a case-sensitive scrape of `src/` with comments
  stripped, for the **regex construction** `QRegularExpression` applied to a
  trailer-key literal — matching the pattern text
  `Kind:`/`Lanes:`/`Layman:`/`Evidence:`/`Source:` *inside a regex*, not the
  plain `"Kind: "` output literals `renderBullet()` legitimately emits, which a
  naive scrape hits. The shape ANTS-3758's INV-11 had to be corrected into after
  matching English prose.

  **Written as "one exemption" this invariant was red against today's tree, and
  the three live sites are resolved individually rather than by widening it.**
  Verified 2026-08-03:

  | Site | Disposition |
  |---|---|
  | `src/roadmapparse.cpp` | the grammar itself — **exempt**, by definition |
  | `src/remotecontrol.cpp:6576` and `:6749` (`rxBoldLayman`, ANTS-1933) | **exempt, and it cannot be otherwise.** Both deliberately capture the Layman sentence *including* its trailing period, because `rec.layman` is period-stripped by ANTS-1154 INV-4 and a period-less CHANGELOG body was the bug ANTS-1933 fixed. `trailerValuesIn()` returns the stripped value, so routing these through it would re-introduce that defect. Two sites, not one: the single-entry and batch `add_from_roadmap` paths carry the same block |
  | `src/roadmapdialog.cpp:640` (`rxKind`) | **moves to `trailerValuesIn(bodyFull).kind`** — a deliverable of this spec. It re-implements both the trailer regex *and* `parseBullets()`'s continuation-line assembly to build a kind-filter map, which is the second grammar this invariant exists to forbid. Its `s_lastInput` / `s_lastKindMap` memo is unaffected |
  | `src/remotecontrol.cpp:22382` (`rxCommitSha()`) | **exempt.** Its pattern embeds the literal `\bSource:\s*` as one alternative in a commit-SHA locator — it parses SHAs out of prose, and the trailer key is a *lead-in* it skips past, not a value it extracts. Routing it through `trailerValuesIn()` is meaningless. It is listed because the scrape in this invariant's test **will** match it: a `QRegularExpression` whose pattern text contains a trailer-key literal is exactly the shape being searched for |

  So the exemption list is those **four sites in three files**, named
  individually in the test, and any site outside this table is a failure. The
  fourth was missed by an earlier pass that asserted three and then declared "any
  fourth site is a failure" — which would have shipped `Inv5SingleGrammar` red on
  day one.
- **INV-6** — **The render and the reader agree about every trailer key.**
  Re-parsing a rendered bullet yields the same `kind` / `source` / `lanes` /
  `layman` / `evidence` the store holds. **This includes the accessor itself:
  for any body, `trailerValuesIn(body)` equals the values `parseBullets()`
  assigns to a record parsed from that body**, over all five keys and § 2.3's
  normalisation table. Without that equality the suppression compares
  incommensurable values, never fires, and ANTS-3808 stays live behind a passing
  spec. *Breaks when:* the accessor returns raw captures, skips the
  `rxTrailerKey` truncation or a trailing-period chop, splits `lanes` or
  `evidence` differently; or the render suppresses a key whose stored value
  differs from what the body re-parses to, or emits one the reader then reads
  twice. *Test:* `Inv6RenderReaderAgree`, three fixtures —
  a migrated bullet (values equal by construction, suppression fires), a
  post-cutover bullet with residual body (all keys emitted from columns), and
  § 2.3's un-anchored corner: a body whose prose shadows `Source:` and whose
  column was then rewritten, which must **refuse the write** rather than render
  a bullet that re-parses to the prose value.
- **INV-7** — **The full round-trip loses nothing and invents nothing.**
  ANTS-3758 INV-1's deferred half: render → `findRoadmaps()` → `planFrom()` →
  `load()` → export, compared against the source store's export under § 2.6's
  projection. *Breaks when:* any non-defaultable field is dropped from the
  bullet, or an element is emitted out of order. *Test:* `Inv7RoundTrip`, shown
  **red against the pre-§ 2.3 migration** before the fix is applied.
- **INV-8** — **A relationship cycle is reported, not refused and not ignored.**
  *Breaks when:* the check reports only self-relationships (which the `CHECK`
  already covers), or `relateItems()` starts rejecting writes. *Test:*
  `Inv8Acyclicity`, which stores `A → B → A` and asserts both the write
  succeeding and the check naming the cycle.
- **INV-9** — **A whole-project store read stays inside its declared budgets:
  ≤ 16 MiB resident and p95 < 50 ms on the corpus's largest project.** § 4 states
  both numbers; without an invariant they were assertions no case could fail, and
  a budget nothing measures is a comment. Over the ceiling the read **refuses**
  with `too_large` (the taxonomy's existing input-resource-over-cap code) rather
  than degrading or truncating, because a partial record set is
  indistinguishable to every consumer from a project with fewer bullets.
  *Breaks when:* `bulletsFromStore()` materialises the whole store rather than
  one project, the connection is reopened per call (§ 2.2), or the resolver
  re-reads the roadmap file to run the ants-v1 gate. *Test:* `Inv9Budgets`, which
  loads a generated project at ~2× this project's bullet count, asserts the
  refusal at the ceiling, and measures the warm p95 over repeated reads. The
  latency half is a benchmark and is **excluded from the default presets** for
  the reason `perf` already is — a timing assertion on a loaded host is a flake
  generator, and this project's own CI parity notes record it.

## 4. RAM / build cost

**RAM.** `bulletsFromStore()` materialises one project's records — the same
`QVector<BulletRecord>` the markdown path already materialises, from the same
data, so peak is unchanged for every existing caller. It is bounded by item
count, not store size, and the corpus's largest project is this one. Measured
2026-08-03 (`wc -l`, `wc -c`, and `grep -cE '^- (✅|📋|🚧|💭)'` over this
project's roadmap; `find /mnt/Games/Scripts/Linux -maxdepth 2 -name 'ROAD*MAP.md'
-exec wc -l {} +` confirms it is the largest, at 5.3× the next):

| Measure | Value |
|---|---|
| lines | 33,879 |
| bytes (UTF-8) | 3,075,143 (2.9 MiB) |
| top-level emoji bullets | 1,832 |

`BulletRecord` is dominated by `body`, and `QString` is UTF-16, so those bullets
cost ~5.9 MiB held as records plus the fields `body` also duplicates
(`headline`, `kind`, `source`, `layman`) — **≤ 8 MiB for a whole-project read**,
the same order as the 2.9 MiB markdown string the current path already holds and
re-parses on every call. **Budget: 16 MiB**, above which the read refuses with
`too_large` rather than degrading; that is ~2× the largest real project, so the
ceiling is a runaway guard and not a working limit. INV-9 asserts it and
`Inv9Budgets` measures it — an unmeasured budget is a comment. ANTS-3761 INV-12's 4 MiB export budget is
a different path and is unaffected. The oracle (§ 2.6) holds two stores and two
exports at once and is test-only, which is where ANTS-3758 § 2.6 already put
that cost.

**Latency.** `bulletsFor()` is called on the `roadmap_query` path, which the MCP
bridge budgets at 60 s since ANTS-3444 but which users experience per keystroke
in the dialog. Budget: **p95 < 50 ms** for a whole-project read on the corpus's
largest project, measured warm, and asserted by INV-9. The markdown path's own
parse is the baseline to beat, not a free comparison — it re-reads and re-parses
the file every call. The budget assumes § 2.2's process-owned connection: a
per-call `open()` plus pragma application would not fit inside it, which is why
that rule is stated as surface rather than left to the implementer.

**Build, and it is not free — the earlier claim that it was is withdrawn.**
`remotecontrol.cpp` compiles into `ants_core_lib` and `roadmapdialog.cpp` into
`ants_dialogs_lib`; **neither links `ants_roadmapstore_lib`**. An earlier draft
added that it is "linked only by the `ants-terminal` executable", which is
wrong: the `test_core` bundle links it too, beside `ants_core_lib`
(`CMakeLists.txt`, the `ants_add_core_bundle(test_core …)` `LIBS` list, tagged
`# ANTS-3756`). The layering argument is unaffected — what matters is that
neither *library* links it, so the edge below is genuinely new — but the
narrower claim is the true one and the wider one would have been read as
permission to skip the link change for tests. So calling
§ 2.1's resolver from either consumer adds a real link edge, and `ants_core_lib`
is the sensitive one — it is the lib every test bundle subsets.

**`roadmapsource.cpp` therefore lands in `ants_roadmapstore_lib`, and the two
consumers gain an explicit dependency on it** rather than the resolver being
hoisted into core. Hoisting would drag SQL into `ants_core_lib` for every
consumer that links core and never touches a roadmap, which is the layering
`CMakeLists.txt` records as deliberate ("Sql (ANTS-3756): the roadmap store.
Linked ONLY by `ants_roadmapstore_lib`"). The cost is two new edges and a longer
link for the test bundles that subset core; the alternative spends it on every
bundle. One exported accessor is added to the existing `roadmapparse.cpp`
(§ 2.3), which is already in `ants_core_lib` and adds no edge. Per this project's
cap, builds run under `cmake --build build` with the `JOB_POOLS` limit and tests
at `ctest -j4`.

## 5. Out of scope

- **Deleting the markdown splice paths.** They serve every unmigrated project
  for as long as the rollout takes (§ 2.2), and the id that retires them is not
  this one.
- **Non-emoji formats.** The store backend serves `ants-v1` emoji bullets only.
  3D_Engine (GFM task lists) and RetroDB (pass headings) keep the markdown path,
  which is the same scoping decision ANTS-3758's render made and for the same
  reason.
- **Fixing the un-anchored-key corner.** § 2.3 states the corner and § 2.4 makes
  it refuse loudly; *repairing the grammar* is out of scope here, because
  anchoring `rxSource` / `rxLanes` would discard the 157 inline values they were
  un-anchored for. That is a format decision needing its own id, **owed and not
  yet filed**.
- **Non-`parseBullets` roadmap readers** — `parseLastTouchDates()`,
  `collectCurrentBullets()`, `extractToc()`, `loadMarkdown()` (§ 1). They read
  the rendered `ROADMAP.md`, which survives cutover, and are not moved here.
- **The auto-publish cadence and the remaining health checks** — ANTS-3794,
  which § 2.7's check is scheduled by.
- **`roadmap-format.md` § 3.5.1's counter definition**, inherited as an
  obligation from ANTS-3758 § 7 and discharged in § 7 below rather than as
  surface here.

## 6. Tests

`tests/features/roadmap_consumer_cutover/`, label `features`, compiled into the
**`test_core` bundle** per `tests/features/README.md` (no `add_executable`).
That bundle, not merely "an existing" one: it is the only one already linking
both `ants_core_lib` and `ants_roadmapstore_lib` (§ 4), which is exactly the
pair these cases need.

| Case | Invariants |
|---|---|
| `Inv1NoDuplication` | INV-1 |
| `Inv2BackendsAgree` | INV-2 |
| `Inv3DispatchMarker` | INV-3 |
| `Inv4WriteRollsBack` | INV-4 |
| `Inv5SingleGrammar` | INV-5 |
| `Inv6RenderReaderAgree` | INV-6 |
| `Inv7RoundTrip` | INV-7 |
| `Inv8Acyclicity` | INV-8 |
| `Inv9Budgets` | INV-9 (latency half excluded from the default presets) |

Per this project's convention, **every case is verified RED against its *Breaks
when* mutation before the implementation is restored** (`testing.md` owns the
mutation-harness rules, including mtime busting, and they are not restated here).

**The fixtures are this directory's own.** INV-1's bullet is defined in
`roadmap_consumer_cutover/` rather than reached out of
`tests/features/roadmap_migrate_archive_root/`, whose `spec.md` scopes it to
preamble round-tripping and lists bullet-body fidelity as out of scope — so a
case here depending on its internals couples two contracts that were
deliberately split. (An earlier draft justified this by saying that fixture
"does contain the `DEMO-0003` bullet § 2.3 quotes". § 2.3 quotes no bullet; the
observation belongs to ANTS-3806's verification, where it was made, and the
coupling argument above stands without it.)

One rule worth restating because it is a silent data-loss trap rather than a
convention: **never default-construct `RoadmapStore`.** It resolves
`defaultPath()` — the developer's real store under `XDG_DATA_HOME` — so every
case would write into it. Always
`std::make_unique<RoadmapStore>(dir.filePath("store.db"))`.

## 7. Cross-doc impact

- **ANTS-3757 § 2.1.1's `body` row** reads "the reader's `headline` / `body`".
  § 2.3 changes what is stored, so the row is amended on ship to say the head
  line is dropped — otherwise the migration's own spec describes the defect.
- **ANTS-3765 § 2.10** says the fallback is "ANTS-3758's, not this half's".
  ANTS-3758 did not build it; § 2.2 does. That sentence is amended to name this
  id.
- **ANTS-3758's INV-1 (in its § 3; the oracle it rests on is its § 2.6) is
  REWORDED, not annotated.** As written it states the full render → load →
  export comparison and cites a shipped test that asserts only the standalone
  half. On ship it is narrowed to the half `Inv1ExportsMatch` proves, and points
  at this spec's INV-7 for the rest. Two live statements of one contract is the
  defect; a note beside one of them does not remove it.
- **ANTS-3758's INV-12 is untouched and that is the point.** § 2.3's per-key
  suppression compares *values*, so the required `Kind:` is always in the
  rendered text — INV-12's own test asserts against rendered text, not against
  the render's choice of source. Recorded here because a reader of § 2.3 will
  reasonably ask, and an unstated reconciliation reads as a silent repeal.
- **`RoadmapStore` gains `readProjectByRoot()` and a `root` field on
  `ProjectRow`** (§ 2.2) — a surface addition to ANTS-3756, in the shape
  ANTS-3758 § 2.1 used for `listElements()`. Without it the dispatch marker
  cannot be read at all.
- **`roadmap-data-model.md` should carry a source-format column** so § 2.2's
  emoji-only gate can be answered from the store rather than by re-detecting on
  the live file. Owed, not filed; the file-based guard ships meanwhile.
- **`roadmap-format.md` § 3.5.1's counter definition** still needs its cutover
  amendment — inherited from ANTS-3758 § 7, and this is the id that owns it. The
  amendment is **narrow**: § 3.5.1 already says `.roadmap-counter` is a derived
  per-machine cache and not source (ANTS-3450), with allocations flooring to the
  committed-corpus high-water, so nothing about that is overturned. What it does
  not yet say is that on a **migrated** project the per-machine cache's role is
  taken by `RoadmapStore::idHighWater()`, with the corpus floor still applying
  (§ 2.4). One added paragraph, not a rewrite of the section.
- **`roadmap-data-model.md`'s *What checks this* table** gains INV-7 against the
  round-trip row and INV-8 against relationship acyclicity, which ANTS-3760
  finding 9 left with no owner, and INV-9 against the read budgets.
- **`docs/standards/mcp-error-codes.md` gains `locator_unsupported` and
  `body_shadowed`** (§ 2.4) — two unrelated refusals that an earlier draft
  collapsed onto one code, which the standard forbids because callers branch on
  `code` alone. **Separately, and not caused by this id:** `bad_op_combo` is used
  23 times across `src/remotecontrol.cpp` and `src/claudeintegration.cpp` and is
  absent from that taxonomy entirely (verified 2026-08-03). Recorded here as a
  pre-existing gap so the ship-time edit can close all three at once; if it is
  split out it needs its own id.
- **`src/roadmapdialog.cpp`'s kind-filter pre-walk** (its own `rxKind` plus a
  re-implementation of `parseBullets()`'s continuation assembly) moves onto
  `trailerValuesIn()` as part of INV-5. Listed here because it edits a
  *rendering* path no other section would lead a reader to expect. **It is NOT
  behaviour-preserving** (corrected 2026-08-04): the dialog's local `rxKind`
  carries `MultilineOption` only, while `roadmapparse.cpp`'s adds
  `CaseInsensitiveOption` (ANTS-3407), so the swap newly matches `kind:` /
  `KIND:`. ANTS-3808's spec owns this item now.
- **`CLAUDE.md`'s module map and `docs/subsystems.md`** gain `roadmapsource`.
- **ANTS-3808's ROADMAP bullet is NOT closed by this spec** — reversed
  2026-08-03 when the umbrella was split four ways. It has its own spec and
  ships on its own; § 2.3 above is superseded. The bullet's own body records
  both the original homing and the reversal.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 3 (cap) | 2026-08-03 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet) | 3 / 5 / 8 / 7 / 0 | **Converged-by-cap. 4 fixed, 23 verified-and-filed, 0 dismissed. The spec is NOT accepted, and the recommendation is to SPLIT it rather than fix it in place.** Full tail at [`docs/reviews/ANTS-3793-cold-eyes-loop3-tail.md`](../reviews/ANTS-3793-cold-eyes-loop3-tail.md) — fold in directly, do NOT re-dispatch. Both lanes independently led on the same CRITICAL: **INV-2 is unsatisfiable against § 2.1's membership rules**, because the markdown backend parses the *rendered* file, from which ANTS-3758 § 2.4 removes `internal`/`dropped` and on which its INV-4 refuses outright for an unfiled item — so the backends provably cannot agree on any project holding one, and `Inv2BackendsAgree` fails on real data while looking like a store bug. Two further CRITICALs: `bulletsFromStore()` **cannot reach** the `renderBullet()` its own `body` contract is written in terms of (anonymous namespace, different TU, different library); and `body_shadowed` is **unimplementable** through the declared `TrailerValues`, which carries five values and no match provenance, leaving INV-6's third fixture unsatisfiable. Fixed in this loop, all contained and factual: `status` was specified as a verbatim column copy when the store holds a lifecycle **word** and `BulletRecord::status` holds an **emoji** (would have failed INV-2 on every record); `bundle_row`'s payload was called "the rendered row" when ANTS-3756 INV-24 requires **canonical JSON**, making it a read-modify-write; INV-5 asserted three sites then declared "any fourth a failure" while a fourth exists (`rxCommitSha()`, `src/remotecontrol.cpp:22382`, embeds `\bSource:\s*`), so it would have shipped red on day one; and § 2.5 named `readProject()` where the dialog holds only a root, over a `legendText` that is raw text needing a parse. **Stopped at the cap on both of Phase 5's triggers, not merely because the count ran out:** ~7 of loop 3's findings are collateral from loop 2's own fixes against ~5 draft defects (collateral outnumbering draft defects two loops running), and two *structural* draft defects surfaced only now — no declared store surface produces document order or a batched read (`listItems()` returns five fields and no ordering), and the `bundle_row` payload model — which two prior cold reads never reached. That is evidence of an oversized document, not of an insufficient review: 934 lines carrying seven contracts. Lane spend 46k / 34k input tokens, both inside the 60k budget. |
| 2-folded | 2026-08-03 | 0 (fold-in only — no dispatch) | 2 / 5 / 10 / 7 / 0 → all fixed, +3 found by the sweep | **Loop 2's recorded tail folded in directly from [`docs/reviews/ANTS-3793-cold-eyes-loop2-tail.md`](../reviews/ANTS-3793-cold-eyes-loop2-tail.md) rather than re-dispatched**, since the findings were already verified and a fresh two-lane run would have cost ~295k subagent tokens to regenerate them. Per Phase 5's stop-and-consolidate trigger the blast-radius sweep over loop 1's ledger ran **before** any per-finding edit, and it is what resolved both CRITICALs together: C1 (`body` diverging between backends) and C2 (`boldId`/`idToken` over-filled) are one defect, not two — loop 1 wrote § 2.1's field table without pinning *which text* the store path reconstructs. § 2.1 now derives every text field from **the bullet `renderBullet()` emits**, whose single shape (`- <emoji> [<id>] **<headline>**`, read off `src/roadmaprender.cpp:60`) makes `body` reconstructible, `idToken` equal to `id`, and `boldId` **always empty** — the last correcting loop 1, which had turned "wrongly left empty" into "wrongly filled". INV-2 is retargeted at the *rendered* text accordingly, which is what a migrated project's file actually contains. **Three findings the lanes did not have**, all from verifying citations the tail assumed good: `RoadmapRender::renderBullet()` **does not exist** — it is a free function in an anonymous namespace, so the tests cannot call it directly (the same defect class ANTS-3758's loop 2 caught for `writeElements()`); INV-5 was red at **three** sites, not the one H1 named (`roadmapdialog.cpp:640` re-implements `rxKind` *and* the parser's continuation assembly — it moves to `trailerValuesIn()`, while the two `rxBoldLayman` sites are permanent exemptions because ANTS-1933 needs the un-stripped sentence); and `bad_op_combo`, which the spec cited as an existing code, appears **nowhere** in `mcp-error-codes.md` despite 23 uses in `src/` — a pre-existing taxonomy gap now recorded in § 7 beside the two codes this spec adds. H4 inverted on verification: `roadmap-format.md` § 3.5.1 already says the counter is *not* source, so the risk was never "two allocators" but dropping the corpus floor, and § 2.4 now keeps `max(idHighWater(), corpusHighWater())`. H2's normalisation contract is stated as a table against the parser's actual post-match work. Budgets gained INV-9 + `Inv9Budgets` (M6); § 4's "linked only by the executable" was wrong (`test_core` links it too, M4). **One loop still owed** — this row is a fold-in, not a review. |
| 2-stopped | 2026-08-03 | 2 (same partition, cold; no prior-loop briefing) | 2 / 5 / 10 / 7 / 0 | **STOPPED before the fix pass, at the user's request to end the session — recorded so nobody reads this as convergence. One loop is owed and the spec is NOT accepted.** All 24 findings are written up at lane-level detail in [`docs/reviews/ANTS-3793-cold-eyes-loop2-tail.md`](../reviews/ANTS-3793-cold-eyes-loop2-tail.md); fold them in directly rather than re-dispatching to rediscover them. The two CRITICALs are both **loop-1 collateral**, which is the signal Phase 5 says to read: § 2.1's new field table and INV-2's "field-for-field, `firstLine`/`lastLine` the only difference" were written in loop 1 against a § 2.3 head-line-strip rule that predates them, so the two backends' `body` provably differ and `Inv2BackendsAgree` fails by construction; and the same table over-fills `boldId`/`idToken` "from the item's `id`", when `roadmapparse.h` records that `boldId` is EMPTY for the ordinary `[ANTS-NNNN]` bullet — loop 1 fixed "wrongly left empty" into "wrongly filled". Collateral outnumbering draft defects at the CRITICAL level is one of Phase 5's two stop-and-consolidate triggers; loop 3 should re-run the 4b sweep over loop 1's ledger before dispatching anything. Verified independently before the stop: `rxBoldLayman` (`src/remotecontrol.cpp`) is a second trailer-key regex outside INV-5's single exemption, so that invariant is red against today's tree. Two loop-1 lane findings were re-dismissed and are listed as such in the tail. |
| 1 | 2026-08-03 | 2 (single doc, cold; genre pinned `spec`) | 4 / 4 / 7 / 10 / 0 | 25 verified, all fixed; 2 dismissed. **Both lanes independently led on the dispatch marker, and it was unimplementable as written.** § 2.2 resolved a project *root* through `readProjectBySlug()`, which takes an `export_slug`, and `ProjectRow` carries no `root` at all — while ANTS-3765 § 2.10 makes `project.root` the marker. The store gains `readProjectByRoot()` and a `root` field, the same shape ANTS-3758 § 2.1 used for `listElements()`. Second: `migratedProject()` returned `std::optional<qint64>` with no error channel, so the spec's own no-silent-fallback rule could not be expressed — `nullopt` already meant "not migrated"; it now takes `QString *error` and has three outcomes. Third: the migration reads **all three** dialects (`roadmapmigrate.cpp` branches on `pass-headings`), so a migrated GFM or pass-headings project satisfied INV-3's "migrated" and § 5's "markdown only" at once; the marker is now gated on ANTS-3758 § 5's `detectRoadmapFormat` + `sawSignal` guard, read off the live file because **no store column records a source format** — `format` lives on the migration's `SourceRow` only, and § 7 records the column as owed. Fourth: § 4 claimed "no new link edge", and `remotecontrol.cpp` (`ants_core_lib`) and `roadmapdialog.cpp` (`ants_dialogs_lib`) do **not** link `ants_roadmapstore_lib` — only the executable does; § 4 now states the two real edges and why the resolver is not hoisted into core. Also fixed: the "cannot fill" field list was presented as exhaustive and omitted `headlineFull`, `sectionSlug`, `format`, `firstLine`/`lastLine`, while wrongly calling `boldId`/`idToken` dialect-only (ANTS-1987 extracts them on the native path) — replaced by a table over every field, which is INV-2's comparison set; § 1's "every consumer read goes through one function" ignored four further readers in `roadmapdialog.cpp` and is now scoped to bullet-record reads with those four named out of scope; ANTS-3758's INV-1 states the full oracle while its shipped test proves half, so § 7 rewords it rather than annotating; the render's membership rules were silently imported into the query path; "three copies" contradicted § 1's "a second time" (it is twice); § 2.7 asserted a check with no declared surface; § 4 pinned no numbers at all. **One fix's own collateral, caught by the 4b sweep rather than a lane:** § 2.3 and INV-6 were rewritten to cite a § 2.4 write-refusal that § 2.4 did not state. **Two lane findings dismissed on verification:** that no refusal exists for a non-`Bulk` connection (it does — `roadmapmigrateload.cpp` refuses `project_refused`; the file was missing from the packet I built, so this was my under-build, not a doc defect), and that the ANTS-3806 fixture has no `DEMO-0003` bullet (it does; the coupling concern survived as a LOW and the fixture is now local). The un-anchored `Source:`/`Lanes:` corner drove the largest design change: suppression compares **values**, not presence, which keeps ANTS-3758's INV-12 intact and makes the mismatch impossible for a migrated item by construction. |
