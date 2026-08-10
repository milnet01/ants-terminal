# ANTS-4065 — define the markdown→store import mapping, so importing neither loses nor invents a field

**Status:** amended 2026-08-10, re-gate pending — accepted 2026-08-08 after a
rule-14 gate run to its 3-loop cap, 2 cold lanes per loop, 66 findings verified
and 63 fixed; 3 filed as non-build-changing in the loop-3 row. The amendment
rewrites § 2.2's match-precedence rule and INV-11 (ANTS-4086, a defect this
spec's own rule caused in five live bullets) and corrects INV-8's test clause
(ANTS-4076). Both are authoring edits, so rule 14's gate re-arms and the loop
log below carries the run. Build order at
[`docs/plans/ANTS-4065-import-mapping-contract.md`](../plans/ANTS-4065-import-mapping-contract.md).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-4065 (user-request-2026-08-08, after the first real
migration was found to rewrite 123 items' `Kind` and to render a `Source:` line
for all 383 items whose source column it had defaulted).
**Blocked by:** nothing — this is the gate ANTS-3853's remaining migrations wait on.
**Composes with:** ANTS-4063 (the render's fabricated `Source:`, a symptom this
contract's § 2.4 rules out), ANTS-4062 (the corpus's off-taxonomy `Kind:` values,
whose mapping § 2.1 fixes).

## 1. Problem

A roadmap is imported by `RoadmapMigrate::makeItem()` (`src/roadmapmigrate.cpp`),
which reads fields the parser extracted in `src/roadmapparse.cpp` and writes
store rows. Once a project is migrated the direction reverses: `roadmaprender.cpp`'s
`render()` becomes the only writer of the markdown, so the import's decisions
become permanent and the file stops being able to correct them.

The first real migration (this project, 2026-08-08) shows the import is not yet
safe to trust with that authority. Three distinct failures — not ranked, because
the third is the acceptance test for the other two.

1. **It loses a declared field silently.** `rxKind()` (`src/roadmapparse.cpp`)
   is anchored `^\s*Kind:` under `MultilineOption`, so it matches only a
   line-leading label. A bullet writing its trailer inline — `… not in this
   fold. Kind: doc-fix.` — is not matched, `rec.kind` arrives empty, and
   `makeItem()`'s first branch assigns `implement` with
   `provenance.kind = "defaulted"` and **emits no note**. The `kind_unmapped`
   note fires only on the *fourth* branch, for an unrecognised **value**; an
   unparseable **field** is silent by construction.

   **This class was already diagnosed once and the fix stopped one pattern
   short.** ANTS-2058 un-anchored `rxLanes()` for exactly this reason, and its
   comment records the reprieve verbatim: the old anchor "matched only
   line-leading `Lanes:` … while rxKind worked purely because `Kind:` happened
   to sit first." The items where it does not sit first are the ones that lose
   their field.

2. **It invents a field.** `makeItem()` assigns `source = "planned"` when the
   bullet declares none, marking `provenance.source = "defaulted"`.
   `bulletText()` (`src/roadmaprender.cpp`) then emits it under
   `if (!it.source.isEmpty() && !shadows(tv.source, it.source))` — two
   conditions, **neither of which a default fails**: a defaulted value is never
   empty, and ANTS-3808's `shadows()` suppresses only a trailer the *body*
   already repeats. So a default that existed only as an absence renders as an
   assertion, and the next import reads it back as one — the loss is
   self-amplifying. `roadmap-format.md` § 3.5.3 is explicit that this is
   backwards: `planned` is "(default; usually omitted)".

3. **It does not round-trip.** Importing the file `render()` had just written
   reports **714 items updated** and ~200 `field_conflict` notes on `headline`,
   `layman`, `lanes` and `extras`. `store → render → parse` is not identity, so
   "the file is regenerated on every release" currently means "the file drifts
   on every release" — and that assumption is what the whole cutover rests on.

**The store already knows which fields it invented, which is why this is
fixable rather than archaeological.** `provenance` is written per field at
import. Measured on the live store
(`sqlite3 roadmap.sqlite "SELECT json_extract(provenance,'$.kind'), COUNT(*)
FROM item GROUP BY 1"`):

| Field | asserted | defaulted |
|---|---|---|
| `kind` | 1,425 | **476** |
| `source` | 1,518 | **383** |

Of the 476 defaulted kinds, **438 carry no `extras.source_kind`** — the branch
that preserves the original value never ran, so the store cannot say whether the
bullet was silent or merely unparsed. Diffing against the pre-render file
(`git show 6d9e743d:ROADMAP.md`) resolves it for this project: **at least 48 of
those 438 declared a valid taxonomy value** — `fix` ×21, `security` ×6,
`doc-fix` ×6, `perf` ×3 and others — every one of them written inline.

**Layman:** Importing the roadmap into the database quietly changed some
entries' type and made up a "where this came from" line for hundreds of others,
and re-importing the file the database itself wrote would change 714 entries
again. This writes down exactly how each field must convert, so importing stops
changing the data.

## 2. Surface

### 2.1 The value maps

Every enumerated field gets one table, and the tables are the contract. Corpus
figures throughout are from `tools/roadmap-corpus-survey.py` over all 14
projects (4,378 items, re-measured 2026-08-08 after Phase B2).

**`kind` — the mapping already exists and is normative; this spec extends it,
it does not restate it.** `roadmap-data-model.md` § 7.4 carries the table
("the migration-scoped mapping is normative") and `mappedKind()`
(`src/roadmapmigrate.cpp`) implements exactly its **eleven** entries, applied as
written. They are deliberately not reproduced here: a second copy would be free
to diverge from the first, which is the failure this whole spec exists to stop.

**§ 7.4's table is incomplete, and the reason it looks complete is a measurement
artefact this spec has to correct.** Its "11 others" was derived from
`tools/roadmap-corpus-survey.py`, **whose `KIND_VALUE` pattern was anchored
`^\s+…$` — the same blind spot as `rxKind()`, in the same shape, for the same
reason.** The instrument could not see an inline trailer either, so every value
written inline was invisible to the evidence base § 7.4 rests on.

An earlier draft of this spec booked the gap against render contamination
instead — the survey having been run after the first store render, which had
rewritten `Kind: bug` to `Kind: implement` in the file it read. **That
explanation was tested during Phase A and does not hold:** re-running the survey
against the reverted pre-render source still reported no `bug` at all. Only
un-anchoring the survey's own matcher surfaced it. The render damage was real
and is reverted; it was not what hid these values.

With the survey corrected (un-anchored, backtick-guarded, case-sensitive,
last-match — the guards § 2.2 gave the parser **as of Phase B1**), the inventory
surfaces seven values the table and `mappedKind()` both miss. All seven occur in
this project only; measured over `ROADMAP.md` plus both archives at the reverted
source:

| Corpus value | Count | → | Rationale |
|---|---|---|---|
| `bug` | 29 | `fix` | the single largest unmapped value in the corpus, and invisible to the anchored survey |
| `performance` | 2 | `perf` | long form of a canonical value |
| `process + tooling` | 1 | `chore` | § 3.5.3's "housekeeping"; `tooling` already maps there |
| `audit` | 1 | `audit-fix` | the canonical name for the same work |
| `feature/fix` | 2 | **ruling needed** | compound; the column is single-valued |
| `design + implement` | 1 | **ruling needed** | compound |
| `design + fix` | 1 | **ruling needed** | compound |

The four with a rationale are mechanical and this spec adopts them. **The three
compounds are deferred, with stated interim behaviour** rather than left blank:
until a ruling exists they fall through `makeItem()`'s unmapped branch —
`implement`, `extras.source_kind` preserved, `kind_unmapped` note emitted — which
is today's behaviour and is visible rather than silent. § 5 carries the ruling.

**A mapped value keeps its original at import — and loses it at the first
regeneration.** `makeItem()` writes `extras.source_kind` on the mapping branch,
and this contract makes that mandatory for every non-identity mapping. But
`bulletText()` renders **no `extras` at all** (verified: no `extras` reference in
`src/roadmaprender.cpp`), so on a migrated project the next render→import cycle
drops it: the file says `Kind: fix.`, the re-import takes the canonical branch,
and `bugfix` is gone.

