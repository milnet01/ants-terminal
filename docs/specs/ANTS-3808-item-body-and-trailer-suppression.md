# ANTS-3808 — `item.body`: what the migration stores and what the render re-derives

**Status:** spec draft (2026-08-03).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3808, found while verifying ANTS-3806 (2026-08-03);
split out of ANTS-3793 at that spec's cold-eyes cap the same day.
**Blocked by:** ANTS-3758 (the render this changes) — shipped.
**Blocker for:** ANTS-3794 (publish), which would otherwise write § 1's
duplication into every migrated project's `ROADMAP.md`; and ANTS-3809, whose
`body_shadowed` refusal is built on § 2.2's match provenance.
**Pairs with:** ANTS-3757 — the migration read; this spec's § 2.1 amends its
§ 2.1.1. And ANTS-3793 — the read seam, whose `body` field is defined in terms
of § 2.4's export.

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 What the migration stores](#21-what-the-migration-stores) ·
[2.2 Asking the grammar](#22-asking-the-grammar-trailervaluesin) ·
[2.3 Suppression on value equality](#23-suppression-on-value-equality) ·
[2.4 The render's one export](#24-the-renders-one-export)) ·
[3. Invariants](#3-invariants) · [4. RAM / build cost](#4-ram--build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

**Two contracts share one column and mean opposite things by it.**

`RoadmapParse::parseBullets()` seeds `QString body = head` — the bullet line
minus its `"- "` and minus its status emoji — then appends every continuation
line trimmed of indentation. So `body`'s first line is the id-and-headline text
and the rest is every continuation, the `Layman:` / `Kind:` / `Source:` /
`Lanes:` / `Evidence:` trailer included. `RoadmapMigrate`'s `makeItem()` copies
that verbatim into `item.body`, which is exactly what ANTS-3757 § 2.1.1 tells it
to do.

`renderBullet()` (`src/roadmaprender.cpp`) then emits a synthesised head line,
**then** `body`, **then** the trailer again from the columns. So a rendered
bullet carries its headline twice — once as the head, once as `body`'s first
line — and each trailer key twice, once inside `body` and once from its column.

**Observed, not inferred**, rendering the ANTS-3806 fixture out of a real store.

**Why the obvious repair is wrong.** "Store `body` minus its head line minus its
trailer lines" assumes the trailer *is* a set of lines. `src/roadmapparse.cpp`'s
own regex comments carry the corpus measurements that say otherwise: 157
`Source:` values sit inline in a prose sentence rather than at a line start
(which is why `rxSource` and `rxLanes` are deliberately un-anchored, ANTS-2058 /
ANTS-3764), 10 lines carry two keys (`rxTrailerKey` exists for exactly those),
and 22 are backticked mentions of a key that ANTS-3722's guard excludes. For a
large share of the corpus there is no field *line* to drop — the metadata is a
span inside a sentence, and excising it either deletes prose or leaves half a
sentence. Computing the residual is a new parsing contract, and re-deriving it
outside `RoadmapParse` is the second bullet parser ANTS-3757 § 2.3 forbids.

**The decision: the migration drops one line, and the render asks before it
re-derives.** Neither half needs a schema change, a new provenance value, or a
second parser.

## 2. Surface

### 2.1 What the migration stores

**`item.body` is the bullet body with the render's own head-line prefix removed
— NOT with its first line removed.**

The distinction is the whole of this section, because "drop the first line" is
the obvious rule and it is **lossy on 14.6% of this project's corpus**.

`body` is seeded from `head` and every continuation is appended after a `'\n'`,
so the head line is exactly the text before the first `'\n'`. But for a native
`[id] **headline**` bullet the reader takes `headline` from the **bold token
only** (`src/roadmapparse.cpp`, the `boldMatch.captured(1).trimmed()` branch) —
so any text *after* the closing `**`, on that same line, exists **nowhere but
`body`'s first line**. It is not in `headline`, and the columns capture only the
trailer keys within it. Measured against this project's `ROADMAP.md` on
2026-08-04: **241 of 1645** bracket-id bullets carry text after the closing
`**`, and for a single-line bullet dropping the head line discards the item's
entire substance. `[ANTS-1649]` and `[ANTS-1650]` are two such bullets; the
second carries roughly a thousand characters of methodology that a first-line
drop would delete outright.

**So the rule is a prefix strip, not a line drop.** Remove from `body`'s first
line exactly what `renderBullet()` reconstructs into its head — the leading
`[<id>] ` token and the `**<headline>**` token, through the closing `**` — and
keep everything else on that line, joined to the continuations as before. The
prefix is defined by the render's head shape (§ 2.4), which is why the two must
be read together.

Three shapes, all determinate:

| Bullet shape | First-line residual |
|---|---|
| `[id] **headline**` and nothing else | empty |
| `[id] **headline** <prose>` | `<prose>` — the case the naive rule loses |
| GFM, where `headline` **is** the whole first line | empty, and the line drop and the prefix strip agree |

**An empty residual is a normal outcome, not an error** — it is what the first
row produces, and § 2.3's suppression then fires for no key, so every column is
emitted exactly once. INV-1 covers that row explicitly.

ANTS-3757 § 2.1.1's row reads "the reader's `headline` / `body`" and is amended
on ship to record the strip (§ 7) — the `headline` half of the row is unchanged
and must not be disturbed.

### 2.2 Asking the grammar: `trailerValuesIn()`

The six matchers (`rxKind`, `rxLanes`, `rxLayman`, `rxEvidence`, `rxSource`, and
`rxTrailerKey`, which bounds a value at a following key on the ten two-key
lines) are today `static const` **inside `parseBullets()`** and reachable from
nowhere. They move to file scope and are **shared from there, not copied** —
`parseBullets()` keeps using the same objects and still needs their captures, so
a boolean predicate could not serve it — behind one exported accessor:

```cpp
// src/roadmapparse.h, inside `namespace RoadmapParse` — the matchers, asked
// rather than duplicated. The namespace is not optional: INV-2 is phrased as
// "RoadmapParse remains the only bullet grammar", and that is this namespace.
//
// Per key: the value AS parseBullets() ASSIGNS IT (§ 2.2.1), plus where the
// match sat and whether it began a line. The provenance is not decoration:
// ANTS-3809's `body_shadowed` refusal has to name the shadowing sentence, and a
// bare value cannot answer "was this an un-anchored mid-prose match?".
struct TrailerMatch {
    QString value;         // empty when the key is absent
    // Index of the CAPTURE (`capturedStart(1)`) into `body`, in QString
    // positions — UTF-16 code units, NOT bytes. -1 when absent. An
    // implementer converting this to a UTF-8 offset breaks ANTS-3809's
    // sentence extraction on the first non-ASCII bullet.
    int     offset   = -1;
    // True when THIS MATCH begins a line — `offset == 0`, or the character
    // at `offset - 1` is '\n'. A property of the match POSITION, computed
    // the same way for all five keys. Deliberately NOT "the pattern carries
    // ^": as a pattern property it would be a per-key constant (always true
    // for kind/layman/evidence, always false for lanes/source), carry no
    // information, and leave ANTS-3809 unable to tell a canonical trailer
    // line from a mid-prose match — which is the one question it exists to
    // answer.
    bool    anchored = false;
};
struct TrailerValues {
    TrailerMatch layman, kind, source;
    // For these two, `value` is the post-normalisation text BEFORE the split
    // — for `evidence` that means after the trailing-period chop, so it is
    // `evidenceList.join(", ")`'s pre-split source and not the raw capture.
    // § 2.2.1 pins both.
    TrailerMatch lanes, evidence;
    QStringList  lanesList, evidenceList;   // the split forms parseBullets() assigns
};
TrailerValues trailerValuesIn(const QString &body);
```

`offset` and `anchored` are what an earlier single-value draft could not
express, and their absence made ANTS-3809's refusal unimplementable through this
struct. Both are cheap — `QRegularExpressionMatch` already carries the capture
position, and `anchored` is one character comparison against `body` at that
position.

This is **not** a second parser. It is the one reader answering a question about
its own grammar, which is the distinction ANTS-3757 § 2.3 draws.

#### 2.2.1 The normalisation contract

**The accessor returns each value exactly as `parseBullets()` assigns it to the
record — post-match, not raw captures — and this contract is the whole fix
rather than a detail of it.** § 2.3's "equals the column by construction" is
true only if the two run the same normalisation; against raw captures nothing
would ever compare equal, no suppression would ever fire, and this spec would be
*documented* as fixed while the defect remained live.

The steps are already in `roadmapparse.cpp` and are named here so an implementer
cannot reasonably re-derive them differently:

| Field | Normalisation the accessor must reproduce |
|---|---|
| `kind.value`, `layman.value` | `captured(1).trimmed()` |
| `lanesList` | split `captured(1)` on `,` (`SkipEmptyParts`), each part trimmed, empties dropped |
| `lanes.value` | `captured(1)` — the raw capture, no trim and no period chop, because `parseBullets()` applies none before splitting |
| `evidenceList` | trim; drop **one** trailing `.` unless the value ends `..`; then split/trim as `lanesList` |
| `evidence.value` | the trimmed, period-chopped text **before** the split — i.e. the input `evidenceList` is split from |
| `source.value` | truncate at the first following `rxTrailerKey` match, trim, drop one trailing `.`, trim again |
| every `offset` | `capturedStart(1)` of the match that produced the value; `-1` when the key is absent |
| every `anchored` | `offset == 0 \|\| body.at(offset - 1) == '\n'`; `false` when the key is absent |

**The last three rows exist because `parseBullets()` assigns no counterpart to
them**, so INV-4's "equals what `parseBullets()` assigns" cannot grade them.
They are pinned here instead, and INV-4 asserts them against this table rather
than against the reader.

INV-4 asserts the rest of the equality directly, so a divergence fails a test
rather than silently disabling the feature.

### 2.3 Suppression on value equality

**`renderBullet()` emits a trailer key from its column only when the body would
not already re-parse to that same value**, asked through § 2.2's accessor.

**Value equality and not mere presence — and the reason is value loss, not an
INV-12 breach.** Stated precisely, because the tempting shorter argument is
wrong: ANTS-3758's INV-12 asserts against the *rendered text*, and a body that
made a presence check fire necessarily contains the literal `Kind:` itself, so
that text is still in the output and INV-12 still passes. Presence-based
suppression does not break INV-12.

What it breaks is **correctness of the value**. A body line reading
`Kind: refactor` — stale prose, or a key the author typed into the body — has
the key present and a value the store disagrees with. Suppress on presence and
the render drops the canonical column line, leaving the *stale* value as the
only one in the file; the next migration reads that back and the store has
silently adopted it. Comparing values instead means a mismatch always emits from
the column, which is canonical, and INV-12 continues to hold as written: the
required piece is in the rendered text either way, exactly once.

**Why per-key and not a stored flag.** A `verbatim`-versus-`residual` flag on
`provenance.body` was the original sketch on the ANTS-3808 bullet, and it is
wrong for a reason worth recording: `provenance` is per-field **write origin** —
`migrated` / `asserted` / `defaulted` (`src/roadmapstore.h`, the `setItemField`
overload comment) — and body *shape* is a different axis. A migrated body edited
by a consumer flips its provenance to `asserted` while staying verbatim, and a
post-cutover body written residual would carry `asserted` too. One key cannot
answer both questions, and the per-key predicate answers the question actually
being asked without storing anything.

#### 2.3.1 The corner this design has

**Named rather than papered over, and narrower than it looks — but not as narrow
as "anchored means safe".**

`rxSource` and `rxLanes` are deliberately **un-anchored** (ANTS-2058 for
`Lanes:`; ANTS-3764 extended it to `Source:` on its own measurement of 157
inline occurrences), so either can be extracted from mid-sentence prose.

`rxKind`, `rxLayman` and `rxEvidence` are `^`-anchored — **but with
`QRegularExpression::MultilineOption`**, so `^` anchors at the start of *any line
within the bullet body*, not at the start of the body. A continuation line that
merely *begins* `Kind:` in prose still matches them. The residual exposure is
therefore "a body whose continuation line starts with a trailer key" rather than
"none", and ANTS-3722's backtick guard is what usually absorbs it.

**For a migrated item the mismatch cannot produce a WRONG value — but the
equality is not "by construction", and saying so would be false.** The store's
column was extracted by `parseBullets()` from the **full** body including the
head line; § 2.1 stores the **residual** after the prefix strip. The two inputs
differ whenever the matched key sat in the stripped prefix, and that is not
hypothetical: `[ANTS-1649]`'s `Kind:` and `Lanes:` both sit on its head line
after the closing `**`, so they survive the strip, while a bullet whose only
`Source:` sat *inside* the bold headline would not.

So the honest statement is a **direction**, not an identity:

- Key present in the residual with the same value → suppression fires, emitted
  once from the body. Correct.
- Key absent from the residual, or present with a different value →
  `trailerValuesIn()` disagrees with the column, suppression does not fire, and
  the render emits the **column** value. Also correct, because the column is
  canonical.

Both branches land on exactly one occurrence of the canonical value, which is
what INV-1 and INV-3 actually assert. The failure mode the equality argument was
reaching for — a *stale* value winning over the column — needs a residual that
carries the key with a value the column disagrees with, and that requires a
consumer to have written the column without rewriting the body: a `flip` or
`annotate` under ANTS-3809. That is why that spec owns the write-side refusal
and this one owns the accessor it is built on.

**"Post-cutover"**, used here and in INV-3, means an item whose columns have
been written by a consumer through the store after migration — as opposed to a
**migrated** item, whose columns and body were both produced by one
`parseBullets()` pass over the source markdown.

**Fixing the grammar is out of scope** (§ 5): anchoring `rxSource` / `rxLanes`
would discard the 157 inline values they were un-anchored for.

### 2.4 The render's one export

`renderBullet(const RoadmapStore::ItemWrite &)` is a **free function in an
anonymous namespace** in `src/roadmaprender.cpp`. There is no
`RoadmapRender::renderBullet()`.

That is fine for § 2.3, which is a TU-local edit adding no header surface. It is
**not** fine for **ANTS-3793's `bulletsFromStore()`**, which defines its
`BulletRecord::body` as the rendered bullet's text and cannot reach a file-local
function. That one caller is the whole justification, and it is enough:

```cpp
// src/roadmaprender.h — the bullet's markdown, byte-identical to what the
// file writer emits for this item. One export, for ANTS-3793's reader seam;
// the alternative is a second renderer that must be kept in step by hand.
namespace RoadmapRender {
    QString bulletText(const RoadmapStore::ItemWrite &it);
}
```

`RoadmapRender` is already a namespace in `src/roadmaprender.h`, so this is an
addition to it rather than a new scope.

**The tests do not need the export**, and do not use it: INV-1 and INV-3 drive
the public entry point `RoadmapRender::render()` over a store they populated,
which is also the stronger assertion — it exercises the bytes that actually
reach the file.

`renderBullet()` becomes its body; the anonymous-namespace helper goes away
rather than being wrapped, so there is one definition and not two.

**The export itself costs no link edge** — `roadmaprender.cpp` is already in
`ants_roadmapstore_lib` (`CMakeLists.txt:449`, tagged `# ANTS-3758`), the same
library ANTS-3793's `roadmapsource.cpp` lands in. **§ 2.3's *call* is a
different matter and is not settled**: it puts a `trailerValuesIn()` call inside
that library, and the accessor lives in `ants_core_lib`. § 4 carries it as an
open decision.

## 3. Invariants

- **INV-1** — **A migrated project's rendered bullet contains its headline
  exactly once and each trailer key it carries exactly once**, `Kind:` included.
  *Rationale, not part of the assertion:* "exactly once" is a two-sided bound.
  The upper side is the duplication this spec exists to remove; the lower side is
  that the value which survives must be the **canonical** one, which is why § 2.3
  compares values rather than presence (§ 2.3 states why presence is unsafe, and
  it is not an INV-12 breach). *Breaks when:* the migration stores the render's
  own head-line prefix in `item.body` (today's defect), or the render emits a
  column-sourced trailer line whose value the body already carries. *Test:*
  `Inv1NoDuplication`, over this directory's own bullet fixture, asserting
  exactly one occurrence of the headline and exactly one of each trailer key in
  the rendered text — **including a bullet with an empty residual**, where every
  column is emitted unsuppressed and "exactly once" is the whole assertion.
- **INV-2** — **`RoadmapParse` remains the only bullet grammar in `src/`, outside
  the enumerated exemptions below.** *Breaks when:* the render or a consumer
  grows its own trailer-key matcher instead of calling `trailerValuesIn()`.
  *Test:* `Inv2SingleGrammar`, a case-sensitive scrape of `src/` with comments
  stripped, for the **regex construction** `QRegularExpression` applied to a
  trailer-key literal — matching the pattern text
  `Kind:`/`Lanes:`/`Layman:`/`Evidence:`/`Source:` *inside a regex*, not the
  plain `"Kind: "` output literals the render legitimately emits, which a naive
  scrape hits. The shape ANTS-3758's INV-11 had to be corrected into after
  matching English prose.

  **Five sites exist today across three files, and each is dispositioned rather
  than the invariant being widened** (verified 2026-08-03; the count is of
  sites, not of rows — the `rxBoldLayman` row below covers two):

  | Site | Disposition |
  |---|---|
  | `src/roadmapparse.cpp` | the grammar itself — **exempt** by definition |
  | `src/remotecontrol.cpp:6576` and `:6749` (`rxBoldLayman`, ANTS-1933) | **exempt, and it cannot be otherwise.** Both deliberately capture the Layman sentence *including* its trailing period, because `rec.layman` is period-stripped by ANTS-1154 INV-4 and a period-less CHANGELOG body was the bug ANTS-1933 fixed. `trailerValuesIn()` returns the stripped value, so routing these through it re-introduces that defect. Two sites: the single-entry and batch `add_from_roadmap` paths carry the same block |
  | `src/roadmapdialog.cpp:640` (`rxKind`) | **moves to `trailerValuesIn(bodyFull).kind.value`** — a deliverable of this spec. It re-implements both the trailer regex *and* `parseBullets()`'s continuation-line assembly to build a kind-filter map, which is the second grammar this invariant exists to forbid. Its `s_lastInput` / `s_lastKindMap` memo is unaffected |
  | `src/remotecontrol.cpp:22382` (`rxCommitSha()`) | **exempt.** Its pattern embeds `\bSource:\s*` as one alternative in a commit-SHA locator — the trailer key is a lead-in it skips past, not a value it extracts, so routing it through the accessor is meaningless. Listed because the scrape **will** match it |

  **The table above is today's inventory. The invariant's allowlist is a
  different, smaller list, and conflating the two would let the test pass with
  the defect still in the tree.** `roadmapdialog.cpp:640` is the site this spec
  *removes*; if `Inv2SingleGrammar` treats the inventory as its allowlist, the
  one deliverable INV-2 exists to force is exactly the one it stops checking.
  So the test encodes:

  | After this spec ships | Sites |
  |---|---|
  | **Permitted** (the scrape must find these and no others) | `src/roadmapparse.cpp`; `src/remotecontrol.cpp:6576`, `:6749`; `src/remotecontrol.cpp:22382` — **four sites, two files** |
  | **Must be ABSENT** | `src/roadmapdialog.cpp` — any trailer-key regex construction at all |

  Any site outside the permitted row is a failure, and so is any surviving
  match in the must-be-absent row.

  **INV-2 catches the regex only, and that is a deliberate narrowing.** The
  dialog also hand-rolls continuation-line assembly (§ 2.3's table row says so),
  and its `bodyFull` is *not* `parseBullets()`'s `body`: it is built from
  `row.mid(2)`, which strips `"- "` but leaves the status emoji, and its walk
  breaks on the first blank line rather than applying ANTS-1426's loose-list
  tolerance. Routing the *regex* through `trailerValuesIn()` therefore removes
  the second grammar without making the two assemblers agree. Closing that gap
  is ANTS-3809's, which owns the consumer-write paths; INV-2 is scoped to regex
  construction so it asserts something true rather than something aspirational.
- **INV-3** — **The render and the reader agree about every trailer key.**
  Re-parsing a rendered bullet yields the same `kind` / `source` / `lanes` /
  `layman` / `evidence` the store holds, **for every item reachable without a
  shadowing consumer write** — which is every migrated item and every
  post-cutover item whose body was written under § 2.1's strip. § 2.3.1 shows
  why: whichever branch the value-equality test takes, exactly one canonical
  value reaches the file. *Breaks when:* the render suppresses a key whose
  stored value differs from what the body re-parses to, or emits one the reader
  then reads twice. *Test:* `Inv3RenderReaderAgree`, two fixtures — a migrated
  bullet whose residual carries the key (suppression fires) and a post-cutover
  bullet with a residual body that does not (all keys emitted from columns).

  **The scope clause is load-bearing, and it excludes exactly one state.** A
  column written *without* rewriting the body leaves the body's own key
  shadowing the canonical value on a re-parse (§ 2.3.1), and only a consumer
  write can create that. ANTS-3809 owns making it refuse, and carries the
  fixture for it. Asserting the unscoped form here would ship this invariant red
  against a state this spec cannot reach. Both of this invariant's own fixtures
  are inside the scope — a post-cutover item is only excluded when it is
  *shadowed*, not for being post-cutover.
- **INV-4** — **`trailerValuesIn(body)` equals what `parseBullets()` assigns from
  the same body**, over all five keys and § 2.2.1's normalisation table. Without
  this equality the suppression compares incommensurable values, never fires, and
  the defect stays live behind a passing spec. *Breaks when:* the accessor
  returns raw captures, skips the `rxTrailerKey` truncation or a trailing-period
  chop, or splits `lanes` / `evidence` differently. *Test:* `Inv4AccessorAgrees`,
  over a fixture table covering each normalisation step including the two-key
  line and the backticked-key guard, plus § 2.2.1's `offset` and `anchored` rows
  — which have no `parseBullets()` counterpart and are asserted against that
  table directly.
- **INV-5** — **No bullet text is lost across migrate-then-render.** For every
  bullet, the text of the head line **after** the closing `**` survives into the
  rendered output. This is the assertion § 2.1's prefix strip exists to satisfy,
  and the one a first-line drop fails: measured 2026-08-04, **241 of 1645**
  bracket-id bullets in this project's `ROADMAP.md` carry such text, and for a
  single-line bullet it is the item's entire substance. *Breaks when:* § 2.1 is
  implemented as "remove everything before the first `'\n'`" — the reading this
  invariant exists to catch, because it is the obvious one and it is silently
  destructive. *Test:* `Inv5NoBodyLoss`, over a fixture triple — a bullet with
  prose after the closing `**` and no continuation, one with both, and one with
  neither — asserting the prose is present in the rendered text and that the
  third renders clean rather than emitting a stray blank body.

## 4. RAM / build cost

**RAM.** Unchanged and not merely negligible: § 2.1 makes `item.body`
**smaller** by the stripped `[id] **headline**` prefix on every bullet, and
§ 2.2's accessor allocates one `TrailerValues` per call — five short strings
plus two `QStringList`s, on the stack, freed at the end of the render's
per-bullet loop. Nothing is retained.

**Per-bullet cost.** § 2.3 turns the render's per-bullet work from zero regex
matches into **at most six** (`rxKind`, `rxLanes`, `rxLayman`, `rxEvidence`,
`rxSource`, plus `rxTrailerKey` when `Source:` matched), against
`static const` patterns already compiled once per process. Budget: a full render
of this project's roadmap is **1645 bullets × ≤6 matches** over bodies averaging
well under 1 KiB — bounded work on a path that already does one full file write
per section, so it is not the dominant term and no new invariant guards it.

The one place this ratio matters is **`src/roadmapdialog.cpp`'s kind filter**,
which INV-2 moves onto the accessor: that pre-walk runs one regex per bullet
today and would run up to six, on a path memoised by `s_lastInput` /
`s_lastKindMap` *precisely because* it was once the dominant render cost
(ANTS-2119). The memo is keyed on the whole source text and is unaffected by
this change, so the 6× lands only on a cache miss — a filter toggle or an edit,
not a keystroke. **If that proves wrong in profiling, the accessor gains a
single-key overload rather than the dialog regrowing its own regex.**

**Build — one open decision, and it is not editorial.** § 2.4's export costs
nothing: `roadmaprender.cpp` is already in `ants_roadmapstore_lib`
(`CMakeLists.txt:449`, `# ANTS-3758`), the library ANTS-3793's consumer lands
in. The accessor likewise costs nothing where it is *defined* —
`roadmapparse.cpp` is already in `ants_core_lib`.

**What is not free is § 2.3's call.** It puts a `trailerValuesIn()` call inside
`ants_roadmapstore_lib`, whose link surface is today
`PUBLIC Qt6::Core Qt6::Sql` and **nothing else** (`CMakeLists.txt:456-458`) —
no `ants_core_lib`. That minimal surface is a stated design property, not an
accident: `src/roadmaprender.h:11-12` says the library is "Qt6::Core + Qt6::Sql
only … because ANTS-3794 will call it from a headless publish path." A static
archive will still *build*, since the symbol resolves at final link and every
current consumer (`test_core`, the executable) links both libraries — which is
exactly why this would pass CI and surface later, at ANTS-3794.

Three ways out, and **this spec does not pick one**:

| Option | Cost |
|---|---|
| Declare `ants_roadmapstore_lib → ants_core_lib` | honest and one line, but every store consumer — including the headless publish path — now drags core |
| Hoist `trailerValuesIn()` into a leaf TU both libraries link | keeps both surfaces minimal; costs a new file and a decision about where the grammar's home really is |
| Move suppression out of `renderBullet()` — caller computes `TrailerValues` and passes it in | no new edge at all; pushes the § 2.3 contract onto every render caller, and ANTS-3793 is one |

**Owner: whoever implements this, before writing § 2.3.** The choice changes
`CMakeLists.txt` and possibly § 2.2's home, so it is a precondition of the work
and not a cleanup after it.

Per this project's cap, builds run under `cmake --build build` with the
`JOB_POOLS` limit and tests at `ctest -j4`.

## 5. Out of scope

- **Anchoring `rxSource` / `rxLanes`** to close § 2.3.1's corner. It would
  discard the 157 inline `Source:` values ANTS-3764 measured and un-anchored for
  (extending ANTS-2058's finding for `Lanes:`) — a format decision, filed as
  **ANTS-3811**, which may well close as "no grammar change needed" once
  ANTS-3809's refusal exists.
- **The write-side refusal** that makes the corner fail loudly rather than
  silently — ANTS-3809, built on § 2.2's `offset` / `anchored`.
- **The reader seam** that consumes § 2.4's export — ANTS-3793.
- **Deleting the markdown splice paths** — they serve every unmigrated project.
- **Backfilling stores migrated before this ships** — there are none, and that
  is a fact rather than an omission: publishing is ANTS-3794, and its own header
  records that this spec blocks it. No store has been written that a rendered
  file was produced from, so § 2.1's change applies to every migration that will
  ever run. Should that stop being true before this ships, a re-migration — not
  a backfill — is the remedy, because the head-line text a first-line drop
  discarded is not recoverable from the store.

## 6. Tests

`tests/features/roadmap_item_body/`, label `features`, compiled into the
**`test_core` bundle** per `tests/features/README.md` (no `add_executable`) —
the only bundle already linking both `ants_core_lib` and
`ants_roadmapstore_lib`.

| Case | Invariants |
|---|---|
| `Inv1NoDuplication` | INV-1 |
| `Inv2SingleGrammar` | INV-2 |
| `Inv3RenderReaderAgree` | INV-3 |
| `Inv4AccessorAgrees` | INV-4 |
| `Inv5NoBodyLoss` | INV-5 |

Per this project's convention, **every case is verified RED against its *Breaks
when* mutation before the implementation is restored** (`testing.md` owns the
mutation-harness rules, including mtime busting, and they are not restated here).

**The fixtures are this directory's own**, not reached out of
`tests/features/roadmap_migrate_archive_root/`, whose `spec.md` scopes it to
preamble round-tripping and lists bullet-body fidelity as out of scope — a case
here depending on its internals couples two contracts that were deliberately
split.

One rule worth restating because it is a silent data-loss trap rather than a
convention: **never default-construct `RoadmapStore`.** It resolves
`defaultPath()` — the developer's real store under `XDG_DATA_HOME` — so every
case would write into it. Always
`std::make_unique<RoadmapStore>(dir.filePath("store.db"))`.

## 7. Cross-doc impact

- **ANTS-3757 § 2.1.1's `body` row** reads "the reader's `headline` / `body`".
  § 2.1 changes what is stored, so the row is amended on ship to say the
  render's head-line prefix is stripped — otherwise the migration's own spec
  describes the defect. **The `headline` half of that row is unchanged**; the
  amendment must not disturb it.
- **ANTS-3758's INV-12 is untouched and that is the point.** § 2.3's per-key
  suppression compares *values*, so the required `Kind:` is always in the
  rendered text — INV-12's own test asserts against rendered text, not against
  the render's choice of source. Recorded because a reader of § 2.3 will
  reasonably ask, and an unstated reconciliation reads as a silent repeal.
- **`src/roadmaprender.h` gains `RoadmapRender::bulletText()`** (§ 2.4), the
  render's first exported per-bullet surface.
- **`src/roadmapdialog.cpp`'s kind-filter pre-walk** moves onto
  `trailerValuesIn()` as part of INV-2 — a behaviour-preserving edit to a
  *rendering* path no other section would lead a reader to expect.
- **ANTS-3809 depends on § 2.2's `TrailerMatch`**; its `body_shadowed` refusal is
  unimplementable without `offset` / `anchored`.
- **ANTS-3793's `BulletRecord::body`** is defined in terms of § 2.4's export.
- **`CLAUDE.md`'s module map and `docs/subsystems.md`** need no change: no new TU.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet) | 3 / 3 / 7 / 11 / 0 | **24 verified, 23 fixed, 1 surfaced, 2 dismissed.** Both lanes independently led on the same two — `anchored` carrying two incompatible definitions, and § 2.3.1's "same body" equality — and the second was the draft's own central correctness argument, false because § 2.1 stores a residual while the column was extracted from the full body. **The sharpest finding came out of verifying a lane's weaker version of it:** § 2.1's "drop the first line" is *lossy*, not merely imprecise. A native bullet takes `headline` from the bold token only, so text after the closing `**` lives nowhere but `body`'s first line — measured **241 of 1645** bracket-id bullets in this project's own `ROADMAP.md`, and for a single-line bullet it is the item's entire substance (`[ANTS-1649]`, `[ANTS-1650]`). The rule became a prefix strip and gained INV-5 to catch the naive reading. **Surfaced, not fixed:** § 2.3 puts a `trailerValuesIn()` call inside `ants_roadmapstore_lib`, which links `Qt6::Core` + `Qt6::Sql` and nothing else by deliberate design (`src/roadmaprender.h:11-12`, for ANTS-3794's headless path) — § 4 claimed "no edge"; it now carries three options and a named owner. Sweep also found ANTS-3793's umbrella holding a stale duplicate of this contract, including the false equality claim: banners added there rather than reconciling two copies. Lane spend 107k / 105k against a 60k budget. |
