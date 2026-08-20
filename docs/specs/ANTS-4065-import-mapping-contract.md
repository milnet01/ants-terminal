# ANTS-4065 — define the markdown→store import mapping, so importing neither loses nor invents a field

**Status:** amended 2026-08-10 and again 2026-08-13, re-gate pending — accepted 2026-08-08 after a
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
   `if (!it.source.isEmpty() && !shadows(tv.source))` — two
   conditions, **neither of which a default fails**: a defaulted value is never
   empty, and ANTS-3808's `shadows()` suppresses only a trailer the *body*
   already declares at a line start. (The predicate lost its value argument
   under ANTS-4505; the reasoning here is unaffected.) So a default that existed only as an absence renders as an
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
(`src/roadmapmigrate.cpp`) implements all **fifteen** of its rows, applied as
written. § 2.2's predicate is defined by reference to those keys, so the count
is load-bearing. They are deliberately not reproduced here: a second copy would be free
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
surfaced seven values the table and `mappedKind()` both missed **at the time
this section was written**. All seven occur in this project only; measured over
`ROADMAP.md` plus both archives at the reverted source:

**Four of the seven have since shipped — do not re-open them.** Corrected
2026-08-10: `mappedKind()` now carries **fifteen** rows, the last four being
`bug → fix`, `performance → perf`, `process + tooling → chore` and
`audit → audit-fix`, added by this spec's own Phase C on 2026-08-09 and
recorded in § 7. The table below is kept as the *evidence that justified them*,
not as outstanding work; the remaining three are the compounds, and § 5 closes
those with no rule rather than deferring them.

| Corpus value | Count | → | Rationale |
|---|---|---|---|
| `bug` | 29 | `fix` | the single largest unmapped value in the corpus, and invisible to the anchored survey |
| `performance` | 2 | `perf` | long form of a canonical value |
| `process + tooling` | 1 | `chore` | § 3.5.3's "housekeeping"; `tooling` already maps there |
| `audit` | 1 | `audit-fix` | the canonical name for the same work |
| `feature/fix` | 2 | **no rule — closed** | compound; § 5 |
| `design + implement` | 1 | **no rule — closed** | compound; § 5 |
| `design + fix` | 1 | **no rule — closed** | compound; § 5 |

The four with a rationale are mechanical, this spec adopted them, and **they
shipped in Phase C** — `mappedKind()` carries all four today.

**The three compounds are CLOSED with no rule, not deferred** — corrected
2026-08-10, where this paragraph previously said "deferred … § 5 carries the
ruling" while § 5 said "closed in Phase B3, **not deferred** … no rule was
added". § 5 is right: the four affected bullets were corrected at source, the
values are gone from the corpus, and § 7.4 records that no general compound
rule exists beyond its named rows. Should one reappear it falls through
`makeItem()`'s unmapped branch — `implement`, `extras.source_kind` preserved,
`kind_unmapped` note emitted — which is visible rather than silent. **Nothing
is owed here**; the earlier wording would have had an implementer stall waiting
for a ruling that was already made.

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
compute ANTS-3808's `shadows()` suppression. **AMENDED 2026-08-19 (ANTS-4505):
that predicate is now LINE-INITIAL PRESENCE — `m.offset >= 0 && m.anchored`,
with a recognised-vocabulary rider for `kind` — where it used to be value
equality.** It reversed because value equality was one-directional: the render
appends its block at the END of the bullet and the parser takes the last
line-initial match, so a wrong column wrote itself back to the tail and a human
correcting the real trailer line was silently ignored, permanently. ANTS-3808
§ 2.3 owns the rule and the argument; do not restate it here.

**Un-anchoring therefore changes render suppression not at all**, which is a
narrowing of what this section used to claim. `anchored` is what suppression
reads, and un-anchoring moves only whether a MID-LINE match is found — so a
mid-prose `Kind:` never suppresses now, whatever its value. INV-10 pins that.

**Match precedence becomes load-bearing the moment the anchor goes, and it must
be stated.** `bulletText()` appends `it.body` *before* the trailer lines, so in
a rendered bullet a mid-prose trailer key sits ahead of the canonical one, and
an unspecified precedence lets a re-import adopt the stale prose value — the
same read-back hazard ANTS-3808 names, arriving from the parse side and
breaking INV-6 on a governed column.

**Current state, re-read 2026-08-19.** `trailerValuesIn()` resolves **all five
keys** through `matchLastIn()`, and `matchIn()` no longer exists. ANTS-4497
moved the remaining four on the measurement below; ANTS-4504 deleted the
function once it had no callers. Two earlier drafts of this paragraph described
a `kind`-only split and both are superseded — read the source, not this
history.

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

This was not hypothetical. **ANTS-3808 in this project's own `ROADMAP.md` —
the file Phase D imports first — imported FOUR wrong values.** Its body embeds
an illustrative sample bullet whose `Layman:` / `Kind:` / `Source:` lines sit
*above* its own trailer; `collectBulletBody()` trims them, so they are
line-initial in the body and first-match took `source = "test."` and
`layman = "An older thing."` from the illustration. Its `kind` was corrupt too,
by the *other* mechanism — a prose mention below the trailer winning under
unconditional last-match, which is why ANTS-3808 appears in the five-bullet
list below. **One bullet, both directions of the same defect**, which is what
makes it the worked case. And it declares **no** `Lanes:` at all yet imported
`lanes = ["packaging"]`, harvested out of a backticked example spanning a line
break.

**Three of the four are closed as of 2026-08-19, and only `kind` is still
live.** ANTS-4497 moved `source` and `layman` onto the shared resolver, so the
sample above the trailer no longer wins; ANTS-4504 masks the backticked span,
so the `lanes` value is no longer harvested (§ 5). `kind` is the one this
section's vocabulary predicate still owes. **The STORE is a separate matter**:
a column already holding a wrongly-harvested value is not corrected by
re-reading, which is ANTS-4505.

**"Line-initial" needs no indentation rule, and the predicate already exists.**
`collectBulletBody()` builds `body` by appending `'\n'` followed by each
continuation line, the block **dedented to its common left edge** (per-line
`cont.trimmed()` until ANTS-4554, 2026-08-20), so the body **preserves line
breaks and carries no leading whitespace on any line sitting at that edge** — a
source line `   Kind: test.` reaches the matcher as `Kind: test.` at a line
start whenever the rest of the block shares that indent, which the format's
two-space continuation guarantees. A line the author indented deeper keeps the
difference and is NOT line-initial, which is the intended reading: a
declaration inside a nested block is structure, not the bullet's trailer. `TrailerMatch::anchored`, computed in
`matchLastIn()` as `(at == 0) || body.at(at - 1) == '\n'` over
`capturedStart(0)`, is therefore exactly this test and is the predicate to
use. `matchLastIn()` already computes it on every global match and
simply never consults it: today it overwrites its result each iteration and
returns the last match unconditionally. **The change is which match it keeps,
not a new predicate.**