**So the reversibility this contract can honestly promise is one-shot, not
durable**, and § 2.6 says which of the two it governs. Making it durable means
either rendering the original back into the file — putting a machine artefact
into a human-facing document — or governing `extras.source_kind` under INV-6.
**This spec takes neither and says so**: the mapping's audit trail is the import
note (§ 2.3), which is emitted at the run that did the mapping and is where a
reader looks for it. Claiming a durable `extras` guarantee the render cannot
keep would be worse than claiming none.

**`status` — the store admits five, the markdown legend documents four.**
`planned`, `in-progress`, `shipped`, `considered` carry emoji (📋 🚧 ✅ 💭);
`dropped` has none. Until § 2.6's round-trip holds, **`dropped` must not be
written by import**, because a row the render cannot express is a row that
cannot survive a regeneration.

**`status`, pass-headings dialect — the larger job.** Those roadmaps carry 164
`- **Status**:` lines of which **142 hold a value outside the five-status enum**.
They are out of scope here and tracked separately (§ 5): this contract governs
the `ants-v1` dialect, and folding a second vocabulary in would double the
document before the first one is proven.

**The remaining enums**, from the store's own `CHECK` constraints
(`sqlite3 roadmap.sqlite "SELECT sql FROM sqlite_master WHERE name='item'"`):
`id_origin` ∈ {parsed, synthesised, quarantined}; `visibility` ∈ {public,
internal}; `priority` is `INTEGER 1..5 OR NULL`; the three date columns are
`GLOB 'YYYY-MM-DD'`; `element.kind` ∈ {item, narration, table}.

**`priority` is governed by `roadmap-data-model.md` § 7.5, which this spec
points at rather than restating.** The corpus declares `Priority:` 88 times and
**86 of those are already integers 1–5** (re-measured 2026-08-08); the only two
prose values are `medium` and `LOW`. Import parses an integer straight through,
maps a severity word by § 7.5's `CRITICAL → 1, HIGH → 2, MEDIUM → 3, LOW → 4`,
and **leaves the column NULL where the field is absent** — § 3.3's rule for a
field with no source-side counterpart, and the only safe behaviour when 4,290 of
4,378 corpus items declare none: defaulting them would invent 4,290 values and
label a standing top-priority item as least urgent. The source string is
preserved in `extras` either way.

**An earlier draft of this spec deferred that scale to § 5** on the grounds that
"a severity vocabulary this project has never written down — no doc defines the
set, and the direction (is `CRITICAL` 1 or 5?) is a convention, not a
derivation". **Every clause of that was false when written**: § 7.5 is normative
and states the range, the direction, the four-word mapping, band 5's reservation
for someday-maybe work, and `INFO` having no band. The deferral is deleted, and
the sentence is kept here because this is the *second* place the same defect
appeared — the `Kind` table was the first (above), caught by loop 1 of this
spec's own cold-eyes gate, while the identical defect in this paragraph survived
all three loops. *Does this document re-open something a standard already
settles?* is the check that would have caught both, and ANTS-4067 carries it.

### 2.2 Un-anchor `rxKind()`, with the guard its sibling already has

`rxKind()` drops the `^\s*` anchor, exactly as `rxLanes()` did under ANTS-2058,
and gains ANTS-3722's negative lookbehind so a bullet *quoting* the label is not
read as declaring it:

```cpp
// was: "^\\s*Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]"
//      MultilineOption | CaseInsensitiveOption
QStringLiteral("(?<!`)(?<!`\\*)(?<!`\\*\\*)"
               "(?:\\*\\*)?Kind:(?:\\*\\*)?\\s*([^\\.\\n]+?)\\s*[\\.\\n]")
