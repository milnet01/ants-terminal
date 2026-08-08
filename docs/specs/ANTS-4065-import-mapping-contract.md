# ANTS-4065 — define the markdown→store import mapping, so importing neither loses nor invents a field

**Status:** spec draft (2026-08-08).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-4065 (user-request-2026-08-08, after the first real
migration was found to rewrite 123 items' `Kind` and materialise 363
`Source: planned.` lines).
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
safe to trust with that authority. Three distinct failures, in falling order of
severity.

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
   bullet declares none, marking `provenance.source = "defaulted"`;
   `bulletText()` (`src/roadmaprender.cpp`) then emits `Source: ` + the value
   unconditionally. A default that existed only as an absence is rendered as an
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

**`kind` — 32 distinct values in the corpus, 21 canonical.** The 11 others map:

| Source value | → | Rationale |
|---|---|---|
| `bug`, `bugfix` | `fix` | same work, informal label |
| `docs` | `doc` | plural |
| `enhance`, `improve` | `enhancement` | verb form |
| `feat` | `feature` | abbreviation |
| `testing` | `test` | gerund |
| `performance` | `perf` | long form |
| `spike` | `research` | § 3.5.3's "exploratory / feasibility work" |
| `tooling`, `process + tooling` | `chore` | § 3.5.3's "housekeeping" |
| `behaviour-change` | **ruling needed** | neither `fix` nor `enhancement` is obviously right |
| `perf / optimize`, `perf / fix`, `feature/fix`, `design + implement`, `design + fix` | **ruling needed** | compound; the column is single-valued |

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

**`priority` is a type conflict, not a value map.** The corpus writes
`Priority:` 88 times as prose (`CRITICAL`, `HIGH`); the column is an integer
1–5. Import maps the five documented severities onto 1–5 and preserves the
original string in `extras`; anything else is left NULL rather than guessed.

### 2.2 Un-anchor `rxKind()`, with the guard its sibling already has

`rxKind()` drops the `^\s*` anchor, exactly as `rxLanes()` did under ANTS-2058,
and gains ANTS-3722's negative lookbehind so a bullet *quoting* the label is not
read as declaring it:

```cpp
// was: "^\\s*Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]"
QStringLiteral("(?<!`)Kind:\\s*([^\\.\\n]+?)\\s*[\\.\\n]")
```

`CaseInsensitiveOption` is **dropped with the anchor**, for the reason
`rxLanes()` records: an un-anchored case-insensitive label matches prose
("…changed the kind: of work…"). The canonical capital form is the only one the
writer emits, and the anchored-lowercase tolerance ANTS-3407 added exists for
hand-edited files, which § 2.6 makes a transitional concern rather than a
permanent one.

### 2.3 A defaulted field is always noted

`makeItem()`'s empty-`rawKind` branch gains the note its unmapped-value sibling
already emits. The rule generalises to every field the import may default:

> **Import may default a field. It may not default one silently.**

Each default emits a note naming the field and the bullet's line, so a migration
report shows what the import supplied rather than what the document said. This
is the check that would have surfaced all 438 cases on the first run.

### 2.4 A defaulted field is not rendered

`bulletText()` emits a key only when `provenance` marks that field `asserted`.
A field the import invented renders as the absence it came from, which is what
makes § 2.6 reachable: today's render cannot round-trip because it writes
fields the parse will read back as assertions.

`roadmap-format.md` § 3.5's `Kind:` is required, so it is the one exception —
it always renders, and § 2.3's note is what keeps a defaulted `implement`
visible rather than silent.

### 2.5 A field naming a file is validated at import

20+ `Source:` values in this corpus name real paths (`*_Ants_MCP_Feedback.md`,
`remotecontrol.cpp`, `terminalwidget.h`, `rpmlint.log`), while `Evidence:` — the
field `roadmap-format.md` § 3.5 designates for paths — is used **once** across
all 4,377 items.

Import resolves any `Source:`/`Evidence:` value that looks like a path against
the project root. A path that does not exist is **not** a refusal: it is a note
plus `extras.unresolved_path`, because a roadmap legitimately cites files that
have since moved or shipped. Refusing would make a historical roadmap
unimportable, which no reading of the problem asks for.

**Validating that a spec is a *valid* spec is out of scope** (§ 5) — existence
is mechanical, validity is `/doc-lint`'s job and belongs to whichever verb
consumes the reference, not to the import.

### 2.6 Round-trip identity is the gate

For a migrated project, `import(render(store)) == store` over every column this
contract governs. That is the property making the markdown safely regenerable,
and it is the acceptance test for the whole item: without it, each release
rewrites the file and each rewrite moves the data.

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
- **INV-5** — A field marked `provenance = defaulted` does not render as a
  declared key, `Kind:` excepted. *Test:* render a project whose item has
  `provenance.source='defaulted'`; assert the output has no `Source:` line for
  it. *Breaks when:* `bulletText()` emits a key from the value rather than from
  the provenance — today's behaviour, which is why the test reds first.
- **INV-6** — `import(render(store))` changes no column this contract governs.
  *Test:* migrate a fixture project, render, re-import, assert
  `items_updated == 0` and no `field_conflict` note. *Breaks when:* any render
  emits a form the parser reads back differently — the 714-item drift § 1
  measures.
- **INV-7** — A `Source:`/`Evidence:` value naming a path that does not exist
  imports successfully, with a note and `extras.unresolved_path`. *Test:*
  fixture citing `docs/gone.md`; assert `ok`, the note, and the extras key.
  *Breaks when:* validation is written as a refusal, which would make a
  historical roadmap unimportable.
- **INV-8** — Import never writes `status='dropped'`. *Test:* source-grep — no
  assignment of `"dropped"` in `src/roadmapmigrate.cpp`. *Breaks when:* the
  fifth status is wired in before the render can express it, producing a row
  that cannot survive its own regeneration.

## 4. RAM / build cost

**No new state and no new target.** The parser change is one regex literal; the
migrator and render changes are branches on data already in hand — `provenance`
is written today and read nowhere at render time, so § 2.4 costs one JSON lookup
per field per bullet, against a render that already walks every bullet.

`extras.source_kind` and `extras.unresolved_path` add at most one short string
per affected item: **61** items carry `source_kind` today
(`SELECT COUNT(*) FROM item WHERE json_extract(extras,'$.source_kind') IS NOT
NULL`), and the path population is the 20+ of § 2.5. Both are bounded by the
corpus, not by usage over time.

## 5. Out of scope

- **The pass-headings status vocabulary** — 142 values outside the enum. A
  second dialect, tracked separately; this contract governs `ants-v1`.
- **Validating that a referenced spec is well-formed** (§ 2.5) — existence is
  mechanical, validity belongs to `/doc-lint` and to the verb that consumes the
  reference.
- **The 448 non-standard field keys** beyond mapping them into `extras`
  verbatim. Deciding which deserve real columns (`Dependencies` 98,
  `Acceptance` 44, `Scope` 42) is a data-model change, not an import mapping.
- **Back-filling `Kind:` onto the 1,613 corpus items that carry none.** They
  default legitimately under § 3.5.3's own rule; INV-1 makes the default
  visible, which is all this item owes them.
- **Re-migrating the other 13 projects.** ANTS-3853 owns the rollout; this is
  the gate it waits on.

## 6. Tests

Feature test: `tests/features/roadmap_import_mapping/`, covering INV-1 through
INV-7; INV-8 is a source-grep case in the same file. Label `features;fast` —
every fixture is a few-line roadmap, so nothing here needs the `perf` label.

Per the project test convention, **verify each case fails against pre-change
source first**. INV-2 and INV-5 are the two that must red on today's code, and
they are the reason this spec is written before the fix rather than after:
INV-2 fails because `rxKind()` is anchored, INV-5 because `bulletText()` renders
from the value.

INV-6 needs a fixture project rather than a bullet — a small roadmap with one
item per interesting shape (inline trailer, quoted label, absent kind, mapped
kind, unresolved path), migrated into a temp store. Per the standing trap,
construct `RoadmapStore` with an **explicit path**; the default resolves under
`XDG_DATA_HOME` and would run the test against the live store.

## 7. Cross-doc impact

- **`docs/standards/roadmap-format.md` § 3.5** — the § 2.2 change makes an
  inline `Kind:` trailer genuinely supported rather than accidentally supported.
  The standard should say so: **99 bullets in this project alone** write that
  shape (1,435 own-line against 99 inline, counted per bullet over the
  pre-render file and both archives), and the format has never admitted it.
- **`docs/standards/roadmap-data-model.md`** — gains the § 2.1 tables as the
  mapping's home, and § 2.3's defaults-are-noted rule.
- **`ROADMAP.md`** — ANTS-4063 (fabricated `Source:`) is discharged by INV-5,
  and ANTS-4062 (off-taxonomy `Kind:`) by § 2.1; both flip when this ships.
- **`CHANGELOG.md`** — one `Fixed` entry; the import losing declared fields is
  user-visible to anyone who migrates.
- **`CLAUDE.md`** — no change. The module map names subsystems, not field maps.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Dimensions | Outcome |
|---|---|---|---|---|---|