> **OPEN — line-initial is not a sufficient test for "is a trailer", and this
> is not yet resolved.** Found 2026-08-10, after the rule above was written.
> `collectBulletBody()` trims each continuation line, so **a prose sentence
> that happens to wrap immediately before the label becomes line-initial.**
> Live case: `ANTS-3754`'s body line 86 reads `Kind:), § 3.1's format marker,
> and §§ 3.6.2-3.6.3's headline matching —` because the source wrapped after
> `(emoji, ID, bold headline,`. Its real trailer, `Kind: doc.`, is 22 body
> lines earlier, so **last-line-initial returns `), § 3` instead of `doc`.**
> Source wrapping is arbitrary and re-wraps whenever anyone reflows a
> paragraph, so this is not a one-off to correct in the source.
>
> The `anchored` preference below is therefore **necessary but not
> sufficient**, and what completes it was **settled 2026-08-13 (user
> decision)**.
>
> **The candidate tightening this section used to carry — require the captured
> value to begin with a letter — was rejected on measurement.** It rejects
> `), § 3`, `\s*([^\` and `; when absent …`, and it **accepts** the capture
> that corrupts ANTS-3608, `explicit on every actionable bullet; section
> context is a hint …`. Prose appended after a trailer usually begins with a
> word, so the test misses the common shape.
>
> **The rule taken instead: for `kind`, a match is a candidate only when its
> value is RECOGNISED VOCABULARY** — one of § 3.5.3's 21 taxonomy values, or
> **any key of `mappedKind()`**, which is all fifteen and not merely § 2.1's
> four additions: `improve`, `docs`, `bugfix`, `testing`, `spike`, `feat`,
> `enhance`, `perf / fix`, `perf / optimize`, `tooling`, `behaviour-change`,
> `bug`, `performance`, `process + tooling`, `audit`. The test is against the
> **lowercased, trimmed** capture, because `mappedKind()` takes a lowercased
> key and two of them carry spaces and a slash. The mapping inputs are in
> deliberately: this project carries none today, but other corpus projects do,
> and dropping them would make the rollout regress on exactly the values § 2.1
> exists to translate — so the predicate reads `mappedKind()`'s table rather
> than restating it, or the two drift the first time a value is added. Among
> recognised candidates the `anchored` preference decides.
>
> **When NO match is recognised the resolver returns the last match seen, raw**
> — not "absent". `makeItem()` then takes its unmapped branch unchanged:
> `implement`, `extras.source_kind`, and both the `kind_unmapped` and
> `field_defaulted` notes. **An absent field is a different branch** and emits
> `field_defaulted` alone with no `extras`; § 2.4's discriminator depends on
> the two staying distinct. **The predicate chooses among candidates; it never
> destroys evidence.**
>
> **The guard is `kind`-only, deliberately.** `layman`, `source` and `evidence`
> are free text and `lanes` is an open list, so there is no vocabulary to check
> them against; those four take the `anchored` preference alone. The resolver
> is therefore shared and takes an optional per-key predicate, rather than four
> callers pretending to a fifth's constraint.

**`matchLastIn()` gains the preference** — among candidates, keep the last
whose `anchored` is true, else the last candidate seen — plus an optional
`isCandidate` predicate defaulting to accept-everything, which `kind` supplies
as the recognised-vocabulary test above.

**Built in two steps, and the narrowing was reversed.** The 2026-08-13 build
moved `kind` alone, on the reasoning that only `kind` is re-emitted by the
render into a body that may already discuss it. That reasoning was wrong about
the other four rather than merely cautious: first-match is displaced by any
*earlier* mention, so a bullet quoting a sample above its own trailers imported
the illustration's values — ANTS-3808 imported `source = "test."` and
`layman = "An older thing."` **ANTS-4497 moved all five on 2026-08-19**, and
ANTS-4504 deleted `matchIn()` with its last caller. **Order matters within the
resolver:** the predicate filters first
and `anchored` decides among what survives, never the reverse. **ANTS-3754 is
TWO line-initial matches, not one of each** — `collectBulletBody()` trims every
continuation line, so its wrapped `Kind:), § 3.1's …` is line-initial exactly
as the real `Kind: doc.` above it is. Both are anchored and the wrapped one is
later, so only the predicate separates them.

**Every consumer takes the predicate, `rlDeriveTrailerColumns()` included.**
There it cannot cause a clear: that function clears only when the old body
carried a value and the new one does not, and an off-taxonomy `Kind:` resolves
to the same raw last match on both sides — the resolver returns it rather than
"absent", per the no-recognised-match rule above, so neither side is empty. It fixes a live defect instead — today `op:annotate` appending
prose that mentions `Kind:` overwrites the stored value. Adding
precedence to a second helper instead would be wrong: one resolver serves all
five keys, so a rule added anywhere else is a rule two functions can disagree
about.

Two consequences to carry, neither optional. `rxSource()` captures to
end-of-line (`([^\n]+)`) rather than to the first period, so a first-match hit
on a prose mention swallows the rest of that line — which is why `source` is
the field where this defect is most visible. And **`rxEvidence()` is still
anchored** (below), so for that one key every match is line-initial by
construction and the rule is inert until the anchor comes off; it is included
here so the resolver is uniform and stays correct when it does.

**`trailerValuesIn()` has FOUR consumers, not two, and one of them writes to
the store.** Added 2026-08-10; this section previously named only the parse and
render paths, which understated the blast radius:

| Consumer | What a precedence change reaches |
|---|---|
| `roadmaprender.cpp` — `bulletText()` | `shadows()` suppression (INV-10) |
| `remotecontrol_roadmap_query.cpp` — `rlDeriveTrailerColumns()` | **writes or clears the store's trailer columns** on `roadmap_log` `append` / `annotate` / `amend_body` |
| `remotecontrol_roadmap_query.cpp` (second site) | trailer values on a query path |
| `roadmapdialog.cpp` | the kind the dialog displays |

The second is the one that matters and it is **not an import path**:
`rlDeriveTrailerColumns()` compares `trailerValuesIn(before.body)` against
`trailerValuesIn(newBody)` and clears a column only when the replaced body also
yielded that key. Changing which match wins changes both sides of that
comparison, so a bullet carrying a sample or a prose mention alongside its own
trailer can have a stored column **rewritten or cleared by the next
`roadmap_log` write** — a live user-facing write, triggered by annotating a
bullet. **That seam is answered below rather than delegated** (2026-08-13):
`rlDeriveTrailerColumns()` follows both the precedence and the vocabulary
predicate, and the predicate makes that path strictly safer — see § 2.2's
consumer paragraph for the mechanism and why it cannot cause a clear.
§ 4's "no new stored state" is a statement about *this spec's* additions and
does not price these writes.

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
because **the fallback is what keeps § 1's original defect fixed**: 55 bullets
declare `Kind:` only mid-line (the table below; an earlier scan said 52), so a rule that simply restored the `^\s*` anchor
would lose every one of them again.

| Bullets with … | Count | Under this rule |
|---|---|---|
| line-initial `Kind:` only | 1,453 | unchanged |
| mid-line only | 55 | the fallback serves them |
| both | 26 | the line-initial one wins |
| no `Kind:` | 189 | defaults, per INV-1 |
| **>1 line-initial** | **16** (all exactly 2) | see the boundary below |

**These figures replace an earlier set (1,414 / 52 / 42 / 187 and "20 bullets"),
measured 2026-08-10 with a scan that did not reproduce `collectBulletBody()`'s
boundaries.** That scan ran from each bullet to the next bullet line, while the
real collector also stops at a **col-0 heading** and at any other col-0 content.
It therefore attributed trailing sections' trailers to whichever bullet preceded
them. Any figure about bullet bodies must be taken with the collector's own
stop conditions or it measures a different object; the corrected numbers come
from a re-implementation of those conditions.

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
response envelope, beside `notes_count`, and it is **an object keyed by field
name with integer counts** — `{"kind": 476, "source": 383}` — not a scalar
total. The shape is pinned here because the two are not interchangeable: the
completeness rule below is stated *per field*, and a single integer cannot
report that 476 of the defaults were `kind`, which is the thing the tally
exists to say. **This documents shipped behaviour rather than deciding it** —
`roadmapmigrateverb.cpp` assigns `defaultedFieldTally(plan)` and
`RoadmapImportMapping.DefaultedFieldsTallyIsPerFieldAndComplete` reads the key
with `.toObject()`; the spec was simply ambiguous where the code was not. This
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
roadmap and archives, under the **PRE-amendment** predicate — the one that
accepted a bare filename. Its four worked examples
(`*_Ants_MCP_Feedback.md`, `remotecontrol.cpp`, `terminalwidget.h`,
`rpmlint.log`) carry no separator and are therefore **prose** under the rule
below, not path tokens; they are kept as evidence of how often the field is
used this way, never as cases the predicate accepts. **Nobody has re-measured
the corpus under the amended predicate**, so treat 93/150 as an upper bound on
the population and do not size a fixture from it. Meanwhile `Evidence:` — the field `roadmap-format.md` § 3.5
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