//      MultilineOption      — retained for parity with rxLanes(); inert
//                              once ^ is gone (the \n stop comes from the class)
//      CaseInsensitiveOption — DROPPED, see below
```

**Amended 2026-08-09 (ANTS-4077), after implementation measured the corpus.**
The pattern above carried a single `(?<!`)` and no `(?:\*\*)?`, which this spec
stated literally; both were wrong, and the second made the first insufficient.

- **The label may be written BOLD**, exactly as `rxLayman()` has accepted since
  ANTS-1861 and `rxSource()` since ANTS-3764. 29 corpus lines write
  `**Kind:**` and 3 write `**Lanes:**`; every one of them had always parsed as
  declaring nothing, and un-anchoring alone made that worse rather than better —
  the match landed inside the label and captured the closing `**` plus the prose
  after it into `extras.source_kind`. The contract is **parity with the plain
  spelling**, not a new rule: a qualifier-bearing value still runs to the first
  period in both, so `refactor (no behaviour change)` remains one unrecognised
  value rather than gaining a stripping rule no other label has.
- **The backtick guard needs three fixed-length lookbehinds, not one.** PCRE2
  requires each to be fixed-length, and a single `(?<!`)` cannot see past the
  optional `**` — so `` `**Kind:**` ``, which the bullets *documenting this
  format* write constantly (4 corpus lines, plus 2 for `Lanes:` and 1 for
  `Source:`), matched at the label and was read as a declaration. INV-3's guard
  case is extended to cover the bold form for this reason.

`rxLanes()` and `rxSource()` take the same two corrections, because the keys are
written side by side on one line and only one of them parsing is the defect this
fixes, not a fix for it. `rxSource()` already had the optional pair; it gained
only the widened guard. `rxEvidence()` is untouched — it is still anchored, and
the corpus writes no bold form of it.

**`CaseInsensitiveOption` is dropped with the anchor, and that is a deliberate
reversal of ANTS-3407 for this one label.** `rxLanes()` records the reason: an
un-anchored case-insensitive label matches prose — "…changed the kind: of work
we do…" would parse as a declaration. ANTS-3407 added the tolerance so a
hand-edited `kind:` / `KIND:` still parses, and dropping it means those stop
parsing.

That trade is acceptable **only** because § 1's own premise makes hand-editing
transitional: once a project is migrated the render is the sole writer, so the
only files carrying a hand-typed label are pre-migration ones. It is not free —
INV-9 pins the chosen behaviour so the reversal is tested rather than assumed,
and § 7 records it against the standard.

**`rxKind()` is shared with the render, so this is not a parse-only change.**
`bulletText()` reads the same matchers through `trailerValuesIn(it.body)` to
compute ANTS-3808's `shadows()` suppression, which is **value equality**, not
presence — `m.offset >= 0 && m.value == v`, and its own comment says so. So
un-anchoring does *not* make a body that merely discusses the label shadow the
trailer; it changes behaviour only where a mid-prose `Kind:` carries a value
**equal to** the column's, which previously went unseen and now suppresses the
trailer line. INV-10 covers exactly that case.

**Match precedence becomes load-bearing the moment the anchor goes, and it must
be stated.** `bulletText()` appends `it.body` *before* the trailer lines, so in
a rendered bullet a mid-prose trailer key sits ahead of the canonical one, and
an unspecified precedence lets a re-import adopt the stale prose value — the
same read-back hazard ANTS-3808 names, arriving from the parse side and
breaking INV-6 on a governed column.

**Current state, corrected 2026-08-10.** An earlier draft of this paragraph
said `trailerValuesIn()` resolves `kind` through `matchIn()`, taking the first
occurrence. It does not, and the distinction decides *which function is
edited*: `trailerValuesIn()` calls **`matchLastIn()` for `kind` only**, which
takes the last match unconditionally, while **`layman`, `lanes`, `evidence` and
`source` all use `matchIn()`**, which takes the first. So the two halves of the
corpus fail in opposite directions, and an implementer who edits `matchIn()` to
add precedence changes four fields and not the one this section is about.

**The trailer wins, and a LINE-INITIAL match is what "the trailer" means.**
A trailer key at the start of a line in the bullet's collected body, in either
spelling, always beats one appearing mid-sentence. Among line-initial matches
the parser takes the **last**; the mid-line fallback is consulted **only** when
a bullet has no line-initial match at all, and there too takes the last.
INV-11 pins both halves.

**The rule governs all five trailer keys, not `Kind:` alone.** `kind`,
`source`, `layman`, `lanes` and `evidence` are all in § 2.6's governed set, so
INV-6 cannot pass while any of them resolves by position alone. Scoping the
rule to `Kind:` would fix the field whose defect was noticed and leave the
identical defect live on four others — and on the first-match keys it is
**worse**, because first-match is beaten by *any* earlier mention rather than
only a later one.

This is not hypothetical. **ANTS-3808 in this project's own `ROADMAP.md` —
the file Phase D imports first — imports three wrong values today.** Its body
embeds an illustrative sample bullet whose `Layman:` / `Kind:` / `Source:`
lines sit *above* its own trailer; `collectBulletBody()` trims them, so they
are line-initial in the body and first-match takes `source = "test."` and
`layman = "An older thing."` from the illustration. Its `kind` survives only by
accident of ordering. And it declares **no** `Lanes:` at all yet imports
`lanes = ["packaging"]`, harvested out of a backticked example that spans a
line break — a separate mechanism this rule does not address, since the
backtick guard is a fixed-length lookbehind and cannot see a span crossing
lines. That third defect is **out of scope here** and filed separately; the
rule below fixes the first two.

**"Line-initial" needs no indentation rule, and the predicate already exists.**
`collectBulletBody()` builds `body` by appending `'\n'` followed by
`cont.trimmed()` for each continuation line, so the body **preserves line breaks
and carries no leading whitespace** — a source line `   Kind: test.` reaches the
matcher as `Kind: test.` at a line start. `TrailerMatch::anchored`, computed
identically in `matchIn()` and `matchLastIn()` as `(at == 0) || body.at(at - 1)
== '\n'` over `capturedStart(0)`, is therefore exactly this test and is the
predicate to use. `matchLastIn()` already computes it on every global match and
simply never consults it: today it overwrites its result each iteration and
returns the last match unconditionally. **The change is which match it keeps,
not a new predicate.**

**Implement it once, in one shared resolver, and route all five keys through
it.** `matchLastIn()` gains the preference — keep the last match whose
`anchored` is true, else the last match seen — and `trailerValuesIn()` moves
`layman`, `lanes`, `evidence` and `source` onto it from `matchIn()`. Adding
precedence inside `matchIn()` instead would be wrong twice over: it is the
first-match helper other callers rely on, and it would leave `kind`, which does
not route through it, unchanged. `matchIn()` keeps its current behaviour and
its current callers outside `trailerValuesIn()`.

Two consequences to carry, neither optional. `rxSource()` captures to
end-of-line (`([^\n]+)`) rather than to the first period, so a first-match hit
on a prose mention swallows the rest of that line — which is why `source` is
the field where this defect is most visible. And **`rxEvidence()` is still
anchored** (below), so for that one key every match is line-initial by
construction and the rule is inert until the anchor comes off; it is included
here so the resolver is uniform and stays correct when it does.

**Amended 2026-08-10 (ANTS-4086), after the rule was measured against the
authored file rather than the rendered one.** This section previously said the
parser takes the last match *anywhere*, reasoning from `bulletText()`, which
appends `it.body` before the trailer lines — true of a **rendered** bullet,
where the trailer is therefore always last. It is false of the authored
`ROADMAP.md`, because `roadmap_log op:annotate` appends notes to the **end** of
a body, below the trailer. Any later note mentioning the label in running text
becomes the last match and displaces the real declaration, so this section's
own closing claim — "a real trailer, when present, always wins" — did not hold
on the file Phase D imports first. Five bullets carry a sentence fragment in
`kind` today: ANTS-1278, ANTS-3608, ANTS-3755, ANTS-3808 and ANTS-3810.
ANTS-3810 is the clean case — its `Kind: test.` is line-initial and the prose
`Kind:` eight lines below it wins.

Line-initial is the discriminator because it partitions the corpus cleanly, and
because **the fallback is what keeps § 1's original defect fixed**: 52 bullets
declare `Kind:` only mid-line, so a rule that simply restored the `^\s*` anchor
would lose every one of them again.

| Bullets with … | Count | Under this rule |
|---|---|---|
| line-initial `Kind:` only | 1,414 | unchanged |
| mid-line only | 52 | the fallback serves them |
| both | 42 | the line-initial one wins |
| no `Kind:` | 187 | defaults, per INV-1 |

**Keying on a canonical `Kind:`→`Source:` pair was considered and is not
available.** It would be the cleanest discriminator, but **594 bullets carry a
line-initial `Kind:` with no adjacent `Source:`** and would all lose their
declaration; only 862 have the pair at all.

**A nested sub-entry's trailer is NOT resolved by this rule, and is fixed in
the source instead.** Roughly 20 bullets embed a nested bullet *list* in their
body — each sub-entry written as a full roadmap bullet with its own
line-initial `**Source:**` / `**Kind:**` block — and no positional rule
separates a parent's trailer from a child's. Taking the first breaks the
rendered shape; taking the last makes ANTS-3573 report `fix` where its own
trailer says `test`, and makes ANTS-3780 — which has **no trailer of its own at
all** — invent `enhancement` from a child's. That invention is what § 1 exists
to prevent, so the rule must not be the thing that resolves it.

The source is corrected instead, and **the correction differs by which of two
shapes the body carries.** Both put a second line-initial trailer in a parent's
body; they do not take the same repair.

- **A nested live sub-entry** — a real bullet indented under its parent, as in
  ANTS-3780's 20. Its labels are backticked, so they read as quotations rather
  than declarations, which is what INV-3's guard already pins. The precedent is
  in the data: those sub-entries' ids were already deliberately mangled
  (`[ANTS-116&]`) so the reader would not take them as items, and only their
  labels were missed. Afterwards ANTS-3780 correctly carries no kind and
  defaults with a note.
- **An indented sample of a bullet** — an illustration of the *format*, present
  to be read, as in ANTS-3808's `DEMO-0003` block. **Backticking is the wrong
  repair here**: the block is indented as a sample, so the backticks would
  render literally and corrupt the thing it exists to show. Reword the sample's
  labels instead so they are no longer the reserved keys (`Layman:` →
  `Layman(sample):`, and likewise for the others), which keeps the illustration
  legible and removes the declaration. Where a sample must show the exact
  bytes, move it into a fenced block **and** accept that the parser does not
  read fences — the fence is for the human reader, the relabelling is what the
  parser needs.

Fencing alone is **not** a remedy: `roadmapparse.cpp` tracks backticks but not
fences, and the affected blocks carry none. Teaching the parser about nesting,
or about fenced regions, is the principled fix for both shapes; it needs its
own id and is out of scope here (§ 5), because it changes how bullets are
recognised.

**A capitalised `Kind:` in prose still matches, and dropping the case option
does not save it.** Case-sensitivity removes the lowercase prose match only;
`… we changed the Kind: of work …` at a sentence start, or a bullet *about* the
format that forgets its backticks, still parses. INV-3 covers the backticked
form and ANTS-3722's guard handles it; this residue is accepted rather than
fixed, because narrowing further (requiring a line start or a sentence boundary)
would re-introduce the anchor this section removes. The line-initial precedence
above is what limits the damage, and it limits it precisely: a real trailer,
when present, always wins, because a prose `Kind:` is mid-line by construction —
a sentence that begins with the label at column 0 would be a declaration by any
reading. The residue is a bullet that has **no** line-initial declaration and a
capitalised prose mention, which falls through to the mid-line branch; INV-1's
note is what makes that visible rather than silent.

### 2.3 A defaulted field is always noted

`makeItem()`'s empty-`rawKind` branch gains the note its unmapped-value sibling
already emits. The rule generalises to every field the import may default:

> **Import may default a field. It may not default one silently.**

Each default emits **`field_defaulted`**, whose detail names the field and the
bullet's line, so a migration report shows what the import supplied rather than
what the document said. The run-level tally is **`defaulted_fields`** in the
response envelope, beside `notes_count`. This
is the check that would have surfaced the defaulted-kind population on the
first run — all **476**, not merely the 438 that preserve no original, since the
note fires on the default itself rather than on whether anything survived it.
**The per-field COUNT is what must be complete, not the notes array**: § 4's
`notes_truncated` cap means the list is a sample, so the migration reports a
defaulted-field tally alongside it. A truncated list that silently under-reports
the count would reproduce this spec's own failure mode one level up.

### 2.4 A defaulted `source` is not rendered

**Scoped to the two fields import can default — `kind` and `source` — and to
those only.** `makeItem()` writes `provenance` for `id`, `kind` and `source` and
for nothing else, so a rule phrased over "any field whose provenance is not
`asserted`" would suppress `Layman:`, `Lanes:` and `Evidence:` on *every*
bullet, none of which ever carries a provenance entry. That would destroy three
fields `roadmap-format.md` § 3.5 defines — a larger loss than the one this spec
exists to stop. **Absent provenance means "not a defaultable field", never
"defaulted".**

For `source`: `bulletText()`'s existing condition gains one term, becoming
`!it.source.isEmpty() && !shadows(...) && assertedSource`, where

> **`assertedSource` is `provenance.source != "defaulted"`** — *not*
> `== "asserted"`.

The direction matters and getting it backwards re-creates the loss this spec
exists to stop. `provenance` is `NOT NULL DEFAULT '{}'`, so a row written by
anything other than `makeItem()` — `roadmap_log op:append`, for one — carries no
`source` key at all. Tested for equality with `"asserted"`, every such row would
lose its `Source:` line. Absent provenance therefore renders. This is
**additional to** ANTS-3808's `shadows()` suppression, not a replacement.

**`Kind:` is required by § 3.5, so it always renders — and that is what makes
INV-6 unreachable unless provenance is excluded from the governed set.** A
`provenance.kind = 'defaulted'` item renders `Kind: implement.`; re-importing
that line matches the canonical branch and writes `provenance.kind = 'asserted'`.
Provenance flips on all 476 defaulted-kind rows at the first round trip, INV-1's
note stops firing for exactly the population it exists to make visible, and
`items_updated == 0` cannot hold.

Two ways out, and this spec takes the second:

- Render a defaulted `Kind:` with a marker the parser reads back as defaulted.
  Rejected: it puts a machine artefact into a file § 3.5 governs for humans.
- **Exclude `provenance` from INV-6's governed set** (§ 2.6 lists it among the
  exclusions), and
  accept that a defaulted kind becomes asserted once rendered. The audit trail is § 2.3's
  note, emitted at the import that *did* the defaulting, which is the run where
  it matters. **`extras.source_kind` is not that discriminator** — § 1 measures
  438 of 476 defaulted kinds without it, so 38 defaulted kinds *do* carry one
  (`makeItem()`'s unmapped-value branch writes it alongside
  `provenance.kind = "defaulted"`). Present-with-defaulted means an unrecognised
  value; absent-with-defaulted means no `Kind:` was parsed at all.

### 2.5 A field naming a file is validated at import

`Source:` names a path far more often than the standard anticipates: **93
distinct path tokens across 150 occurrences** in this project's pre-render
roadmap and archives, applying the predicate below
(`*_Ants_MCP_Feedback.md`, `remotecontrol.cpp`, `terminalwidget.h`,
`rpmlint.log`). Meanwhile `Evidence:` — the field `roadmap-format.md` § 3.5
designates for paths — is used **22 times** across all 4,378 corpus items, in
three projects. (An earlier draft said "once"; that came from a line-anchored
count, which sees only the 4 own-line declarations and none of the inline
trailers — the same blind spot § 2.2 fixes for `Kind:`, `Lanes:` and `Source:`.
**It is deliberately NOT fixed for `Evidence:`**: § 2.2 leaves `rxEvidence()`
anchored, so those 18 inline declarations stay invisible to the import. That is
a known, accepted loss of a governed field and it is recorded in § 5, not a
thing this section's measurement implies is repaired.) The convention the
standard describes and the one the corpus practises are still not the same
convention.

**"Looks like a path" is a predicate, not a judgement**, because § 3.5.3's own
`Source:` vocabulary is full of hyphenated tokens (`upstream-<dep>`,
`external-CVE-NNNN-NNNN`) that must not be mistaken for filenames. A `Source:`
value is a path reference when:

> **Split the value on whitespace. A TOKEN is a path reference when it
> contains `/` OR matches `\.[A-Za-z0-9]{1,5}$`. The value is a path reference
> when any of its tokens is.**

**The unit is the whitespace-delimited token, not the value** — corrected
2026-08-10, having twice been left undefined. A `Source:` value is frequently a
sentence (`in-session-2026-05-16`, but also `ROADMAP.md ANTS-4065` and
`rpmlint.log warnings`), so "its final segment" had no referent whenever the
value contained no `/`: it could mean the whole value, the last whitespace
token, or the last token before a parenthetical, and the three classify
different values. Tokenising first removes the question, and it is also what
makes the multi-path case work without a second rule.

`user-2026-08-08` has no slash and no extension in any token;
`rpmlint.log warnings` has one qualifying token; and
`docs/specs/ANTS-3863-pre-read-dispatch.md` qualifies on both counts.

**Apply the predicate AFTER `trailerValuesIn()`'s trailing-period chop, not
before.** The regex anchors on `$`, so a token ending in a sentence period —
`rpmlint.log.` — does not match, and a value tested before the chop is
classified as prose. `trailerValuesIn()` already drops one trailing period from
`source` (and from `evidence`, keeping `..`), so the ordering exists; it simply
has to be relied on rather than re-implemented. Verified by running the
predicate over the corpus's real `Source:` shapes: every § 3.5.3 form is
correctly rejected, `ROADMAP.md ANTS-4065` and `rpmlint.log warnings` are
correctly accepted, and `in-session-2026-08-03, found verifying ANTS-3806.` is
correctly rejected.

**The "not a recognised source form" conjunct is deleted, not relocated.** It
read as a predicate and was a judgement: § 3.5.3 gives *templates with
placeholders* (`upstream-<dep>`, `external-CVE-NNNN-NNNN`), not literals, so
membership needed a pattern set this spec never supplied — leaving the
implementer to invent one. It also earned nothing: **no recognised source form
in § 3.5.3 contains a `/` or ends in an extension**, so no such form can reach
the first conjunct, and the exclusion could never fire. The earlier claim that
the parentheses mattered because "any recognised source form containing a slash
would be treated as a path" named no such form because there is none. A
false-positive here costs a note and an `extras` key on a value that resolves
to nothing — not a refusal (below) — which is the right price for dropping an
unbuildable clause.

**`Evidence:` needs no predicate — every element is a path by definition**
(`roadmap-format.md` § 3.5 defines the field as file paths), and it is
comma-separated, so each element is validated independently.
**`extras.unresolved_path` is therefore an array, not a scalar**, since one item
can cite several paths and lose more than one.

A path that does not resolve against the project root is **not** a refusal: it
is a note plus `extras.unresolved_path`, because a roadmap legitimately cites
files that have since moved, shipped or been archived. Refusing would make a
historical roadmap unimportable, which no reading of the problem asks for.

**Validating that a spec is a *valid* spec is out of scope** (§ 5) — existence
is mechanical, validity is `/doc-lint`'s job and belongs to whichever verb
consumes the reference, not to the import.

### 2.6 Round-trip identity is the gate

For a migrated project, `import(render(store)) == store` over the columns this
contract governs. That is the property making the markdown safely regenerable:
without it, each release rewrites the file and each rewrite moves the data.

**The governed set, enumerated — an acceptance test with an unstated scope has
no pass condition.** `id`, `status`, `headline`, `kind`, `source`, `layman`,
`lanes`, `evidence`, `body`.

**Excluded, each for a stated reason:** `provenance` (§ 2.4 — a rendered
`Kind:` re-imports as asserted by construction); `id_origin` (a synthesised id
becomes parsed once written into the file, which is the allocation working, not
drifting); `extras` (the render emits none, so § 2.1's `source_kind` is
one-shot by construction — governing it would make INV-6 permanently red).

**`items_updated` counts an item whose *governed* columns changed**, not any row
the importer rewrote. Without that definition INV-6's assertion cannot pass for
reasons unrelated to the contract — an excluded column moving still bumps a
row-level counter. The migration reports both figures; INV-6 reads the governed
one.

**§ 1's four drifting fields, each accounted for — a gate with an unexplained
column is a wish.** The measured drift named `headline`, `layman`, `lanes` and
`extras`.

- **`lanes`** — **partly diagnosed; the residue is deferred with `headline` in
  § 5.** An earlier draft booked this against § 2.2's *un-anchoring*, which
  indeed cannot affect it: `rxLanes()` was already un-anchored by ANTS-2058.
  **But § 2.2 makes two other changes to `rxLanes()` that do move `lanes`
  values**, and ruling the whole section out on the un-anchoring alone was
  wrong. The bold-label pair changes 3 corpus lines (`**Lanes:** core`
  previously reported a lane named `** core`), the widened guard changes 2 more
  that quote the key, and § 2.2's new precedence rule moves `lanes` off
  first-match. Measure with those four changes in before attributing any
  remaining drift elsewhere. A sixth known contributor is out of scope: a
  backticked example spanning a line break defeats the guard entirely, which is
  how ANTS-3808 imports a `lanes` value it never declared (§ 5).
- **`extras`** — excluded above; the render emits none.
- **`headline` and `layman`** — **undiagnosed, and deferred together in § 5.**
  INV-6 cannot pass while either stands. Whoever implements this measures them
  first (§ 6's INV-6 fixture is the instrument) and either folds the cause in as
  a § 2.x or splits it out. Naming them as unfinished is the point — the
  alternative is an implementer building § 2.2 through § 2.5 in full and finding
  the acceptance test still red with nothing to work from.

## 3. Invariants

- **INV-1** — No import defaults a field without emitting a note naming that
  field. *Test:* `tests/features/roadmap_import_mapping/` — import a fixture
  whose bullet declares no `Kind:`, assert the result carries a note whose code
  is exactly `field_defaulted` and **whose detail names the defaulted field** —
  one code for every defaulted field, not a per-field code. § 2.3 owns the
  shape; this clause previously said "a note whose **code** names the defaulted
  field", which reads as `kind_defaulted` / `source_defaulted` and is a
  different envelope contract for anything filtering on code.
  *Breaks when:* a branch assigns a default and
  returns without `addNote`, which is the exact shape of today's empty-`rawKind`
  path.
- **INV-2** — A bullet declaring `Kind:` inline, not at line start, imports with
  that kind. *Test:* fixture bullet `**H.** Body text. Kind: security.` imports
  as `kind='security'` with `provenance.kind='asserted'`. *Breaks when:* the
  pattern is re-anchored, which is the state this spec is written against —
  the test fails on today's source and that is the must-fail-first proof.
- **INV-3** — A bullet *quoting* the label does not declare it, **in either
  spelling**. *Test:* fixture bodies containing ``the `Kind:` trailer`` and
  ``the `**Kind:** implement.` line`` both import with a defaulted kind, not
  `kind='trailer'` and not `kind='implement'`. *Breaks when:* the lookbehind is
  dropped while un-anchoring — the regression ANTS-3722 already paid for once on
  `rxLanes()` — or when only the plain form is guarded, which is the state
  ANTS-4077 found: one `(?<!`)` cannot see past the optional `**`, so the bold
  quotation parsed as a declaration while the plain one did not.
- **INV-4** — Every non-identity `kind` mapping preserves the original in
  `extras.source_kind`. *Test:* import `Kind: bugfix.`; assert `kind='fix'` and
  `extras.source_kind='bugfix'`. *Breaks when:* a map is added to
  `mappedKind()` without the `extras` write, making that map irreversible.
- **INV-5** — A `source` marked `provenance = defaulted` does not render a
  `Source:` line; `layman`, `lanes` and `evidence` render exactly as they do
  today. *Test:* render a fixture whose item has `provenance.source='defaulted'`
  and a non-empty `layman`/`lanes`/`evidence`; assert no `Source:` line **and**
  that all three other trailers are present. *Breaks when:* the render condition
  is phrased over absent-or-non-asserted provenance rather than over the two
  defaultable fields — which suppresses three trailers that never carry
  provenance at all.
- **INV-6** — `import(render(store))` changes none of § 2.6's nine governed
  columns. *Test:* migrate a fixture project, render, re-import, assert
  `items_updated == 0` and no `field_conflict` note naming a governed column.
  *Breaks when:* any render emits a form the parser reads back differently —
  the 714-item drift § 1 measures. **Expected to fail on `headline`, `layman`
  and `lanes` until that drift is diagnosed** (§ 2.6, deferred in § 5); the
  fixture is the instrument for diagnosing it. `items_updated` here means
  § 2.6's governed-column counter, not the row-level one.
  **The expected-red claim is scoped to the corpus, not to the shipped test,
  and the two were conflated until 2026-08-10.** The registered case
  `RoadmapImportMapping.RenderThenImportIsIdentityOverGovernedColumns` is
  **green**, because its fixture is a small purpose-built roadmap that does not
  reproduce the three drifting columns. That is the correct arrangement — a
  permanently-red ctest case would make the project suite permanently red, and
  a suite nobody can read green stops being read — but it means **the shipped
  test is a regression guard, not the measurement.** The measurement is Phase D
  (D3/D4), run against this project's real roadmap. An implementer must not
  "fix" the green case by widening its fixture until the drift is diagnosed;
  when it is, the fixture grows to cover the diagnosed cause and the case stays
  green for a reason rather than by omission.
- **INV-7** — A `Source:`/`Evidence:` value naming a path that does not exist
  imports successfully, with a **`path_unresolved`** note and
  `extras.unresolved_path`. *Test:* **two** fixtures — one citing
  `Source: docs/gone.md`, one citing `Evidence: docs/gone.md, docs/also-gone.md`
  — asserting `ok`, a note whose code is exactly `path_unresolved`, and the
  extras key, which is an **array** and carries both elements in the second
  case. *Breaks when:* validation is written as a refusal, which would make a
  historical roadmap unimportable, **or** when only the `Source:` half is
  covered — `Evidence:` is the multi-valued one, so a single-path fixture
  cannot exercise the array at all.
  **The code was unnamed until 2026-08-10**, alone among this spec's notes
  (`field_defaulted` § 2.3, `kind_unmapped` § 1, `field_conflict` § 2.6,
  `notes_truncated` § 4). "Assert the note" is satisfied by any note the
  fixture happens to emit, including a `field_defaulted` from the same bullet,
  so the clause could not fail until the code was fixed.
- **INV-8** — The emoji→status mapping is **total and closed over the four
  documented markers**, so no input can reach the fifth. *Test:* import a
  fixture carrying all four emoji plus a line with a malformed marker; assert
  each of the four maps to its documented status, that the malformed line
  **becomes no item at all** — carried as narration by the structural walk in
  [`ANTS-3757`](ANTS-3757-roadmap-migration-read.md) § 2.11, with no `dropped`
  row and no defaulted-status item — and that
  `SELECT COUNT(*) … status='dropped'` is 0.
  **Corrected 2026-08-10 (ANTS-4076).** This clause previously said the
  malformed marker "defaults to `planned` with a note". It does not, and the
  correction is against source rather than preference:
  `RoadmapParse::stripInlineEmoji()` recognises exactly the four documented
  markers and returns false for anything else, so the line is never classified
  as a bullet and never reaches `makeItem()`. Nothing is lost. Admitting it as
  an item instead would mean every unmarked `- ` line in an `ants-v1` document
  became one — a change to ANTS-3757's bullet grammar, and a far larger loss
  than this invariant guards against.
  Asserting only the last clause would be a tautology — § 2.1 says `dropped` has
  no emoji, so no fixture can request it — which is why the totality of the
  mapping is what is actually tested. *Breaks when:* the fifth status
  is wired in before the render can express it, producing a row that cannot
  survive its own regeneration.
- **INV-9** — A lowercase `kind:` label does not parse as a declaration.
  *Test:* import a fixture bullet `**H.** Body. kind: security.`; assert the
  kind is defaulted with a note, not `security`. This pins § 2.2's deliberate
  ANTS-3407 reversal so it is tested rather than assumed. *Breaks when:*
  `CaseInsensitiveOption` is restored alongside the un-anchored pattern, which
  re-admits the prose match ("…changed the kind: of work…") that dropping the
  anchor exposes.
- **INV-10** — Un-anchoring changes render suppression **only** where a
  mid-prose `Kind:` value equals the column's. *Test:* three fixtures rendered —
  body with no `Kind:` text (trailer emitted), body whose mid-prose `Kind:`
  value **differs** from the column (trailer emitted, since `shadows()` is value
  equality), body whose mid-prose `Kind:` value **equals** it (trailer
  suppressed). `rxKind()` is shared with `bulletText()` via `trailerValuesIn()`
  (§ 2.2), so the parser change reaches the render. *Breaks when:* the widened
  match is treated as presence rather than equality, which would drop a required
  trailer from any bullet whose body happens to discuss the label.
- **INV-11** — When a bullet contains more than one match for a trailer key, a
  **line-initial** match beats a mid-line one; the resolver takes the last
  line-initial match, and falls back to the last mid-line match only when there
  is no line-initial match at all. **This holds for all five keys** — `kind`,
  `source`, `layman`, `lanes`, `evidence` — not for `Kind:` alone.
  *Test:* five fixtures. (a) body `… the old Kind: refactor. …` **followed by**
  trailer `Kind: security.` → `kind='security'`; green under plain last-match
  too, so it is a regression guard, not a discriminator. (b) trailer
  `Kind: security.` **followed by** a later note reading `… the canonical
  Kind: while the column …` → still `kind='security'`. (c) two line-initial
  matches, `Kind: implement.` then `Kind: fix.` → `kind='fix'`. (d) a bullet
  whose only match is mid-line → that value, proving the fallback still serves
  § 1's 52 inline-only declarations. (e) **the same shape as (b) for a
  first-match key**: a body whose line-initial `Source:` / `Layman:` trailer
  is *preceded* by an indented sample carrying those labels → the bullet's own
  values, not the sample's.
  *Breaks when:* precedence is dropped back to plain positional resolution.
  That fails in **opposite directions** per key, which is why one rule is
  stated for all five: `kind` routes through `matchLastIn()` and is displaced
  by a *later* prose mention, while `source`, `layman`, `lanes` and `evidence`
  route through `matchIn()` and are displaced by an *earlier* one. Neither is
  hypothetical — five bullets in this project's roadmap carry a corrupt `kind`
  and ANTS-3808 carries a wrong `source` and `layman`, at the time this
  invariant was written.
  **Not covered by this invariant:** a bullet whose body carries a *second*
  line-initial trailer — a nested sub-entry, or an indented sample of a bullet.
  No positional rule resolves that (§ 2.2); the source is corrected instead.
  Nor is a backtick span crossing a line break, which defeats the guard
  entirely and is a separate mechanism (§ 5).

## 4. RAM / build cost

**No new target, and no new stored state.** `RoadmapStore::ItemWrite` already
carries `provenance` as a `QJsonObject` (`src/roadmapstore.h`) and `bulletText()`
already takes an `ItemWrite &`, so § 2.4 reads a field the render is holding —
one JSON lookup per bullet, against a render that already walks every one. No
struct gains a member and no query changes.

**The notes are the one thing that grows, and they are bounded per run, not
cumulative.** INV-1 emits a note per defaulted field: for this project's first
import that is 476 + 383 = **859** notes — an upper bound measured on the
pre-fix run, since § 2.2's un-anchoring removes at least 48 by parsing the field
instead of defaulting it. They live in the migration's response
envelope, not in the store, so they are bounded by one import's item count and
discarded when it returns — and `roadmap_migrate` already truncates its `notes[]`
(`notes_truncated`) rather than emitting unboundedly. The count is a reporting
concern, and § 2.3's value is the count *falling* on the next run.

`extras.source_kind` and `extras.unresolved_path` add at most one short string
per affected item: **61** items carry `source_kind` today
(`SELECT COUNT(*) FROM item WHERE json_extract(extras,'$.source_kind') IS NOT
NULL`), and the path population is § 2.5's 150 occurrences. Both are bounded by
the corpus, not by usage over time.

## 5. Out of scope

- **The pass-headings status vocabulary** — 142 values outside the enum. A
  second dialect, tracked separately; this contract governs `ants-v1`.
- **Validating that a referenced spec is well-formed** (§ 2.5) — existence is
  mechanical, validity belongs to `/doc-lint` and to the verb that consumes the
  reference.
- **The non-standard field keys** beyond mapping them into `extras` verbatim.
  The survey counts **446 distinct keys**, which includes the five trailer keys
  § 3.5 defines (`Kind`, `Lanes`, `Source`, `Layman`, `Evidence`) — so roughly
  441 are extensions. Deciding which deserve real columns
  (`Dependencies` 98, `Acceptance` 44, `Scope` 42) is a data-model change, not
  an import mapping.
- **A compound-`Kind:` rule** — **closed in Phase B3, not deferred.** The three
  compounds § 2.1 raised (`feature/fix`, `design + implement`, `design + fix`)
  occurred in this project only, and reading them says no rule should exist: the
  two `feature/fix` items are a bug (ANTS-1219) and a feature (ANTS-1160), so
  any single mapping is wrong half the time, and `design` is not one of § 3.5.3's
  21 values so `design + X` has only one legal half. The four bullets were
  corrected instead; the values are gone from the corpus and no rule was added.
- **`headline`, `layman` and `lanes` round-trip drift** (§ 2.6) — named,
  measured and undiagnosed. INV-6's fixture is the instrument; whoever runs it
  owns the follow-up.
- **Back-filling `Kind:` onto the corpus items that carry none** — **1,814** of
  4,378, re-measured 2026-08-08 after Phase B2. Three earlier figures are all
  superseded: 1,613 and 2,050 were taken with the anchored matcher and so missed
  every inline declaration, and 1,817 was the corrected survey run against the
  pre-Phase-B2 source. They default legitimately under
  § 3.5.3's own rule; INV-1 makes the default visible, which is all this item
  owes them.
- **Re-migrating the other 13 projects.** ANTS-3853 owns the rollout; this is
  the gate it waits on.

## 6. Tests

Feature test: `tests/features/roadmap_import_mapping/`, covering **INV-1 through
INV-11** — every one is a behavioural case; this spec carries no source-grep
invariant, INV-8 having been rewritten as one after the grep form was found
unable to fail. Label `features;fast` — every fixture is a few-line roadmap, so
nothing here needs the `perf` label.

Per the project test convention, **verify each case fails against pre-change
source first**. Six must red on today's code, and they are why this spec is
written before the fix rather than after: **INV-2** (`rxKind()` is anchored),
**INV-5** (`bulletText()` renders from the value, not the provenance),
**INV-9** (`CaseInsensitiveOption` is still set), **INV-1** (the empty-kind
branch emits no note), **INV-7** (§ 2.5's path validation does not exist in
current source at all) and **INV-10's equal-value fixture** (today's anchor
leaves `offset == -1`, so `shadows()` is false and the trailer is emitted). **INV-6 is expected to red and stay red** on `headline`, `layman` and `lanes`
until § 2.6's undiagnosed drift is resolved — it is the measurement, not a
regression.

