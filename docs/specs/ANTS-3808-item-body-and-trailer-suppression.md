# ANTS-3808 — `item.body`: what the migration stores and what the render re-derives

**Status:** spec draft (2026-08-04).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3808, found while verifying ANTS-3806 (2026-08-03);
split out of ANTS-3793 at that spec's cold-eyes cap the same day.
**Blocked by:** ANTS-3758 (the render this changes) — shipped.
**Blocker for:** ANTS-3794 (publish), which would otherwise write § 1's
duplication into every migrated project's `ROADMAP.md`; and ANTS-3809, whose
`body_shadowed` refusal is built on § 2.2's match positions.
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
and 22 are backticked mentions of a key that ANTS-3722's guard excludes. So for
**roughly one `Source:` value in eight** (157 of 1282 measured by ANTS-3764)
there is no field *line* to drop — the metadata is a span inside a sentence, and
excising it either deletes prose or leaves half a sentence. Computing the residual is a new parsing contract, and re-deriving it
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
2026-08-04: **241 of 1646** bracket-id bullets carry text after the closing
`**` — **roughly one in seven, and that ratio is the durable claim**, since the
denominator moves with every roadmap append. For a single-line bullet dropping
the head line discards the item's
entire substance. `[ANTS-1649]` and `[ANTS-1650]` are two such bullets; the
second carries roughly a thousand characters of methodology that a first-line
drop would delete outright.

**So the rule is a prefix strip, not a line drop**, and it is stated against
**text the render will reconstruct**, never against token syntax — a syntactic
rule (`[id]` then `**…**`) silently strips nothing from a GFM bullet, which
writes neither delimiter:

1. Take `body`'s first line. Remove the leading `[<id>]` token **if present**,
   then remove the headline text — matched as the exact string
   `ItemWrite.headline` will carry — together with any `**` delimiters
   immediately around it.
2. **Trim what is left, then — if nothing but whitespace remains — drop the
   line entirely.** Both halves are load-bearing. The separator space after
   `[<id>]` and the space before any surviving prose are still there after
   step 1, so an untrimmed residual is `" <prose>"` rather than `<prose>`, and
   an untrimmed *empty* residual is `" "` — which is **not** empty, so the
   drop never fires. Joining that back with `'\n'` yields a `body` beginning
   with whitespace, which passes `renderBullet()`'s `if (!it.body.isEmpty())`
   guard and emits a stray blank indented line on nearly every bullet.
3. Join whatever survives with the continuations, `'\n'`-separated, as before.

**The headline matched in step 1 is the untruncated one.**
`src/roadmapmigrate.cpp`'s `makeItem()` stores
`rec.headlineFull.isEmpty() ? rec.headline : rec.headlineFull`, and
`rec.headline` is display-truncated to 120 characters with an ellipsis
(ANTS-1811 / ANTS-2075). Strip using the truncated form and the tail of every
long headline survives into the residual, where the render then emits it a
second time — reintroducing this spec's own defect on exactly the bullets most
likely to have it. `ItemWrite.headline` is the value to match, and it is the
value § 2.4's export re-emits.

Four shapes, all determinate:

| Bullet shape | Stored `body` |
|---|---|
| `[id] **headline**`, no continuation | empty |
| `[id] **headline**` + continuations | the continuations only — **no leading newline** |
| `[id] **headline** <prose>` | `<prose>`, then any continuations — the case the naive rule loses |
| GFM, where the headline **is** the whole first line | the continuations only; matching on headline *text* is what makes this row work |

**An empty `body` is a normal outcome, not an error** — it is what row 1
produces, and § 2.3's suppression then fires for no key, so every column is
emitted exactly once. INV-1 and INV-5 both cover it.

ANTS-3757 § 2.1.1's `body` row is amended on ship; § 7 owns that obligation and
its caveat.

### 2.2 Asking the grammar: `trailerValuesIn()`

