# ANTS-3793 — the read seam: one reader function, two backends

**Status:** spec draft (2026-08-04).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3793 (ANTS-3758 split, spec seam 3b of 5).
Rewritten 2026-08-04 from the 934-line umbrella of the same name, which the
user split four ways on 2026-08-03 after its cold-eyes run stopped at the loop
cap. See the loop log's `0-rewrite` row.
**Covers:** ANTS-3793 — the **read** half of the consumer cutover only.
**Blocked by:** ANTS-3758 (the render these consumers read back) — shipped.
ANTS-3808 (`RoadmapRender::bulletText()` and `ants_roadmapparse_lib`, both of
which § 2.1 and § 4 consume) — accepted, not yet built. **§ 4's link change
cannot land before ANTS-3808's**, and § 4 says why.
**Blocker for:** ANTS-3809 (the write half), ANTS-3810 (the round-trip oracle
and acyclicity check), ANTS-3794 (publish + health checks).
**Pairs with:** ANTS-3765 (the load half, whose § 2.10 marker § 2.2 consumes).

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 The seam](#21-the-seam-one-reader-function-two-backends) ·
[2.2 The dispatch marker](#22-the-dispatch-marker) ·
[2.3 RoadmapDialog](#23-roadmapdialog-and-the-legend)) ·
[3. Invariants](#3-invariants) ·
[4. RAM, latency and build cost](#4-ram-latency-and-build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

The store is primary after cutover (`roadmap-data-model.md` INV-3) and
ANTS-3758 can now write markdown back out of it, but **nothing reads the store
yet**. Every consumer still parses `ROADMAP.md`. Until they move, the store is
a write-only copy that drifts from the file the moment anyone edits it.

**The measurement that shapes this spec.** The consumer surface reads as
enormous, but every consumer read **of bullet records** goes through **one
function**:

```
$ grep -c 'parseBullets(' src/remotecontrol.cpp src/roadmapdialog.cpp
src/remotecontrol.cpp:23
src/roadmapdialog.cpp:6
$ grep -c 'RoadmapDialog::parseBullets(' src/remotecontrol.cpp
23
```
(2026-08-04.) Three of `roadmapdialog.cpp`'s six matches are the forwarder's
own comment, signature and one-line body, so there are **26 call sites**, and
23 of them are `RoadmapDialog::parseBullets(...)` in `remotecontrol.cpp`. That
forwarder's entire body is `return RoadmapParse::parseBullets(markdownText);`
(ANTS-3764). `src/roadmapmigrate.cpp`'s single call is **not** a consumer — it
is the migration reading source markdown to build the store, and it stays
markdown forever.

So the read cutover is not 26 rewrites. It is **one function with two backends
plus one wrapper per consumer**, and each of the 26 sites swaps its one call for
its own owner's wrapper (§ 2.1).

**The wrapper is the reason each site stays a one-line change, and saying so
precisely matters because the obvious claim is false.** § 2.1's seam takes a
`RoadmapStore &` and returns three outcomes, one of which means "parse the
markdown yourself" — so a call site wired **directly** to the seam would grow a
store reference *and* a three-way branch, 26 times over. It is the wrapper that
holds the store, the dispatch and the markdown fallback, exactly once per owner,
so what each site actually gains is a different one-line call with the same
return type. Sizing this cutover off "one function, two backends" alone
understates it by two owner-level members; sizing it at 26 branches overstates
it by 26. The 82 test call sites keep parsing markdown directly
(`grep -rn 'parseBullets(' tests/ --include=*.cpp | wc -l` → 82, 2026-08-04).

**"Bullet records" is a restriction, not a hedge, and the readers it excludes
are named so § 2.3 cannot quietly inherit the wider claim.**
`src/roadmapdialog.cpp` holds four further readers that do not go through
`parseBullets()` and are **out of scope**. Three read the *rendered*
`ROADMAP.md` — `parseLastTouchDates()`, `extractToc()` and `loadMarkdown()` —
taking headings, raw bullet lines and file mtimes, and that file keeps existing
after cutover because ANTS-3758's render writes it. The fourth,
`collectCurrentBullets()`, **reads no roadmap at all**: it returns
`readUnreleasedBullets(m_changelogPath)` plus `readRecentCommitSubjects()`, so
its inputs are `CHANGELOG.md` and `git log`. It is listed only because its name
invites the opposite assumption.

## 2. Surface

### 2.1 The seam: one reader function, two backends

`RoadmapParse::parseBullets(markdownText)` is a pure text-to-records function
and stays exactly that. The cutover adds a **sibling producer of the same
record type**, sourced from the store, and a resolver that picks between them:

```cpp
// src/roadmapsource.h — declaring src/roadmapsource.cpp, a new TU in
// ants_roadmapstore_lib (§ 4). BulletRecord is RoadmapParse's; the using
// below is what keeps the signatures below readable.
namespace RoadmapSource {
    using RoadmapParse::BulletRecord;

    // Why the ceiling needs its own channel rather than an *error string:
    // INV-3's refusal has to reach an MCP envelope as the code `too_large`,
    // and RoadmapDialog — which is not a verb and emits no envelope — has to
    // tell "too big, tell the user" apart from "the store is broken". A
    // caller branching on error TEXT is a caller that breaks on a reworded
    // message, which mcp-error-codes.md exists to forbid.
    enum class ReadError { None, StoreFailed, TooLarge };

    // The records a consumer would have got from parsing this project's
    // rendered markdown, sourced from the store instead. Document order
    // (§ 2.1.3), in the SAME shape RoadmapParse::parseBullets returns.
    //
    // Two outcomes only, and NEITHER is a fallback: engaged, or nullopt with
    // `*why` set (and `*error` carrying the human-readable detail). Reaching
    // this function already means the project is migrated, so there is no
    // "parse markdown instead" answer available to it. `why` is REQUIRED
    // rather than defaulted, because a caller that drops it cannot tell
    // TooLarge from StoreFailed.
    std::optional<QVector<BulletRecord>>
    bulletsFromStore(RoadmapStore &store, qint64 projectId,
                     ReadError *why, QString *error = nullptr);

    // § 2.2's dispatch. Three outcomes, not two:
    //   engaged               → migrated; read the store at this projectId
    //   nullopt, *error empty → not migrated; parse markdown as today
    //   nullopt, *error set   → REFUSE; never fall back (INV-1)
    //
    // `markdown` is REQUIRED and is the project's live roadmap text: § 2.2's
    // ants-v1 gate runs detectRoadmapFormat() over it, and no store column
    // records a source format. § 4 prices that retained read; § 7 names the
    // column that would remove it.
    std::optional<qint64> migratedProject(RoadmapStore &store,
                                          const QString &projectRoot,
                                          const QString &markdown,
                                          QString *error = nullptr);

    // The library seam the two owner wrappers call — the two above are its
    // halves, exposed because the tests drive them separately.
    //
    // Its three outcomes are the dispatch's, one layer on:
    //   engaged               → the store's records (migrated project)
    //   nullopt, *why == None → NOT migrated. The caller parses `markdown`
    //                           itself, exactly as it does today. This is the
    //                           common case and it is not an error.
    //   nullopt, *why != None → REFUSE and surface it. Never parse.
    //
    // `markdown` is the caller's existing text, used by the gate above and by
    // the caller on the unmigrated path. It is never re-read from disk here.
    std::optional<QVector<BulletRecord>>
    bulletsFor(RoadmapStore &store, const QString &projectRoot,
               const QString &markdown, ReadError *why,
               QString *error = nullptr);
}
```

`QString *error` is defaulted throughout, matching every existing
`RoadmapStore` reader; `ReadError *why` is not, for the reason its comment
gives.

**Each consumer owns one wrapper above that seam, and the wrapper is where the
store's lifetime and the markdown fallback live.** `RemoteControl` and
`RoadmapDialog` each gain one member of the shape

```cpp
// Returns the records for `projectRoot`, from the store when it is migrated
// and from `markdown` when it is not. The three-outcome branch, the
// process-owned RoadmapStore and the ReadError→envelope mapping all live
// here — once per owner, not once per call site.
QVector<BulletRecord> roadmapBullets(const QString &projectRoot,
                                     const QString &markdown,
                                     RoadmapSource::ReadError *why,
                                     QString *error);
```

and each of the 26 sites calls **that**. Two things follow, and both are why
the wrapper is surface rather than an implementation note. It is the only place
that may construct a `RoadmapStore`, which settles § 2.2's stat-and-open rule;
and it is where `ReadError` becomes either an MCP refusal code (`RemoteControl`)
or a dialog message (`RoadmapDialog`), which is the mapping INV-3's `too_large`
otherwise has no route to.

**`BulletRecord` is the interface, and that is a deliberate constraint rather
than a convenience.** Every consumer, every response field and all 82 test call
sites are already written against it, so a store backend that fills the same
struct changes no caller and no test. It also makes the two backends directly
comparable, which is what INV-2 asserts.

#### 2.1.1 The derivation rule

**One rule is normative, and everything below it is a consequence.**

> `bulletsFromStore()` fills each record with **exactly what
> `RoadmapParse::parseBullets()` would assign if it parsed the bullet
> `RoadmapRender::bulletText(item)` returns for that item**, under the section
> heading that item's section renders — **except `firstLine` and `lastLine`,
> which are 0.**

**The carve-out is stated in the rule and not only in the table, because the
table loses every disagreement with the rule.** `parseBullets()` assigns real
1-based spans; a store has no lines to number and no walk can invent them.
Leaving the exception to the table alone would make INV-2's own declared
difference a defect by the rule that governs it.

This is the *only* derivation under which INV-2 can be total, because a
migrated project's `ROADMAP.md` **is** that rendered text — so "what the store
backend returns" and "what the markdown backend returns for this project after
cutover" are the same object by construction rather than by coincidence.

**`bulletText()` is why this is reachable at all.** `renderBullet()` is a free
function in an anonymous namespace in `src/roadmaprender.cpp`; ANTS-3808 § 2.4
exports `RoadmapRender::bulletText(const RoadmapStore::ItemWrite &)` as its
body, in `src/roadmaprender.h`, **for this caller**. Both files are in
`ants_roadmapstore_lib`, so § 2.1's resolver reaches it with no link change and
no second renderer. An implementation that re-derives the bullet text itself is
the second renderer ANTS-3808's INV-2 forbids.

**The table below is consequences of the rule, not independent assertions.**
Where a row and the rule disagree, the rule wins and the row is the defect.
Stating it this way is what stops a mis-copied row failing INV-2 on real data —
the class that cost the umbrella two review loops. It is total over
`BulletRecord`'s **22 members** — 21 declaration lines, one of which declares
`firstLine` and `lastLine` together
(`awk '/^struct BulletRecord/,/^};/' src/roadmapparse.h | grep -cE '^\s+(QString|QStringList|int|bool)\s'`
→ 21, 2026-08-04) — which is INV-2's comparison set:

| Field | On the store path |
|---|---|
| `id`, `kind`, `lanes`, `evidence`, `layman`, `source` | from the item columns, verbatim |
| `status` | **mapped, never copied.** The store holds a lifecycle *word* (`roadmap-data-model.md` § 3.4); `BulletRecord::status` holds the *emoji*. The rule supplies the mapping for free — it is whatever `emojiFor()` put in the head line. Copying the column verbatim would put `"planned"` where every consumer expects `"📋"` and fail INV-2 on **every** record |
| `headline`, `headlineFull` | both from the stored `headline` through the parser's own `assignHeadline()`: `headlineFull` unchanged, `headline` **truncated to 120 characters and then `"…"` appended**, so an over-long display headline is 121 characters, not 120 (`truncateEllipsis()`, ANTS-1811 — it truncates first, surrogate-safe, then appends) |
| `body` | `bulletText(item)` with its leading `"- "`, the status emoji, **and the whitespace that follows the emoji** removed, and **each continuation line `trimmed()`**. Those are precisely `parseBullets()`'s own steps: `head = raw.mid(2)`, then `stripInlineEmoji()` — whose final statement is `while (!head.isEmpty() && head.front().isSpace()) head.remove(0, 1);` — and `body.append(cont.trimmed())` per continuation. **`trimmed()`, not "strip two spaces"**: an implementer who writes `mid(2)` diverges from the markdown backend on any continuation carrying indentation of its own and fails INV-2. It is therefore **not** an exact inverse of `appendIndented()`, which prefixes exactly two spaces — a stored body line that is itself indented renders deeper and parses back flat, so `item.body`'s own indentation does not survive a render/parse round trip. That asymmetry is ANTS-3810's oracle to characterise, not this seam's: **both backends run the same `trimmed()` over the same rendered text**, so they agree either way, which is all INV-2 asserts. **It is NOT the stored `item.body`**, which ANTS-3808 § 2.1 makes the residual alone |
| `sectionHeading`, `sectionLevel`, `sectionSlug` | from the item's `SectionRow` via its element. `sectionSlug` is `uniqueSlug()`'s form, since that is what the parser assigns from the rendered heading — not `SectionRow::slug`, which is the store's key and can differ |
| `format` | always `"ants-v1"` — the store path serves no other dialect (§ 2.2, § 5) |
| `firstLine`, `lastLine` | **0.** Markdown line numbers do not exist in a store, and no store read can invent them. Declared difference #1 in INV-2; ANTS-3809 owns what `roadmap_log`'s `line_range` locator does about it |
| `sourceStatus`, `passDesignator`, `anchor` | empty — each is an artefact of a dialect the store path does not serve |
| `synthetic` | `false` — a store id is a real id; nothing is content-hashed on this path |
| `idToken`, `boldId` | whatever the rule yields, which depends on the head line's shape and **not** on the item's `id` column. For the ordinary item, `renderBullet()` emits `[<id>] **<headline>**` and `rxLeadToken` captures the bracket, so `idToken == id` and `boldId` is empty — `extractBoldId()` is head-anchored and the head starts with `[`. **For an item whose `id` column is empty the bracket is not emitted at all** (`if (!it.id.isEmpty())`), the head starts `**<headline>**`, and a single id-shaped headline word then satisfies `rxIdShaped` and fills *both*. Filling these from the `id` column instead would fail INV-2 on the commonest bullet in the corpus |

#### 2.1.2 What the store path returns

**Every item the store holds for the project, except those whose `status` is
`dropped`.** The exclusion is forced by the record type, not imported from the
render's membership rules, and the distinction decides INV-2's scope.

- **`dropped` items are excluded because `BulletRecord` cannot represent
  them.** `emojiFor()` returns an **empty string** for `dropped` by design
  (`roadmap-format.md` § 3.11 makes a fifth glyph an anti-pattern), so
  `bulletText()` emits a head line with no emoji; `parseBullets()`' native path
  then fails `stripInlineEmoji()` and **skips the bullet entirely**
  (`if (!stripInlineEmoji(head, status)) { ++i; continue; }`). Under § 2.1.1's
  rule the record for a dropped item is therefore *no record*. Returning one
  anyway would put an empty string in a `status` field documented as one of
  four glyphs.
- **`internal` items ARE returned.** `BulletRecord` has no visibility field and
  `renderBullet()` never consults one, so the record is well-formed. Filtering
  here would give `roadmap_query` a visibility concept it does not have and has
  never had, which is a silent behaviour change to a verb this spec is meant to
  leave alone.
- **Unfiled items ARE returned**, with `sectionHeading` / `sectionSlug` empty
  and `sectionLevel` 0. ANTS-3758's INV-4 **refuses the whole render** on an
  item with no element row; a query that hid it instead would make that fault
  invisible to the only tool likely to surface it.

**A freshly migrated project holds none of the three, which is why INV-2 is
satisfiable at all.** Verified 2026-08-04: `item.visibility` is
`NOT NULL DEFAULT 'public' CHECK (visibility IN ('public','internal'))` and
neither `src/roadmapmigrate.cpp` nor `src/roadmapmigrateload.cpp` ever writes
it; `statusFromMarker()` maps ✅→`shipped`, 🚧→`in-progress`, 💭→`considered`
and everything else→`planned`, so migration cannot produce `dropped`; and every
migrated item is filed, `sectionId == 0` being described at `ItemRef` as
"transiently, mid-rebuild". So on a migrated-and-not-yet-written project the
store set and the renderable set **coincide**, and the divergence is reachable
only once ANTS-3809's consumer writes exist.

#### 2.1.3 Document order

**No single declared store surface produces it, and the derivation is stated
here because an implementer left to guess reaches for `listItems()`, which is
ordered by rowid.** The walk is:

1. `listSections(projectId)` — returns `ORDER BY section_id`, which its own
   comment calls "only for determinism between two reads of the same store".
2. Sort that vector with the free function `sectionOrderLess()`, whose key is
   `(position, slug)`. It is a C++ comparator and not an `ORDER BY` because
   `QString::compare()` is UTF-16 code-unit order and SQLite's BINARY collation
   is UTF-8 byte order, and the two disagree on the supplementary-plane
   characters an emoji heading slug reaches.
3. Per section: `findSection(projectId, slug)` for the id — `SectionRow`
   carries no `section_id` — then `listElements(sectionId)`, which **is**
   `ORDER BY e.position` and whose comment says the SQL sort is the contract
   there, `UNIQUE (section_id, position)` having made position total.
4. `listItems(projectId)` once, then `readItem(itemPk)` per item into a
   `QHash<qint64, ItemWrite>`. `listItems()` returns `ItemRef`
   (`itemPk`, `idFold`, `headline`, `sectionId`, `idFromMigration`) — no
   status, kind or body — and there is no batched full-item reader.
5. Walk the elements in order; each `kind == "item"` element emits one record.
   `narration` and `table` elements are skipped: they are not bullets and
   `parseBullets()` does not return records for them either.
6. **Unfiled items last**, after every filed item, ordered by `idFold`. They
   have no element row and therefore no position; appending them in a
   deterministic order is the only answer that is both total and reproducible.

**This is `RoadmapRender::render()`'s own walk, and taking the same one is a
requirement rather than a coincidence.** That function reads
`listSections()` → `sectionOrderLess` → `findSection()` → `listElements()` and
builds the same `QHash<qint64, ItemWrite>` from `listItems()` + `readItem()`.
Two walks that were supposed to agree and drifted would break INV-2 in a way
that reads as a store bug.

### 2.2 The dispatch marker

ANTS-3765 § 2.10 fixed the marker and its guarantee: **a `project` row exists
exactly when that project's whole plan committed**, per-project atomicity making
"half a project" unreachable. That spec states the marker and does not build the
fallback. This one builds it.

**The store has no reader that can take that marker, and adding one is this
spec's first obligation.** `registerProject(root, name, exportSlug)` writes the
root and ANTS-3756 INV-8 keys a project on its **canonical root**, but the only
readers are `readProject(projectId)` and `readProjectBySlug(exportSlug)`, and
`ProjectRow` carries no `root` at all — the `project` table's `root TEXT UNIQUE`
column is write-only through today's reader surface. So:

```cpp
// src/roadmapstore.h — the same gap ANTS-3758 § 2.1 found for listElements().
std::optional<ProjectRow> readProjectByRoot(const QString &canonicalRoot,
                                            QString *error = nullptr) const;
```

`ProjectRow` gains `root` in the same change, because a caller that resolved a
project by root and cannot read the root back cannot confirm the
canonicalisation INV-8 depends on.

**`migratedProject()` canonicalises `projectRoot` before it looks anything up**,
by the same `QFileInfo::canonicalFilePath()` call `registerProject()` used.
(Throughout this document **"the resolver" means `bulletsFor()`** and nothing
else; the dispatch half and the owner wrapper are always named outright, because
the three have different signatures and different obligations.) A reader
passing the caller's raw path would miss on every symlinked or non-normalised
root and report "not migrated" — the silent fallback INV-1 exists to forbid,
arriving through the one door the invariant does not watch. An **empty** result
(the path does not resolve) is an `*error`-set refusal, not a `nullopt`
fallback.

Seven rules follow, and each exists because the obvious reading is wrong:

- **The store not existing is `nullopt` with no error.** Most machines running
  this code have never migrated anything, and a verb that refused there would
  break every unmigrated project on the day the store shipped.
- **The store existing and failing to open IS an error** — `nullopt` *with
  `*error` set*. A two-valued return cannot express it: `nullopt` already means
  "not migrated, parse markdown", so without the third outcome the no-silent-
  fallback rule is unimplementable through its own signature. A corrupted store
  that quietly falls back is a store nobody notices is corrupt.
- **The OWNER'S WRAPPER stats the path and owns the lazy open — not
  `RoadmapSource`, which cannot.** `RoadmapStore` separates construction from
  `open()`/`isOpen()`, so "not existing" and "failing to open" are
  distinguishable only by whoever constructs, and every function in § 2.1 takes
  an already-constructed `RoadmapStore &`. So the two rules above are the
  wrapper's obligations, in this order: stat `RoadmapStore::defaultPath()`;
  **absent → do not call the seam at all**, parse `markdown` (rule 1); present
  → construct once, `open()`, and on failure return `ReadError::StoreFailed`
  without calling the seam (rule 2); open → call `bulletsFor()`. Naming the
  owner matters because these two outcomes are otherwise decided by whoever
  happened to call `open()`, and an earlier draft assigned them to a function
  that receives its store by reference and can decide neither.
- **A migrated project whose roadmap is not `ants-v1` is `nullopt`, not
  engaged.** The migration reads all three dialects
  (`src/roadmapmigrate.cpp` branches on `"pass-headings"`), so a project row
  existing does **not** imply this path can serve it, and § 5 scopes the store
  backend to emoji bullets. Without this rule INV-1 and § 5 contradict each
  other for every migrated RetroDB- or 3D_Engine-shaped project. The test is
  ANTS-3758 § 5's guard, reused unchanged:
  `detectRoadmapFormat(lines, &sawSignal)` must return `"ants-v1"` **and** set
  `sawSignal`, because that function answers `ants-v1` for input it does not
  recognise and `sawSignal` is what separates the two. It is read off the live
  file because no store column records a source format — `format` lives on
  `RoadmapMigrate::Source` and nowhere in `roadmapstore.h`.
- **On a project that HAS a store row, an UNRECOGNISABLE roadmap is an error,
  not a fallback — which is a different case from a recognisably foreign
  dialect.** `detectRoadmapFormat()` returns `"ants-v1"` with
  `sawSignal == false` for input it does not recognise, so a migrated project
  whose `ROADMAP.md` is absent, empty or mangled would otherwise take the
  `nullopt` branch and be served markdown — precisely the silent fallback INV-1
  forbids, and the case where the file is least trustworthy. So `sawSignal`,
  not the returned dialect, is what separates the two outcomes:

  | Store row | `detectRoadmapFormat()` | Outcome |
  |---|---|---|
  | yes | `"ants-v1"`, `sawSignal` set | **store** |
  | yes | `"github-task-list"` / `"pass-headings"` (`sawSignal` set) | **markdown**, `nullopt`, no error — legitimately markdown-served (rule above) |
  | yes | any dialect, `sawSignal` **false** | **refuse** — `ReadError::StoreFailed`, never markdown |
  | no | — | **markdown**, `nullopt`, no error |

  **The detector reads at most the first 300 non-blank lines**
  (`if (++seen >= 300) break;`), which is safe here for a reason worth stating
  rather than relying on: a *rendered* roadmap carries the
  `ants-roadmap-format: 1` HTML-comment marker in its preamble, and the detector
  returns on that line with `sawSignal` set before any bullet is examined. A
  migrated project whose file has lost that marker AND whose first 300
  non-blank lines carry no emoji bullet lands in row 3 — refused, which is the
  correct answer for a file that no longer looks like what the store says it
  is.
- **The connection profile is `Access::Bulk` for a migration and
  `Access::Interactive` for a consumer**, named at each call site rather than
  defaulted. `RoadmapMigrateLoad::load()` **refuses** a non-Bulk store outright
  (ANTS-3765 § 2.2 / INV-12), so the wrong profile fails loudly. `Access`
  selects a busy deadline and page-cache size — 5 s and Qt's default cache for
  `Interactive`, 30 s and 16 MiB for `Bulk` — and a five-second deadline is
  right for a verb call and wrong for a corpus load.
- **The `RoadmapStore &` the consumers pass is one long-lived, process-owned
  `Access::Interactive` connection per store path, opened on the first dispatch
  that finds a store file and never per call.** Opening SQLite, applying pragmas
  and warming the page cache is the dominant term in § 4's p95 budget, and a
  per-call connection would blow it on its own. Ownership sits with the object
  that owns the other per-process integration state (`RemoteControl` for the
  verbs, `RoadmapDialog` for the dialog); `RoadmapSource` takes a reference
  precisely so it owns no lifetime itself.

**The marker is resolved once per call; the connection is opened once per
process.** A verb call that cached the marker at startup would serve markdown
for the rest of the session to a project migrated in between, so the
`readProjectByRoot()` lookup runs every call. Conflating the two is how this
design gets implemented as a per-call `QSqlDatabase::open()`, which is why they
are written as separate sentences.

### 2.3 RoadmapDialog, and the legend

The dialog's three consumer reads go through the same resolver and need no other
change: it renders `BulletRecord`s and will receive `BulletRecord`s. They sit in
`RoadmapDialog::renderCardsHtml()`, `captureScrollAnchor()` and
`restoreScrollAnchor()`. The latter two each re-parse the whole file to resolve
one anchor, which the code accepts because it happens on dialog close and
reopen, not per scroll event ("One parse on close is cheap relative to the user
action"). That cadence is what makes a store read acceptable in the same place,
and INV-2's record-for-record equality is what keeps the anchor landing where it
did — the anchor key is `BulletRecord::sectionSlug`, which § 2.1.1 fills.

**On the store path the dialog renders that project's own stored legend; on
every other path its existing behaviour is unchanged.** ANTS-3793's bullet
leaves this open — the data model's § 5.1 "makes it possible, not mandatory".

**Both halves are load-bearing, and the second is what stops this being a
regression.** `src/roadmapdialog.cpp` today holds the four status emojis and
their labels as compile-time constants (a `kEmojiDone`/`kEmojiInProgress`/
`kEmojiPlanned`/`kEmojiConsidered` → `QT_TR_NOOP` label table). Those constants
**stay, and stay in use for every unmigrated project** — which is most of them
during the rollout. Written as the flat rule "a project with no stored legend
shows none", this section would have silently deleted the legend from every
markdown-served project, a user-visible regression arriving as a side effect of
a read-path change. So:

| Path | Legend shown |
|---|---|
| store (migrated, `ants-v1`) with a stored legend | that project's stored legend |
| store, no stored legend | none — for the reason ANTS-3758 § 2.8 gives: the legend is per project precisely so one renderer serves every project's vocabulary, and a default would show this project's vocabulary for another's statuses |
| markdown (unmigrated, or a foreign dialect) | today's compile-time constants, unchanged |

Deciding it *renders* costs one extra `readProject(projectId)` — using the id
`migratedProject()` has just returned, since that function yields
`std::optional<qint64>` and not a `ProjectRow`, so the dispatch lookup does
**not** hand the dialog the row it needs. (An earlier draft had this backwards,
claiming the dispatch already supplied the row and that `readProject()` was
unusable for want of an id.) It also costs a **parse**: `ProjectRow::legendText`
is "the RAW stored text, not a parsed `QJsonObject`", held that way so the
export's byte-identity contract does not round-trip through the middle of
ANTS-3761's INV-1, and **no declared reader returns it parsed** — so the dialog
parses it itself, exactly as `roadmaprender.cpp` does
(`QJsonDocument::fromJson(legendText.toUtf8()).object()`).

## 3. Invariants

Renumbered from the umbrella, matching the sibling parts' convention
(ANTS-3808 renumbered from 1 the same way). The mapping, so the split record on
the ROADMAP bullet resolves: umbrella INV-3 → **INV-1**, umbrella INV-2 →
**INV-2** (unchanged), umbrella INV-9 → **INV-3**.

- **INV-1** — **An `ants-v1` project with a store row is served by the store; a
  project with no store row, and a store-row project whose roadmap is a
  *recognisable* non-`ants-v1` dialect, are served markdown; a store that fails
  to open, and a store-row project whose roadmap is *unrecognisable*
  (`detectRoadmapFormat()` leaves `sawSignal` false), are served neither.**
  § 2.2's table is this sentence in full, and the distinction it turns on is
  `sawSignal` rather than the returned dialect — a migrated GFM project is
  legitimately markdown-served, and an earlier draft that refused it would have
  broken every such project. *Breaks when:* the marker is resolved once per
  process rather than per call; a store that fails to open falls back silently;
  a migrated pass-headings or GFM project is routed to the store (§ 5 scopes it
  out) **or is refused rather than served markdown**; or a store-row project
  with an absent, empty or unparseable `ROADMAP.md` falls back to markdown
  instead of refusing. *Test:* `Inv1DispatchMarker`, five cases in one process
  — a project with no store file at all (must serve markdown, the case that
  protects every unmigrated project); a project queried before and after
  loading it; a loaded pass-headings project (must serve markdown, `why ==
  None`); an unreadable store file (must refuse, `why == StoreFailed`); and a
  loaded project whose roadmap text is empty (must refuse, not fall back).
- **INV-2** — **Both backends produce the same `BulletRecord`s for the same
  project**, field-for-field over § 2.1.1's table, **over the renderable, filed
  subset**. Two declared differences and no others: `firstLine` / `lastLine`
  are 0 on the store path, and **membership** — the store path also returns
  `internal` and unfiled items, which the rendered file does not contain
  (ANTS-3758 § 2.4 excludes `internal`; its INV-4 refuses the render outright
  on an unfiled item, so such a project has no rendered text to compare
  against at all). `body` is **not** a declared difference. *Breaks when:* the
  store backend orders by `id` or by `item_pk` rather than § 2.1.3's walk; it
  drops narration and table elements' effect on position; it returns
  `item.body` raw; it fills `idToken` or `boldId` from the `id` column rather
  than from the rendered head line; it omits `headline`'s 120-character
  ellipsis; it copies `status` verbatim instead of the emoji; or it fills a
  field § 2.1.1's table does not assign. *Test:* `Inv2BackendsAgree`, which
  migrates a fixture's markdown, renders that store back to markdown, parses
  the **rendered** text, and compares record-for-record against
  `bulletsFromStore()` — **equality over the 20 compared fields, with
  `firstLine` and `lastLine` asserted to be 0 on the store path rather than
  compared**, which is what "field-for-field over 22 members with two declared
  differences" means operationally. **The comparison is against the
  rendered text and not the source fixture, and that is the invariant's
  substance rather than a testing convenience:** a migrated project's
  `ROADMAP.md` *is* the render's output, so the rendered text is what the
  markdown backend will actually be handed. The fixture is written
  already-canonical, so the two texts coincide and a drift between them fails
  ANTS-3810's round-trip oracle rather than silently weakening this one.
- **INV-3** — **A whole-project store read is refused above a 3,500-item
  ceiling, and stays under p95 < 50 ms below it, on the corpus's largest
  project.** Both numbers are derived in § 4; a budget nothing measures is a
  comment. Over the ceiling the read **refuses** with `ReadError::TooLarge`,
  which § 2.1's wrapper maps to `too_large` at an MCP boundary, rather than
  degrading or truncating — a partial record set is indistinguishable to every
  consumer from a project with fewer bullets. **The refusal fires when
  `listItems().size() > 3500`, tested before any `readItem()` runs**, so
  exactly 3,500 is accepted.

  **The item count is a deliberately over-inclusive proxy for a 16 MiB record
  budget, and the invariant asserts the count, not the bytes.** Resident size is
  knowable only after materialising, which is the thing being guarded, and
  `too_large`'s taxonomy entry describes a resource sized *before* it is read
  into RAM. Two consequences, both accepted rather than hidden: `listItems()`
  counts every item row including the `internal` and `dropped` ones § 2.1.2 does
  not return, so the gate can refuse a project whose materialised set is under
  budget; and § 4's 16 MiB figure is a *sizing input*, not an assertion — no
  case measures resident bytes, and a project of 3,500 atypically large bullets
  passes the gate and exceeds 16 MiB. Making it exact needs a
  `SUM(LENGTH(body))` reader (ANTS-3816) for a guard that exists to catch
  runaways. *Breaks when:* `bulletsFromStore()` materialises the whole store
  rather than one project; the connection is reopened per call (§ 2.2);
  **`RoadmapSource` itself re-reads the roadmap file from disk** (the caller's
  retained read is § 4's, and is not this breach); or the ceiling is tested
  after the item bodies are already resident, which no input can then trip.
  *Test:* `Inv3Budgets`, over generated projects sized **from the ceiling** —
  3,501 items asserting the refusal and **3,500 asserting it does not fire**,
  which is what pins `>` rather than `>=` — plus a warm p95 over repeated reads
  of a corpus-sized project. The latency half is a benchmark and is **excluded
  from the default presets** for the reason `perf` already is: a timing
  assertion on a loaded host is a flake generator. § 6 says what runs it.

## 4. RAM, latency and build cost

**RAM, and the markdown load is retained rather than replaced — the umbrella
claimed otherwise and that was its largest costing error.** § 2.2 makes
`markdown` REQUIRED so the ants-v1 gate has an input, so on a migrated project
the caller **still reads the whole `ROADMAP.md`**, and the store read is
**additive**. Measured 2026-08-04 over this project's roadmap, the largest in
the corpus (`find /mnt/Games/Scripts/Linux -maxdepth 2 -name 'ROAD*MAP.md' -exec
wc -l {} +` puts it at 5.3× the next):

| Measure | Command | Value |
|---|---|---|
| lines | `wc -l` | 34,261 |
| bytes (UTF-8) | `wc -c` | 3,100,428 (3.0 MiB) |
| top-level emoji bullets | `grep -cE '^- (✅\|📋\|🚧\|💭)'` | 1,839 |

(ANTS-3808 § 2.1's 1,646 is a different population — bracket-id bullets only.
All three figures above move with every roadmap append, so a re-measure that
disagrees in the last digits is drift, not a defect; the arithmetic below is
insensitive to it.)

**`QString` is UTF-16, and the doubling applies to BOTH strings — an earlier
draft applied it only to the records and so priced the retained read at half.**
The file's 3.0 MiB of UTF-8 is **~6 MiB held as a `QString`**, and that is what
§ 2.2's gate retains on every migrated call. The records are bounded by the same
text: `BulletRecord` is dominated by `body`, and every byte of every bullet
lands in exactly one record's `body`, so **~6 MiB is an upper bound for the
bodies** — the whole file's UTF-16 size used as a bound, not a measurement of
the bullets alone — plus the fields `body` also duplicates (`headline`, `kind`,
`source`, `layman`). Call it **≤ 8 MiB of records**. So the migrated path peaks
near **14 MiB**, not the ~11 MiB the halved figure implied, and **the retained
markdown is the larger single term until ANTS-3815 removes it**.

**Budget: 16 MiB for the records**, ~2× the largest real project: a runaway
guard, not a working limit. **The pre-read proxy is item count**, since INV-3
must decide before materialising: 8 MiB / 1,839 items ≈ 4.5 KiB per item, so
16 MiB is ~3,650 items, **rounded down to a ceiling of 3,500**, statted from
`listItems()`' size — cheap, because `ItemRef` carries a headline and four
scalars, not a body. It is deliberately a *count* and not a
byte total: a project with atypically large bodies could pass the count gate and
still exceed 16 MiB, and closing that would need a new `SUM(LENGTH(body))`
reader for a guard that exists to catch runaways rather than to be exact.
ANTS-3761 INV-12's 4 MiB export budget is a different path and is unaffected.

**Latency.** Budget: **p95 < 50 ms** for a whole-project read on the corpus's
largest project, measured warm, asserted by INV-3. The markdown path's own
parse is the baseline to beat, not a free comparison — it re-reads and
re-parses 3.0 MiB every call. The budget assumes § 2.2's process-owned
connection.

**The query shape is § 2.1.3's walk, and it is N+1 by construction** — one
`readItem()` per item, ~1,839 on this project, because no batched full-item
reader exists. That is not novel cost: `RoadmapRender::render()` already pays
exactly this N+1 on every roadmap write, against the same store. It is
nonetheless the term most likely to break the budget, so the remedy is named
here rather than invented under pressure: **if `Inv3Budgets` reds on latency,
the fix is a batched `readItems(projectId)` on `RoadmapStore`, not a cache and
not a relaxed budget.** § 7 records it as conditionally owed.

**Build — and it consumes a stated design property, which is why it is argued
rather than asserted.** `roadmapsource.cpp` lands in `ants_roadmapstore_lib`,
beside the `roadmaprender.cpp` whose `bulletText()` § 2.1.1 calls and the
`roadmapstore.cpp` it reads. Nothing leaves that library's link surface: it is
`PUBLIC Qt6::Core Qt6::Sql`, and after ANTS-3808 § 4 it also links the
`Qt6::Core`-only `ants_roadmapparse_lib`, which supplies `BulletRecord`,
`parseBullets()` and `detectRoadmapFormat()`. **So ANTS-3808's link change must
land first** — without `ants_roadmapparse_lib` the resolver's only route to the
grammar is `ants_core_lib`, the edge ANTS-3808 § 4 rejected on measurement —
`target_link_libraries(ants_core_lib)` exports
`Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus util` **PUBLIC**.

**What this spec does spend is two new edges INTO the store library:**
`ants_core_lib` (`remotecontrol.cpp`) and `ants_dialogs_lib`
(`roadmapdialog.cpp`) both gain `ants_roadmapstore_lib`, and therefore
`Qt6::Sql`. `CMakeLists.txt` records the current arrangement as deliberate —
"Sql (ANTS-3756): the roadmap store. Linked ONLY by `ants_roadmapstore_lib`",
and "keeping it out of `ants_core_lib` means the test bundles that link core do
not drag the SQL module". **That property ends here, and it ends because the
cutover is the event it was waiting for:** it was true while nothing in core
touched the store, and after this spec the roadmap consumers in core *are*
store consumers. The comment is amended in the same change (§ 7) rather than
left contradicting the build. The cost is `Qt6::Sql` — one module, no widgets,
no network — on the test bundles that subset core; `test_core` already links
both, so it pays nothing new.

The alternative was rejected: keeping `ants_core_lib` clean by reaching the
resolver through an injected callback the executable wires at startup. It buys
a link edge back at the price of an indirection every consumer must not forget
to check, and a null callback is a silent fallback to markdown — the exact
failure INV-1 exists to forbid, reintroduced as a wiring bug.

## 5. Out of scope

- **The write half.** `roadmap_log`'s eight ops, the `line_range` locator, id
  allocation on a migrated project, and the `body_shadowed` refusal are
  **ANTS-3809**.
- **The round-trip oracle and relationship acyclicity** — **ANTS-3810**.
- **`item.body`'s storage rule and the trailer suppression** — **ANTS-3808**,
  whose `bulletText()` export and `ants_roadmapparse_lib` this spec consumes as
  a fixed contract.
- **Deleting the markdown parse paths.** They serve every unmigrated project
  for as long as the rollout takes, and the id that retires them is not this
  one.
- **Non-emoji formats.** The store backend serves `ants-v1` emoji bullets only.
  3D_Engine (GFM task lists) and RetroDB (pass headings) keep the markdown
  path — the same scoping ANTS-3758's render made, for the same reason.
- **Non-`parseBullets` roadmap readers** — `parseLastTouchDates()`,
  `collectCurrentBullets()`, `extractToc()`, `loadMarkdown()` (§ 1).
- **The auto-publish cadence and the health checks** — ANTS-3794.

## 6. Tests

`tests/features/roadmap_read_seam/`, label `features`, compiled into the
**`test_core` bundle** per `tests/features/README.md` (no `add_executable`).
That bundle, not merely "an existing" one: it is the only one already linking
both `ants_core_lib` and `ants_roadmapstore_lib`.

| Case | Invariants |
|---|---|
| `Inv1DispatchMarker` | INV-1 |
| `Inv2BackendsAgree` | INV-2 |
| `Inv2Membership` | INV-2's membership difference (below) |
| `Inv3Budgets` | INV-3 (latency half excluded from the default presets) |

**`Inv2Membership` exists because INV-2 cannot reach § 2.1.2.** That invariant
scopes itself to the renderable, filed subset, which excludes `internal` and
unfiled items *by definition* — so the three membership rules that decide what
`bulletsFromStore()` returns would otherwise ship with no case at all. It
populates a store directly (not via migration, which cannot produce any of the
three — § 2.1.2) with one `internal` item, one `dropped` item and one unfiled
item beside two ordinary ones, and asserts: the `dropped` item is absent, the
`internal` item is present, the unfiled item is present with an empty
`sectionSlug` and sorted after every filed item.

**The latency half of `Inv3Budgets` is excluded from the default presets and is
therefore scheduled explicitly, or it never runs.** It is registered under the
`perf` label and runs with `ctest --preset=perf`; the release checklist is what
exercises it, the same cadence the project's other benchmarks get. A budget
asserted by a case nothing invokes is the "budget nothing measures" INV-3 opens
by rejecting.

Per this project's convention, **every case is verified RED against its *Breaks
when* mutation before the implementation is restored** (`testing.md` owns the
mutation-harness rules, including mtime busting).

**The fixtures are this directory's own**, not reached out of
`tests/features/roadmap_migrate_archive_root/`, whose `spec.md` scopes itself to
preamble round-tripping and lists bullet-body fidelity as out of scope — a case
here depending on its internals would couple two contracts that were
deliberately split.

One rule worth restating because it is a silent data-loss trap rather than a
convention: **never default-construct `RoadmapStore`.** It resolves
`defaultPath()` — the developer's real store under `XDG_DATA_HOME` — so every
case would write into it. Always
`std::make_unique<RoadmapStore>(dir.filePath("store.db"),
RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Interactive)` —
`Access` is the **third** constructor parameter, after the history cap, and this
is the form the existing store cases already use. Name the profile rather than
defaulting it, per § 2.2's sixth rule.

## 7. Cross-doc impact

- **`RoadmapStore` gains `readProjectByRoot()` and a `root` field on
  `ProjectRow`** (§ 2.2) — a surface addition to ANTS-3756, in the shape
  ANTS-3758 § 2.1 used for `listElements()`. Without it the dispatch marker
  cannot be read at all.
- **`RemoteControl` and `RoadmapDialog` each gain one `roadmapBullets()`
  member** (§ 2.1) — the owner wrapper that holds the process-owned
  `RoadmapStore`, performs § 2.2's stat-and-open, and maps
  `RoadmapSource::ReadError` to either an MCP refusal code or a dialog message.
  Listed as cross-doc impact because it is surface in two files this spec
  otherwise only reads from, and because the 26 call sites bind to *it*, not to
  `RoadmapSource`.
- **`docs/standards/mcp-error-codes.md` needs no new code.** `too_large` is
  already in the taxonomy and INV-3 reuses it; `ReadError` is an internal C++
  enum that never reaches an envelope by that name. Recorded because a reader
  of § 2.1 will reasonably ask, and an unstated answer reads as an omission.
- **`CMakeLists.txt`'s two SQL-isolation comments are amended in the same
  change as the link edges** (§ 4) — the one at the `Qt6::Sql` find_package
  ("Linked ONLY by `ants_roadmapstore_lib`") and the one over
  `add_library(ants_roadmapstore_lib …)` ("the test bundles that link core do
  not drag the SQL module"). Both become false the moment `ants_core_lib` links
  the store, and a build comment that contradicts the build is worse than none.
- **ANTS-3765 § 2.10** reads "What consumes the marker is ANTS-3758, not this
  half". ANTS-3758 did not build the fallback; § 2.2 does. That sentence is
  amended to name this id.
- **`roadmap-data-model.md` should carry a source-format column** so § 2.2's
  emoji-only gate can be answered from the store rather than by re-detecting on
  the live file — which is what forces § 4's retained 3.0 MiB read on every
  migrated call. Filed as **ANTS-3815**; the file-based guard ships meanwhile.
- **`roadmap-data-model.md`'s *What checks this* table** gains INV-3 against the
  read budgets.
- **A batched `RoadmapStore::readItems(projectId)` is conditionally owed** —
  § 4 names it as the remedy if `Inv3Budgets` reds on latency. Filed as
  **ANTS-3816**, together with the size aggregate INV-3's item-count proxy
  stands in for. Not built speculatively: the N+1 walk is what `render()`
  already does, and a second reader nothing needs is surface that has to be
  kept correct forever.
- **§ 2.1.2's `dropped` exclusion is enforced by this spec and by nothing in
  the code.** `bulletText()` on a dropped item would emit a head line with no
  status marker, which `parseBullets()` then skips entirely — so the exclusion
  is a precondition of an exported function that does not assert it. Filed as
  **ANTS-3820**; this spec's § 2.1.2 is the only thing keeping it true today.
- **`CLAUDE.md`'s module map and `docs/subsystems.md`** gain `roadmapsource`.
- **ANTS-3808's ROADMAP bullet is NOT closed by this spec.** It has its own
  spec and ships on its own; this one consumes its exports.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, 89 KB) | 3 / 5 / 8 / 13 / 1 | **29 verified, 29 fixed, 0 dismissed** — an unusually clean verification rate, and the packet is why: both lanes were handed bounded code windows, so their code claims were checkable rather than recalled. Both lanes independently led on the same defect, which is the signal that makes it credible: **§ 2.2's "the resolver owns the lazy open, and stats the path before it constructs" is unimplementable** — all three § 2.1 functions take an already-constructed `RoadmapStore &`, and § 2.2's own last rule says `RoadmapSource` owns no lifetime, so nobody owned the absent-vs-unopenable distinction INV-1 rests on. Fixed by naming the **owner wrapper** as the only thing that may construct a store, which also resolved two more findings at once: § 1's "no site grows a dispatch branch or a store reference" was false against `bulletsFor(RoadmapStore &, …)` and its three-outcome return, and INV-3's `too_large` had no channel — all three functions exposed only `QString *error`, and `RoadmapDialog` emits no MCP envelope at all, so the enum `ReadError` now carries it and the wrapper maps it. Third CRITICAL: **INV-1's headline sentence contradicted § 2.2's fourth rule** — it refused every store-row project "whose roadmap fails the ants-v1 gate", which would have broken every migrated GFM and pass-headings project that § 2.2 deliberately serves markdown; the gate now turns on `sawSignal`, not on the returned dialect, and § 2.2 carries the four-row table. Also fixed: the § 2.1.1 rule made INV-2's own `firstLine`/`lastLine` difference a defect (the carve-out moved into the rule sentence, which the table cannot override); the `body` row said "trimmed of its two-space indent" beside `cont.trimmed()`, two incompatible instructions, and an implementer writing `mid(2)` would have failed INV-2; § 4 applied the UTF-16 doubling to the records but not to the retained markdown, pricing this spec's own headline cost at half (~6 MiB, not 3.0, and a ~14 MiB peak); § 2.3's legend parenthetical was inverted (`migratedProject()` returns an id, not a `ProjectRow`) **and** its flat "no stored legend shows none" rule would have deleted the legend from every unmigrated project, since `roadmapdialog.cpp` holds the four emoji and labels as compile-time constants — a user-visible regression arriving as a side effect of a read-path change. `Inv2Membership` was added because INV-2 scopes itself to the renderable filed subset and so could not reach § 2.1.2's three membership rules at all. **One fix was caught wrong by its own verification before it landed:** § 6's `Access` example put the profile second when it is the constructor's **third** parameter, after `historyCapBytes`. Lane spend ~140k and ~145k tokens cumulative across three turns each, against a 60k per-turn budget — over, and reported rather than asserted; the packet itself was ~38k. |
| 0-rewrite | 2026-08-04 | 0 (no reviewer dispatched — a document operation) | — | **Rewritten in place from the 934-line umbrella of the same name, as the READ SEAM only.** Not a review loop and not inherited review: the umbrella's three loops ran against a document that no longer exists, so the gate runs from loop 1 on these bytes. The umbrella carried seven contracts and stopped at `/cold-eyes`' cap with collateral outnumbering draft defects two loops running; the user split it four ways on 2026-08-03 — this id (read seam), ANTS-3808 (`item.body` + trailer suppression, accepted), ANTS-3809 (write half), ANTS-3810 (oracle + acyclicity). Sections kept: old §§ 2.1, 2.2, 2.5 and INV-2 / INV-3 / INV-9, renumbered per § 3. **The filed loop-3 tail** ([`docs/reviews/ANTS-3793-cold-eyes-loop3-tail.md`](../reviews/ANTS-3793-cold-eyes-loop3-tail.md)) **was RESOLVED here, not carried:** C1 (INV-2 unsatisfiable against the membership rules) → § 2.1.2 excludes `dropped` on representability grounds (`emojiFor()` returns empty and `stripInlineEmoji()` then skips the bullet), keeps `internal` and unfiled, and INV-2 declares membership as a second difference; C2 (`bulletsFromStore()` cannot reach `renderBullet()`) → ANTS-3808 § 2.4's `RoadmapRender::bulletText()`, same library, no new edge; H2 (no store surface produces document order or a batched read) → § 2.1.3 states the six-step walk and § 4 prices the N+1 and names its remedy; H4 (the migrated path still loads the whole roadmap) → § 4 states the read as additive, INV-3's break clause now scopes to *the resolver*, and the removal path is filed as ANTS-3815; H5 (continuation indentation) → folded into § 2.1.1's `body` row; M1/M2 → § 2.2's third and fifth rules; M3 → `bulletsFor()`'s three outcomes are in the header comment; M4/M5 → INV-3's pre-read item-count proxy and a fixture sized from the ceiling; M7 → the `idToken`/`boldId` row covers the id-less item; M8 → § 2.1.3 step 6. **One filed LOW was verified WRONG and is dismissed, not fixed:** the claim that stripping `"- "` and the emoji leaves a leading space on `body` — `stripInlineEmoji()` ends with `while (!head.isEmpty() && head.front().isSpace()) head.remove(0, 1);`, so the parser strips it too and the umbrella's illustration was correct. Findings belonging to the other parts (C3, H1, H3, M6 and two § 2.3 LOWs) travelled with them. Three counts were re-measured and **all three had moved** since the umbrella recorded them: 33,879 → 34,240 lines, 3,075,143 → 3,099,130 bytes, 1,832 → 1,838 bullets. The filename is unchanged deliberately: `spec_query` resolves `<id>-*.md`, and three historical documents cite this path. |
