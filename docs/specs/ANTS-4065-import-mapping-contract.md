# ANTS-4065 — define the markdown→store import mapping, so importing neither loses nor invents a field

**Status:** spec draft (2026-08-08).
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
projects (4,377 items).

**`kind` — the mapping already exists and is normative; this spec extends it,
it does not restate it.** `roadmap-data-model.md` § 7.4 carries the table
("the migration-scoped mapping is normative") and `mappedKind()`
(`src/roadmapmigrate.cpp`) implements exactly its eleven entries: `improve` and
`enhance` → `enhancement`, `docs` → `doc`, `bugfix` → `fix`, `testing` → `test`,
`spike` → `research`, `feat` → `feature`, `perf / fix` and `perf / optimize` →
`perf`, `tooling` → `chore`, `behaviour-change` → `enhancement`. **Restating
those eleven here would create a second mapping free to diverge from the first,
so § 7.4 stays their only home.**

**§ 7.4's table is incomplete, and the reason it looks complete is a measurement
artefact this spec has to correct.** Its "11 others" was derived from a corpus
survey run *after* this project's first store render — by which point the render
had already rewritten `Kind: bug` to `Kind: implement` in the file the survey
read. Re-running the inventory against the pre-render source
(`git show 6d9e743d:ROADMAP.md`, plus both archives) surfaces seven values the
table and `mappedKind()` both miss:

| Corpus value | Count | → | Rationale |
|---|---|---|---|
| `bug` | 29 | `fix` | the single largest unmapped value in the corpus, and invisible to the contaminated survey |
| `performance` | 2 | `perf` | long form of a canonical value |
| `process + tooling` | 1 | `chore` | § 3.5.3's "housekeeping"; `tooling` already maps there |
| `audit` | 1 | `audit-fix` | the canonical name for the same work |
| `feature/fix` | 1 | **ruling needed** | compound; the column is single-valued |
| `design + implement` | 1 | **ruling needed** | compound |
| `design + fix` | 1 | **ruling needed** | compound |

The four with a rationale are mechanical and this spec adopts them. The three
compounds need a ruling because the column admits one value and discarding half
a declared intent is exactly the loss § 1 exists to stop — **`extras.source_kind`
is what makes that ruling safe rather than lossy**, since the original string
survives whichever half is chosen.

**A mapped value keeps its original.** `makeItem()` already writes
`extras.source_kind` on the mapping branch; this contract makes that mandatory
for every non-identity mapping, so no map is lossy and a bad map is reversible.

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

**`priority` is a type conflict, not a value map — and it is deferred to § 5
rather than half-specified here.** The corpus writes `Priority:` 88 times as
prose (`CRITICAL`, `HIGH`); the column is `INTEGER 1..5`. A mapping needs a
severity vocabulary this project has never written down — no doc defines the
set, and the direction (is `CRITICAL` 1 or 5?) is a convention, not a
derivation. Import therefore **leaves `priority` NULL and preserves the string
in `extras`**, which loses nothing and invents nothing; choosing the scale is
its own item.

### 2.2 Un-anchor `rxKind()`, with the guard its sibling already has

`rxKind()` drops the `^\s*` anchor, exactly as `rxLanes()` did under ANTS-2058,
and gains ANTS-3722's negative lookbehind so a bullet *quoting* the label is not
read as declaring it:

```cpp
// was: "^\\s*Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]"
//      MultilineOption | CaseInsensitiveOption
QStringLiteral("(?<!`)Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]")
//      MultilineOption      — retained: the capture still stops at \n
//      CaseInsensitiveOption — DROPPED, see below
```

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
compute ANTS-3808's `shadows()` suppression. Un-anchoring therefore widens what
the render considers "already in the body" — a body mentioning `Kind:` mid-prose
now shadows the trailer where before it did not. INV-10 covers it.

### 2.3 A defaulted field is always noted

`makeItem()`'s empty-`rawKind` branch gains the note its unmapped-value sibling
already emits. The rule generalises to every field the import may default:

> **Import may default a field. It may not default one silently.**

Each default emits a note naming the field and the bullet's line, so a migration
report shows what the import supplied rather than what the document said. This
is the check that would have surfaced all **476** defaulted kinds on the first
run — not merely the 438 that preserve no original, since the note fires on the
default itself rather than on whether anything survived it.

### 2.4 A defaulted field is not rendered

**Scoped to the two fields import can default — `kind` and `source` — and to
those only.** `makeItem()` writes `provenance` for `id`, `kind` and `source` and
for nothing else, so a rule phrased over "any field whose provenance is not
`asserted`" would suppress `Layman:`, `Lanes:` and `Evidence:` on *every*
bullet, none of which ever carries a provenance entry. That would destroy three
fields `roadmap-format.md` § 3.5 defines — a larger loss than the one this spec
exists to stop. **Absent provenance means "not a defaultable field", never
"defaulted".**

For `source`: `bulletText()`'s existing condition gains one term, becoming
`!it.source.isEmpty() && !shadows(...) && assertedSource`. This is **additional
to** ANTS-3808's `shadows()` suppression, not a replacement for it.

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
- **Exclude `provenance` from INV-6's governed set** (§ 2.6 enumerates it), and
  accept that a defaulted kind becomes asserted once rendered. The information
  is not lost — `extras.source_kind` is absent for a defaulted kind and present
  for a mapped one, so the two stay distinguishable — and § 2.3's note is
  emitted at the import that *did* the defaulting, which is the run where it
  matters.

### 2.5 A field naming a file is validated at import

`Source:` names a path far more often than the standard anticipates: **93
distinct path tokens across 150 occurrences** in this project's pre-render
roadmap and archives, applying the predicate below
(`*_Ants_MCP_Feedback.md`, `remotecontrol.cpp`, `terminalwidget.h`,
`rpmlint.log`). Meanwhile `Evidence:` — the field `roadmap-format.md` § 3.5
designates for paths — is used **once** across all 4,377 corpus items. The
convention the standard describes and the one the corpus practises are not the
same convention.

**"Looks like a path" is a predicate, not a judgement**, because § 3.5.3's own
`Source:` vocabulary is full of hyphenated tokens (`upstream-<dep>`,
`external-CVE-NNNN-NNNN`) that must not be mistaken for filenames. A value is a
path reference when it contains `/`, **or** its final segment matches
`\.[A-Za-z0-9]{1,5}$` — an extension — **and** it is not one of § 3.5.3's
recognised source forms. `user-2026-08-08` has no slash and no extension;
`rpmlint.log` has an extension; `docs/specs/ANTS-3863-pre-read-dispatch.md` has
both.

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
drifting); `extras` (§ 2.1's `source_kind` is written on the mapping branch and
a canonical value re-imports without it).

**§ 1's four drifting fields are not all addressed by this contract, and saying
so is the difference between a gate and a wish.** The measured drift named
`headline`, `layman`, `lanes` and `extras`. `lanes` and `extras` are explained
above — un-anchoring `rxKind()` (§ 2.2) is expected to fix a share of the
`lanes` conflicts for the same reason ANTS-2058 fixed the rest. **`headline`
drift has no diagnosis in this spec**, and INV-6 cannot pass while it stands, so
it is called out here rather than discovered at the gate: whoever implements
this measures it first (§ 6's INV-6 fixture is the instrument) and either folds
the cause in as a § 2.x or splits it out. Naming it as unfinished is the point —
the alternative is an implementer building § 2.2 through § 2.5 in full and
finding the acceptance test still red with nothing to work from.

## 3. Invariants

- **INV-1** — No import defaults a field without emitting a note naming that
  field. *Test:* `tests/features/roadmap_import_mapping/` — import a fixture
  whose bullet declares no `Kind:`, assert the result carries a note whose code
  names the defaulted field. *Breaks when:* a branch assigns a default and
  returns without `addNote`, which is the exact shape of today's empty-`rawKind`
  path.
- **INV-2** — A bullet declaring `Kind:` inline, not at line start, imports with
  that kind. *Test:* fixture bullet `**H.** Body text. Kind: security.` imports
  as `kind='security'` with `provenance.kind='asserted'`. *Breaks when:* the
  pattern is re-anchored, which is the state this spec is written against —
  the test fails on today's source and that is the must-fail-first proof.
- **INV-3** — A bullet *quoting* the label does not declare it. *Test:* fixture
  body containing ``the `Kind:` trailer`` imports with a defaulted kind, not
  `kind='trailer'`. *Breaks when:* the lookbehind is dropped while un-anchoring
  — the regression ANTS-3722 already paid for once on `rxLanes()`.
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
  the 714-item drift § 1 measures. **Expected to fail on `headline` until that
  drift is diagnosed** (§ 2.6); the fixture is the instrument for diagnosing it,
  so the invariant is written now and lands red on purpose.
- **INV-7** — A `Source:`/`Evidence:` value naming a path that does not exist
  imports successfully, with a note and `extras.unresolved_path`. *Test:*
  fixture citing `docs/gone.md`; assert `ok`, the note, and the extras key.
  *Breaks when:* validation is written as a refusal, which would make a
  historical roadmap unimportable.
- **INV-8** — Import never writes `status='dropped'`. *Test:* behavioural, not
  a grep — import a fixture carrying every status emoji plus a bullet with a
  malformed marker, then assert `SELECT COUNT(*) FROM item WHERE
  status='dropped'` is 0. A source-grep was the first draft of this clause and
  is unusable: it cannot tell an assignment from a comment, and a status
  reaching the row through a variable passes it. *Breaks when:* the fifth status
  is wired in before the render can express it, producing a row that cannot
  survive its own regeneration.
- **INV-9** — A lowercase `kind:` label does not parse as a declaration.
  *Test:* import a fixture bullet `**H.** Body. kind: security.`; assert the
  kind is defaulted with a note, not `security`. This pins § 2.2's deliberate
  ANTS-3407 reversal so it is tested rather than assumed. *Breaks when:*
  `CaseInsensitiveOption` is restored alongside the un-anchored pattern, which
  re-admits the prose match ("…changed the kind: of work…") that dropping the
  anchor exposes.
- **INV-10** — Un-anchoring does not change what the render suppresses for an
  item whose body does **not** mention the label. *Test:* render an item whose
  body has no `Kind:` text; assert the `Kind:` trailer is still emitted, before
  and after the pattern change. `rxKind()` is shared with `bulletText()` via
  `trailerValuesIn()` (§ 2.2), so a parser change reaches the render.
  *Breaks when:* the widened match makes `shadows()` fire on a body that merely
  discusses the label, silently dropping a required trailer.

## 4. RAM / build cost

**No new target, and no new stored state.** `RoadmapStore::ItemWrite` already
carries `provenance` as a `QJsonObject` (`src/roadmapstore.h`) and `bulletText()`
already takes an `ItemWrite &`, so § 2.4 reads a field the render is holding —
one JSON lookup per bullet, against a render that already walks every one. No
struct gains a member and no query changes.

**The notes are the one thing that grows, and they are bounded per run, not
cumulative.** INV-1 emits a note per defaulted field: for this project's first
import that is 476 + 383 = **859** notes. They live in the migration's response
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
  The survey counts **448 distinct keys**, which includes the six the standard
  defines — so roughly 442 are extensions. Deciding which deserve real columns
  (`Dependencies` 98, `Acceptance` 44, `Scope` 42) is a data-model change, not
  an import mapping.
- **The `priority` severity scale** (§ 2.1) — import leaves the column NULL and
  keeps the string. Choosing the vocabulary and its direction is its own item.
- **`headline` round-trip drift** (§ 2.6) — named, measured and undiagnosed.
  INV-6's fixture is the instrument; whoever runs it owns the follow-up.
- **Back-filling `Kind:` onto the 1,613 corpus items that carry none.** They
  default legitimately under § 3.5.3's own rule; INV-1 makes the default
  visible, which is all this item owes them.
- **Re-migrating the other 13 projects.** ANTS-3853 owns the rollout; this is
  the gate it waits on.

## 6. Tests

Feature test: `tests/features/roadmap_import_mapping/`, covering **INV-1 through
INV-10** — every one is a behavioural case; this spec carries no source-grep
invariant, INV-8 having been rewritten as one after the grep form was found
unable to fail. Label `features;fast` — every fixture is a few-line roadmap, so
nothing here needs the `perf` label.

Per the project test convention, **verify each case fails against pre-change
source first**. Four must red on today's code, and they are why this spec is
written before the fix rather than after: **INV-2** (`rxKind()` is anchored),
**INV-5** (`bulletText()` renders from the value, not the provenance),
**INV-9** (`CaseInsensitiveOption` is still set) and **INV-1** (the empty-kind
branch emits no note). **INV-6 is expected to red and stay red** on `headline`
until § 2.6's undiagnosed drift is resolved — it is the measurement, not a
regression.

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
- **`docs/standards/roadmap-data-model.md` § 7.4** — **already the normative home
  of the kind mapping**, so this spec adds to it rather than giving it one: the
  seven values of § 2.1 that its eleven miss, and a note that its "11 others"
  figure came from a post-render survey and therefore cannot see `bug`. § 2.3's
  defaults-are-noted rule lands here too.
- **`ROADMAP.md`** — ANTS-4063 (fabricated `Source:`) is discharged by INV-5,
  and ANTS-4062 (off-taxonomy `Kind:`) by § 2.1; both flip when this ships.
- **`CHANGELOG.md`** — one `Fixed` entry; the import losing declared fields is
  user-visible to anyone who migrates.
- **`CLAUDE.md`** — no change. The module map names subsystems, not field maps.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Dimensions | Outcome |
|---|---|---|---|---|---|
| 1 | 2026-08-08 | 2, cold — identical byte-stable shared packet (~12k tok) carrying bounded windows of `roadmapparse.cpp` / `roadmapmigrate.cpp` / `roadmaprender.cpp`, the store's CHECK constraints, `roadmap-format.md` §§ 3.5/3.5.3, and the five cited ANTS ids resolved via `roadmap_query` | C 3 · H 6 · M 6 · L 6 · I 0 — verified 20, dismissed 1 | dim 7×5, dim 2×5, dim 4×4, dim 5×4, dim 6×4, dim 15×2, dim 11×2, dim 9×1, dim 10×1, dim 1×1 | **Both lanes led on the same two contradictions, and verification found a third defect in the spec's own evidence. (1) CRITICAL, both lanes: § 2.4's render rule — "emits a key only when `provenance` marks that field `asserted`" — would have suppressed `Layman:`, `Lanes:` and `Evidence:` on **every** bullet, because `makeItem()` writes provenance for `id`/`kind`/`source` and nothing else, so those three never carry one. An implementer following it ships a larger data loss than the one the spec exists to stop. Now scoped to the two defaultable fields, with "absent provenance means not-a-defaultable-field, never defaulted" stated outright. (2) CRITICAL, both lanes: the `Kind:`-always-renders exception makes INV-6 unsatisfiable by construction — a defaulted kind renders `Kind: implement.`, re-imports through the canonical branch as `asserted`, and provenance flips on all 476 rows at the first round trip. Resolved by excluding `provenance` from the governed set and saying why, rather than by inventing a machine marker in a human-facing file. (3) CRITICAL: § 2.1 restated a mapping that **already exists and is normative** — `roadmap-data-model.md` § 7.4 carries it and `mappedKind()` implements exactly its eleven entries — and re-opened three of those as "ruling needed" (`behaviour-change`, `perf / fix`, `perf / optimize`), which would have had an implementer either stall or overwrite a shipped decision. The duplicate table is deleted; § 2.1 now points at § 7.4 and contributes only what it misses. **Found by verification, not by either lane, and it corrects the spec's own evidence:** § 7.4's "11 others" and this spec's corpus figures both came from `tools/roadmap-corpus-survey.py` run *after* the first store render — by which point the render had already rewritten `Kind: bug` to `Kind: implement` in the file being surveyed. Re-running the inventory against the pre-render source surfaces **seven** unmapped values the table misses, led by `bug` at 29 items, the single largest unmapped value in the corpus and invisible to the contaminated run. **HIGH ×6:** § 1 described `bulletText()` as emitting `Source:` "unconditionally" when it is doubly conditional (`!isEmpty() && !shadows(...)`, ANTS-3808) — the symptom was right and the mechanism wrong; the header said 363 where the store measures 383; the kind table claimed "11 others" above 17 rows; `rxKind()` is **shared with the render** via `trailerValuesIn()`, so un-anchoring reaches `shadows()` and the spec called it "one regex literal" (new INV-10); dropping `CaseInsensitiveOption` silently reverses shipped ANTS-3407 (new INV-9, and § 7 now records it); and § 1's four drifting round-trip fields were diagnosed and then never addressed, leaving INV-6 unreachable — `headline` is now named as undiagnosed rather than left for an implementer to discover at the gate. **MEDIUM ×6:** `priority` had no table despite § 2.1's own rule and is now deferred to § 5 with the column left NULL; "looks like a path" was undefined and is now a stated predicate; the governed column set is enumerated (nine columns, three exclusions with reasons); the note budget (859 on the first run) is priced; INV-8's source-grep could not fail and is behavioural; and "448 non-standard keys" is 448 distinct *including* the six standard ones. **LOW ×6**, all fixed, including the "20+ path references" estimate — measured at **93 distinct tokens across 150 occurrences**, an eyeballed undercount from a truncated list. **Dismissed (1):** both lanes suspected `ItemWrite` carries no `provenance` member, which would have made § 2.4 unimplementable. It does — `QJsonObject provenance` at `src/roadmapstore.h`, and `bulletText()` already takes an `ItemWrite &`, so the render holds it. § 4's "no new state" is correct as written. Doc 301 → 448 lines; invariants 8 → 10. |