The six matchers (`rxKind`, `rxLanes`, `rxLayman`, `rxEvidence`, `rxSource`, and
`rxTrailerKey`, which bounds a value at a following key on the ten two-key
lines) are today `static const` **inside `parseBullets()`** and reachable from
nowhere. They move **out of `parseBullets()` and are shared, not copied** —
`parseBullets()` keeps using the same objects and still needs their captures, so
a boolean predicate could not serve it — behind one exported accessor. **§ 4
pins the shape they move to**; this section pins only that there is one copy:

```cpp
// src/roadmapparse.h, inside `namespace RoadmapParse` — the matchers, asked
// rather than duplicated. The namespace is not optional: INV-2 is phrased as
// "RoadmapParse remains the only bullet grammar", and that is this namespace.
//
// Per key: the value as parseBullets() derives it (§ 2.2.1 pins each), plus
// where the match sat and whether it began a line — its MATCH POSITION.
// Deliberately not called "provenance": the store already uses that word for
// per-field write origin (§ 2.3), and one term with two meanings in one spec
// is how an implementer conflates them. The match position is not decoration:
// ANTS-3809's `body_shadowed` refusal has to name the shadowing sentence, and a
// bare value cannot answer "was this an un-anchored mid-prose match?".
struct TrailerMatch {
    QString value;         // empty when the key is absent
    // Index of the CAPTURE (`capturedStart(1)`) into `body`, in QString
    // positions — UTF-16 code units, NOT bytes. -1 when absent. An
    // implementer converting this to a UTF-8 offset breaks ANTS-3809's
    // sentence extraction on the first non-ASCII bullet.
    int     offset   = -1;
    // True when the MATCH — `capturedStart(0)`, not `offset` — begins a line.
    //
    // The distinction is the whole field. Every one of the five patterns puts
    // literal text between the line start and group 1 (`^\s*Kind:\s*(…)`,
    // `Lanes:\s*(…)`, `Source:(?:\*\*)?\s*(…)`), so in every canonical bullet
    // shape the character before the CAPTURE is a space, a colon or a `*` —
    // not '\n'. Computed off `offset` the field is therefore false on
    // essentially every key of every bullet: strictly less than the per-key
    // constant this struct rejects below, while looking like it carries more.
    //
    // Also deliberately NOT "the pattern carries ^": as a pattern property it
    // is a per-key constant (true for kind/layman/evidence, false for
    // lanes/source), which tells ANTS-3809 nothing it did not already know
    // and cannot separate a canonical trailer line from a mid-prose match —
    // the one question the field exists to answer.
    bool    anchored = false;
};
struct TrailerValues {
    TrailerMatch layman, kind, source;
    // `value` for these two is the text the split is applied TO, which is not
    // the same step for each: `lanes` splits the raw capture, `evidence`
    // splits after a trim and one trailing-period chop. § 2.2.1 pins both
    // exactly; do not assume they are symmetric.
    TrailerMatch lanes, evidence;
    QStringList  lanesList, evidenceList;   // the split forms parseBullets() assigns
};
TrailerValues trailerValuesIn(const QString &body);
```

`offset` and `anchored` are what an earlier single-value draft could not
express, and their absence made ANTS-3809's refusal unimplementable through this
struct. Both are cheap — `QRegularExpressionMatch` already carries both
positions, and `anchored` is one character comparison against `body` at
`capturedStart(0)`.

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
| every `offset` | `capturedStart(1)` — the CAPTURE, which is what ANTS-3809 needs to quote the value; `-1` when the key is absent |
| every `anchored` | let `m = capturedStart(0)` — the MATCH, not `offset`; then `m == 0 \|\| body.at(m - 1) == '\n'`. `false` when the key is absent |

**Only two rows have no `parseBullets()` counterpart** — `offset` and
`anchored`. They are pinned here instead, and INV-4 asserts them against this
table rather than against the reader.

