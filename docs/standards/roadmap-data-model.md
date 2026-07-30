# Roadmap Data Model Standard (ANTS-3753)

> **Status:** Draft 2026-07-30 — not yet implemented. Defines *what a roadmap
> item is*. The schema, export serialisation, migration algorithm and check
> implementations live in the implementation spec (§ 9), not here.
>
> **Source:** user-request-2026-07-30 (roadmap moves to a shared database).

`roadmap-format.md` describes one *serialisation* of a roadmap item (markdown).
This describes the model those bytes carry, so a store, a backup and a published
page can each be checked against one definition rather than against each other.
Where the two standards touch the same rule, § 8 records which governs.

## Contents

- [1. Three artifacts](#1-three-artifacts)
- [2. Scope](#2-scope)
- [3. Obligations](#3-obligations)
- [4. Fields](#4-fields)
- [5. Structure](#5-structure)
- [6. Relationships](#6-relationships)
- [7. Enumerations and identity](#7-enumerations-and-identity)
- [8. Relationship to roadmap-format.md](#8-relationship-to-roadmap-formatmd)
- [9. What the implementation spec owns](#9-what-the-implementation-spec-owns)
- [Cold-eyes loop log](#cold-eyes-loop-log)

---

## 1. Three artifacts

| Artifact | Role | Home |
|---|---|---|
| Store | Working store. All reads and writes go here. | Local, untracked |
| Backup export | Durable record. One file per project. | The **private** `claude-config` repo |
| Published render | For human readers. Layman content only. | Each project's own repo, as `ROADMAP.md` |

**The backup lives in a private repo, and that is a safety requirement rather
than a convenience.** The export carries every item's full technical body,
including items marked `internal` (§ 7.5). An earlier draft placed it inside
each project's own repository — and `ants-terminal` is public, so that draft
would have published in full detail exactly what the visibility flag exists to
withhold. One private home also means cross-project relationships resolve from a
single checkout, and a project that is not a git repository at all can still be
backed up.

**INV-1 — The export is a complete copy of the store.** Exporting the live
store, rebuilding from that export, and re-exporting produces an identical
file. **Both legs are required**: re-export equality alone is satisfied by an
export that already lost half the model, since an empty file is a fixed point.
The check therefore compares the committed export against a fresh export of the
**live store**, not only against itself.

**INV-2 — The published render is derived and lossy, deliberately.** It carries
`layman` text and nothing technical, is never parsed back, and is **not** a
backup — which is why INV-1 targets the export instead.

**INV-3 — After a project cuts over, the store is its only writer of record.**
The export and the render are generated; a hand-edit to either is lost at the
next generation, so both are fidelity-checked. Before cutover the markdown
remains authoritative and neither check applies (§ 9).

**INV-4 — Relationships may cross projects.** One project's work being blocked
by another's is currently expressible only as prose.

**INV-5 — A citation is not a relationship.** An ID appearing in body or intro
prose never creates one; roadmaps routinely cite other projects' IDs, and this
project's own contains `FIBR-` and `ADR-` references in prose. A relationship is
declared by a dedicated field, never inferred from text.

---

## 2. Scope

Every project indexed by the shared store, including projects with no roadmap
yet. The model MUST NOT assume the shape of any one project's corpus — most
installations have one project, not nine. Corpus measurements below are evidence
that a requirement is *satisfiable*; they never define what the model must hold.

---

## 3. Obligations

Three tiers. Conflating them is what makes a data model unmigratable: a corpus
assembled under laxer rules cannot retroactively satisfy stricter ones, and a
standard that demands it simply never runs.

### 3.1 Required at write — items created or curated after cutover

`project`, `id`, `status`, `headline`, `kind`, `source`, `section`,
`sort_order`, `created`, `last_modified`; plus `layman` and `priority` for
**open** items; plus `resolution` for **closed** items, and `shipped` for status
`shipped` specifically.

`shipped` is required only for `shipped`, not for every closed item — a
`dropped` item has no ship date, and demanding one would be a nonsense state.

A status flip on a migrated item is **not** a curating write. Requiring the full
set there would reject the commonest operation on legacy data, for fields the
item was never obliged to carry.

### 3.2 Required before publish

`layman`. An item without it cannot appear on the published render, so a project
with any public open item lacking one fails the publish gate — it does not fail
migration. This is the **only** publish-gating field; `priority` and
`resolution` gate neither publishing nor migration.

### 3.3 Accepted at migration — historical items

Migration MUST NOT reject an item for a field the source format never required.
Where `roadmap-format.md` § 3.5.3 defines a default, migration applies it
(`kind` → `implement`, `source` → `planned`) and records that the value was
defaulted. Where no default exists — `layman`, `priority`, `resolution`, and any
date not derivable from history — the field is left empty.

This is the only arrangement the corpus permits. Across the nine surveyed
projects, **half** the items carry no `Kind:`, **half** carry no `Source:`,
and **56%** carry no `Layman:`. A write-time-only reading would refuse every
project.

Figures here and below come from `tools/roadmap-corpus-survey.py`, run against
the nine projects on 2026-07-30. They are quoted as proportions and orders of
magnitude on purpose: the corpus grows every time anyone files an item, so an
exact count written into a standard is wrong within the day. Re-run the script
rather than trusting a number in this file — per § 2 these measurements only
evidence that a requirement is *satisfiable*.

### 3.4 Open and closed

**Open** = `planned`, `in-progress`, `considered`. **Closed** = `shipped`,
`dropped`. Every conditional obligation above uses these two words with exactly
this meaning.

---

## 4. Fields

### 4.1 Item fields

| Field | Obligation | Notes |
|---|---|---|
| `project` | write | Owning project. Items are never global. |
| `id` | write | § 7.1. **Unique within its project**, not within the store — the same ID may legitimately exist in two projects. |
| `status` | write | § 7.3. |
| `headline` | write | One line, technical. |
| `layman` | write (open) / publish (all) | One sentence, non-technical. The only text a public reader sees. |
| `kind` | write | § 7.4. |
| `source` | write | Provenance. "What did project X ask for" is a query on this field. |
| `priority` | write (open) | § 7.5. Meaningless once closed. |
| `created`, `last_modified` | write | § 7.6. |
| `shipped` | write (status `shipped`) | § 7.6. |
| `resolution` | write (closed) | What was done and why, or why it was not. |
| `section`, `sort_order` | write | § 5. |
| `body` | optional | Free-form technical detail. Optional because many items are complete in one line, and a mandatory body produces filler that reads like content. |
| `lanes`, `evidence` | optional | Subsystems touched; paths to screenshots, logs, repros. |
| `visibility` | optional | § 7.5. Defaults to `public`. |
| `milestone` | optional | Target release. Distinct from `section`, which is where the item is *filed*. |
| `blocked` | derived | True iff a `blocked-by` relationship targets a **resolvable, same-project** item that is not closed. Cross-project targets are excluded — a partial rebuild cannot see them, and deriving from what is absent would make the value depend on which projects happen to be present. |
| `extras` | optional | § 4.3. |
| `provenance` | derived | Per field: whether the value was asserted by an author, defaulted, harvested, or derived from git history. Never silently promoted to asserted. |

`resolution` is required at close and `body` is not, because the institutional
value sits in closed items: a shipped item with no resolution records that
something happened and nothing about what. Specs do not substitute — this
project holds 218 spec documents against roughly 1,800 items, so for the large
majority the item is the only technical record.

### 4.2 Dates have a known limitation

`created` and `shipped` backfilled from git history are marked `git-derived`,
and are **not** reliable for items that have been through archive rotation:
`roadmap-format.md` § 3.9 moves bullets byte-identically into an archive file,
so the commit touching such an item is the rotation, not the work. Cycle-time
reporting is therefore sound for items created after cutover and best-effort
before it. Saying so is better than publishing a figure nobody can trust.

### 4.3 The extension mechanism is a correctness requirement

Four field keys dominate the corpus — `Kind:`, `Source:`, `Layman:` and
`Lanes:`, together accounting for the overwhelming majority of all field lines
— followed by a **long tail of over 280 distinct keys** that individual
projects invented. About twenty of those are in real use rather than one-off:
`Fix:`, `Scope:`, `Why:`, `Dependencies:`, `Spec:`, `Test:`, `Repro:`,
`Root cause:`, `Risk:`, `Design:`, `Related:`, `Verified:`, `Problem:`,
`Impact:`, `Acceptance:` and others.

A model with only § 4.1's fields would **silently delete that tail at the first
regeneration**. `extras` prevents it. Two tail keys are promoted out of
`extras` because they are relationships: `Spec:` and `Dependencies:` (§ 6).

---

## 5. Structure

Items live in a section tree, and their order carries meaning: position is the
current corpus's prioritisation, so `sort_order` is required and preserved.

`priority` and `sort_order` are complementary. `sort_order` is an exact order
within one project; `priority` is a coarse band comparable *across* projects,
which position can never be.

A section has a slug, title, level, optional intro prose, an optional parent,
and an **ordered element list**. Elements hold the content that is not an item:

| Element | Carries |
|---|---|
| `item` | A reference to a § 4 item. |
| `narration` | Section-summary prose that belongs to no single item. |
| `table` | A markdown table: header plus ordered rows. |

**Prose belongs to its item.** Sub-bullets, step lists and detail lines beneath
an item are that item's `body`; fenced code blocks belong to a `body` or an
`intro`. Neither is a section element, because both are subordinate to something
specific rather than interleaved at section level.

That distinction is load-bearing rather than tidy. Of the corpus's 86
status-marked bullets carrying no bold headline, 78 are item detail —
MAME Curator's `` `tests/api/test_fp09_fixes.py:362` — wrapped the SSE
history-replay `` and Music Production's `Step 3 — failing tests for
TC-06-17/18/19` are sub-steps of a parent item, not free-standing content.
Modelling them as section elements would detach them from the item they
describe, and the item would then be published without the prose that explains
it — § 4.3's failure by another route.

### 5.1 The status legend is structured, not prose

Every project writes its own key for the status vocabulary, in its own words:
this project's `In progress (active commit work — usually direct-to-main on
this project; rarely a branch / PR)` against Fin Break's `In progress (being
tackled now)`. Eight such lines exist across the corpus, and they are the
document's *metadata* — not items, and not narration.

They are therefore their own per-project structure: status value → that
project's wording. **This is what lets one renderer serve every project.**
RoadmapDialog currently looks different across these roadmaps because it is
parsing each one's freeform key out of prose; given the legend as data, it
renders the vocabulary natively and the per-project divergence disappears.

### 5.2 Structures the model must survive

Measured across the nine-project corpus, in descending order of how much of
the corpus each accounts for:

| Order of magnitude | Structure | Home |
|---|---|---|
| ~1,200 | sub-bullets | item `body` |
| ~80 | status-marked detail lines | item `body` |
| ~170 | markdown table **data** rows | `table` element |
| ~20 | fenced code blocks | `body` or `intro` |
| 8 | status-legend lines | § 5.1 legend structure |
| residual | section-summary prose | `narration` element |

A table's separator row (`|---|---|`) is delimiter, not content: it carries no
cell values and is regenerated by any renderer, so it is not a row the model
stores. Counting it as one inflates the table figure by roughly a tenth and
would have the store round-tripping a line that means nothing.

---

## 6. Relationships

Typed and directed.

| Type | Meaning | Replaces |
|---|---|---|
| `splits-from` | Carved out of a parent. | ANTS-3748 — a split currently leaves feedback files citing only the parent, whose completion then over-claims. |
| `blocked-by` | Cannot start until the target closes. | Prose markers parsed out of body text. |
| `duplicate-of` | Same work as the target. | Manual dedup. |
| `supersedes` | Replaces an earlier decision. | Nothing — currently unrecorded. |
| `relates-to` | Untyped association. | `Dependencies:` (18 uses). |
| `specified-by` | Target is a spec **document**, addressed by path. | `Spec:` (14 uses). |

A relationship's target is an item *or* a spec document, and the two are
addressed differently — items by identity, specs by path. `Spec:` values are
paths, so INV-5's "declared, never inferred" governs the *field*, not the
value's shape.

`splits-from`, `blocked-by` and `duplicate-of` are acyclic.

Three adjacent record types exist for the same reason: **`feedback_ref`** (which
cross-session feedback file cites which item — making ANTS-3744 a query),
**`citation`** (item or spec → file and symbol, making `documentation.md` § 1.7
machine-checkable), and **`history`** (one row per field change).

**`history` is exported, not store-only.** Git currently carries the history of
every roadmap edit because the roadmap *is* a tracked file. Under this model the
store is untracked and the render is lossy, so that history would have nowhere
to live — which makes exporting it the point, not an optimisation.

---

## 7. Enumerations and identity

### 7.1 Identity

An item ID matches `roadmap-format.md` § 3.5.1's grammar, **not a narrower
one**. Live prefixes include `ANTS`, `DOOM`, `FIBR`, `CL`, `ONEUP`, `ROLO`,
`3D_E` and `mame-curator` — so a prefix may contain hyphens, underscores and
digits, a project may declare **several**, and comparison is case-insensitive
per that standard's § 3.10.4.

**Some live IDs do not match that grammar, and the model must say what happens
to them rather than assume they are absent.** § 3.5.1 requires a dash between
prefix and number; 3D_Engine writes three items as `[Cl9]`, `[Cl10]` and
`[CE18]`, with no dash. A migration built on the grammar alone drops them
silently, which is the one outcome § 3.3 forbids. They are **items with an
unparseable ID**: migration MUST NOT invent a dash (rewriting an ID breaks
§ 3.5.1's append-only rule and every citation of it), MUST NOT treat them as
ID-less (§ 7.2's bulk allocation would issue a second identity for an item
that already has one), and MUST surface them for a per-project decision.

**An ID is recognised only at the bullet-leading position**, immediately after
the status marker. A bracketed token matching the grammar anywhere else is
text: `DOOM_Ants` contains `players[idx-1]` and `row[right-1]`, both of which
match, so a grammar-anywhere rule would refuse that project outright.

The store owns allocation. Each project currently keeps a gitignored per-machine
counter that is explicitly not the source of truth, with a floor recomputed by
scanning the corpus so a wiped counter cannot reissue a live ID; a shared store
allocates directly and that failure mode disappears.

### 7.2 Allocating IDs to items that have none

Every item needs an ID, and roughly 1,550 in the corpus have none — about 40%
of it. Allocation splits by whether anyone will ever need to cite the item:

| | Share | Rule |
|---|---|---|
| **Closed** | ~950 | Allocated in bulk, in document order, marked `provenance: migrated`. Nobody cites a finished item, so no curation is required and none is invented. |
| **Open** | ~600 | Allocated into the project's normal sequence and treated as a real item — it will be cited, worked on, and referenced in commits. § 3.2's publish gate then applies, so it must be curated before that project publishes. |

The two differ in *obligation*, not in ID shape. A separate archival prefix
would add a second prefix per project to reconcile, and an ID's text should
never encode metadata that can change; `provenance` carries the distinction
instead.

**Only bullets that are items get an ID.** The discriminator is the bold
headline `roadmap-format.md` § 3.5 already requires. 86 status-marked bullets
across the corpus carry neither an ID nor a bold headline; they are not items,
and they are not one thing either — 78 are detail lines belonging to a parent
item's `body`, and 8 are status-legend lines (§ 5.1). Neither gets an ID.

### 7.3 Status

`planned` · `in-progress` · `shipped` · `considered` · `dropped`

`dropped` is new — abandoning an item currently means deleting its line, erasing
the decision with it. It has **no markdown serialisation**:
`roadmap-format.md` § 3.11 makes a fifth status emoji an anti-pattern, so
dropped items are excluded from the published render until that standard adds
one (§ 8).

### 7.4 Kind

The canonical set is `roadmap-format.md` § 3.5.3's 21-value enum. **Writes
accept canonical values only.** The corpus holds 32 distinct values — all 21
canonical ones plus 11 others — and the migration-scoped mapping is normative:

| Corpus value | Canonical | Corpus value | Canonical |
|---|---|---|---|
| `improve` (7) | `enhancement` | `docs` (2) | `doc` |
| `bugfix` (6) | `fix` | `testing` (1) | `test` |
| `spike` (5) | `research` | `feat` (1) | `feature` |
| `enhance` (3) | `enhancement` | `perf / fix` (1) | `perf` |
| `perf / optimize` (2) | `perf` | `tooling` (1) | `chore` |
| `behaviour-change` (1) | `enhancement` | | |

Two entries are compound — a project wrote `perf / optimize` and `perf / fix`
where one value was required. Both map to their **first** term; a rule that
picked the second would silently reclassify performance work.

The table is generated, not recalled: `tools/roadmap-corpus-survey.py` prints
every non-canonical value with its count. A project joining later re-runs it
and extends the table by amendment.

### 7.5 Priority and visibility

`priority` is `1` (highest) to `5` (lowest), required on open items. Five bands
rather than ten: ten levels are not reliably distinguishable, so they collapse
in practice to three with the rest defaulting to the middle, and the number
stops carrying information. `roadmap-format.md` § 3.5.2's prose severity form
maps CRITICAL → 1, HIGH → 2, MEDIUM → 3, LOW → 4; band 5 is reserved for
someday-maybe work that no severity word expresses.

`visibility` is `public` or `internal`. The published render includes only
`public` items, and excludes `dropped` items regardless. Today everything is
published because the file *is* the record — including security findings that
are still open.

### 7.6 Dates

`created`, `last_modified`, `shipped`, ISO 8601 (`documentation.md` § 1.3).

**No per-item date field exists today** — closure is prose
(`Resolved (2026-07-30): …`). Release blocks and CHANGELOG sections carry dates
at *release* granularity, so "what shipped in version X" is already answerable;
"when did this item close", "how long do items stay open" and "is the backlog
growing" are not. Those three are the gain, subject to § 4.2's limitation.

Effort and size estimates are deliberately excluded: they are guesses that
attract false precision, and `created` + `shipped` yield real cycle time.

---

## 8. Relationship to roadmap-format.md

That standard governs the markdown format and keeps doing so. Four points where
the two touch, stated so neither is silently overridden:

- **Optional fields.** Its § 3.5 files `Layman:` and `Source:` as optional and
  § 3.5.3 gives defaults for absent `Kind:` / `Source:`. This document does not
  change that for markdown; § 3.3 *adopts* those defaults, and § 3.1's stricter
  obligations apply only to writes through the store.
- **ID allocation.** Its § 3.5.1 puts the high-water mark in `.roadmap-counter`
  under an flock. After a project cuts over, the store owns allocation and that
  counter is retired **for that project**; the corpus floor it describes is
  recomputed from the export instead, since a layman-only render no longer
  carries IDs.
- **Status vocabulary.** Its § 3.11 makes a fifth status emoji an anti-pattern,
  so `dropped` has no markdown form and is excluded from the render. Adding one
  is that standard's decision, not this one's.
- **Pass headings.** Its § 3.10.5 documents `#### Pass N.M` as a supported read
  *and* write format. No project currently uses it, so it is out of scope for
  **migration** — but the model must stay able to express it, because § 2
  forbids assuming the current corpus is the whole world.

---

## 9. What the implementation spec owns

This document deliberately stops at the model. The following are design work
with real decisions in them, and belong in a spec that goes through the
implementation gate rather than in a standard:

- The schema: entity keys, cardinalities, per-element columns, and how `extras`
  and `provenance` are stored.
- The export: record types, field order, sort collation, encoding, and every
  other rule that makes INV-1's "identical file" testable rather than
  aspirational.
- Migration: the algorithm, per-project atomicity, re-run matching, what happens
  to an item deleted from source, and the cutover transition — including the
  interim in which some projects are migrated and others are not.
- The check suite: each check's input, pass condition, scheduling, and behaviour
  on a machine where the store does not yet exist.
- Concurrency across projects sharing one store, and the auto-publish cadence to
  the backup repo — including that a push conflict means two stores diverged and
  must surface rather than auto-merge, and that a silent backup failure is worse
  than no backup because it stops anyone checking.
- Whether the published render lists closed items at all, or only open work plus
  recent releases.
- The fate of `roadmap_query`, `roadmap_log` and `RoadmapDialog`, all of which
  parse and write `ROADMAP.md` today and cannot continue to against a
  layman-only render.

ANTS-3754 carries the verified finding list that is this spec's input.

---

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-30 | 3 (model coherence, corpus drift, failure modes) | 6 / 12 / 14 / 18 / 1 | Structural rewrite: obligations split into tiers, export scope defined, INV-1 given its missing leg, identity grammar corrected after the survey regex was found wrong about two projects, migration source shapes corrected. |
| 2 | 2026-07-30 | 3 (same partition, cold) | 13 / 19 / 17 / — / — | **Stopped and split.** ~8 of the 13 CRITICALs were collateral from loop 1's own fixes; the findings were overwhelmingly schema-level, i.e. this document was a standard carrying an implementation spec. Split per ANTS-3754: the model stays here, the schema goes to a spec. Backup relocated to the private config repo, closing a leak the draft shipped. ID allocation for the corpus's 1,528 ID-less items decided (user, 2026-07-30). |