**Amended 2026-08-10 (ANTS-4086 / ANTS-4076).** The six above are the must-red
set against **pre-Phase-C** source and are green now that Phase C has shipped;
they are left as written because the list records what the first
implementation had to prove. The amendment adds one further must-red case,
against **post-Phase-C** source:

- **INV-11 fixture (b)** — a trailer followed by a later note mentioning the
  label mid-line. Today's implementation takes the last match anywhere, so it
  reds; that is the ANTS-4086 defect and its must-fail-first proof. Fixtures
  (a), (c) and (d) are expected green, since plain last-match already satisfies
  them — which is exactly why (b) is the one that discriminates.

**INV-8 gains no new test.** `tests/features/roadmap_import_mapping/` already
asserts the verified behaviour (no item, the line carried as narration, no
`dropped` row); it was the spec's clause that was wrong, not the test, so the
correction is documentation-only and nothing reds.

INV-6 needs a fixture project rather than a bullet — a small roadmap with one
item per interesting shape (inline trailer, quoted label, absent kind, mapped
kind, unresolved path), migrated into a temp store. Per the standing trap,
construct `RoadmapStore` with an **explicit path**; the default resolves under
`XDG_DATA_HOME` and would run the test against the live store.

## 7. Cross-doc impact

- **`docs/standards/roadmap-format.md` § 3.5** — two changes. The § 2.2 change
  makes an inline `Kind:` trailer genuinely supported rather than accidentally
  supported, and the standard should say so: **99 bullets in this project alone**
  write that shape (1,435 own-line against 99 inline, counted per bullet over the
  pre-render file and both archives). And § 3.5 must record that `Kind:` is now
  **case-sensitive**, reversing ANTS-3407 for that one label (§ 2.2).