**`lanes.value` and `evidence.value` do have counterparts, and INV-4 must grade
them against the reader rather than against this table** — they are the reader's
own intermediates: `const QString lanesRaw = lanesMatch.captured(1)` and
`QString evRaw = evidenceMatch.captured(1).trimmed()` after its period chop.
Grading them against the table instead would test the spec against itself on
exactly the two keys whose normalisation is asymmetric.

INV-4 asserts the rest of the equality directly, so a divergence fails a test
rather than silently disabling the feature.

### 2.3 Suppression on value equality

**`renderBullet()` emits a trailer key from its column only when the body would
not already re-parse to that same value**, asked through § 2.2's accessor.

**"That same value" is a different comparison for the list-valued keys, and
naming which is not a detail.** Two of the five columns are `QStringList`s, and
the accessor hands back both a pre-split string and a split list for each;
comparing the pre-split string against a joined column diverges on separator
spacing alone, so suppression would never fire for those two and half the
defect would survive a passing spec:

**One accessor call per bullet, reused across all five comparisons** — `const
TrailerValues tv = RoadmapParse::trailerValuesIn(it.body);` once, then:

| Column | Suppress when |
|---|---|
| `kind`, `layman`, `source` | `tv.<key>.value == it.<key>` |
| `lanes`, `evidence` | `tv.lanesList == it.lanes` / `tv.evidenceList == it.evidence` — **list equality, element by element**, never a join |

Calling the accessor per key would be five passes and up to thirty matches per
bullet, against § 4's budget of six.

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
differ only when the matched key sat in the stripped prefix — that is, *inside*
the bold headline, since anything after the closing `**` survives the strip.
`[ANTS-1649]` carries its `Kind:` and `Lanes:` after the `**`, so it is an
example of the **agreeing** case, not the divergent one; a bullet whose only
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

**Fixing the grammar is out of scope** — § 5 states why and ANTS-3811 owns the
decision.

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

**The export itself costs no link edge; § 2.3's *call* is a different matter and
is not settled. § 4 owns both, and is the only place they are argued.**

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

  **Outside the grammar itself there are four sites, in two files, and each is
  dispositioned rather than the invariant being widened** (verified 2026-08-04).
  Counting rule, stated because the earlier phrasing did not survive its own
  arithmetic: a **site** is one `QRegularExpression` construction, so the
  `rxBoldLayman` row below is two of the four. `src/roadmapparse.cpp` appears in
  the table as the exempt grammar and is **not** counted at all — its six
  matchers are the thing the invariant protects, not something it permits:

  | Site | Disposition |
  |---|---|
  | `src/roadmapparse.cpp` | the grammar itself — **exempt** by definition |
  | `src/remotecontrol.cpp:6576` and `:6749` (`rxBoldLayman`, ANTS-1933) | **exempt, and it cannot be otherwise.** Both deliberately capture the Layman sentence *including* its trailing period, because `rec.layman` is period-stripped by ANTS-1154 INV-4 and a period-less CHANGELOG body was the bug ANTS-1933 fixed. `trailerValuesIn()` returns the stripped value, so routing these through it re-introduces that defect. Two sites: the single-entry and batch `add_from_roadmap` paths carry the same block |
  | `src/roadmapdialog.cpp:640` (`rxKind`) | **moves to `RoadmapParse::trailerValuesIn(bodyFull).kind.value`** — a deliverable of this spec. It re-implements both the trailer regex *and* `parseBullets()`'s continuation-line assembly to build a kind-filter map, which is the second grammar this invariant exists to forbid. **Not behaviour-preserving** — the local pattern omits `CaseInsensitiveOption`; § 7 states the widening, § 4 the cost |
  | `src/remotecontrol.cpp:22382` (`rxCommitSha()`) | **exempt.** Its pattern embeds `\bSource:\s*` as one alternative in a commit-SHA locator — the trailer key is a lead-in it skips past, not a value it extracts, so routing it through the accessor is meaningless. Listed because the scrape **will** match it |

  **The table above is today's inventory. The invariant's allowlist is a
  different, smaller list, and conflating the two would let the test pass with
  the defect still in the tree.** `roadmapdialog.cpp:640` is the site this spec
  *removes*; if `Inv2SingleGrammar` treats the inventory as its allowlist, the
  one deliverable INV-2 exists to force is exactly the one it stops checking.
  So the test encodes:

  | After this spec ships | What the scrape must find |
  |---|---|
  | **Permitted** | `src/roadmapparse.cpp` — **any number** of matches; it is the grammar. Plus, in `src/remotecontrol.cpp`, exactly the two `rxBoldLayman` constructions and the one inside `rxCommitSha()` — **three matches in that file** |
  | **Must be ABSENT** | `src/roadmapdialog.cpp` — **zero** matches; any trailer-key regex construction at all is a failure |
  | **Every other file under `src/`** | **zero** matches |

  **The allowlist is keyed on file and symbol, never on line number.** A test
  encoding a literal line number rots the first time an unrelated edit lands
  above it, and would then fail for a reason that has nothing to do with the
  invariant.
  `roadmapparse.cpp` is uncapped rather than counted for the same reason: it
  holds six matchers today and the count is not the thing being protected.

  **INV-2 catches the regex only, and that is a deliberate narrowing.** The
  dialog also hand-rolls continuation-line assembly (its inventory row above
  says so),
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
  line and the backticked-key guard, plus the four rows with no `parseBullets()`
  counterpart — `lanes.value`, `evidence.value`, `offset`, `anchored` — asserted
  against § 2.2.1 directly. **`anchored` needs both polarities or it asserts
  nothing**: a line-leading `Kind:` (true) and an inline mid-prose `Source:`
  (false). A fixture set that only ever expects `false` passes against the
  always-false implementation this field was specified twice to avoid.