> **A value that is a recognised source form carries no path reference at all
> — test that FIRST, on the whole value, and stop.** Otherwise split the value
> on whitespace and chop each token's trailing run of `. , ; : ) ] " '`. A
> TOKEN is a path reference when it contains `/` **AND** its final segment
> matches `\.[A-Za-z0-9]{1,5}$`. The value is a path reference when any of its
> tokens is.

**The unit of the SPLIT is the whitespace-delimited token, not the value** —
corrected 2026-08-10, having twice been left undefined. The recognised-form
gate above is the one deliberate exception, and it is measured rather than
inherited (below). A `Source:` value is frequently a
sentence (`in-session-2026-05-16`, but also `ROADMAP.md ANTS-4065` and
`rpmlint.log warnings`), so "its final segment" had no referent whenever the
value contained no `/`: it could mean the whole value, the last whitespace
token, or the last token before a parenthetical, and the three classify
different values. Tokenising first removes the question, and it is also what
makes the multi-path case work without a second rule.

`user-2026-08-08` has no slash in any token; `rpmlint.log warnings` has a
filename-shaped token but no separator, so it is prose under this rule;
`DOOM-0057/0081` has a separator but no filename-shaped segment, so it is prose
too; and `docs/specs/ANTS-3863-pre-read-dispatch.md` qualifies on both counts.

**AMENDED 2026-08-19 (ANTS-4502) — the two tests were an OR and are now an
AND.** The original rule accepted a token on either a `/` or a trailing
`\.[A-Za-z0-9]{1,5}$`, and either half alone is a bad predicate: the first
matches every prose slash, the second every bare filename and every
`§4.4`-shaped fragment. Requiring both is what says *path* — a separator says
the author meant a location, a filename-shaped final segment says they meant a
file.

**The OR-form's cost, measured on two live corpora after ANTS-4481 shipped:**

- **Claude Code config**, `roadmap_migrate dry_run` 2026-08-19: **11 of 12
  notes** are bare filenames, and **every one of the five distinct filenames
  exists** — `roadmap-format.md` at `standards/roadmap-format.md`,
  `charters.md` at `draft/skills/charters.md`, and so on. The resolver tries
  the project root and stops, so a file one directory down reads as missing.
- **DOOM Ants**, a clean migration the same day: **15 of 16 notes** are this
  class and not one names a path — `§4.4`, `~7.2ms`, `(0.6.0`, `Ultra`,
  `DOOM-0057/0081`, and two fragments of an English sentence.

So the note class runs at 92–94% false on both, which buries the one
`field_conflict` that mattered — the signal-to-noise failure ANTS-4481 was
filed for, one layer down.

**A separator ALONE is not the discriminator, and this is the measurement that
settles the section.** A first draft of this amendment deleted the extension
half and kept the separator, on the premise that "a reference that means a path
says so with a separator". Run against this project's own corpus — 1,781
bullets carrying a `source`, resolved through `RoadmapParse::parseBullets()`
over `ROADMAP.md` and `docs/roadmap/*.md`, with the recognised-form gate
applied — the separator-only rule classifies **97** tokens as paths, of which
**2** resolve. The other 95 are: **40** slash-command names (`/cold-eyes` ×12,
`/test-audit` ×8, `/audit` ×4, `/indie-review` ×4, `/doc-lint` ×2, `/model` ×2
and a bare `/`), which are absolute-looking and so can never resolve under the
root by construction (`resolvesUnderRoot()`); **9** id citations; and **46**
prose slashes — `precision/convenience`, `4d/5`, `§A/§B`, `#7/#8`,
`2026-05-17/18`, `62/64`. **About 98% false**, which is worse than the rule it
replaces. English writes alternatives, ranges and section pairs with a slash at
least as often as it writes paths.

**The AND-form, same corpus, same gate: 1 candidate, 1 resolves, 0 notes.** It
removes every class above without a special case for any of them — a
slash-command name has no extension, an id pair has no extension, and a prose
slash has neither.

**It also removes two rules the separator-only draft needed.** An explicit
id-citation exclusion (`DOOM-0057/0081` reads as a two-segment path) is
unnecessary: an id pair has no filename-shaped segment. And the question of
whether the test runs before or after `withoutTrailingPunctuation()` stops
deciding classification for the separator — though the chop is still specified
above, because the extension test anchors on `$` and `docs/gone.md.` must
still classify as a path.

**What the AND-form gives up, measured: one reference on this corpus.**
`docs/specs`, cited by ANTS-3675 — a DIRECTORY, which carries no extension.
A path with no extension at all (`tools/Makefile`, a bare directory) goes
unvalidated. That is the whole cost, and it is the right trade against 95
false notes.

**A bare filename in a sentence is a MENTION, not a claim that the file sits at
the project root** — the premise the extension half rested on, and it stands:
`Source:` is provenance prose by design (§ 3.5.3's vocabulary is hyphenated
tokens and dates), while `Evidence:` is the field `roadmap-format.md` § 3.5
designates for paths and keeps its unconditional validation.

**Walking the tree for a unique match was considered and rejected.** It is
undefined on exactly the corpus that motivates it: in that project
`documentation.md` and `commits.md` each match twice, the second under a
`backups/` directory. An existence-only walk would avoid the ambiguity but
costs a filesystem scan per migration to recover a signal the field was never
carrying.

**What this gives up, stated.** A `Source:` naming a genuinely missing file
with no directory part is no longer reported. That is the accepted price: the
alternative is a note class that is wrong nine times in ten, which is not a
report.

**`Evidence:` is deliberately untouched, and its own noise is filed
separately.** DOOM's `user screenshot` and `E1M1 outdoor courtyard` reach the
notes through `Evidence:`, which has no predicate because the standard defines
every element as a path. That corpus writes prose there anyway. Suppressing it
here would silently drop real evidence paths that carry no separator, and the
right repair is at the WRITE side, so it is out of scope for this section.

**The trailing-period ordering no longer decides classification, and the
paragraph that said it did is deleted.** It rested on the extension regex's
`$` anchor: a token ending `rpmlint.log.` failed to match, so a value tested
before `trailerValuesIn()`'s period chop was classified as prose. With the
disjunct gone the separator test is indifferent to a trailing period —
`docs/gone.md.` contains a `/` either way — and `withoutTrailingPunctuation()`
already retries a trimmed form at RESOLUTION time. So the chop matters to
whether a path is found, never to whether a token is one.

**The "not a recognised source form" test is KEPT, applying to the whole value.
The claim that it could never fire was false, and so was the instruction to
delete it** — both corrected 2026-08-19 (ANTS-4502), having stood since
2026-08-10.

`isRecognisedSourceForm()` is a whole-VALUE test: three exact strings plus
seven `startsWith` prefixes (`user-`, `audit-`, `indie-review-`,
`debt-sweep-`, `doc-review-`, `external-CVE-`, `upstream-`), and
`pathTokensIn()` returns an empty list, before tokenising anything, when it
matches. So it fires on any value merely *beginning* with a prefix, and it
exempts every token in that value. **Measured against the shipped migrator on
2026-08-19:** `Source: upstream-qt6, see docs/gone.md.` emits no note, while
the same missing path under `Source: in-session-2026-08-19, see docs/gone.md.`
emits one. It fires, and it fires often.

**Whether to keep it was settled by measuring, and the measurement reversed the
first answer.** Applying the test per TOKEN instead — the tidier rule, matching
§ 2.5's own unit — was drafted and then withdrawn. Ten `Source:` values in this
project's roadmap and archives begin with a recognised prefix **and** carry a
`/`, resolved through `RoadmapParse::parseBullets()` over `ROADMAP.md` and
`docs/roadmap/*.md` rather than by grepping the lines (the value is truncated
at a following trailer key, and a line-level count returns eleven). **Not one
of the ten names a path**: they are `ANTS-1721/1722`, `medium/low`,
`RC/release`, `green/red`, `L1/L2`, `vt-parser/grid` and their kin — prose
slashes inside a parenthetical. A per-token rule reports every one of them, so
the tidier rule costs ten new false notes and recovers nothing.

So the whole-value form stays, and the *reason* is the corpus rather than
elegance. **The accepted cost:** a genuinely missing path inside a prefix-led
value goes unreported. Measured instances of that on this corpus: zero. A false
positive here would cost a note and an `extras` key on a value that resolves to
nothing — not a refusal (below) — but ten of them is the noise class this
amendment exists to remove.

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
  remaining drift elsewhere. **A fifth contributor SHIPPED on 2026-08-19 and
  must be counted in, not held back:** a backticked example spanning a line
  break defeated the guard entirely — how ANTS-3808 imported a `lanes` value it
  never declared — and ANTS-4504 closed it by masking code spans (§ 5).
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
  whose bullet declares no `Kind:` **but does declare a `Source:`**, assert the
  result carries a note whose code is exactly `field_defaulted` and **whose
  detail names `kind` specifically** — one code for every defaulted field, not
  a per-field code. § 2.3 owns the shape; this clause previously said "a note
  whose **code** names the defaulted field", which reads as `kind_defaulted` /
  `source_defaulted` and is a different envelope contract for anything
  filtering on code.
  **Both halves of that fixture are load-bearing** (2026-08-10). Without the
  declared `Source:`, the bullet defaults *two* fields, and the run emits a
  `field_defaulted` naming `source` — which satisfies "a note whose code is
  `field_defaulted`" verbatim while the empty-`rawKind` branch stays silent,
  i.e. the assertion passes in exactly the state *Breaks when* describes. This
  is the same hole INV-7 carried and had repaired; the reasoning was never
  applied back to INV-1, where the note code originates.
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
  ``the `**Kind:** implement.` line`` both import with a defaulted kind. **The
  plain fixture asserts `provenance.kind == 'defaulted'` **and that
  `extras.source_kind` is absent**. Corrected 2026-08-13: `kind != 'trailer'`
  could not fail in the state *Breaks when* names, since dropping the lookbehind
  captures `` ` trailer``, which is unrecognised and defaults anyway. The
  bold one CANNOT assert on the
  value at all** — a defaulted kind *is* `implement`, so "defaulted" and
  "`implement`" are the same observation and the two outcomes are
  indistinguishable by value. Its discriminator is
  **`provenance.kind == 'defaulted'` and `extras.source_kind` absent**; with
  the guard broken the same bullet imports `kind='implement'` with
  `provenance.kind='asserted'`. Corrected 2026-08-10: the clause previously
  demanded "a defaulted kind" *and* `kind != 'implement'`, which are jointly
  unsatisfiable, so a literal implementation reds on correct code.
  *Breaks when:* the lookbehind is
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
  the 714-item drift § 1 measures. **Expected to fail ON THE CORPUS for
  `headline`, `layman` and `lanes` until that drift is diagnosed** (§ 2.6,
  deferred in § 5). `items_updated` here means
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
  imports successfully, with a **`unresolved_path`** note and
  `extras.unresolved_path`. *Test:* **two** fixtures — one citing
  `Source: docs/gone.md`, one citing `Evidence: docs/gone.md, docs/also-gone.md`
  — asserting `ok`, a note whose code is exactly `unresolved_path`, and the
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
- **INV-10** — **Amended 2026-08-19 (ANTS-4505): un-anchoring changes render
  suppression NOT AT ALL.** Suppression is line-initial presence (ANTS-3808
  § 2.3, which owns the rule), so a mid-prose `Kind:` never suppresses whatever
  its value, and un-anchoring moves only which mid-line matches are *found*.
  The invariant read *only where a mid-prose `Kind:` value equals the column's*,
  which is now false in the one direction it named. *Test:* four fixtures
  rendered — body with no `Kind:` text (emitted), body whose mid-prose `Kind:`
  value **differs** from the column (emitted), body whose mid-prose `Kind:`
  value **equals** it (**emitted too** — this is the fixture the amendment
  flips), and body declaring `Kind:` **line-initially** (suppressed, which is
  the only shape that does). `rxKind()` is shared with `bulletText()` via
  `trailerValuesIn()` (§ 2.2), so the parser change still reaches the render —
  it simply no longer changes its outcome. *Breaks when:* suppression is keyed
  on the match rather than on `anchored`, which drops a required trailer from
  any bullet whose body happens to discuss the label mid-sentence.
- **INV-11** — When a bullet contains more than one match for a trailer key, a
  **line-initial** match beats a mid-line one; the resolver takes the last
  line-initial match, and falls back to the last mid-line match only when there
  is no line-initial match at all. **This holds for all five keys** — `kind`,
  `source`, `layman`, `lanes`, `evidence` — not for `Kind:` alone.
  **For `kind`, § 2.2's recognised-vocabulary predicate filters the candidate
  set BEFORE that ordering applies** (settled 2026-08-13), so an unrecognised
  line-initial capture loses to a recognised mid-line one. Ordering alone is
  not sufficient; the two rules are one resolver and neither works without the
  other.
  *Test:* five fixtures. (a) body `… the old Kind: refactor. …` **followed by**
  trailer `Kind: security.` → `kind='security'`; green under plain last-match
  too, so it is a regression guard, not a discriminator. (b) trailer
  `Kind: security.` **followed by** a later note reading `… the canonical
  Kind: while the column …` → still `kind='security'`. (c) two line-initial
  matches, `Kind: implement.` then `Kind: fix.` → `kind='fix'`. (d) a bullet
  whose only match is mid-line → that value, proving the fallback still serves
  § 1's 55 inline-only declarations. (e) **the same shape as (b) for a
  first-match key**: a body whose line-initial `Source:` / `Layman:` trailer
  is *preceded* by an indented sample carrying those labels → the bullet's own
  values, not the sample's.
  **(f) the predicate's own fixture, added 2026-08-13.** (a)–(e) are all
  decided by `anchored` ordering, so a resolver with no predicate passes them
  all. (f) is ANTS-3754's shape: **two line-initial** matches, the earlier
  recognised (`Kind: doc.`) and the later an unrecognised wrap artefact
  (`Kind:), § 3.1's format marker,`) → `kind='doc'`, `extras.source_kind`
  absent. Ordering alone returns `), § 3`.
  *Breaks when:* precedence is dropped back to plain positional resolution.
  **Before ANTS-4497 that failed in opposite directions per key**, which is why
  one rule is stated for all five: `kind` was on last-match and displaced by a
  *later* prose mention, while `source`, `layman`, `lanes` and `evidence` were
  on first-match and displaced by an *earlier* one. Neither was hypothetical —
  five bullets in this project's roadmap carried a corrupt `kind` and ANTS-3808
  a wrong `source` and `layman` when this invariant was written. All five now
  route through `matchLastIn()`, so the surviving failure direction is the
  `later` one alone.
  **The boundary, re-measured 2026-08-10 against `collectBulletBody()`'s real
  stop conditions.** 16 bullets carry a second line-initial `Kind:`, each
  exactly two. In **13** the two values are identical, so the choice cannot be
  observed. In **ANTS-3808** and **ANTS-3787** the bullet's own trailer is the
  later one, which last-line-initial returns correctly; ANTS-3808 is fixture
  (e)'s real-world case. **In ANTS-3754 ordering alone returns garbage**
  (`), § 3`) — the wrapping defect below is a different mechanism, and it is
  what § 2.2's vocabulary predicate was added to close. Ordering does not fix
  it; the predicate does, by making the wrapped capture no candidate at all.
  **An earlier draft named ANTS-3573 and ANTS-3780 here as the below-the-trailer
  cases. Both were wrong**, artefacts of the superseded scan: ANTS-3573 carries
  exactly one `Kind:` (its body ends at a col-0 heading), and ANTS-3780 is a
  short bullet with an inline trailer and no nested entries. **That left the
  below-the-trailer case with no demonstrated instance — which the first real
  Phase D import then supplied.** Measured against the live store 2026-08-13,
  after Phase C shipped: **56** items carry more than one `Kind:` mention, **9**
  of them stored a prose capture in `extras.source_kind`, and **4 are confirmed
  wrong** — ANTS-3814 (`investigate` → `implement`), ANTS-1278 (`chore` →
  `implement`), ANTS-1866 and ANTS-3608 (both `doc-fix` → `implement`). Phase C
  changed the *shape* of the corruption rather than removing it: the raw
  fragment now lands in `extras.source_kind` and the field defaults to
  `implement`, which reads as a real answer. The remaining risk is the case with
  no marker at all — a prose capture that happens to BE a taxonomy word, silently
  taken. Nor did this invariant cover a
  backtick span crossing a line break, which defeated the guard entirely and
  was a separate mechanism — **closed 2026-08-19 by ANTS-4504**, which matches
  every key through a length-preserving mask built from
  `MarkdownScan::codeSpans()`.
- **INV-12** — A `Source:` token is a path reference only when it carries a
  directory separator **and** a filename-shaped final segment. A token failing
  either half yields no `unresolved_path` note and no `extras.unresolved_path`
  element. *Test:* six fixtures, each a bullet whose `Source:` is otherwise
  unresolvable, plus one control.
  **Separator missing** — (a) a bare filename that exists one directory down
  (`Source: in-session, per roadmap-format.md.`), (b) a bare filename that
  exists nowhere, (c) a token ending in a dot plus alphanumerics that is not a
  filename at all (`§4.4`, `~7.2ms`, `(0.6.0`).
  **Extension missing** — (d) an id pair (`… closes DOOM-0057/0081.`), (e) a
  slash-command name (`… raised during /cold-eyes.`), (f) a prose slash
  (`… a precision/convenience trade.`).
  **Control** — a bullet citing `docs/gone.md` in the same import still emits
  its note (INV-7 unchanged).
  *Breaks when:* the two tests are made a disjunction again — (a)–(c) red if
  the extension half alone can accept, (d)–(f) red if the separator half alone
  can accept. **Every fixture is a must-red against today's source**, which
  ships the OR-form: (a)–(c) on the extension disjunct, (d)–(f) on the
  separator disjunct.
  **(d), (e) and (f) are the corpus's three shapes, not invented ones.**
  Measured 2026-08-19 over `ROADMAP.md` and `docs/roadmap/*.md`: 9 id
  citations, 40 slash-command names, 46 prose slashes — 95 of the 97 tokens a
  separator-only rule accepts (§ 2.5).
  **Fixture (a) is the one that must not be "fixed" by a tree walk.** Resolving
  a bare filename by searching the project is undefined on the corpus that
  motivates it (§ 2.5), and the accepted cost is stated there: a `Source:`
  naming a genuinely missing file with no directory part goes unreported, as
  does one with no extension (`docs/specs`, `tools/Makefile`).

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
NULL`), and the path population is at most § 2.5's 150 occurrences — an upper
bound taken under the pre-amendment predicate, so the amended one validates
fewer. Both are bounded by the corpus, not by usage over time.

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
- **`Evidence:` stays anchored, and its inline declarations stay unread.**
  § 2.2 widens `rxKind()`, `rxLanes()` and `rxSource()` and deliberately leaves
  `rxEvidence()` with its `^\s*` anchor. By § 2.5's own count that leaves
  **18 of 22** corpus occurrences invisible to the import — a declared value in
  a governed column, silently dropped, which is § 1's defect class exactly. It
  is accepted here rather than fixed because widening a fourth matcher is a
  behaviour change on a field with no provenance entry (§ 2.4), so INV-1's
  `field_defaulted` note cannot fire for it and the loss would be untracked in
  a different way. **Whoever fixes it owes `evidence` a provenance entry
  first.** Recorded so § 2.5's measurement is not read as implying a repair.
- **A backtick span crossing a line break defeated the guard — CLOSED
  2026-08-19 by ANTS-4504, kept here because it was this spec's own filing.**
  The three lookbehinds in § 2.2 are fixed-length and inspect only the one to
  three characters before the match, so a quoted example broken across two
  source lines was read as a declaration. Live: ANTS-3808 declares no `Lanes:`
  and imported `lanes = ["packaging"]`, harvested out of
  `` `Source: regression. Lanes: `` / `` packaging.` ``. The repair is the
  multi-line-aware guard named here: every key is now matched through a
  length-preserving mask built from `MarkdownScan::codeSpans()`, with captures
  sliced from the unmasked text. **What is still open is the STORE**, not the
  parse — a column already holding a wrongly-harvested value is not corrected
  by re-reading (ANTS-4505).
- **A trailer key on a FENCED line is still read as a declaration.** ANTS-4504
  masks inline code spans only. It computes a fence mask because
  `codeSpans()` requires one, and deliberately does not blank fenced lines:
  there is no measurement of how many corpus bullets carry a fenced block with
  a trailer key in it, and that change moves corpus values in a second way on
  top of the one already shipped. Filed as **ANTS-4526**, not folded in.
- **`Evidence:` values that are not paths.** § 2.5 gives `Evidence:` no
  predicate, because `roadmap-format.md` § 3.5 defines every element as a
  path. The corpus disagrees: DOOM Ants' clean migration reports
  `user screenshot` and `E1M1 outdoor courtyard` as unresolved paths, and both
  are prose an author wrote into the field. Applying § 2.5's separator
  predicate here would silently drop real single-token evidence paths, so the
  repair belongs at the WRITE side — a `roadmap_log` refusal or a note when an
  `Evidence:` element is not path-shaped — and not in this contract. Filed as
  **ANTS-4527**.
- **Re-migrating the other 13 projects.** ANTS-3853 owns the rollout; this is
  the gate it waits on.

## 6. Tests

Feature test: `tests/features/roadmap_import_mapping/`, covering **INV-1 through
INV-12** — every one is a behavioural case; this spec carries no source-grep
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
leaves `offset == -1`, so `shadows()` is false and the trailer is emitted). **INV-6's shipped ctest case is GREEN and must stay green** — see INV-6, which
owns the distinction. The expected-red claim belongs to the **corpus**
(`headline`, `layman`, `lanes`, until § 2.6's drift is diagnosed), and Phase D
is what measures it; the registered fixture is a narrow regression guard, not
the measurement. An earlier draft of this line said the reverse — "expected to
red and stay red … it is the measurement, not a regression" — which read
literally instructs an implementer to land a permanently-red case and so make
the project suite permanently red.

**Amended 2026-08-10 (ANTS-4086 / ANTS-4076).** The six above are the must-red
set against **pre-Phase-C** source and are green now that Phase C has shipped;
they are left as written because the list records what the first
implementation had to prove. The amendment adds one further must-red case,
against **post-Phase-C** source:

- **INV-11 fixture (b)** — a trailer followed by a later note mentioning the
  label mid-line. Today's implementation takes the last match anywhere for
  `kind`, so it reds; that is the ANTS-4086 defect and its must-fail-first
  proof.
- **INV-11 fixture (e)** — the same shape for `Source:` / `Layman:`: a
  line-initial trailer *preceded* by an indented sample carrying those labels.
  **It was the must-fail-first proof for extending the rule beyond `Kind:`, and
  ANTS-4497 built that extension, so it is GREEN today and is now a regression
  guard.** Measured 2026-08-19 against the live parser: that body resolves to
  `source = "in-session-2026-08-19"` and `layman = "The bullet's own summary"`,
  the bullet's own values. An implementer must not read it as a must-red case
  and widen it until it fails.
- **INV-11 fixture (f)** — the predicate's only must-red proof. (b) is
  satisfied by `anchored` ordering alone, so without (f) the predicate could be
  omitted and the suite would stay green.

Fixtures (a), (c), (d) and (e) are expected **green**. (a), (c) and (d) always
were — plain last-match satisfies them, so they are regression guards rather
than discriminators. **(e) joined them on 2026-08-19**: it was the must-red
proof for extending the rule beyond `Kind:`, and ANTS-4497 built the extension,
so only (b) and (f) still red.

**Amended 2026-08-19 (ANTS-4502).** INV-12 adds its own must-red set, and it is
the § 2.5 half rather than the § 2.2 half. **All six fixtures red on today's
source**, which ships the OR-form and so accepts on either half alone:

- **INV-12 (a), (b), (c)** — red on the extension disjunct, which reports a
  bare filename and a `§4.4`-shaped token as paths.
- **INV-12 (d), (e), (f)** — red on the separator disjunct, which reports an id
  pair, a slash-command name and a prose slash as paths.

There is no green regression guard among them, which is what an AND of two
tests that were an OR should look like: every class the old rule accepted on
one half alone is now rejected, so every fixture discriminates.

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
  supported, and the standard should say so: **81 bullets in this project alone**
  write that shape — 55 mid-line only plus 26 carrying both, against 1,453
  line-initial only, from § 2.2's table. **Take the figures from that table, not
  from here**: an earlier draft said 1,435 / 99, measured with a scan that did
  not reproduce `collectBulletBody()`'s stop conditions, which § 2.2 records as
  measuring a different object. And § 3.5 must record that `Kind:` is now
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
| 9 | 2026-08-19 | 3, cold — identical brief, packet rebuilt from disk and extended with four measurements taken while verifying loop 8 (all facts about unchanged source) | **Q1 4 · Q2 3 · Q3 0 · Q4 0** — verified 7, fixed 7, dismissed 0 | (Q-counts) | **Cap reached (2 for a spec); the run files its tail and ships. A CALM cap: 2 of the 7 landed on text this run wrote, and the decisive finding is about the amendment's premise rather than its prose.** **All three lanes independently found the same defect, and it is loop 8's own collateral**: loop 8 anchored the id-citation test `\A…\z` against a RAW whitespace token, and the corpus writes `ANTS-2108/2109)` — 4 of this project's 9 id citations carry trailing punctuation, so the clause added to remove that note class would have left it firing. **Following lane B's open question then overturned the amendment itself**, which is the run's whole value. The drafted rule kept the separator and deleted the extension test, on the premise that *a reference that means a path says so with a separator*. Measured over 1,781 sourced bullets through the real parser: the separator-only rule accepts **97** tokens, of which **2** resolve. The other 95 are 40 slash-command names (`/cold-eyes` ×12, `/test-audit` ×8, `/audit` ×4 …), which are absolute and so can never resolve by construction; 9 id citations; and 46 prose slashes (`precision/convenience`, `4d/5`, `§A/§B`, `#7/#8`, `62/64`). **~98% false — worse than the rule it replaces.** English writes alternatives and ranges with a slash at least as often as it writes paths. The two tests were an OR and are now an **AND**: same corpus, same gate, **1 candidate, 1 resolves, 0 notes**, dropping exactly one real reference — `docs/specs`, a directory with no extension. **The AND-form also deleted two rules the draft needed**: the id-citation clause (an id pair has no extension) and the whole punctuation question that all three lanes raised. Loop 8's `\z` fix and its anchoring paragraph are gone with them — the sharpest illustration on this document of *delete first, write second*, since the rule they guarded should never have existed. **One more was loop 8's collateral:** closing § 5's backtick-span entry left § 2.6 holding it back as a pending contributor to `lanes` drift. **Four pre-existing, all Q1/Q2 census or tense drift:** § 2.2's ANTS-3808 worked case still said it *imports FOUR wrong values today* when ANTS-4497 and ANTS-4504 closed three of them hours earlier (only `kind` is live; the STORE is ANTS-4505); `rlDeriveTrailerColumns()` was said to see an off-taxonomy `Kind:` as *empty on both sides* where the resolver returns the raw last match; § 7 carried 1,435/99 against § 2.2's corrected 1,453/81, from a scan § 2.2 itself records as measuring a different object; and `52` survived in two places, one of them a test clause, against the table's 55. Doc 1301 → 1310 lines. **Deferred tail: none — every verified finding was fixed.** |
| 8 | 2026-08-19 | 3, cold — genre pinned `spec`, cap 2; packet rebuilt from disk with nine bounded source windows and an explicit note that the code still carries the OLD predicate | **Q1 5 · Q2 2 · Q3 1 · Q4 1** — verified 9, fixed 9, dismissed 0 | (Q-counts) | **The gate on ANTS-4502's § 2.5 amendment, run before any code. All three lanes independently found the same three defects**, which is the strongest signal the run produced. **The most consequential was not the amendment's**: § 2.2 and INV-11 still routed four keys through `matchIn()`, a function ANTS-4497 emptied and ANTS-4504 deleted *the same day, hours earlier* — so an implementer would have opened a function that no longer exists, and § 6's must-fail-first list said fixture (e) reds when it is green. Measured against the live parser rather than argued: that body resolves to the bullet's own `source` and `layman`. Collateral from a sibling item, landing in a document neither item names. **Three were the amendment's own 4a-min failure — a fix that did not delete the sentence it made wrong.** The census (`93 distinct path tokens across 150 occurrences`) is the OLD predicate's and its four worked examples carry no separator; a verification sentence still said `rpmlint.log warnings` is `correctly accepted` where the new rule four paragraphs above calls it prose; and an ordering paragraph rested on `the regex anchors on `$`` after the amendment deleted the only `$`-anchored regex. **The run's best finding was a false claim the amendment INHERITED and repeated**: § 2.5 has said since 2026-08-10 that the `not a recognised source form` conjunct `could never fire`. It fires constantly — `isRecognisedSourceForm()` is a whole-VALUE `startsWith` test and `pathTokensIn()` returns empty before tokenising. Proved by linking a probe against the shipped migrator: `Source: upstream-qt6, see docs/gone.md.` emits no note while the same path under a plain source emits one. **And then 4a step 3 reversed my own fix.** I had rewritten the rule to apply the test per TOKEN — tidier, and matching § 2.5's own unit. Running the refuting case first: ten `Source:` values in this corpus begin with a recognised prefix AND carry a `/`, and **not one names a path** (`ANTS-1721/1722`, `medium/low`, `RC/release`, `vt-parser/grid`), so the tidier rule costs ten new false notes and recovers nothing. The whole-value form is kept, on the corpus rather than on elegance. That measurement also corrected my own figure — a line-level grep said eleven, and the value is truncated at a following trailer key. **One Q3 that would have disabled the check silently:** `idTokenPattern()` is a bare unanchored fragment whose callers splice their own anchors, and the amendment said only `a token matching` it. Unanchored it matches inside `docs/specs/ANTS-3863-pre-read-dispatch.md` — the section's own qualifying example — so every spec path naming an id would go unvalidated with no fixture reding, INV-7's control carrying no id. Verified by running both forms; the rule now states `\A(?:…)(?:/\d+)*\z` and INV-12 gained fixture (f). **Two pre-existing:** the DOOM tally attributed all 15 notes to the extension disjunct when three causes produce them (`Ultra` and the sentence fragments match neither disjunct and reach the notes through `Evidence:`, now ANTS-4527), and a sentence called the `kind` corruption `a shape that may not occur` nine lines below four named confirmed-wrong items. Doc 1223 → 1301 lines; invariants 11 → 12. **Not converged; loop 2 owed, and the cap is 2.** |
| 7 | 2026-08-13 | 2, cold — genre pinned `spec`, cap 2. First run under the materiality gate (would the implementer build something different?) and the widened Q3, both mid-run corrections sent to the lanes | **Q1 1 · Q2 1 · Q3 1 · Q4 2** — verified 5, fixed 5, dismissed 2 | (Q-counts) | **The gate on ANTS-4086's settled resolver, and four of the five were that amendment's own collateral.** **The Q2 is the one that mattered:** § 2.2's new reject path said an unrecognised capture defaults "with the raw capture in `extras.source_kind`, which is what an absent field already does" — and an absent field does no such thing. `makeItem()`'s empty-`rawKind` branch emits `field_defaulted` alone and writes no `extras`; only the unmapped branch preserves the value and emits `kind_unmapped`. Left as written, the predicate would have made that branch unreachable for every parsed-but-unrecognised value, killing the `kind_unmapped` signal corpus-wide and reddening the shipped `BoldAndPlainKindLabelsAgree`. Settled the way § 2.4's discriminator and the shipped tests already require: the resolver returns the last match seen, raw, and the existing branch runs unchanged — **the predicate chooses among candidates, it never destroys evidence.** **Both Q4s say the amendment was unfalsifiable.** INV-11's five fixtures are all decided by the `anchored` ordering, so a resolver hard-wiring `isCandidate` to accept-everything passed every clause in the spec — fixture (f) is added as the predicate's only must-red proof. Lane A sharpened it decisively: § 2.2 described the case as an unrecognised line-initial capture losing to a recognised **non**-line-initial one, but ANTS-3754 is TWO line-initial matches, because `collectBulletBody()` trims continuation lines and the wrapped `Kind:), § 3.1's …` is line-initial exactly as the real `Kind: doc.` is. A fixture built from the old sentence would have tested a case ordering already handles and left the only measured instance uncovered. The second Q4: INV-3's plain fixture asserted `kind != 'trailer'`, which cannot fail in the state its own *Breaks when* names — with the lookbehind dropped the capture is `` ` trailer``, unrecognised, so the item defaults anyway; the discriminator is the absent `extras.source_kind`, which the shipped test already asserts, so the spec was weaker than the code it governs. **The Q3 is the widened form earning its keep**, and its mechanism was wrong as filed: lane A said the predicate would make `roadmap_log op:annotate` **clear** a stored `kind`. Verified against `rlDeriveTrailerColumns()` and it cannot — the clear is guarded by `oldValue.isEmpty() && newValue.isEmpty()`, and an off-taxonomy value is empty on both sides. The finding survives re-framed: the seam was *delegated* ("whoever implements the resolver owns this"), and a decision twelve migrating projects must agree on cannot be left per-builder. Answered instead — every consumer takes the predicate, and there it is a strict improvement, since today an appended note mentioning `Kind:` overwrites the stored value on a live user write. **Two dismissed, both already-fixed:** both lanes reported § 2.2 naming four `mappedKind()` inputs where there are fifteen, and lane A queried the value's case-folding. Both were caught by 4a step 3 before dispatch and fixed in `7c9d888f` — the lanes read a packet copy built one edit earlier, which is my error, corrected mid-run. Doc 1091 → 1163 lines. |
| 6 | 2026-08-10 | 1, cold — packet rebuilt from disk after loop 5, line count refreshed; no mention of loops 4–5 | **Q1 3 · Q2 0 · Q3 2 · Q4 2** — verified 7, dismissed 0 | (Q-counts) | **Exited at the 3-loop cap for this run. The loop found a defect in the rule loop 4 introduced, and falsified this document's own measurements.** (1) **The population figures were wrong, and so were the two bullets the carve-out rested on.** The scan behind "1,414 / 52 / 42 / 187" and "~20 nested-list bullets" ran bullet-to-bullet and did not reproduce `collectBulletBody()`'s other stop conditions — a col-0 heading, and any other col-0 content — so it attributed later sections' trailers to whichever bullet preceded them. Re-measured against the real conditions: **1,453 / 55 / 26 / 189**, and **16** bullets with a second line-initial `Kind:`, every one carrying exactly two. ANTS-3573 (one `Kind:`, body ends at a col-0 heading) and ANTS-3780 (short bullet, inline trailer, no nested entries) are **not** instances and were named in error; the below-the-trailer case now has **no demonstrated instance in this corpus**. (2) **[Q1, and the run's real finding] Line-initial is not a sufficient test for "is a trailer."** `collectBulletBody()` trims each continuation line, so a prose sentence that happens to **wrap immediately before the label** becomes line-initial. ANTS-3754's body line 86 is `Kind:), § 3.1's format marker…` because the source wrapped after `(emoji, ID, bold headline,`; its real `Kind: doc.` is 22 body lines earlier, so last-line-initial returns `), § 3`. Wrapping is arbitrary and re-wraps on any reflow, so this is not a source fix. **Surfaced, not applied** — a candidate tightening (require the captured value to begin with a letter) is a parser-behaviour decision, and the section now tells an implementer not to build the resolver until it is settled. (3) **[Q1] § 2.1 said four canonical mappings were still missing from `mappedKind()`.** They shipped in this spec's own Phase C: the function carries **fifteen** rows, the last four being `bug`, `performance`, `process + tooling`, `audit`. An implementer following § 2.1 re-opens shipped work. The table is kept as the evidence that justified them. (4) **[Q2, folded] The three compounds** were "deferred, § 5 carries the ruling" in § 2.1 and "closed in Phase B3, **not** deferred" in § 5 — the table's three "ruling needed" cells now read "no rule — closed". (5) **[Q3] `trailerValuesIn()` has four consumers, not two**, and `rlDeriveTrailerColumns()` **writes or clears store trailer columns** on `roadmap_log` append/annotate/amend_body — a live user-facing write path the section had not named. (6) **[Q4] INV-3's bold fixture** demanded "imports with a defaulted kind" **and** `kind != 'implement'` — jointly unsatisfiable, since a defaulted kind *is* `implement`; the discriminator has to be `provenance.kind`. (7) **[Q4] INV-1's fixture passes while the branch it guards stays silent**: a bullet declaring no `Kind:` usually declares no `Source:` either, so a `field_defaulted` note naming `source` satisfies it verbatim — the same hole INV-7 had repaired one loop earlier, never applied back. **Items 6 and 7 were fixed in this loop after all** — both are one-clause corrections with no design question in them. Doc 960 → 1053 lines. |
| 5 | 2026-08-10 | 1, cold — same shared-context file rebuilt from disk after loop 4's fixes, line count refreshed; no mention of loop 4 | **Q1 2 · Q2 3 · Q3 1 · Q4 0** — verified 5, dismissed 0 | (Q-counts) | **Four of the five were loop 4's own collateral, which is the loop working exactly as intended — and one is the sharpest kind: a fact loop 4 INVENTED.** § 2.2's new paragraph argued against editing `matchIn()` partly because it "is the first-match helper other callers rely on". It has no such callers: its only four call sites are inside `trailerValuesIn()`, so once they move it is unreferenced dead code. Loop 4 asserted a claim about the codebase from plausibility instead of opening the file — the exact failure the verify-before-writing rule exists to stop — and the correction now also tells the implementer to delete it. **Three more were repairs loop 4 made in one place and not the other:** § 6 still carried the inverted "INV-6 is expected to red and stay red … it is the measurement, not a regression" after INV-6 itself was corrected to the opposite, so the two passages were a word-for-word inversion prescribing opposite actions (land a permanently-red ctest case, or don't); INV-11 grew fixture (e) while its own exclusion clause still said the indented-sample shape was not covered, so the invariant tested what it declared out of scope; and § 2.2 called ANTS-3808 "three wrong values" with "its `kind` survives only by accident" while the amendment 40 lines below listed that same bullet among five with a corrupt `kind`. It is four, and the bullet carries **both directions** of the defect — which is what makes it the worked case, so the correction strengthens the argument rather than weakening it. **One genuine draft defect:** `defaulted_fields` was described as a "run-level tally … beside `notes_count`" (scalar) and as a per-field complete count in the next sentence — two different JSON shapes for one key, with no invariant pinning either. Resolved against shipped code rather than by preference: `roadmapmigrateverb.cpp` assigns `defaultedFieldTally(plan)` and the shipped test reads it with `.toObject()`, so the spec was ambiguous where the code was not. **Blast-radius sweep before this dispatch caught three further self-inflicted breaks** loop 4 had left — § 2.5 and § 2.2 both cited § 5 for items § 5 did not contain (the `Evidence:` anchored loss and the line-break guard gap, both now written there), and § 6 accounted for four INV-11 fixtures after it grew to five, missing that (e) is a must-red failing in the OPPOSITE direction to (b). Doc 896 → 959 lines. **Not converged; loop 6 owed, and the collateral share is the reason to expect it to be short.** |
| 4 | 2026-08-10 | 2, cold — one byte-stable shared-context file, both lanes given the same path; scrubbed doc copy with the loop log withheld; verified-facts block carrying nine source facts read during packet assembly | **Q1 3 · Q2 5 · Q3 4 · Q4 0** — verified 12, dismissed 2 | (Q-counts; the C/H/M/L/I column is the retired scale) | **Re-gate after an authoring amendment (ANTS-4086 + ANTS-4076), not a fresh review.** Both lanes independently led on the same defect, and it is one the amendment itself introduced by inheriting a false premise from loop 3's row: **§ 2.2 said `trailerValuesIn()` resolves `kind` through `matchIn()` (first match). It does not** — it calls `matchLastIn()` for `kind` **only**, while `layman`/`lanes`/`evidence`/`source` use `matchIn()`. Loop 3 verified the wrong half and wrote the wrong function into the spec; loop 4's fix names both and says which function to edit, because an implementer following the old text edits `matchIn()` and changes four fields while leaving the one the section is about untouched. **The run's largest finding, verified empirically rather than by reading:** the amendment fixed precedence for `Kind:` alone, leaving the identical defect live on four other governed columns — and on first-match keys it is worse, since *any* earlier mention wins. Demonstrated on ANTS-3808 in this project's own roadmap, which embeds an illustrative sample bullet above its real trailer and therefore imports `source = "test."` and `layman = "An older thing."` today. The rule now governs all five keys through one shared resolver (user decision, taken on that evidence). A third mechanism surfaced in the same check and is deliberately **out of scope**: ANTS-3808 also imports `lanes = ["packaging"]` while declaring no `Lanes:` at all, harvested from a backticked example spanning a line break — the guard is a fixed-length lookbehind and cannot see across it. Filed, not fixed. **Also fixed:** § 2.5's path predicate, whose "final segment" was still undefined for a value with no slash *despite loop 3 recording it as fixed* — now a whitespace-token predicate, and its unbuildable "not a recognised source form" conjunct deleted after running the predicate over § 3.5.3's forms proved none can reach it; INV-7's note code was unnamed, alone among this spec's five notes, so its clause could not fail — now `path_unresolved`, with an `Evidence:` fixture added since the array half was untested; INV-1 and § 2.3 disagreed on whether the field name lives in the note's code or its detail; § 2.5 claimed § 2.2 repairs `Evidence:`'s inline blind spot when § 2.2 deliberately leaves `rxEvidence()` anchored, losing 18 of 22 corpus occurrences; § 2.6 ruled § 2.2 out of `lanes` drift on the un-anchoring alone while § 2.2 makes two *other* changes to `rxLanes()` that move values; INV-11 fixture (a)'s ordering parenthetical contradicted § 6's must-red list; and INV-6's "expected to red and stay red" was contradicted by its own shipped case being green — the spec conflated the corpus measurement with the ctest fixture, now separated. **Dismissed (2):** § 2.2's closing "a real trailer always wins" reads as contradicted by the amendment but is scoped to prose mentions, which the amended rule does defeat; and § 2.1-vs-§ 5 on whether the compound `Kind:` values were deferred or closed — a real contradiction, but both readings produce identical code, so it changes no line. Doc 749 → 896 lines. **Not converged; loop 5 owed.** |
| 3 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loops 1–2; packet's verified-facts block extended with loop 2's four source facts, and its stale "8 invariants" line corrected to 10 | C 3 · H 5 · M 8 · L 9 · I 0 — verified 22, dismissed 0 | dim 5×6, dim 2×5, dim 4×5, dim 15×4, dim 6×4, dim 7×2, dim 1×1, dim 9×1, dim 11×1, dim 12×1 | **Converged by cap. Both lanes independently found the same defect loop 2 introduced, which is the clearest possible signal that the cap is the right stop. (1) CRITICAL, both lanes: § 2.6 booked `lanes` drift as fixed by § 2.2's un-anchoring — a mechanism that cannot touch it, since `rxLanes()` was already un-anchored by ANTS-2058 (verified at `src/roadmapparse.cpp:296`) and the two matchers are independent. One of the four gate columns was accounted for by fiction. Moved to the undiagnosed set with `headline` and `layman`, in § 2.6, § 5 and INV-6. (2) CRITICAL, lane B, and the deepest read of the whole run: un-anchoring makes **match precedence** load-bearing and the spec never stated it. Verified — `trailerValuesIn()` calls `matchIn(rxKind(), body)`, a single `match()` taking the **first** occurrence, and `bulletText()` appends the body *before* the trailer. So on a rendered bullet a stale mid-prose `Kind:` sits ahead of the canonical one and a re-import would adopt it, breaking INV-6 on a governed column the spec believed safe. § 2.2 now fixes the rule — **the trailer wins, the parser takes the last match** — and new **INV-11** pins it. (3) CRITICAL, lane A: `assertedSource` was used in § 2.4's render condition and never defined. The store declares `provenance NOT NULL DEFAULT '{}'`, so a row written by anything but `makeItem()` — `roadmap_log op:append`, for one — carries no `source` key; read as `== "asserted"` every such row silently loses its `Source:` line, which is precisely the loss this spec exists to stop. Now defined as `!= "defaulted"`, with the direction called out as the thing to get right. **HIGH ×5:** § 2.4's claim that `extras.source_kind` distinguishes defaulted from mapped is contradicted by the spec's own § 1 measurement — 438 of 476 lack it, so 38 defaulted kinds *do* carry one, written by the unmapped-value branch; the must-fail-first list said five where INV-10's equal-value fixture also reds on today's anchor, making six; a capitalised `Kind:` in prose survives the case-sensitivity fix and the residual exposure was unstated (now accepted explicitly, with the last-match rule as the limiter); the note code, envelope tally key and `extras.priority` key were all unnamed while two sibling `extras` keys were named, so two implementers would emit incompatible envelopes and neither would fail its test (`field_defaulted` / `defaulted_fields` now named); and § 6 disagreed with § 2.6 about whether `layman` is expected red. **MEDIUM ×8:** INV-8's fixture said the malformed marker "refuses **or** defaults", a disjunction no test can assert — now defaults to `planned` with a note; § 2.1's "corpus figures throughout" mislabelled a table whose counts are this project's pre-render file plus archives, not the 14-project survey; `dropped`'s precondition named the round-trip where the real blocker is the render's inability to express a fifth status; "`Kind:` always renders" contradicted INV-10 and `bulletText()`'s own shadow suppression; § 2.3's completeness rule had no invariant behind it; and the § 2.5 predicate's "final segment" was undefined for a value with no slash. **LOW ×9**, including a dangling sentence fragment my own INV-8 edit created and Phase 4c caught before the commit. **Cap reached with three findings filed rather than fixed** — see the deferred tail in the run report: the `Priority:` and `Evidence:` corpus counts want re-measuring against the pre-render source (both were taken from the contaminated survey), and § 4 budgets no filesystem-stat cost for § 2.5's 150 path checks. None is build-changing; all three are recorded so nothing is lost by stopping. Doc 502 → 551 lines; invariants 10 → 11. |
| 2 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loop 1; packet's verified-facts block extended with the six source facts loop 1's verification established | C 2 · H 5 · M 8 · L 9 · I 0 — verified 24, dismissed 0 | dim 5×7, dim 2×5, dim 6×5, dim 15×4, dim 7×3, dim 1×2, dim 10×2, dim 4×1, dim 8×1, dim 11×1 | **Origin split: 10 of 13 distinct defects were collateral from loop 1's own fixes, 3 were draft defects — a decisive margin, so the loop-economics trigger fired and this pass ends with a consolidation sweep rather than a reflex dispatch. (1) CRITICAL, lane A, and it falsifies a guarantee loop 1 introduced: excluding `extras` from § 2.6's governed set means the first render→import cycle **destroys** `extras.source_kind`. Verified directly — `src/roadmaprender.cpp` contains no `extras` reference at all, so the original value cannot survive a regeneration. That makes loop 1's "no map is lossy and a bad map is reversible" false on any migrated project. Rewritten to promise **one-shot, not durable** reversibility, with both ways of making it durable named and both declined, because claiming a guarantee the render cannot keep is worse than claiming none. (2) CRITICAL, lane A: § 2.4 proved `items_updated == 0` unsatisfiable and then "resolved" it by excluding `provenance` from the governed set — but `items_updated` is a **row** counter, so excluding a column does not stop the row counting. The remedy did not reach the assertion it was written for. § 2.6 now defines `items_updated` over governed columns and INV-6 says which counter it means. (3) HIGH, lane B, and the sharpest read of the run: loop 1's § 2.2 claimed un-anchoring makes "a body mentioning `Kind:` mid-prose shadow the trailer". `shadows()` is `m.offset >= 0 && m.value == v` — **value equality**, as its own comment states — so a body that merely mentions the label never shadows. INV-10 inherited the error and tested an unreachable state. Both restated to value equality, and INV-10 grew to three fixtures (no mention / differing value / equal value). (4) HIGH, lane B: § 2.6 named four drifting fields, accounted for three, and **silently dropped `layman`** — in the paragraph whose stated purpose is that no governed column goes unexplained. Now deferred alongside `headline`, in § 2.6, § 5 and INV-6. (5) HIGH, both lanes: § 2.1 enumerated all eleven § 7.4 mappings and then said in the next sentence that restating them "would create a second mapping free to diverge". The spec did the thing it forbade, in the paragraph forbidding it. Enumeration deleted; the count and the pointer remain. (6) HIGH, lane A: the three compound `Kind:` values carried "**ruling needed**" with no interim behaviour and no § 5 entry, leaving an implementer with no contract at all for them. They now fall through the unmapped branch explicitly and § 5 owns the ruling. **MEDIUM ×8:** the path predicate's `A or B and C` needed parentheses (both lanes, independently); `Evidence:` is multi-valued so `extras.unresolved_path` must be an array and every element is a path by definition; § 2.3's "surfaces all 476" contradicted § 4's `notes_truncated`, resolved by making the per-field **count** complete while the list stays a sample; INV-8 as written was a tautology — § 2.1 says `dropped` has no emoji, so no fixture can request it — and now tests that the emoji→status mapping is total and closed; INV-7 was missing from § 6's must-red list though § 2.5's validation does not exist in source; § 2.4's heading said "a defaulted field is not rendered" against its own `Kind:` conclusion; the `MultilineOption` rationale was wrong (it only affects `^`/`$` and is inert once the anchor goes); and "the six the standard defines" is five trailer keys. **LOW ×9**, all fixed, including 859 notes relabelled as a pre-fix upper bound. **Consolidation sweep (trigger response):** the eleven-mapping restatement deleted outright rather than reconciled — the anti-pattern the collateral margin exists to catch. Doc 448 → 502 lines. |
| 1 | 2026-08-08 | 2, cold — identical byte-stable shared packet (~12k tok) carrying bounded windows of `roadmapparse.cpp` / `roadmapmigrate.cpp` / `roadmaprender.cpp`, the store's CHECK constraints, `roadmap-format.md` §§ 3.5/3.5.3, and the five cited ANTS ids resolved via `roadmap_query` | C 3 · H 6 · M 6 · L 6 · I 0 — verified 20, dismissed 1 | dim 7×5, dim 2×5, dim 4×4, dim 5×4, dim 6×4, dim 15×2, dim 11×2, dim 9×1, dim 10×1, dim 1×1 | **Both lanes led on the same two contradictions, and verification found a third defect in the spec's own evidence. (1) CRITICAL, both lanes: § 2.4's render rule — "emits a key only when `provenance` marks that field `asserted`" — would have suppressed `Layman:`, `Lanes:` and `Evidence:` on **every** bullet, because `makeItem()` writes provenance for `id`/`kind`/`source` and nothing else, so those three never carry one. An implementer following it ships a larger data loss than the one the spec exists to stop. Now scoped to the two defaultable fields, with "absent provenance means not-a-defaultable-field, never defaulted" stated outright. (2) CRITICAL, both lanes: the `Kind:`-always-renders exception makes INV-6 unsatisfiable by construction — a defaulted kind renders `Kind: implement.`, re-imports through the canonical branch as `asserted`, and provenance flips on all 476 rows at the first round trip. Resolved by excluding `provenance` from the governed set and saying why, rather than by inventing a machine marker in a human-facing file. (3) CRITICAL: § 2.1 restated a mapping that **already exists and is normative** — `roadmap-data-model.md` § 7.4 carries it and `mappedKind()` implements exactly its eleven entries — and re-opened three of those as "ruling needed" (`behaviour-change`, `perf / fix`, `perf / optimize`), which would have had an implementer either stall or overwrite a shipped decision. The duplicate table is deleted; § 2.1 now points at § 7.4 and contributes only what it misses. **Found by verification, not by either lane, and it corrects the spec's own evidence:** § 7.4's "11 others" and this spec's corpus figures both came from `tools/roadmap-corpus-survey.py` run *after* the first store render — by which point the render had already rewritten `Kind: bug` to `Kind: implement` in the file being surveyed. Re-running the inventory against the pre-render source surfaces **seven** unmapped values the table misses, led by `bug` at 29 items, the single largest unmapped value in the corpus and invisible to the contaminated run. **HIGH ×6:** § 1 described `bulletText()` as emitting `Source:` "unconditionally" when it is doubly conditional (`!isEmpty() && !shadows(...)`, ANTS-3808) — the symptom was right and the mechanism wrong; the header said 363 where the store measures 383; the kind table claimed "11 others" above 17 rows; `rxKind()` is **shared with the render** via `trailerValuesIn()`, so un-anchoring reaches `shadows()` and the spec called it "one regex literal" (new INV-10); dropping `CaseInsensitiveOption` silently reverses shipped ANTS-3407 (new INV-9, and § 7 now records it); and § 1's four drifting round-trip fields were diagnosed and then never addressed, leaving INV-6 unreachable — `headline` is now named as undiagnosed rather than left for an implementer to discover at the gate. **MEDIUM ×6:** `priority` had no table despite § 2.1's own rule and is now deferred to § 5 with the column left NULL; "looks like a path" was undefined and is now a stated predicate; the governed column set is enumerated (nine columns, three exclusions with reasons); the note budget (859 on the first run) is priced; INV-8's source-grep could not fail and is behavioural; and "448 non-standard keys" is 448 distinct *including* the six standard ones. **LOW ×6**, all fixed, including the "20+ path references" estimate — measured at **93 distinct tokens across 150 occurrences**, an eyeballed undercount from a truncated list. **Dismissed (1):** both lanes suspected `ItemWrite` carries no `provenance` member, which would have made § 2.4 unimplementable. It does — `QJsonObject provenance` at `src/roadmapstore.h`, and `bulletText()` already takes an `ItemWrite &`, so the render holds it. § 4's "no new state" is correct as written. Doc 301 → 448 lines; invariants 8 → 10. |