- **`tools/roadmap-corpus-survey.py`** — **done, Phase B1.** Its `KIND_VALUE` was
  anchored `^\s+…$` and so shared `rxKind()`'s blind spot, which is why § 7.4's
  "11 others" looked complete. Un-anchored with the guards § 2.2 carried at
  Phase B1, plus `+` in the value class (three corpus values are
  `+`-joined compounds) and a stated four-word / 30-character bound that reports
  prose matches rather than dropping them. Measured against the pre-Phase-B2
  source: non-canonical values 11 → 19; items with no `Kind:` 2,050 → 1,817.
  **Re-open it before quoting any further figure from it.** § 2.2 has since
  gained two things the survey does not carry: ANTS-4077's optional `(?:\*\*)?`
  bold-label pair, and ANTS-4086's line-initial precedence. § 2.2 measures **29
  corpus lines writing `**Kind:**`** that had always parsed as declaring
  nothing, so a survey without the bold pair undercounts in exactly the way
  this section warns § 7.4's "11 others" did. The Phase B1 figures above stand
  as measured; anything counted after that needs the survey re-synced first.
  **Any figure quoted from a survey run before this fix is an undercount**,
  corpus-wide, not only for this project.
- **`docs/standards/roadmap-data-model.md` § 7.4** — **done, ANTS-4067.**
  It is already the normative home of the kind mapping, so this spec added to it
  rather than giving it one: the four values of § 2.1 that its eleven missed
  (`bug`, `performance`, `process + tooling`, `audit` — the other three of the
  seven were compounds, closed in Phase B3 by correcting the bullets). Its "32
  distinct values / 11 others" is now 21, all canonical, zero others, and its
  per-value counts are marked historical. § 2.3's defaults-are-noted rule lands
  here too, and is still owed.