- **INV-5** — **No bullet text is lost across migrate-then-render.** For every
  bullet, the text of the head line **after** the closing `**` survives into the
  rendered output. This is the assertion § 2.1's prefix strip exists to satisfy,
  and the one a first-line drop fails: measured 2026-08-04, **241 of 1646**
  bracket-id bullets in this project's `ROADMAP.md` carry such text, and for a
  single-line bullet it is the item's entire substance. *Breaks when:* § 2.1 is
  implemented as "remove everything before the first `'\n'`" — the reading this
  invariant exists to catch, because it is the obvious one and it is silently
  destructive. *Test:* `Inv5NoBodyLoss`, over **§ 2.1's four rows exactly** —
  headline only; headline + continuations; headline + prose; and the **GFM**
  row. Two of those carry the weight and neither is optional. The
  headline-plus-continuations row is what a naive join gets wrong: an empty
  first-line residual kept as an empty string makes `body` start with `'\n'`,
  which is non-empty, so the render's `if (!it.body.isEmpty())` guard passes and
  emits a stray indented line. The **GFM row is what proves the strip is
  textual rather than syntactic** — a GFM bullet writes neither `[id]` nor `**`,
  so a token-matching implementation strips nothing and the duplication returns.
  A fixture set omitting either row passes against the defect it was written for.

## 4. RAM / build cost

**RAM.** Unchanged and not merely negligible: § 2.1 makes `item.body`
**smaller** by the stripped `[id] **headline**` prefix on every bullet, and
§ 2.2's accessor allocates one `TrailerValues` per call — five short strings
plus two `QStringList`s, on the stack, freed at the end of the render's
per-bullet loop. Nothing is retained.

**Per-bullet cost.** § 2.3 turns the render's per-bullet work from zero regex
matches into **at most six** (`rxKind`, `rxLanes`, `rxLayman`, `rxEvidence`,
`rxSource`, plus `rxTrailerKey` when `Source:` matched), against
patterns compiled once per process. Budget: a full render of this project's
roadmap is **≥1646 bullets × ≤6 matches** — 1646 is the bracket-id population
§ 2.1 measured, and the total including id-less and GFM bullets is larger — over
bodies averaging well under 1 KiB. Bounded work on a path that already does one
full file write per section, so it is not the dominant term and no new invariant
guards it.

