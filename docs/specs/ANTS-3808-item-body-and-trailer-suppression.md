# ANTS-3808 — `item.body`: what the migration stores and what the render re-derives

**Status:** spec draft (2026-08-03).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3808, found while verifying ANTS-3806 (2026-08-03);
split out of ANTS-3793 at that spec's cold-eyes cap the same day.
**Blocked by:** ANTS-3758 (the render this changes) — shipped.
**Blocker for:** ANTS-3794 (publish), which would otherwise write § 1's
duplication into every migrated project's `ROADMAP.md`; and ANTS-3809, whose
`body_shadowed` refusal is built on § 2.2's match provenance.
**Pairs with:** ANTS-3757 (the migration read, whose § 2.1.1 § 2.1 amends),
ANTS-3793 (the read seam, whose `body` field is defined by § 2.4's export).

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

**`item.body` is the bullet body with its first line removed.**

This half is well-defined and needs no parsing at all. `body` is seeded from
`head` and every continuation is appended after a `'\n'`, so the head line is
exactly the text before the first `'\n'` and the residual is exactly the text
after it — **empty** for a bullet with no continuation.

ANTS-3757 § 2.1.1's row for `body` is amended from "the reader's `body`" to say
so (§ 7).

### 2.2 Asking the grammar: `trailerValuesIn()`

The six matchers (`rxKind`, `rxLanes`, `rxLayman`, `rxEvidence`, `rxSource`, and
`rxTrailerKey`, which bounds a value at a following key on the ten two-key
lines) are today `static const` **inside `parseBullets()`** and reachable from
nowhere. They are **shared, not moved** — `parseBullets()` needs their captures,
and a boolean predicate could not serve it — by hoisting them to file scope
behind one exported accessor:

```cpp
// src/roadmapparse.h — the matchers, asked rather than duplicated.
//
// Per key: the value AS parseBullets() ASSIGNS IT (§ 2.2.1), plus where the
// match sat and whether it was line-anchored. The provenance is not decoration:
// ANTS-3809's `body_shadowed` refusal has to name the shadowing sentence, and a
// bare value cannot answer "was this an un-anchored mid-prose match?".
struct TrailerMatch {
    QString value;            // empty when the key is absent
    int     offset   = -1;    // byte offset into `body`; -1 when absent
    bool    anchored = false; // matched at a line start (^ with MultilineOption)
};
struct TrailerValues {
    TrailerMatch layman, kind, source;
    TrailerMatch lanes, evidence;     // `value` is the joined, pre-split text
    QStringList  lanesList, evidenceList;   // the split forms parseBullets() assigns
};
TrailerValues trailerValuesIn(const QString &body);
```

`offset` and `anchored` are what an earlier single-value draft could not
express, and their absence made ANTS-3809's refusal unimplementable through this
struct. Both are free — `QRegularExpressionMatch` already carries the offset,
and anchoring is a property of the pattern that matched.

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

| Key | Normalisation the accessor must reproduce |
|---|---|
| `kind`, `layman` | `captured(1).trimmed()` |
| `lanes` | split on `,` (`SkipEmptyParts`), each part trimmed, empties dropped |
| `evidence` | trim; drop **one** trailing `.` unless the value ends `..`; then split/trim as `lanes` |
| `source` | truncate at the first following `rxTrailerKey` match, trim, drop one trailing `.`, trim again |

INV-4 asserts this equality directly, so a divergence fails a test rather than
silently disabling the feature.

### 2.3 Suppression on value equality

**`renderBullet()` emits a trailer key from its column only when the body would
not already re-parse to that same value**, asked through § 2.2's accessor.

**Value equality and not mere presence, because presence alone breaks ANTS-3758's
INV-12.** That invariant requires every emitted bullet to *literally carry*
`Kind:`, and its test asserts against the rendered text. Suppressing on presence
would satisfy it for an ordinary bullet and break it for the corner below — a
body mentioning `Kind:` in prose has the key *present* and the wrong *value*, so
the render would drop the real line and emit nothing. Comparing values instead
means a mismatch always emits from the column, which is canonical. INV-12
therefore continues to hold as written: the required piece is in the rendered
text either way, exactly once.

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

**For a migrated item the mismatch cannot arise at all**: the store's column was
populated by this same reader from this same body, so `trailerValuesIn(body)`
equals the column by construction, suppression fires, and nothing is duplicated.
The mismatch appears only once a consumer has written a column without rewriting
the body — a `flip` or `annotate` under ANTS-3809, which is why that spec owns
the write-side refusal and this one owns the accessor it is built on.

**Fixing the grammar is out of scope** (§ 5): anchoring `rxSource` / `rxLanes`
would discard the 157 inline values they were un-anchored for.

### 2.4 The render's one export

`renderBullet(const RoadmapStore::ItemWrite &)` is a **free function in an
anonymous namespace** in `src/roadmaprender.cpp`. There is no
`RoadmapRender::renderBullet()`; earlier drafts cited one.

That is fine for § 2.3, which is a TU-local edit adding no header surface. It is
**not** fine for two other callers, and both are real:

- **The tests** for INV-1 and INV-3 below cannot call it, so they drive the
  render's public entry point over a store they populated.
- **ANTS-3793's `bulletsFromStore()`** defines its `BulletRecord::body` as the
  rendered bullet's text, and cannot reach a file-local function.

So this spec exports exactly one function:

```cpp
// src/roadmaprender.h — the bullet's markdown, byte-identical to what the
// file writer emits for this item. One export, because two callers outside
// this TU need the same bytes and the alternative is a second renderer.
namespace RoadmapRender {
    QString bulletText(const RoadmapStore::ItemWrite &it);
}
```

`renderBullet()` becomes its body; the anonymous-namespace helper goes away
rather than being wrapped, so there is one definition and not two.

**This costs no link edge.** `roadmaprender.cpp` is already in
`ants_roadmapstore_lib` (`CMakeLists.txt`, tagged `# ANTS-3758`), which is the
same library ANTS-3793's `roadmapsource.cpp` lands in. § 4 carries the arithmetic.

## 3. Invariants

- **INV-1** — **A migrated project's rendered bullet contains its headline
  exactly once and each trailer key it carries exactly once**, `Kind:` included.
  *Rationale, not part of the assertion:* "exactly once" is a two-sided bound and
  the lower side is ANTS-3758's INV-12, which is why § 2.3 suppresses on value
  equality rather than on presence — presence-based suppression can reach zero.
  *Breaks when:* the migration stores the head line in `item.body` (today's
  defect), or the render emits a column-sourced trailer line whose value the body
  already carries. *Test:* `Inv1NoDuplication`, over this directory's own bullet
  fixture, asserting exactly one occurrence of the headline and exactly one of
  each trailer key in the rendered text.
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

  **Four sites exist today, in three files, and each is dispositioned rather than
  the invariant being widened** (verified 2026-08-03):

  | Site | Disposition |
  |---|---|
  | `src/roadmapparse.cpp` | the grammar itself — **exempt** by definition |
  | `src/remotecontrol.cpp:6576` and `:6749` (`rxBoldLayman`, ANTS-1933) | **exempt, and it cannot be otherwise.** Both deliberately capture the Layman sentence *including* its trailing period, because `rec.layman` is period-stripped by ANTS-1154 INV-4 and a period-less CHANGELOG body was the bug ANTS-1933 fixed. `trailerValuesIn()` returns the stripped value, so routing these through it re-introduces that defect. Two sites: the single-entry and batch `add_from_roadmap` paths carry the same block |
  | `src/roadmapdialog.cpp:640` (`rxKind`) | **moves to `trailerValuesIn(bodyFull).kind.value`** — a deliverable of this spec. It re-implements both the trailer regex *and* `parseBullets()`'s continuation-line assembly to build a kind-filter map, which is the second grammar this invariant exists to forbid. Its `s_lastInput` / `s_lastKindMap` memo is unaffected |
  | `src/remotecontrol.cpp:22382` (`rxCommitSha()`) | **exempt.** Its pattern embeds `\bSource:\s*` as one alternative in a commit-SHA locator — the trailer key is a lead-in it skips past, not a value it extracts, so routing it through the accessor is meaningless. Listed because the scrape **will** match it |

  Any site outside that table is a failure.
- **INV-3** — **The render and the reader agree about every trailer key.**
  Re-parsing a rendered bullet yields the same `kind` / `source` / `lanes` /
  `layman` / `evidence` the store holds, **for every item this spec can produce**
  — that is, every migrated item, where § 2.3.1 shows equality holds by
  construction. *Breaks when:* the render suppresses a key whose stored value
  differs from what the body re-parses to, or emits one the reader then reads
  twice. *Test:* `Inv3RenderReaderAgree`, two fixtures — a migrated bullet
  (values equal by construction, suppression fires) and a post-cutover bullet
  with a residual body (all keys emitted from columns).

  **The scope clause is load-bearing.** A column written *without* rewriting the
  body can shadow the canonical line on a re-parse (§ 2.3.1), and only a consumer
  write can create that state. ANTS-3809 owns making it refuse, and carries the
  fixture for it. Asserting the unscoped form here would ship this invariant red
  against a state this spec cannot reach.
- **INV-4** — **`trailerValuesIn(body)` equals what `parseBullets()` assigns from
  the same body**, over all five keys and § 2.2.1's normalisation table. Without
  this equality the suppression compares incommensurable values, never fires, and
  the defect stays live behind a passing spec. *Breaks when:* the accessor
  returns raw captures, skips the `rxTrailerKey` truncation or a trailing-period
  chop, or splits `lanes` / `evidence` differently. *Test:* `Inv4AccessorAgrees`,
  over a fixture table covering each normalisation step including the two-key
  line and the backticked-key guard.

## 4. RAM / build cost

**RAM.** Unchanged and not merely negligible: § 2.1 makes `item.body` **smaller**
by one line per bullet, and § 2.2's accessor allocates one `TrailerValues` per
call — five short strings plus two `QStringList`s, on the stack, freed at the end
of the render's per-bullet loop. Nothing is retained.

**Build.** One exported accessor is added to `roadmapparse.cpp`, already in
`ants_core_lib`, adding no edge. § 2.4's export is added to
`src/roadmaprender.h`, and `roadmaprender.cpp` is **already** in
`ants_roadmapstore_lib` (`CMakeLists.txt`, `# ANTS-3758`) — the same library
ANTS-3793's consumer lands in — so that export adds no edge either. Per this
project's cap, builds run under `cmake --build build` with the `JOB_POOLS` limit
and tests at `ctest -j4`.

## 5. Out of scope

- **Anchoring `rxSource` / `rxLanes`** to close § 2.3.1's corner. It would
  discard the 157 inline `Source:` values ANTS-3764 measured and un-anchored for
  (extending ANTS-2058's finding for `Lanes:`) — a format decision needing its
  own id, **owed and not yet filed**.
- **The write-side refusal** that makes the corner fail loudly rather than
  silently — ANTS-3809, built on § 2.2's `offset` / `anchored`.
- **The reader seam** that consumes § 2.4's export — ANTS-3793.
- **Deleting the markdown splice paths** — they serve every unmigrated project.

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
  § 2.1 changes what is stored, so the row is amended on ship to say the head
  line is dropped — otherwise the migration's own spec describes the defect.
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
