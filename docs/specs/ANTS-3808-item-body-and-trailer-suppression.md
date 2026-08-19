# ANTS-3808 — `item.body`: what the migration stores and what the render re-derives

**Status:** accepted (2026-08-04) — cold-eyes loops 1–3 folded, converged by
cap; the loop-3 tail folded in post-cap and § 4's build decision settled. The
two 2026-08-19 amendments (ANTS-4505, ANTS-4506) ran their own gate — loop-log
rows 3 and 4, dated 2026-08-19 — converged by cap. No further review gate is owed.
**SHIPPED 2026-08-19.** The whole surface is now built: § 2.2's accessor and
§ 2.4's export under ANTS-4497 / ANTS-4504, § 2.1's prefix strip with it, and
§ 2.1's trailing-trailer strip plus § 2.3's presence suppression under
ANTS-4506 / ANTS-4505. Tests: `tests/features/roadmap_item_body/`, six cases.
The two the amendments touch were verified red before the fix and against each
*Breaks when* mutation: `Inv3RenderReaderAgree`'s third fixture (value equality
restored) and `Inv6RoundTripAddsNothing` (the strip omitted, then the
only-declaration condition dropped).
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
[2.3 Suppression on a line-initial declaration](#23-suppression-on-a-line-initial-declaration) ·
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

**The decision: the migration strips the render's own head-line prefix at a
boundary the reader records, and the render asks before it re-derives.** Neither
half needs a store-schema change, a new provenance value, or a second parser;
the reader's record gains one integer (§ 2.1).

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

**So the rule is a prefix strip, and the boundary is RECORDED BY THE READER** —
never re-derived by matching the stored headline back against the line it came
from. The reader is the only party that knows where the prefix ends, and it
already knows it exactly.

`BulletRecord` gains one field:

```cpp
// src/roadmapparse.h, in BulletRecord — ANTS-3808. The offset into `body`
// just past the text this parse CONSUMED to build the head line's id and
// headline; equivalently, the first character of the head line the render
// will NOT reconstruct. QString positions (UTF-16 code units), as § 2.2's
// `offset` is. -1 when no headline was assigned, which the migration reads
// as "strip nothing".
int headlineEnd = -1;
```

`parseBullets()` sets it at the **four** sites that call `assignHeadline()`, and
setting it is that call's job — so a fifth headline site cannot be added without
one. Cited by branch rather than line (`documentation.md` § 1.7), all four
inside `parseBullets()` unless noted:

| Site | Shape | `headlineEnd` |
|---|---|---|
| the GFM em-dash split (`splitOnEmDash`, ANTS-1438 INV-4) | `**BoldID** — headline` | end of the head line — the whole line is consumed |
| the head-anchored `rxBold` branch | `[id] **headline** <prose>` | `boldMatch.capturedEnd()`, or `m2.capturedEnd()` where the bold-ID rule took the *second* bold token |
| the GFM prose fallback (ANTS-1428 / ANTS-2046) | the first line **is** the headline | end of the head line |
| the pass-headings reader (its own function) | `#### Pass N.M` | end of the head line |

**Matching the headline as a string was the alternative, and it is rejected on
evidence.** The reader *normalises* what it stores: the GFM prose fallback
removes `**` markers (`h.remove("**")`, ANTS-2046 de-markup) and strips a
trailing caret anchor before `trimmed()`, so on exactly those bullets the stored
headline is **not a substring of the line it came from**. A string match
therefore fails, the head line survives intact, and the duplication INV-1
forbids returns **silently** — this spec's own defect, restored by its repair,
on the format least likely to be in a fixture. An offset cannot fail, cut short,
or cut long.

**Two worries the offset retires rather than answers**, both verified against
`src/roadmapparse.cpp` on 2026-08-04 and recorded so they are not re-raised:

- **Headline truncation.** `rec.headline` is display-truncated to 120 characters
  (ANTS-1811 / ANTS-2075), so a text match would have had to argue it used the
  untruncated form. All four sites go through `assignHeadline()`, which sets
  `headlineFull` and `headline` together, and **no other site assigns
  `rec.headline`** — so `makeItem()`'s
  `rec.headlineFull.isEmpty() ? rec.headline : rec.headlineFull` always yields
  the untruncated value. The offset makes the question moot: nothing is matched.
- **GFM task-list checkboxes.** A `- [ ]` / `- [x]` token never reaches the
  strip. The reader does `head.remove(0, 3)` plus a leading-space loop on the
  checkbox branch, and `body` is seeded from `head` **after** that. No bracket
  token that is not an id can be present, and the strip matches no bracket token
  in any case.

**`makeItem()` in `src/roadmapmigrate.cpp` owns the strip** — its one line
`it.body = rec.body;` becomes the three steps below. Not
`RoadmapMigrateLoad::load()`: `makeItem()` is where the `BulletRecord` is, and
it sits in `ants_core_lib`, which § 4's decision keeps off the store's link
surface.

1. `rest = rec.body.mid(rec.headlineEnd)` when `headlineEnd >= 0`; the body
   unchanged when it is `-1`.
2. **Trim `rest`'s own first line — the text up to its first `'\n'` — and if
   nothing but whitespace remains, drop that line entirely.** Both halves are
   load-bearing. The separator space after `[<id>]` and the space before any
   surviving prose are still there, so an untrimmed residual is `" <prose>"`
   rather than `<prose>`, and an untrimmed *empty* residual is `" "` — which is
   **not** empty, so the drop never fires. Joining that back with `'\n'` yields
   a `body` beginning with whitespace, which passes `renderBullet()`'s
   `if (!it.body.isEmpty())` guard and emits a stray blank indented line on
   nearly every bullet.
3. Join whatever survives with the continuations, `'\n'`-separated, as before.

**`PlannedItem.headline` and `ItemWrite.headline` are the same string**, by
direct assignment (`src/roadmapmigrateload.cpp`, `w.headline = it.headline`).
Stated because § 2.4's render re-emits `ItemWrite.headline` into the head line,
and INV-5's round-trip closes only if what the render puts back is what the
strip took out.

Four shapes, all determinate:

| Bullet shape | Stored `body` |
|---|---|
| `[id] **headline**`, no continuation | empty |
| `[id] **headline**` + continuations | the continuations only, **less any stripped trailing run** — **no leading newline** |
| `[id] **headline** <prose>` | `<prose>`, then any continuations — the case the naive rule loses |
| GFM, where the headline **is** the whole first line | the continuations only; the recorded offset is what makes this row work, with no `[id]` or `**` token to match |

**An empty `body` is a normal outcome, not an error** — it is what row 1
produces, and § 2.3's suppression then fires for no key, so every column is
emitted exactly once. INV-1 and INV-5 both cover it.

**AMENDED 2026-08-19 (ANTS-4506) — a TRAILING run of trailer-only lines is
metadata, not body.** After the prefix strip, drop from the end of `body` every
line that consists of nothing but one trailer declaration
(`^(?:\*\*)?(?:Kind|Lanes|Layman|Evidence|Source):` and its value), stopping at
the first line that is not one. The columns are extracted from the full body as
before — this changes what is STORED, never what is read.

**The predicate lives in `RoadmapParse`, exported beside `trailerValuesIn()`,
and NOT in `makeItem()`.** INV-2 asserts that `RoadmapParse` is the only bullet
grammar in `src/`, and its test is a scrape for a trailer-key literal inside a
`QRegularExpression` construction anywhere else — so writing this regex in
`src/roadmapmigrate.cpp`, where § 2.1 puts the strip's call site, fails this
spec's own invariant on this spec's own deliverable. `makeItem()` calls it;
`RoadmapParse` owns it.

**Case-sensitive, all five keys.** The lines this strips are the render's own
output (§ 2.4), whose spelling is fixed, so tolerance buys nothing and costs the
`Lanes:`-versus-`lanes:` ambiguity ANTS-4065 § 2.2 removed from `rxKind()` /
`rxLanes()` / `rxSource()` for the same reason. **Verified 2026-08-19 against
`src/roadmapparse.cpp`: `rxKind`, `rxLanes` and `rxSource` carry
`MultilineOption` alone; `rxLayman` and `rxEvidence` still carry
`CaseInsensitiveOption`.** So a hand-typed `layman:` at a bullet's tail is
matched by § 2.3 and suppressed; a hand-typed `kind:` is matched by neither the
strip nor the matcher, stays in the body, and the render appends its own
`Kind:` line beside it. That second copy is ANTS-4065 § 2.2's accepted cost of
dropping case tolerance, not a new one — and § 1's premise limits it to
pre-migration files.

**Why: without it the round trip is not stable, and the instability is
measurable.** § 2.4's render appends its trailer block at the END of the
bullet. The next parse has no way to tell that block from prose, so it files it
into `body` — and the body is no longer the residual this section defines. One
re-migration of this project moved **599** bodies; bodies containing a `Kind:`
trailer went 1,605 → 2,063 (**+458**) and a `Source:` trailer 1,493 → 1,590
(**+97**), from a single cycle. It converges after one cycle rather than growing
without bound, so it is accretion and not a leak — but every diff-derived
counter rests on `parse(render(x)) == x`, and `items_updated` is one of them.

**The strip is what makes the round trip an identity rather than a fixed point
reached on the second pass.** A conforming bullet writes its trailers last, so
the strip moves the lines § 2.4 re-emits: parse drops them, render puts them
back. A bullet with prose *below* its trailers keeps them in the body — the run
stops at the prose — and § 2.3's suppression then keeps the render from adding a
second copy.

**"The lines § 2.4 re-emits" is not "every line § 2.4 could emit", and the gap
is where a strip loses data.** Three of the render's five emissions are
conditional: `Layman:`, `Lanes:` and `Evidence:` are gated on a non-empty
column, and `Source:` additionally on `provenance.source != "defaulted"`. Strip
a line the render then declines to write back and it is gone from the file with
nothing to show for it.

**The gates cannot bite a non-empty trailing declaration, and that is why the
strip is safe rather than why it is guarded.** A trailing `Layman:` / `Lanes:` /
`Evidence:` line with a value makes its column non-empty by construction — the
column was extracted from the body that carries it. And `provenance.source` is
`defaulted` only where the bullet declared no `Source:` at all, so a bullet with
a trailing `Source:` line never carries it. `Kind:` is unconditional. **What is
left is the empty declaration** — a bare `Lanes:` with nothing after it — which
strips and is not re-emitted; that loses an empty line and no value, and
`splitTrailerList()` already yields nothing for it.

**It is the trailing RUN, not every trailer line.** A trailer in the middle of a
body is authored prose about the item and stays; only the block at the tail is
the render's. And the strip is per-line-shape, never a count: a body that is
*entirely* trailer lines strips to empty, which § 2.1 already calls a normal
outcome.

**And a line is stripped ONLY when it is the ONLY LINE-INITIAL DECLARATION of
its key in the body.** This is the condition § 2.3.1 and INV-6's fifth fixture
both name, stated here because this is where it is owned; it was added to both
citing passages at loop 4 and to this one only on ship. Without it the residual
is an INFIX of the full body rather than a suffix, and § 2.3.1's migrated-item
guarantee rests on it being a suffix. Worked: a bullet with a stale line-initial
`Kind: bug` in a continuation and its canonical `Kind: implement.` at the tail
migrates with the column `implement`, has the tail stripped, and then suppresses
on `bug` — so `bug` is the file's only `Kind:` and the next migration adopts it.
**A migrated item silently rewriting its own column, with no consumer write, and
the `kind` vocabulary rider is no help because `bug` is recognised.** The column
is `matchLastIn()` over the full body; with the condition, the residual's last
line-initial declaration is still the one the column took.

**A mid-sentence mention does not count as a declaration for this test**, for
the same reason it does not suppress: it cannot outrank the column on a
re-parse either (ANTS-4065 INV-11 — a line-initial match beats a mid-line one).
**And the line must carry ONE key**: 10 corpus lines write two on one line, and
such a line is not "nothing but one trailer declaration".

ANTS-3757 § 2.1.1's `body` row is amended on ship; § 7 owns that obligation and
its caveat.

### 2.2 Asking the grammar: `trailerValuesIn()`

**§§ 2.2 and 2.4 SHIPPED. They are kept in the present tense as the contract
they are, but the work they describe is done** — noted 2026-08-19, when a cold
read could not tell which of this document was still owed. `trailerValuesIn()`
is exported from `src/roadmapparse.h`, `bulletText()` from
`src/roadmaprender.h:106`, and `src/roadmaprender.cpp` already calls the
accessor. **What is still owed is §§ 2.1 and 2.3 as amended on 2026-08-19**, and
INV-6.

The six matchers (`rxKind`, `rxLanes`, `rxLayman`, `rxEvidence`, `rxSource`, and
`rxTrailerKey`, which bounds a value at a following key on the ten two-key
lines) were `static const` **inside `parseBullets()`** and reachable from
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

### 2.3 Suppression on a line-initial declaration

**`renderBullet()` emits a trailer key from its column only when the body does
not already DECLARE that key**, asked through § 2.2's accessor. A declaration is
a line-initial match; a mid-sentence mention is not one, and a mention inside a
code span is not a match at all (ANTS-4504).

**One accessor call per bullet, reused across all five comparisons** — `const
TrailerValues tv = RoadmapParse::trailerValuesIn(it.body);` once, then:

| Column | Suppress when |
|---|---|
| all five | `tv.<key>.offset >= 0 && tv.<key>.anchored` |

**AMENDED 2026-08-19 (ANTS-4505): one row, not two.** The table read
`tv.<key>.value == it.<key>` for the three scalars and element-by-element list
equality for `lanes` and `evidence`, and the list rows existed only because
comparing a pre-split string against a joined column diverges on separator
spacing — a hazard of comparing values at all. Presence does not compare
values, so the split forms are no longer read here and the two rows collapse
into one. `tv.lanesList` / `tv.evidenceList` stay on the accessor for their
other consumers.

Calling the accessor per key would be five passes and up to thirty matches per
bullet, against § 4's budget of six.

**AMENDED 2026-08-19 (ANTS-4505) — suppression is PRESENCE, line-initial, not
value equality. The argument below is kept because it is what the reversal has
to answer.**

> **Value equality and not mere presence — and the reason is value loss, not an
> INV-12 breach.** Stated precisely, because the tempting shorter argument is
> wrong: ANTS-3758's INV-12 asserts against the *rendered text*, and a body that
> made a presence check fire necessarily contains the literal `Kind:` itself, so
> that text is still in the output and INV-12 still passes. Presence-based
> suppression does not break INV-12.
>
> What it breaks is **correctness of the value**. A body line reading
> `Kind: refactor` — stale prose, or a key the author typed into the body — has
> the key present and a value the store disagrees with. Suppress on presence and
> the render drops the canonical column line, leaving the *stale* value as the
> only one in the file; the next migration reads that back and the store has
> silently adopted it.

**The rule is now:**

> **Suppress a key when `trailerValuesIn(it.body)` matched it and the match is
> LINE-INITIAL — `m.offset >= 0 && m.anchored` — whatever the value.**
>
> **For `kind`, additionally require the value to be one the vocabulary
> recognises** (`RoadmapParse::isRecognisedKind`).

**The `kind` rider is not symmetry-breaking for its own sake.** `matchLastIn()`
takes `kind` only among candidates the vocabulary recognises, but when NO
capture is recognised it returns the last match RAW rather than nothing (§ 2.2)
— deliberately, so `makeItem()`'s unmapped branch still sees the value. Under
presence that raw capture would suppress: a residual whose only line-initial
`Kind:` is an unrecognised fragment would drop the recognised column line, leave
the fragment as the file's only `Kind:`, and the next parse would adopt it. The
column degrades one migration at a time with nothing reporting it. Requiring a
recognised value closes it, and costs nothing the other four keys need — they
have no closed value set, so there is no "unrecognised" state for them to be
in.

**What the value-equality rule cost, which is why it goes.** It is
one-directional. The render appends its block at the END of the bullet, and the
parser takes the last line-initial match, so the appended copy outranks any
trailer written earlier in the body. Once a column holds a wrong value the
render writes that wrong value to the tail, the next migration reads it back,
and **a human correcting the real trailer line in place is silently ignored,
permanently.** Observed on ANTS-3808 itself, whose stored `provenance` reads
`test` and whose layman line reads `An older thing.` — both captured from a
`DEMO-0003` example the bullet quotes, and neither moved by any re-migration.
The file is the authoring surface; an edit to it that can never take effect is
a defect however canonical the column is.

**Why presence is now SAFE, and this is a change in the world rather than a
change of mind.** The old argument needs a body that carries the key with a
value the column disagrees with. Three things have closed the routes to that
state since it was written:

- **The write side already refuses it.** `rlBodyShadows()`
  (`src/remotecontrol_roadmap_query.cpp`) refuses `append` / `append_batch` with
  `body_shadowed` when a supplied column differs from what the body yields —
  and those two are the only ops that set a column independently of the body
  they ship with (§ 2.3.1). ANTS-3809 § 2.6 has every other body-writing op
  re-derive its columns from the body it just wrote.
- **A quoted key is no longer present.** ANTS-4504 masks every inline code span
  before matching, so the mention-in-prose case that made presence jumpy —
  `` `query:'Source:'` `` — declares nothing at all now.
- **`anchored` is part of the rule, not an afterthought.** A mid-sentence
  mention does not suppress; only a line-initial declaration does. Stored bodies
  carry no indent (§ 2.1), so `anchored` is exact for all five keys — the
  precondition ANTS-3809's loop 3 established and stated.

**So the state the old rule protected against is unreachable through a
supported write, and the one it produced — an uncorrectable column — is
reachable every time a value is read wrong once.** INV-12 continues to hold as
written on either rule: the required piece is in the rendered text exactly once.

**What is given up, stated.** A body that carries a stale line-initial trailer
reached by some route this section has not foreseen now wins over its column,
where before the column won. That is the same trade in both directions; the
amendment picks the direction in which a human can intervene.

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
differ only when the matched key sat in the stripped prefix — for a native
bullet that means *inside* the bold headline, since anything after the closing
`**` survives the strip; for a GFM bullet it means anywhere on the head line,
which § 2.1's recorded offset consumes whole.
`[ANTS-1649]` carries its `Kind:` and `Lanes:` after the `**`, so it is an
example of the **agreeing** case, not the divergent one; a bullet whose only
`Source:` sat *inside* the bold headline would not.

So the honest statement is a **direction**, not an identity:

- Key present in the residual with the same value → suppression fires, emitted
  once from the body. Correct.
- Key declared line-initially with a DIFFERENT value → suppression still fires
  (§ 2.3, amended), the body's line is the only one in the file, and the next
  parse adopts it. **This is the branch ANTS-4505 reversed**, and reversing it
  is the point: it is what lets a human correct a wrong value in the file.
- Key absent from the residual, or present only mid-line → suppression does not
  fire and the render emits the **column** value.

Both branches land on exactly one occurrence of the value that REACHES THE FILE,
which is what INV-1 and INV-3 assert. Reaching the second branch's divergence
needs a residual carrying the key with a value the column disagrees with, and
for a MIGRATED item that cannot happen — **but only because § 2.1's strip
refuses to remove a line when the body carries another line-initial declaration
of the same key.** The column is `matchLastIn()` over the full body; with that
condition the residual's last line-initial declaration is still the one the
column took. Drop the condition and the residual becomes an infix rather than a
suffix, the guarantee fails, and a migrated item can rewrite its own column —
§ 2.1 works the case. It takes a
consumer write — an `append` or `append_batch` under ANTS-3809 § 2.5, the two
ops that set a column independently of their body — and that is the write
`rlBodyShadows()` refuses.

**Corrected 2026-08-05, per ANTS-3809 § 7.** This sentence named `flip` and
`annotate`, which was right when it was written and is now wrong: ANTS-3809
§ 2.6 has every op that writes `body` re-derive its trailer columns from the
body it just wrote, so a `flip`-with-`note` or an `annotate` *cannot* leave a
column disagreeing with its body. The hazard is unchanged; the ops that can
reach it are `append` and `append_batch`, the only two that set a column
independently of the body they ship with.

**"Post-cutover"**, used here and in INV-3, means an item whose columns have
been written by a consumer through the store after migration — as opposed to a
**migrated** item, whose columns and body were both produced by one
`parseBullets()` pass over the source markdown.

**Fixing the grammar is out of scope** — § 5 states why and ANTS-3811 owns the
decision.

### 2.4 The render's one export

`renderBullet(const RoadmapStore::ItemWrite &)` was a **free function in an
anonymous namespace** in `src/roadmaprender.cpp` with no exported counterpart.
**Shipped:** `src/roadmaprender.h:106` declares
`QString bulletText(const RoadmapStore::ItemWrite &it);` and § 7 records the
header surface it gained.

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

**The export itself costs no link edge; § 2.3's *call* did, and § 4 settles it —
the reader moves to a `Qt6::Core`-only leaf library both sides link. § 4 is the
only place either is argued.**

## 3. Invariants

- **INV-1** — **A migrated project's rendered bullet contains its headline
  exactly once, and each trailer key's RENDERED VALUE exactly once**, `Kind:`
  included. **Amended 2026-08-19 (ANTS-4505): the rendered value is the body's
  line-initial declaration where one exists, otherwise the column.** It read
  *canonical value* — the column's — and under presence-based suppression the
  column's value reaches the file zero times on exactly the state INV-3's third
  fixture calls correct, so the old form was red against a conforming
  implementation.

  **It counts values, not key literals, and the difference is a state § 2.3.1
  calls correct.** In the no-suppression branch the residual mentions
  `Source: B` **mid-line** while the column emits `Source: A.` — the key
  *literal* appears twice, the rendered value once. **Mid-line is now the whole
  of it** (corrected 2026-08-19): under ANTS-4505 a *line-initial* `Source: B`
  suppresses, so only `B` reaches the file, the key literal appears once, and a
  key-counting assertion would pass too — the fixture would stop discriminating
  for the reason it exists. § 2.3.1's third branch is the only shape that still
  reaches the no-suppression branch. That output is correct (the column is
  canonical and wins), so an invariant counting key literals would fail on legal
  output and a test author following it would write a case that fails against a
  correct implementation. The value-counting form is strictly weaker than a key
  count, and it is the one § 2.3's suppression actually protects.

  *Rationale, not part of the assertion:* "exactly once" is a two-sided bound.
  The upper side is the duplication this spec exists to remove; the lower side is
  that a value must survive at all — a suppression that fires with nothing in the
  body to take its place loses the key outright, which is why § 2.3 requires a
  LINE-INITIAL declaration rather than any match. *Breaks when:* the migration stores the render's
  own head-line prefix in `item.body` (today's defect), or the render emits a
  column-sourced trailer line whose value the body already carries. *Test:*
  `Inv1NoDuplication`, over this directory's own bullet fixture, asserting
  exactly one occurrence of the headline and exactly one occurrence of each
  trailer key's **rendered value** in the rendered text — **including a bullet
  with an empty residual**, where every column is emitted unsuppressed and
  "exactly once" is the whole assertion, and **including the two-value case
  above**, which a key-counting assertion fails and a value-counting one passes.
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
  why: whichever branch the presence test takes, exactly one value reaches the
  file — the body's own declaration, or the column. **Amended 2026-08-19
  (ANTS-4505): on the suppressing branch the value that reaches the file is the
  BODY's, and the two agree because `rlBodyShadows()` refuses the only write
  that could separate them** (§ 2.3). *Breaks when:* the render suppresses a key
  the body does not declare line-initially, or emits one the reader then reads
  twice. *Test:* `Inv3RenderReaderAgree`, **three** fixtures — a migrated bullet
  whose residual carries the key line-initially (suppression fires), a
  post-cutover bullet with a residual body that does not (all keys emitted from
  columns), and **a bullet whose body declares the key with a value the column
  disagrees with (ANTS-4505: suppression still fires, the body's line is the
  only one in the file, and the re-parse adopts it — the case the value-equality
  rule answered the other way, so it is this amendment's discriminator)**.

  **The scope clause is load-bearing, and it excludes exactly one state.** A
  column written *without* rewriting the body leaves the body's own key
  shadowing the canonical value on a re-parse (§ 2.3.1), and only a consumer
  write can create that. ANTS-3809 owns making it refuse, and carries the
  fixture for it. Asserting the unscoped form here would ship this invariant red
  against a state this spec cannot reach. **The first two** of this invariant's
  fixtures are inside the scope — a post-cutover item is only excluded when it
  is *shadowed*, not for being post-cutover.

  **Fixture 3 is deliberately OUTSIDE that scope, and asserts the opposite
  direction** — corrected 2026-08-19, when the scope clause and the fixture the
  ANTS-4505 amendment added contradicted each other. It is constructed by
  writing the store directly, not through `roadmap_log` (which refuses it), and
  what it asserts is that **the re-parse yields the BODY's value and the column
  is not the expected result**. That is the amendment's whole point: the file is
  the authoring surface, so where the two have been separated the file wins and
  the store follows. Asserting store-equality there reds against a correct
  implementation.
- **INV-4** — **`trailerValuesIn(body)` equals what `parseBullets()` assigns from
  the same body**, over all five keys and § 2.2.1's normalisation table. Without
  the accessor is what both the render and the reader ask, so a divergence would
  put the two out of step on which keys a body declares. **Its `anchored` field
  is now load-bearing for suppression as well** (§ 2.3), not only for a consumer
  quoting a match. *Breaks when:* the accessor
  returns raw captures, skips the `rxTrailerKey` truncation or a trailing-period
  chop, or splits `lanes` / `evidence` differently. *Test:* `Inv4AccessorAgrees`,
  over a fixture table covering each normalisation step including the two-key
  line and the backticked-key guard, plus the **two** rows with no
  `parseBullets()` counterpart — `offset` and `anchored` — asserted against
  § 2.2.1 directly. **`lanes.value` and `evidence.value` are graded against the
  READER's own intermediates, not against the table**: § 2.2.1 says so, and
  grading them against the table would test the spec against itself on exactly
  the two keys whose normalisation is asymmetric. This clause said *four rows*
  and named all four as table-asserted until 2026-08-19. **`anchored` needs both polarities or it asserts
  nothing**: a line-leading `Kind:` (true) and an inline mid-prose `Source:`
  (false). A fixture set that only ever expects `false` passes against the
  always-false implementation this field was specified twice to avoid.
- **INV-6** — **Migrate-then-render is an identity on the FIRST cycle, and the
  stored body gains nothing.** The general assertion is over the BODY:
  `parse(render(parse(x))).body == parse(x).body` for every bullet. **Byte
  identity is the narrower clause and needs the narrower precondition**:
  `render(parse(x)) == x` holds only where x's trailing run is already in
  § 2.4's emission order (`Layman`, `Kind`, `Source`, `Lanes`, `Evidence`) and
  its exact spelling — `**Layman:** `, a trailing period on `Kind:`/`Source:`/
  `Lanes:` and none on `Evidence:` **or on `**Layman:**`, whose value the reader
  period-strips (ANTS-1154 INV-4), so a Layman line written in author style with
  its period cannot round-trip byte-identically** (added on ship, 2026-08-19,
  after a fixture written that way reddened against a correct build), lanes
  joined `", "`. **Two further
  preconditions come from the reader, not the render** (added 2026-08-19): every
  continuation must be indented **exactly two spaces**, because `parseBullets()`
  stores `cont.trimmed()` and `appendIndented()` puts two back; and the head
  line must carry **no text after the closing `**`**, because § 2.1 stores that
  text as a body line the render then emits on its own indented line — so the
  241-of-1646 shape cannot round-trip byte-identically at all, and
  **ANTS-4528** owns the indent half. *Trailers last* alone does not give byte
  identity, and a fixture written in author style reds against a correct
  implementation. *Breaks when:* § 2.1's
  trailing-trailer strip is omitted, which is the state measured on 2026-08-19:
  one re-migration moved **599** bodies, **+458** gaining a `Kind:` line they
  never carried and **+97** a `Source:` line, because the render's own appended
  block was filed back into the body. *Test:* `Inv6RoundTripAddsNothing`, three
  fixtures — trailers last (identity on cycle one); prose BELOW the trailers
  (the run stops at the prose, the trailer stays in the body, and § 2.3's
  suppression keeps the render from adding a second copy); and a body that is
  entirely trailer lines (strips to empty, which § 2.1 calls a normal outcome).
  **The middle fixture is the one an implementation fails by stripping every
  trailer line rather than the trailing run**, and it reds silently: the body
  still round-trips, it has just lost an authored line.

  **A FOURTH fixture is the only one that reds against the *Breaks when*
  mutation, and without it the red run cannot be produced** (added 2026-08-19).
  With the strip omitted, the first three bodies keep their trailing trailer
  lines, which under § 2.3 *declare* those keys, so every column line is
  suppressed and the render emits head + body verbatim — identity holds and the
  body is unchanged, for all three. The discriminator is **a bullet whose
  residual declares NO trailer key**: there the render emits its block, the next
  parse files it into `body`, and the stored body grows — the +458 / +97
  accretion this invariant exists for.

  **A FIFTH fixture pins § 2.1's only-declaration condition** — a body with a
  stale line-initial `Kind: bug` in a continuation and a canonical
  `Kind: implement.` at the tail. The tail line must NOT be stripped, the column
  must still read `implement` after a second cycle, and an implementation that
  strips it rewrites a migrated item's own column (§ 2.1).
- **INV-5** — **No bullet text is lost across migrate-then-render.** For every
  bullet, whatever of the head line the reader did **not** consume — on a native
  bullet, the text after the closing `**` — survives into the rendered output.
  This is the assertion § 2.1's recorded-offset strip exists to satisfy,
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
  emits a stray indented line. The **GFM row is what proves `headlineEnd` is set
  on every headline branch, not just the bold one** — a GFM bullet writes
  neither `[id]` nor `**`, so an implementation that records the offset only in
  the `rxBold` branch leaves it at `-1` there, strips nothing, and the
  duplication returns. A fixture set omitting either row passes against the
  defect it was written for.

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

**Build — settled 2026-08-04, and it was not editorial.** § 2.4's export costs
nothing: `roadmaprender.cpp` is already in `ants_roadmapstore_lib`
(`CMakeLists.txt`, its `# ANTS-3758` source line), the library ANTS-3793's
consumer lands in.

**What was not free is § 2.3's call.** It puts a `trailerValuesIn()` call inside
`ants_roadmapstore_lib`, whose link surface is today
`PUBLIC Qt6::Core Qt6::Sql` and **nothing else** — no `ants_core_lib`. That
minimal surface is a stated design property, not an accident:
`src/roadmaprender.h:11-12` says the library is "Qt6::Core + Qt6::Sql only …
because ANTS-3794 will call it from a headless publish path." A static archive
would still *build*, since the symbol resolves at final link and every current
consumer (`test_core`, the executable) links both libraries — which is exactly
why the wrong choice would pass CI and surface later, at ANTS-3794.

**The decision: hoist the reader into a `Qt6::Core`-only leaf library both sides
link.** `src/roadmapparse.cpp`, together with the heading helpers it depends on
(`src/roadmapindex.cpp` — `headingLevel` / `uniqueSlug`, reached through a
file-scope `using`), moves out of `ants_core_lib` into a new
`ants_roadmapparse_lib` linking `Qt6::Core` alone. `ants_core_lib` and
`ants_roadmapstore_lib` both link it, so every existing consumer keeps the
reader transitively and no call site changes. **No new source files** — the
grammar's home stays `roadmapparse.cpp`, which is what keeps INV-2 true by
construction rather than by discipline. Add the new target to
`_ants_subset_linked_libs` so the opt-in unity build does not coarsen a
subset-linked archive.

The two alternatives, recorded with why they lost:

| Rejected | Why |
|---|---|
| Declare `ants_roadmapstore_lib → ants_core_lib` | one line, but `ants_core_lib` links `Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus util` **PUBLIC** (`CMakeLists.txt:421-422`) — the whole desktop toolkit plus `libutil`, into a path built to run headless. Measured, not assumed: an earlier draft's "drags the whole of core" understated it |
| Caller computes `TrailerValues` and passes it in | no new edge, but it pushes the § 2.3 contract onto every render caller. One that omits it passes an empty `TrailerValues`, suppression silently never fires, and this spec's defect is live with every test green |

**This also settles which seam owns § 2.1's strip:** `makeItem()` stays in
`ants_core_lib` with the `BulletRecord` it reads, the accessor is defined in
`ants_roadmapparse_lib`, and the store's link surface gains `Qt6::Core` only —
which it already had.

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
| `Inv6RoundTripAddsNothing` | INV-6 |

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
  amendment must not disturb it. The same amendment records
  **`BulletRecord::headlineEnd`** (§ 2.1) — an additive field on ANTS-3757's
  read contract that no existing caller reads, so the widening is the same shape
  ANTS-3764 step 2 already made.
- **ANTS-3758's INV-12 is untouched and that is the point.** § 2.3's per-key
  suppression fires only on a line-initial declaration, which IS the literal
  `Kind:` sitting in the body, so the required piece is always in the rendered
  text — INV-12's own test asserts against rendered text, not against the
  render's choice of source. Recorded because a reader of § 2.3 will
  reasonably ask, and an unstated reconciliation reads as a silent repeal.
- **`src/roadmaprender.h` gains `RoadmapRender::bulletText()`** (§ 2.4), the
  render's first exported per-bullet surface.
- **`src/roadmapdialog.cpp`'s kind-filter pre-walk** moves onto
  `RoadmapParse::trailerValuesIn()` as part of INV-2 — an edit to a *rendering*
  path no other section would lead a reader to expect. **Corrected 2026-08-19:
  the swap is behaviour-preserving on case.** This bullet said the dialog's
  local `rxKind` carries `MultilineOption` only while `roadmapparse.cpp`'s adds
  `CaseInsensitiveOption` (ANTS-3407), and declared the resulting widening —
  a hand-edited `kind:` / `KIND:` bullet starting to match the kind filter. It
  no longer holds: **ANTS-4065 § 2.2 removed `CaseInsensitiveOption` from
  `rxKind`, `rxLanes` and `rxSource`**, re-read against
  `src/roadmapparse.cpp` on 2026-08-19, so both patterns are case-sensitive and
  the filter's behaviour on a hand-edited label is unchanged by the swap. A
  fixture asserting that `KIND:` starts matching would fail. What the swap does
  change is the *body assembly* — the local pre-walk re-implements
  `parseBullets()`'s continuation join — and that is the grammar duplication
  INV-2 forbids, which is reason enough on its own. § 4 carries what it costs.
- **ANTS-4065's INV-10 asserts the rule § 2.3 reversed, and is amended on ship**
  (added 2026-08-19, during implementation — neither gate loop found it). It read
  *un-anchoring changes render suppression only where a mid-prose `Kind:` value
  equals the column's*, with a shipped ctest case asserting that the column line
  is **suppressed** there. Under presence that case is false in the one direction
  it names — a mid-sentence mention declares nothing, so the column is emitted —
  and the test went red against a conforming build. ANTS-4065's INV-10 now defers
  to § 2.3 for the rule, its equal-value fixture asserts the trailer is emitted,
  and a fourth line-initial fixture pins the one shape that does suppress. Its
  two prose passages describing `shadows()` as value equality are corrected with
  it; its *must-fail-first* table is left as the historical record of that spec's
  own red run.
- **ANTS-3809 depends on § 2.2's `TrailerMatch`**; its `body_shadowed` refusal is
  unimplementable without `offset` / `anchored`.
- **ANTS-3793's `BulletRecord::body`** is defined in terms of § 2.4's export.
- **`docs/subsystems.md`** needs no change. § 4's accepted option adds no
  translation unit — `roadmapparse.cpp` and `roadmapindex.cpp` move between
  CMake targets, and the doc maps files to subsystems rather than to libraries.
  **`CLAUDE.md` is likewise unaffected**: ANTS-1292 moved the per-file catalogue
  out of it, leaving a pointer.
- **`src/roadmaprender.cpp`'s `Kind:` line carries the comment "Required piece,
  unconditionally (INV-12)"**, and § 2.3 makes that emission conditional. The
  comment is updated in the same change — it is the one place an implementer
  reading the render would be told the opposite of what this spec decided.

## Cold-eyes loop log

**Post-cap fold-in, 2026-08-04 — not a loop, and no reviewer was dispatched.**
The four findings filed at the loop-3 cap were folded in directly, per the tail
file's instruction. Two did not survive verification against source and are
recorded as dismissed in § 2.1 (the headline-truncation half of T1, and T3's
task-list checkbox); two were fixed (T1's match-failure half, by replacing the
string match with a reader-recorded offset — T2's key-versus-value question, in
INV-1); T4's owning seam is named in § 2.1 and § 4. **§ 4's surfaced build
decision was settled by the user**, closing the last precondition on § 2.3.
Detail: [`docs/reviews/ANTS-3808-cold-eyes-loop3-tail.md`](../reviews/ANTS-3808-cold-eyes-loop3-tail.md).
The rows below are frozen past-state records and were not edited.

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 3 (cap) | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, identical to loops 1–2) | 0 / 5 / 9 / 9 / 0 | **Converged by cap. 23 verified, 19 fixed, 4 filed, 2 dismissed** — tail at [`docs/reviews/ANTS-3808-cold-eyes-loop3-tail.md`](../reviews/ANTS-3808-cold-eyes-loop3-tail.md); fold in directly, do NOT re-dispatch. **Phase 5's stop-and-consolidate trigger fired: collateral outran draft defects two loops running (14 v 1, then 20 v 3).** The one CRITICAL raised was **dismissed on verification** — a lane argued the non-suppression branch breaks INV-3 on a migrated item, but § 2.4's render reconstructs the headline into the head line, so a key inside it reappears ahead of the residual and the re-parse takes the same first match; no divergence could be constructed. What loop 3 actually caught was loop 2's repairs: § 2.2's "move to file scope" contradicted § 4's newly-added "not namespace-scope globals" (§ 4 now owns placement); § 2.2.1 claimed four rows lack a `parseBullets()` counterpart when only `offset` and `anchored` do — `lanes.value` and `evidence.value` are the reader's own `lanesRaw` / `evRaw`, so INV-4 was told not to grade two rows it can; INV-5's fixture list had drifted off § 2.1's table and **dropped the GFM row, the one that proves the strip is textual rather than syntactic**; and the strip rule never trimmed, so `" "` is not empty and the stray-blank-line guard never fires. Filed rather than fixed, because each needs a decision and all four land in § 2.1 / § 2.3.1 — the sections whose repairs generated most of loops 2 and 3. Lane spend 119k / 117k against a 60k budget. |
| 4 | 2026-08-19 | 3, cold — identical brief, packet rebuilt from disk plus two settled facts about unchanged source established while verifying loop 3 | **Q1 1 · Q2 5 · Q3 1 · Q4 2** — verified 9, fixed 9, dismissed 0 | (Q-counts) | **Cap reached (2 for a spec); the run files its tail and ships. A CALM cap by count — but the headline finding is one the two amendments create JOINTLY, which neither loop could have seen before both were written.** **Two lanes independently found it:** § 2.1's strip makes the residual an INFIX of the full body rather than a suffix, and § 2.3.1's migrated-item guarantee rests on it being a suffix. Worked: a bullet with a stale line-initial `Kind: bug` in a continuation and its canonical `Kind: implement.` at the tail migrates with column `implement`, has the tail stripped, then suppresses on `bug` — so `bug` is the file's only `Kind:` and the next migration adopts it. **A migrated item silently rewriting its own column, with no consumer write, and the `kind` vocabulary rider is no help because `bug` is recognised.** Closed by a condition on the strip rather than by narrowing the guarantee: a line is stripped only when it is the ONLY line-initial declaration of its key in the body. **All three lanes found the second**, and it is loop 3's own: the `kind` rider that loop added appears in the prose and NOT in the § 2.3 table, which is the surface an implementer transcribes — so the coded predicate would omit exactly the guard loop 3 introduced. **Two Q4s, both about fixtures that cannot fail.** INV-6's three fixtures all pass with the strip omitted, because a body keeping its trailing trailer lines *declares* those keys and § 2.3 suppresses every column line — identity holds and nothing accretes; the discriminator is a residual declaring NO trailer key, which was not in the list. And INV-1's two-value case stops discriminating under presence unless the shadowing mention is MID-LINE. A fifth INV-6 fixture now pins the only-declaration condition. **One Q1 settled by opening the file:** two passages disagreed on whether `rxKind` carries `CaseInsensitiveOption`. It does not — ANTS-3407 added it and ANTS-4065 § 2.2 removed it — so § 7's declared user-visible widening is false and a filter fixture asserting `KIND:` now matches would red. **One more of loop 3's:** INV-3's scope clause excludes the very fixture loop 3 added to discriminate the amendment, and still said *Both* over three; the fixture is now declared as deliberately outside the scope, asserting that the re-parse yields the BODY's value. Also fixed: § 2.1's four-shape table, to which INV-5's test is pinned, still described the unstripped body; INV-6's byte-identity clause gained the two preconditions the reader imposes (two-space continuation indent, no post-`**` text, the latter unreachable and owned by ANTS-4528); and the strip predicate's right edge is pinned to a single-key full line. Doc 941 → 994 lines. **Deferred tail: none.** |
| 3 | 2026-08-19 | 3, cold — genre pinned `spec`, cap 2; packet carried seven bounded source windows and an explicit note that neither amendment is implemented | **Q1 3 · Q2 6 · Q3 2 · Q4 0** — verified 11, fixed 11, dismissed 1 | (Q-counts) | **The gate on ANTS-4505 + ANTS-4506, run before any code. Nine of the eleven are the amendment's own collateral, and the shape is uniform: § 2.3's rule was reversed and every passage that RESTED on the old rule was left standing.** All three lanes independently found the same three, which between them are the whole class. § 2.3.1's two-branch table still read *present with a different value → suppression does not fire … the column is canonical*, which is the shipped `shadows()` an implementer would have rebuilt. **INV-1 asserted the CANONICAL value exactly once** — under presence that value reaches the file zero times on precisely the state INV-3's new third fixture calls correct, so the invariant was red against a conforming build and the tempting repair is to restore value equality. And § 7's ANTS-3758 reconciliation still said *§ 2.3's per-key suppression compares values*. **The sharpest single finding is lane A's, and it is a data-loss path I wrote:** § 2.1 claimed the strip *moves exactly the lines § 2.4 will re-emit*, and three of the render's five emissions are conditional — `Layman:`, `Lanes:` and `Evidence:` on a non-empty column, `Source:` additionally on `provenance.source != "defaulted"`. Resolved by proving the gates cannot bite a non-empty trailing declaration rather than by adding a guard: the column is extracted from the body that carries the line, and `provenance.source` is `defaulted` only where the bullet declared none. **A second of mine, found while verifying rather than by a lane, and it is the one presence-suppression genuinely opens:** `matchLastIn()` returns the last match RAW for `kind` when no capture is recognised, so a residual whose only line-initial `Kind:` is an unrecognised fragment would suppress the recognised column, leave the fragment as the file's only `Kind:`, and be adopted by the next parse — the column degrading one migration at a time with nothing reporting it. The rule gains a vocabulary rider for `kind` alone. **Two more structural:** INV-2 forbids a trailer-key regex anywhere outside `RoadmapParse`, and § 2.1 put the new strip in `makeItem()` — the spec failing its own invariant on its own deliverable, now owned by `RoadmapParse` and stated case-sensitive; and § 6's case table had no `Inv6RoundTripAddsNothing` row, so the amendment's whole deliverable would have shipped untested. **Two pre-existing:** INV-4 named *four rows with no `parseBullets()` counterpart* against § 2.2.1's *only two*, which would have had the test grade the spec against itself on the two asymmetric keys; and §§ 2.2/2.4/§ 4 still described their own work as unbuilt — the accessor and `bulletText()` both shipped under ANTS-4497/4504 — so a reader could not tell what was still owed. INV-6's byte-identity clause was narrowed to the render's own emission order and spelling, *trailers last* alone not being sufficient. **One dismissed:** § 4's *at most six* per-bullet regex matches is stale — `matchLastIn()` global-matches and ANTS-4504 adds a masking pass — but the argument it supports (one accessor call, not five) is unchanged, so it changes no line. Doc 851 → 941 lines; invariants 5 → 6. **Not converged; loop 2 owed, and the cap is 2.** |
| 2 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, identical to loop 1's) | 1 / 4 / 4 / 6 / 0 | **15 verified, all 15 fixed, 3 dismissed — and 14 of the 15 were loop 1's own fix collateral, which is the finding about this run.** Both lanes led on the same CRITICAL, and it was loop 1's repair turned inside out: `anchored` was defined as `offset == 0 \|\| body.at(offset-1) == '\n'` with `offset` pinned to `capturedStart(1)`, but every pattern puts literal text between the line start and group 1, so the field is unreachably **false on every key of every bullet** — carrying less than the per-key constant loop 1 rejected it for. Now computed off `capturedStart(0)`, and INV-4 requires both polarities so a false-only fixture set cannot pass against it. Loop 1's prefix-strip rule also proved under-specified three ways it could not have been read as: stated in `[id]`/`**` tokens it strips nothing from a GFM bullet; an empty first-line residual kept as an empty string makes `body` begin with `'\n'` and emits a stray blank line on nearly every bullet; and it never said the stripped headline is the **untruncated** one, so any headline over 120 chars would have left its tail behind. **One genuine draft defect, and it is the loop-1 ledger's own failure:** § 7's "behaviour-preserving" claim about the dialog edit was recorded as fixed and had only been fixed in ANTS-3793 — both lanes re-found it, which is the cold re-read working exactly as designed. The harder sweep this loop then caught a figure that had gone stale *within the run*: filing ANTS-3811 moved the corpus denominator 1645 → 1646, so the ratio is now the durable claim. Lane spend 109k / 116k against a 60k budget. |
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet) | 3 / 3 / 7 / 11 / 0 | **24 verified, 23 fixed, 1 surfaced, 2 dismissed.** Both lanes independently led on the same two — `anchored` carrying two incompatible definitions, and § 2.3.1's "same body" equality — and the second was the draft's own central correctness argument, false because § 2.1 stores a residual while the column was extracted from the full body. **The sharpest finding came out of verifying a lane's weaker version of it:** § 2.1's "drop the first line" is *lossy*, not merely imprecise. A native bullet takes `headline` from the bold token only, so text after the closing `**` lives nowhere but `body`'s first line — measured **241 of 1645** bracket-id bullets in this project's own `ROADMAP.md`, and for a single-line bullet it is the item's entire substance (`[ANTS-1649]`, `[ANTS-1650]`). The rule became a prefix strip and gained INV-5 to catch the naive reading. **Surfaced, not fixed:** § 2.3 puts a `trailerValuesIn()` call inside `ants_roadmapstore_lib`, which links `Qt6::Core` + `Qt6::Sql` and nothing else by deliberate design (`src/roadmaprender.h:11-12`, for ANTS-3794's headless path) — § 4 claimed "no edge"; it now carries three options and a named owner. Sweep also found ANTS-3793's umbrella holding a stale duplicate of this contract, including the false equality claim: banners added there rather than reconciling two copies. Lane spend 107k / 105k against a 60k budget. |