**Where the six matchers live is an implementation choice with a house style.**
§ 2.2 says they move out of `parseBullets()`; it does not say they become
namespace-scope globals. This project's own precedent for a shared compiled
pattern is the function-local static behind an accessor —
`rxCommitSha()` (`src/remotecontrol.cpp:22382`) returns a
`static const QRegularExpression &` — which keeps construction lazy and ordered.
Follow it rather than introducing six non-POD globals.

The one place this ratio matters is **`src/roadmapdialog.cpp`'s kind filter**,
which INV-2 moves onto the accessor: that pre-walk runs one regex per bullet
today and would run up to six, on a path memoised by `s_lastInput` /
`s_lastKindMap` *precisely because* it was once the dominant render cost
(ANTS-2119). The memo is keyed on the whole source text and is unaffected by
this change, so the 6× lands only on a cache miss — a filter toggle or an edit,
not a keystroke. **Threshold: if a cache-miss pre-walk over this project's
roadmap regresses by more than 20 ms, the accessor gains a single-key overload
— the dialog does not regrow its own regex.** A named number, because "if it
proves slow" is not a criterion anyone can act on.

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
  `RoadmapParse::trailerValuesIn()` as part of INV-2 — an edit to a *rendering*
  path no other section would lead a reader to expect. **It is not
  behaviour-preserving, and the change is user-visible**: the dialog's local
  `rxKind` carries `MultilineOption` only, while `roadmapparse.cpp`'s adds
  `CaseInsensitiveOption` (ANTS-3407), so after the swap a hand-edited
  `kind:` / `KIND:` bullet starts matching the kind filter it is silently
  skipped by today. That is a **widening and almost certainly the correct
  behaviour** — ANTS-3407 case-folded the anchored labels precisely so a
  hand-edited roadmap parses — but it is a behaviour change, so it is declared
  here rather than discovered in a filter fixture. § 4 carries what it costs.
- **ANTS-3809 depends on § 2.2's `TrailerMatch`**; its `body_shadowed` refusal is
  unimplementable without `offset` / `anchored`.
- **ANTS-3793's `BulletRecord::body`** is defined in terms of § 2.4's export.
- **`docs/subsystems.md`** needs no change **under two of § 4's three build
  options**, because neither adds a translation unit. The third — hoisting the
  accessor into a leaf TU both libraries link — does add one, and that file then
  gains a lane entry. Conditional on that decision, not independent of it.
  **`CLAUDE.md` is unaffected either way**: ANTS-1292 moved the per-file
  catalogue out of it, leaving a pointer, so a new TU is `docs/subsystems.md`'s
  business alone.
- **`src/roadmaprender.cpp`'s `Kind:` line carries the comment "Required piece,
  unconditionally (INV-12)"**, and § 2.3 makes that emission conditional. The
  comment is updated in the same change — it is the one place an implementer
  reading the render would be told the opposite of what this spec decided.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 3 (cap) | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, identical to loops 1–2) | 0 / 5 / 9 / 9 / 0 | **Converged by cap. 23 verified, 19 fixed, 4 filed, 2 dismissed** — tail at [`docs/reviews/ANTS-3808-cold-eyes-loop3-tail.md`](../reviews/ANTS-3808-cold-eyes-loop3-tail.md); fold in directly, do NOT re-dispatch. **Phase 5's stop-and-consolidate trigger fired: collateral outran draft defects two loops running (14 v 1, then 20 v 3).** The one CRITICAL raised was **dismissed on verification** — a lane argued the non-suppression branch breaks INV-3 on a migrated item, but § 2.4's render reconstructs the headline into the head line, so a key inside it reappears ahead of the residual and the re-parse takes the same first match; no divergence could be constructed. What loop 3 actually caught was loop 2's repairs: § 2.2's "move to file scope" contradicted § 4's newly-added "not namespace-scope globals" (§ 4 now owns placement); § 2.2.1 claimed four rows lack a `parseBullets()` counterpart when only `offset` and `anchored` do — `lanes.value` and `evidence.value` are the reader's own `lanesRaw` / `evRaw`, so INV-4 was told not to grade two rows it can; INV-5's fixture list had drifted off § 2.1's table and **dropped the GFM row, the one that proves the strip is textual rather than syntactic**; and the strip rule never trimmed, so `" "` is not empty and the stray-blank-line guard never fires. Filed rather than fixed, because each needs a decision and all four land in § 2.1 / § 2.3.1 — the sections whose repairs generated most of loops 2 and 3. Lane spend 119k / 117k against a 60k budget. |