- **`ROADMAP.md`** — ANTS-4063 (fabricated `Source:`) is discharged by INV-5,
  and ANTS-4062 (off-taxonomy `Kind:`) by § 2.1; both flip when this ships.
- **`CHANGELOG.md`** — one `Fixed` entry; the import losing declared fields is
  user-visible to anyone who migrates.
- **`CLAUDE.md`** — no change. The module map names subsystems, not field maps.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Dimensions | Outcome |
|---|---|---|---|---|---|
| 4 | 2026-08-10 | 2, cold — one byte-stable shared-context file, both lanes given the same path; scrubbed doc copy with the loop log withheld; verified-facts block carrying nine source facts read during packet assembly | **Q1 3 · Q2 5 · Q3 4 · Q4 0** — verified 12, dismissed 2 | (Q-counts; the C/H/M/L/I column is the retired scale) | **Re-gate after an authoring amendment (ANTS-4086 + ANTS-4076), not a fresh review.** Both lanes independently led on the same defect, and it is one the amendment itself introduced by inheriting a false premise from loop 3's row: **§ 2.2 said `trailerValuesIn()` resolves `kind` through `matchIn()` (first match). It does not** — it calls `matchLastIn()` for `kind` **only**, while `layman`/`lanes`/`evidence`/`source` use `matchIn()`. Loop 3 verified the wrong half and wrote the wrong function into the spec; loop 4's fix names both and says which function to edit, because an implementer following the old text edits `matchIn()` and changes four fields while leaving the one the section is about untouched. **The run's largest finding, verified empirically rather than by reading:** the amendment fixed precedence for `Kind:` alone, leaving the identical defect live on four other governed columns — and on first-match keys it is worse, since *any* earlier mention wins. Demonstrated on ANTS-3808 in this project's own roadmap, which embeds an illustrative sample bullet above its real trailer and therefore imports `source = "test."` and `layman = "An older thing."` today. The rule now governs all five keys through one shared resolver (user decision, taken on that evidence). A third mechanism surfaced in the same check and is deliberately **out of scope**: ANTS-3808 also imports `lanes = ["packaging"]` while declaring no `Lanes:` at all, harvested from a backticked example spanning a line break — the guard is a fixed-length lookbehind and cannot see across it. Filed, not fixed. **Also fixed:** § 2.5's path predicate, whose "final segment" was still undefined for a value with no slash *despite loop 3 recording it as fixed* — now a whitespace-token predicate, and its unbuildable "not a recognised source form" conjunct deleted after running the predicate over § 3.5.3's forms proved none can reach it; INV-7's note code was unnamed, alone among this spec's five notes, so its clause could not fail — now `path_unresolved`, with an `Evidence:` fixture added since the array half was untested; INV-1 and § 2.3 disagreed on whether the field name lives in the note's code or its detail; § 2.5 claimed § 2.2 repairs `Evidence:`'s inline blind spot when § 2.2 deliberately leaves `rxEvidence()` anchored, losing 18 of 22 corpus occurrences; § 2.6 ruled § 2.2 out of `lanes` drift on the un-anchoring alone while § 2.2 makes two *other* changes to `rxLanes()` that move values; INV-11 fixture (a)'s ordering parenthetical contradicted § 6's must-red list; and INV-6's "expected to red and stay red" was contradicted by its own shipped case being green — the spec conflated the corpus measurement with the ctest fixture, now separated. **Dismissed (2):** § 2.2's closing "a real trailer always wins" reads as contradicted by the amendment but is scoped to prose mentions, which the amended rule does defeat; and § 2.1-vs-§ 5 on whether the compound `Kind:` values were deferred or closed — a real contradiction, but both readings produce identical code, so it changes no line. Doc 749 → 897 lines. **Not converged; loop 5 owed.** |
| 3 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loops 1–2; packet's verified-facts block extended with loop 2's four source facts, and its stale "8 invariants" line corrected to 10 | C 3 · H 5 · M 8 · L 9 · I 0 — verified 22, dismissed 0 | dim 5×6, dim 2×5, dim 4×5, dim 15×4, dim 6×4, dim 7×2, dim 1×1, dim 9×1, dim 11×1, dim 12×1 | **Converged by cap. Both lanes independently found the same defect loop 2 introduced, which is the clearest possible signal that the cap is the right stop. (1) CRITICAL, both lanes: § 2.6 booked `lanes` drift as fixed by § 2.2's un-anchoring — a mechanism that cannot touch it, since `rxLanes()` was already un-anchored by ANTS-2058 (verified at `src/roadmapparse.cpp:296`) and the two matchers are independent. One of the four gate columns was accounted for by fiction. Moved to the undiagnosed set with `headline` and `layman`, in § 2.6, § 5 and INV-6. (2) CRITICAL, lane B, and the deepest read of the whole run: un-anchoring makes **match precedence** load-bearing and the spec never stated it. Verified — `trailerValuesIn()` calls `matchIn(rxKind(), body)`, a single `match()` taking the **first** occurrence, and `bulletText()` appends the body *before* the trailer. So on a rendered bullet a stale mid-prose `Kind:` sits ahead of the canonical one and a re-import would adopt it, breaking INV-6 on a governed column the spec believed safe. § 2.2 now fixes the rule — **the trailer wins, the parser takes the last match** — and new **INV-11** pins it. (3) CRITICAL, lane A: `assertedSource` was used in § 2.4's render condition and never defined. The store declares `provenance NOT NULL DEFAULT '{}'`, so a row written by anything but `makeItem()` — `roadmap_log op:append`, for one — carries no `source` key; read as `== "asserted"` every such row silently loses its `Source:` line, which is precisely the loss this spec exists to stop. Now defined as `!= "defaulted"`, with the direction called out as the thing to get right. **HIGH ×5:** § 2.4's claim that `extras.source_kind` distinguishes defaulted from mapped is contradicted by the spec's own § 1 measurement — 438 of 476 lack it, so 38 defaulted kinds *do* carry one, written by the unmapped-value branch; the must-fail-first list said five where INV-10's equal-value fixture also reds on today's anchor, making six; a capitalised `Kind:` in prose survives the case-sensitivity fix and the residual exposure was unstated (now accepted explicitly, with the last-match rule as the limiter); the note code, envelope tally key and `extras.priority` key were all unnamed while two sibling `extras` keys were named, so two implementers would emit incompatible envelopes and neither would fail its test (`field_defaulted` / `defaulted_fields` now named); and § 6 disagreed with § 2.6 about whether `layman` is expected red. **MEDIUM ×8:** INV-8's fixture said the malformed marker "refuses **or** defaults", a disjunction no test can assert — now defaults to `planned` with a note; § 2.1's "corpus figures throughout" mislabelled a table whose counts are this project's pre-render file plus archives, not the 14-project survey; `dropped`'s precondition named the round-trip where the real blocker is the render's inability to express a fifth status; "`Kind:` always renders" contradicted INV-10 and `bulletText()`'s own shadow suppression; § 2.3's completeness rule had no invariant behind it; and the § 2.5 predicate's "final segment" was undefined for a value with no slash. **LOW ×9**, including a dangling sentence fragment my own INV-8 edit created and Phase 4c caught before the commit. **Cap reached with three findings filed rather than fixed** — see the deferred tail in the run report: the `Priority:` and `Evidence:` corpus counts want re-measuring against the pre-render source (both were taken from the contaminated survey), and § 4 budgets no filesystem-stat cost for § 2.5's 150 path checks. None is build-changing; all three are recorded so nothing is lost by stopping. Doc 502 → 551 lines; invariants 10 → 11. |
| 2 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loop 1; packet's verified-facts block extended with the six source facts loop 1's verification established | C 2 · H 5 · M 8 · L 9 · I 0 — verified 24, dismissed 0 | dim 5×7, dim 2×5, dim 6×5, dim 15×4, dim 7×3, dim 1×2, dim 10×2, dim 4×1, dim 8×1, dim 11×1 | **Origin split: 10 of 13 distinct defects were collateral from loop 1's own fixes, 3 were draft defects — a decisive margin, so the loop-economics trigger fired and this pass ends with a consolidation sweep rather than a reflex dispatch. (1) CRITICAL, lane A, and it falsifies a guarantee loop 1 introduced: excluding `extras` from § 2.6's governed set means the first render→import cycle **destroys** `extras.source_kind`. Verified directly — `src/roadmaprender.cpp` contains no `extras` reference at all, so the original value cannot survive a regeneration. That makes loop 1's "no map is lossy and a bad map is reversible" false on any migrated project. Rewritten to promise **one-shot, not durable** reversibility, with both ways of making it durable named and both declined, because claiming a guarantee the render cannot keep is worse than claiming none. (2) CRITICAL, lane A: § 2.4 proved `items_updated == 0` unsatisfiable and then "resolved" it by excluding `provenance` from the governed set — but `items_updated` is a **row** counter, so excluding a column does not stop the row counting. The remedy did not reach the assertion it was written for. § 2.6 now defines `items_updated` over governed columns and INV-6 says which counter it means. (3) HIGH, lane B, and the sharpest read of the run: loop 1's § 2.2 claimed un-anchoring makes "a body mentioning `Kind:` mid-prose shadow the trailer". `shadows()` is `m.offset >= 0 && m.value == v` — **value equality**, as its own comment states — so a body that merely mentions the label never shadows. INV-10 inherited the error and tested an unreachable state. Both restated to value equality, and INV-10 grew to three fixtures (no mention / differing value / equal value). (4) HIGH, lane B: § 2.6 named four drifting fields, accounted for three, and **silently dropped `layman`** — in the paragraph whose stated purpose is that no governed column goes unexplained. Now deferred alongside `headline`, in § 2.6, § 5 and INV-6. (5) HIGH, both lanes: § 2.1 enumerated all eleven § 7.4 mappings and then said in the next sentence that restating them "would create a second mapping free to diverge". The spec did the thing it forbade, in the paragraph forbidding it. Enumeration deleted; the count and the pointer remain. (6) HIGH, lane A: the three compound `Kind:` values carried "**ruling needed**" with no interim behaviour and no § 5 entry, leaving an implementer with no contract at all for them. They now fall through the unmapped branch explicitly and § 5 owns the ruling. **MEDIUM ×8:** the path predicate's `A or B and C` needed parentheses (both lanes, independently); `Evidence:` is multi-valued so `extras.unresolved_path` must be an array and every element is a path by definition; § 2.3's "surfaces all 476" contradicted § 4's `notes_truncated`, resolved by making the per-field **count** complete while the list stays a sample; INV-8 as written was a tautology — § 2.1 says `dropped` has no emoji, so no fixture can request it — and now tests that the emoji→status mapping is total and closed; INV-7 was missing from § 6's must-red list though § 2.5's validation does not exist in source; § 2.4's heading said "a defaulted field is not rendered" against its own `Kind:` conclusion; the `MultilineOption` rationale was wrong (it only affects `^`/`$` and is inert once the anchor goes); and "the six the standard defines" is five trailer keys. **LOW ×9**, all fixed, including 859 notes relabelled as a pre-fix upper bound. **Consolidation sweep (trigger response):** the eleven-mapping restatement deleted outright rather than reconciled — the anti-pattern the collateral margin exists to catch. Doc 448 → 502 lines. |
| 1 | 2026-08-08 | 2, cold — identical byte-stable shared packet (~12k tok) carrying bounded windows of `roadmapparse.cpp` / `roadmapmigrate.cpp` / `roadmaprender.cpp`, the store's CHECK constraints, `roadmap-format.md` §§ 3.5/3.5.3, and the five cited ANTS ids resolved via `roadmap_query` | C 3 · H 6 · M 6 · L 6 · I 0 — verified 20, dismissed 1 | dim 7×5, dim 2×5, dim 4×4, dim 5×4, dim 6×4, dim 15×2, dim 11×2, dim 9×1, dim 10×1, dim 1×1 | **Both lanes led on the same two contradictions, and verification found a third defect in the spec's own evidence. (1) CRITICAL, both lanes: § 2.4's render rule — "emits a key only when `provenance` marks that field `asserted`" — would have suppressed `Layman:`, `Lanes:` and `Evidence:` on **every** bullet, because `makeItem()` writes provenance for `id`/`kind`/`source` and nothing else, so those three never carry one. An implementer following it ships a larger data loss than the one the spec exists to stop. Now scoped to the two defaultable fields, with "absent provenance means not-a-defaultable-field, never defaulted" stated outright. (2) CRITICAL, both lanes: the `Kind:`-always-renders exception makes INV-6 unsatisfiable by construction — a defaulted kind renders `Kind: implement.`, re-imports through the canonical branch as `asserted`, and provenance flips on all 476 rows at the first round trip. Resolved by excluding `provenance` from the governed set and saying why, rather than by inventing a machine marker in a human-facing file. (3) CRITICAL: § 2.1 restated a mapping that **already exists and is normative** — `roadmap-data-model.md` § 7.4 carries it and `mappedKind()` implements exactly its eleven entries — and re-opened three of those as "ruling needed" (`behaviour-change`, `perf / fix`, `perf / optimize`), which would have had an implementer either stall or overwrite a shipped decision. The duplicate table is deleted; § 2.1 now points at § 7.4 and contributes only what it misses. **Found by verification, not by either lane, and it corrects the spec's own evidence:** § 7.4's "11 others" and this spec's corpus figures both came from `tools/roadmap-corpus-survey.py` run *after* the first store render — by which point the render had already rewritten `Kind: bug` to `Kind: implement` in the file being surveyed. Re-running the inventory against the pre-render source surfaces **seven** unmapped values the table misses, led by `bug` at 29 items, the single largest unmapped value in the corpus and invisible to the contaminated run. **HIGH ×6:** § 1 described `bulletText()` as emitting `Source:` "unconditionally" when it is doubly conditional (`!isEmpty() && !shadows(...)`, ANTS-3808) — the symptom was right and the mechanism wrong; the header said 363 where the store measures 383; the kind table claimed "11 others" above 17 rows; `rxKind()` is **shared with the render** via `trailerValuesIn()`, so un-anchoring reaches `shadows()` and the spec called it "one regex literal" (new INV-10); dropping `CaseInsensitiveOption` silently reverses shipped ANTS-3407 (new INV-9, and § 7 now records it); and § 1's four drifting round-trip fields were diagnosed and then never addressed, leaving INV-6 unreachable — `headline` is now named as undiagnosed rather than left for an implementer to discover at the gate. **MEDIUM ×6:** `priority` had no table despite § 2.1's own rule and is now deferred to § 5 with the column left NULL; "looks like a path" was undefined and is now a stated predicate; the governed column set is enumerated (nine columns, three exclusions with reasons); the note budget (859 on the first run) is priced; INV-8's source-grep could not fail and is behavioural; and "448 non-standard keys" is 448 distinct *including* the six standard ones. **LOW ×6**, all fixed, including the "20+ path references" estimate — measured at **93 distinct tokens across 150 occurrences**, an eyeballed undercount from a truncated list. **Dismissed (1):** both lanes suspected `ItemWrite` carries no `provenance` member, which would have made § 2.4 unimplementable. It does — `QJsonObject provenance` at `src/roadmapstore.h`, and `bulletText()` already takes an `ItemWrite &`, so the render holds it. § 4's "no new state" is correct as written. Doc 301 → 448 lines; invariants 8 → 10. |