| 2 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, identical to loop 1's) | 1 / 4 / 4 / 6 / 0 | **15 verified, all 15 fixed, 3 dismissed — and 14 of the 15 were loop 1's own fix collateral, which is the finding about this run.** Both lanes led on the same CRITICAL, and it was loop 1's repair turned inside out: `anchored` was defined as `offset == 0 \|\| body.at(offset-1) == '\n'` with `offset` pinned to `capturedStart(1)`, but every pattern puts literal text between the line start and group 1, so the field is unreachably **false on every key of every bullet** — carrying less than the per-key constant loop 1 rejected it for. Now computed off `capturedStart(0)`, and INV-4 requires both polarities so a false-only fixture set cannot pass against it. Loop 1's prefix-strip rule also proved under-specified three ways it could not have been read as: stated in `[id]`/`**` tokens it strips nothing from a GFM bullet; an empty first-line residual kept as an empty string makes `body` begin with `'\n'` and emits a stray blank line on nearly every bullet; and it never said the stripped headline is the **untruncated** one, so any headline over 120 chars would have left its tail behind. **One genuine draft defect, and it is the loop-1 ledger's own failure:** § 7's "behaviour-preserving" claim about the dialog edit was recorded as fixed and had only been fixed in ANTS-3793 — both lanes re-found it, which is the cold re-read working exactly as designed. The harder sweep this loop then caught a figure that had gone stale *within the run*: filing ANTS-3811 moved the corpus denominator 1645 → 1646, so the ratio is now the durable claim. Lane spend 109k / 116k against a 60k budget. |
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet) | 3 / 3 / 7 / 11 / 0 | **24 verified, 23 fixed, 1 surfaced, 2 dismissed.** Both lanes independently led on the same two — `anchored` carrying two incompatible definitions, and § 2.3.1's "same body" equality — and the second was the draft's own central correctness argument, false because § 2.1 stores a residual while the column was extracted from the full body. **The sharpest finding came out of verifying a lane's weaker version of it:** § 2.1's "drop the first line" is *lossy*, not merely imprecise. A native bullet takes `headline` from the bold token only, so text after the closing `**` lives nowhere but `body`'s first line — measured **241 of 1645** bracket-id bullets in this project's own `ROADMAP.md`, and for a single-line bullet it is the item's entire substance (`[ANTS-1649]`, `[ANTS-1650]`). The rule became a prefix strip and gained INV-5 to catch the naive reading. **Surfaced, not fixed:** § 2.3 puts a `trailerValuesIn()` call inside `ants_roadmapstore_lib`, which links `Qt6::Core` + `Qt6::Sql` and nothing else by deliberate design (`src/roadmaprender.h:11-12`, for ANTS-3794's headless path) — § 4 claimed "no edge"; it now carries three options and a named owner. Sweep also found ANTS-3793's umbrella holding a stale duplicate of this contract, including the false equality claim: banners added there rather than reconciling two copies. Lane spend 107k / 105k against a 60k budget. |
