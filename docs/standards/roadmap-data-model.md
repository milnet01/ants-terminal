# Roadmap Data Model Standard (ANTS-3753)

> **Status:** Draft 2026-07-30 — not yet implemented. `roadmap-format.md`
> remains the authority for the markdown format; where the two touch the same
> rule, § 11 records which one governs and why.
>
> **Source:** user-request-2026-07-30 (roadmap moves to a shared database).

This standard defines **what a roadmap item is**, independent of how it is
stored. `roadmap-format.md` describes one *serialisation* of it (markdown).
This describes the model those bytes carry, so a store, an export file and a
published page can all be checked against one definition rather than against
each other.

## Contents

- [1. Three artifacts](#1-three-artifacts)
- [2. Scope](#2-scope)
- [3. Obligations](#3-obligations)
- [4. Fields](#4-fields)
- [5. Structure and ordering](#5-structure-and-ordering)
- [6. Entities and relationships](#6-entities-and-relationships)
- [7. Enumerations and identity](#7-enumerations-and-identity)
- [8. The export](#8-the-export)
- [9. Migration](#9-migration)
- [10. Health checks](#10-health-checks)
- [11. Relationship to roadmap-format.md](#11-relationship-to-roadmap-formatmd)
- [12. Deferred to the implementation spec](#12-deferred-to-the-implementation-spec)
- [Cold-eyes loop log](#cold-eyes-loop-log)

---

## 1. Three artifacts

| Artifact | Role | Location | In git? |
|---|---|---|---|
| Store | Working store. All reads and writes go here. | `${XDG_DATA_HOME}/ants-terminal/roadmap.sqlite`, one shared store | **No** |
| Export | Durable record and backup. | `<project>/docs/roadmap/export.jsonl`, **one per project** | **Yes** |
| Published render | For human readers. Layman content only. | `<project>/ROADMAP.md` | **Yes** |

**The export is per project, committed to that project's own repository.** A
single global export has no natural home among independently-versioned repos
and would leave every project but one unable to rebuild. Per-project exports
make each repo self-sufficient for its own items; § 6.3 states what happens to
a link whose target lives in a project not present.

**INV-1 — The export is a complete copy of its project's slice of the store.**
Exporting the live store, rebuilding a store from that export, and re-exporting
MUST produce a byte-identical file. **Both legs are required**: re-export
equality alone is satisfied by an export that already lost half the model (an
empty file is a fixed point of rebuild-then-re-export), so § 10 checks the
committed export against a fresh export of the **live store**, not only against
itself. Every field in § 4, every element in § 5, and every row of every entity
in § 6 travels; § 8 pins the serialisation that makes byte-identity meaningful.

**INV-2 — The published render is derived and lossy, deliberately.**
`ROADMAP.md` carries § 4's `layman` text and nothing technical. It is never
parsed back and is **not** a backup — which is why INV-1 targets the export.

**INV-3 — The store is the only writer of record.** The export and the render
are generated. A hand-edit to either is lost at the next generation, so § 10
carries a fidelity check for **both** artifacts. What the tooling does after
detecting one is § 12's only open question on this point; that it MUST detect
one is settled here.

---

## 2. Scope

Applies to every project indexed by the shared store, including projects with
no roadmap yet. The model MUST NOT assume the shape of any one project's
corpus — most installations have one project, not nine. Where this document
cites corpus measurements, they are evidence that a requirement is
*satisfiable*, never a definition of what the model must hold.

---

## 3. Obligations

Two tiers. Conflating them is what makes a data model unmigratable: a corpus
assembled under laxer rules cannot retroactively satisfy stricter ones, and a
standard that demands it simply never runs.

### 3.1 Required at write time — new and updated items

Rejected at write if absent: `project`, `id`, `status`, `headline`, `kind`,
`source`, `created`, `last_modified`, plus `layman` and `priority` for **open**
items, and `shipped` + `resolution` for **closed** items.

### 3.2 Required before publish — any item

`layman`. An item without it cannot appear on the published render, so a
project with any public open item lacking one **fails § 10's publish gate**
rather than failing migration.

### 3.3 Accepted at migration — historical items

Migration MUST NOT reject an item for a field the source format never
required. Where `roadmap-format.md` § 3.5.3 defines a default, migration
applies it (`kind` → `implement`, `source` → `planned`) and records that the
value was defaulted (§ 4.2). Where no default exists — `layman`, `priority`,
`resolution` — the field is left empty and the item is marked incomplete.

Incompleteness blocks **publishing**, never **migrating**. This is the only
arrangement the corpus permits: measured across the nine surveyed projects,
1,159 of 3,216 items carry no `Kind:`, 1,269 no `Source:`, and 1,499 no
`Layman:` (~47%). A write-time-only reading of § 3.1 would refuse every
project.

### 3.4 Open and closed

**Open** = `planned`, `in-progress`, `considered`. **Closed** = `shipped`,
`dropped`. Every conditional obligation above and every check in § 10 uses
these two words with exactly this meaning.

---

## 4. Fields

### 4.1 Item fields

| Field | Type | Obligation | Notes |
|---|---|---|---|
| `project` | ref → § 6.1 | write | Owning project. Items are never global. |
| `id` | text | write | § 7.1. Unique within the store. |
| `status` | enum | write | § 7.2. |
| `headline` | text | write | One line, technical. |
| `layman` | text | write (open) / publish (all) | One sentence, non-technical, ≤ 200 characters. The only text a public reader sees. |
| `kind` | enum | write | § 7.3. |
| `source` | text | write | Provenance. "What did project X ask for" is a query on this field. |
| `priority` | int 1–5 | write (open) | § 7.4. Meaningless once closed; not required there. |
| `created` | date | write | § 7.5. |
| `last_modified` | date | write | § 7.5. |
| `shipped` | date | write (closed) | § 7.5. |
| `resolution` | text | write (closed) | What was done and why, or why it was not. |
| `body` | text | optional | Free-form technical detail. Optional because many items are complete in one line, and a mandatory body produces filler that reads like content. |
| `lanes` | list | optional | Subsystems touched. |
| `evidence` | list | optional | Paths to screenshots, logs, repros. |
| `visibility` | enum | optional | § 7.6. Defaults to `public`. |
| `milestone` | text | optional | Target release. § 9.4 defines how a release-block heading maps to this versus to `section`. |
| `blocked` | bool | derived | True iff any `blocked-by` link targets an item whose status is not closed (§ 3.4). Never set by hand; recomputed, and exported (INV-1 requires the bytes to match). |
| `sort_order` | int | write | § 5. |
| `section` | ref → § 6.1 | write | § 5. |
| `extras` | key/value | optional | § 4.3. |

`resolution` is required at close and `body` is not, because the institutional
value sits in closed items: a shipped item with no resolution records that
something happened and nothing about what. Specs do not substitute — this
project holds 218 spec documents against its own 1,597 items, so for the large
majority the item is the only technical record.

### 4.2 Provenance of derived values

Any field written by migration rather than asserted by an author carries a
provenance marker naming the rule that produced it (`default`, `git-derived`,
`harvested`). Stored per field, exported, and never silently promoted to
asserted. § 9.3 requires date backfill to be marked; this is the field that
holds the mark.

### 4.3 The extension mechanism is a correctness requirement

Surveyed across all nine roadmaps, four field keys dominate — `Kind:` (2,077),
`Source:` (1,971), `Layman:` (1,717), `Lanes:` (657) — followed by a long tail
of roughly eighteen that individual projects invented: `Fix:`, `Scope:`,
`Why:`, `Goal:`, `Theme:`, `Dependencies:`, `Spec:`, `Test:`, `Repro:`,
`Framework:`, `Root cause:`, `Prerequisites:`, `Risk:` and others.

A model with only § 4.1's fields would **silently delete that tail at the first
regeneration**. `extras` is what prevents it.

Three tail keys are promoted out of `extras` because they are *relationships*:
`Spec:`, `Dependencies:` and `Prerequisites:` become links (§ 6.2). The rest
stay as extras — they describe how someone structured a write-up, which is
prose organisation, not queryable metadata.

---

## 5. Structure and ordering

Items live in a section tree and their order carries meaning. `sort_order` is
required (§ 4.1): position is the current corpus's prioritisation, and losing
it on migration would discard it.

`priority` and `sort_order` are complementary. `sort_order` is an exact total
order within one project; `priority` is a coarse band comparable *across*
projects, which position can never be.

### 5.1 Sections and their elements

A section has `slug`, `title`, `level`, optional `intro` prose, an optional
parent section, and its own `sort_order`.

A section also owns an **ordered element list**, each element carrying a type
discriminator and its own `sort_order`. This is what holds the structures that
are not items:

| Element type | Carries |
|---|---|
| `item` | A reference to a § 4 item. |
| `narrator` | A status-marked bullet with no ID — prose, not an item. |
| `table` | A markdown table: a header row plus ordered data rows. |

Sub-bullets beneath an item are part of that item's `body`, not section
elements — they are always subordinate to a specific item, never interleaved at
section level. Fenced code blocks are likewise `body` or `intro` content.

### 5.2 Structures the model must survive

Measured across the nine-project corpus. Each round-trips under INV-1 via the
mechanism named above:

| Count | Structure | Home |
|---|---|---|
| 1,209 | sub-bullets | item `body` |
| 610 | narrator bullets | section element, type `narrator` |
| 182 | markdown table rows | section element, type `table` |
| 38 | fenced code blocks | `body` or `intro` |

---

## 6. Entities and relationships

### 6.1 Entities

| Entity | Key | Notable columns |
|---|---|---|
| `project` | canonical root path | display name, **prefix set** (§ 7.1), source format, export path, last indexed |
| `item` | (`project`, `id`) | § 4.1 |
| `section` | (`project`, `slug`) | § 5.1; parent → `section` |
| `spec` | (`project`, path) | title, declared status |
| `link` | (`from_item`, `type`, `to`) | § 6.2 |
| `feedback_ref` | (file path, `to`) | which cross-session feedback file cites which item |
| `citation` | (`from`, path, symbol) | source of the citation is an item or a spec |
| `history` | (`item`, `changed_at`, `field`) | old value, new value, actor |

### 6.2 Link types

Typed and directed. `to` is an item, except for `specified-by`, whose target
is a `spec`.

| Type | Meaning | Replaces |
|---|---|---|
| `splits-from` | Carved out of a parent. | ANTS-3748 — a split currently leaves feedback files citing only the parent, whose completion then over-claims. |
| `blocked-by` | Cannot start until the target closes. | Prose markers parsed out of body text. |
| `duplicate-of` | Same work as the target. | Manual dedup. |
| `supersedes` | Replaces an earlier decision. | Nothing — currently unrecorded. |
| `relates-to` | Untyped association. | `Dependencies:` / `Prerequisites:` (26 uses). |
| `specified-by` | Target is a spec document. | `Spec:` (17 uses). |

`splits-from`, `blocked-by` and `duplicate-of` MUST be acyclic (§ 10).

**INV-4 — Links may cross projects.** One project's work being blocked by
another's is currently expressible only as prose.

**INV-5 — A citation is not a link.** An ID appearing in **body or intro
prose** never creates a link; this project's own roadmap contains `FIBR-` and
`ADR-` references in prose. An ID appearing in a **field** (`Spec:`,
`Dependencies:`, `Prerequisites:`) *is* a declaration and does create one. A
non-ID value in such a field (`Dependencies: Qt 6.7`) stays in `extras`.

### 6.3 Cross-project links and a partial checkout

A link's target is stored as an opaque `(project, id)` pair on the owning side.
Rebuilding one project's store from its own export therefore yields links whose
targets are absent, which is expected, not corruption: § 10's referential check
is enforced **within** a project and reports cross-project dangles as
informational.

### 6.4 Why `history` is required, and exported

Git currently carries the history of every roadmap edit, because the roadmap
*is* a tracked text file. Under this model the store is untracked and the
render is lossy, so that history would have nowhere to live — therefore
`history` rows are part of the export (INV-1), not store-only. Exporting them
is what makes the argument in this section true rather than circular.

Growth is bounded by § 8.3.

---

## 7. Enumerations and identity

### 7.1 Identity

An item ID matches `roadmap-format.md` § 3.5.1's grammar, **not a narrower
one**. Prefixes in the live corpus include `ANTS`, `DOOM`, `FIBR`, `CL`,
`ONEUP`, `ROLO`, `3D_E`, `CE` and `mame-curator` — so a prefix may contain
hyphens, underscores and digits, and a project may declare **several**
(3D_Engine uses three). `project.prefix_set` is therefore a set, and § 10's
identity check tests membership of that set.

A first draft of this standard asserted a `PREFIX-NNNN` grammar and that three
projects had no IDs. Both were wrong, from a survey regex that could not
express a hyphenated or digit-initial prefix: MAME Curator has 65
`mame-curator-NNNN` IDs and 3D_Engine has 30 `3D_E-NNNN` IDs. **One** project
(Music Production, 372 bullets) has none. Migration allocating identity over
the other two would have violated `roadmap-format.md` § 3.5.1's append-only
rule across 95 live IDs.

**The store owns allocation.** Each project currently keeps a gitignored
per-machine counter that is explicitly not the source of truth, with a floor
recomputed by scanning the corpus so a wiped counter cannot reissue a live ID.
A shared store allocates directly and that failure mode disappears. Digit width
is not fixed: `roadmap-format.md` § 3.5.1 widens past 9999, and Ants is at
3,753.

### 7.2 Status

`planned` · `in-progress` · `shipped` · `considered` · `dropped`

`dropped` is new. Abandoning an item currently means deleting its line, erasing
the decision with it. **`dropped` has no markdown serialisation** —
`roadmap-format.md` § 3.11 lists any fifth status emoji as an anti-pattern — so
dropped items are excluded from the published render until that standard adds
one (§ 11).

### 7.3 Kind

The canonical set is `roadmap-format.md` § 3.5.3's 21-value enum. The corpus
holds 34 distinct values; all 21 canonical values appear, and 13 do not. The
mapping is normative and lives here, because it is migration's correctness
contract:

| Corpus value | Canonical | Corpus value | Canonical |
|---|---|---|---|
| `bug` (29) | `fix` | `performance` (2) | `perf` |
| `bugfix` (6) | `fix` | `design` (2) | `implement` |
| `spike` (5) | `research` | `testing` (1) | `test` |
| `docs` (3) | `doc` | `feat` (1) | `feature` |
| `enhance` (3) | `enhancement` | `tooling` (1) | `chore` |
| `behaviour-change` (1) | `enhancement` | `process` (1) | `chore` |
| `audit` (1) | `audit-fix` | | |

A value seen **after** migration that is neither canonical nor in this table is
a write-time refusal. Migration itself cannot encounter one, because the table
is exhaustive over the surveyed corpus — a project joining later re-runs the
survey and extends the table by amendment.

### 7.4 Priority

`1` highest … `5` lowest, required on open items (§ 3.1), defaulted empty and
publish-blocking on migrated items (§ 3.3).

Five bands rather than ten: ten levels are not reliably distinguishable, so
they collapse in practice to three with the rest defaulting to the middle, and
the number stops carrying information.

`roadmap-format.md` § 3.5.2 records priority as prose severity words
(`Priority: CRITICAL — …`); § 9.4 maps those to bands. Only 2 such lines exist
corpus-wide, so this is a rule for completeness, not a bulk conversion.

### 7.5 Dates

`created`, `last_modified`, `shipped`, ISO 8601 (`documentation.md` § 1.3).

**No per-item date field exists today** — closure is prose
(`Resolved (2026-07-30): …`). Release blocks and CHANGELOG sections carry dates
at *release* granularity, so "what shipped in version X" is already answerable;
"when did this item close", "how long do items stay open" and "is the backlog
growing" are not. Those three are the gain.

Effort and size estimates are deliberately **excluded**: they are guesses that
attract false precision, and `created` + `shipped` yield real cycle time.

### 7.6 Visibility

`public` · `internal`. The published render includes only `public` items, and
excludes `dropped` items regardless of visibility (§ 7.2).

Today everything is published because the file *is* the record, including
security findings that are still open. § 10 carries a leak check, because this
is the one property whose silent regression has a real-world cost.

---

## 8. The export

### 8.1 Record types

The export is line-delimited JSON, one **record** per line — not one item per
line, which could not carry a section, a link, a narrator element or a history
row. Every record carries a `type` discriminator: `project`, `section`,
`element`, `item`, `link`, `spec`, `feedback_ref`, `citation`, `history`.

### 8.2 Serialisation

Byte-identity is meaningless without these pinned, and two conforming
implementations would otherwise each pass their own round-trip while
disagreeing:

- Records sorted by (`type`, key), types in the order listed above.
- Object keys emitted in schema-declaration order, not insertion or alphabetical.
- UTF-8, LF line endings, one trailing newline, no BOM.
- Absent optional fields **omitted**, never emitted as `null`.
- Dates as `YYYY-MM-DD`; timestamps as ISO 8601 UTC with seconds precision.

### 8.3 Growth

`history` grows per edit and is committed, so it is the one unbounded table.
Rows older than 24 months compact to one summary row per (item, field) pair
recording first and last value. The export MUST stay under 10 MB per project;
exceeding it is a § 10 failure, not a silent condition.

---

## 9. Migration

Derived from the survey, not assumed.

### 9.1 Source shapes

Three are present, and they are **not** the three a first draft named:

| Shape | Projects |
|---|---|
| Emoji bullets with IDs | Ants_Terminal, DOOM_Ants, finbreak, OneUp, Contact_List, Rolodex, MAME_Curator |
| Emoji bullets + a parallel GFM task list | 3D_Engine (30 IDs, 994 checkboxes) |
| Emoji bullets, no IDs | Music_Production (372 bullets) |

**No project uses `#### Pass N.M` headings** — measured, zero occurrences
corpus-wide. A parser for that shape is not a migration prerequisite.

### 9.2 Identity

Allocate IDs only for Music_Production. Every other project's IDs are read
verbatim (§ 7.1). A bracketed token that does not match the ID grammar is not
an ID — the corpus contains bracket text such as `[Tier-2 fold]` — and is
retained as body text.

### 9.3 Fill rules

Apply § 3.3. Backfill dates from git history where derivable and mark them
`git-derived` (§ 4.2). Harvest `resolution` from `Resolved (…)` prose where
present; leave empty otherwise — only 2 explicit `Resolution:` lines exist
corpus-wide against 1,310 shipped items in this project alone, so harvesting is
best-effort and its absence is never a refusal.

### 9.4 Structural mapping

- A `## <version> — …` release block becomes a `section` **and** sets
  `milestone` on its descendant items. A non-versioned `##` becomes a section
  only.
- `Priority: <SEVERITY>` prose maps CRITICAL → 1, HIGH → 2, MEDIUM → 3,
  LOW → 4, absent → empty.

### 9.5 Refusals and atomicity

Migration refuses on: an unparseable item, a `kind` outside § 7.3's table, a
duplicate ID, or an ID violating its project's prefix set. A missing
*historical* field is never a refusal (§ 3.3).

**Per-project migration is atomic** — it commits wholly or discards wholly.
"Stops" is not "rolls back", and a half-migrated project would make the re-run
below produce duplicates.

### 9.6 Re-runs and cutover

Migration is re-runnable and read-only against the source markdown until
cutover. A re-run matches an existing row by `(project, id)`, or for
Music_Production by `(project, section slug, headline hash)`, and is a no-op
for anything already migrated — so a re-run never reallocates identity.

**Cutover** is a named, per-project step taken by the maintainer: it marks the
project store-authoritative, after which § 10's render-fidelity check becomes
active and the markdown is generated rather than read. The pre-cutover commit
of the source markdown is its rollback point.

---

## 10. Health checks

`verify` is what gives this standard teeth. It runs in the **pre-commit** hook
of the project being committed, and must complete within **2 seconds** for a
project of ≤ 5,000 items — a gate that is slow gets bypassed. The two
whole-store checks marked *on-demand* run in the maintenance command instead.

| Check | Fails when |
|---|---|
| Schema integrity | The store's own consistency check fails. |
| **Export current** | A fresh export of the live store differs from the committed export (INV-1, first leg). |
| **Round-trip** *(on-demand)* | Rebuilding from the committed export and re-exporting differs from it (INV-1, second leg). |
| Render fidelity | The committed `ROADMAP.md` differs from a fresh render, i.e. a generated file was hand-edited (INV-3). Active only after cutover (§ 9.6). |
| **Visibility leak** | The rendered `ROADMAP.md` contains text from any item whose `visibility` is `internal`, or any `dropped` item. |
| Write-time fields | Any § 3.1 field is absent on an item written after cutover. |
| Publish gate | A public open item has no `layman` (§ 3.2). |
| Referential | A link, feedback reference or citation points at a missing target **within the same project** (§ 6.3). |
| Acyclicity | `splits-from`, `blocked-by` or `duplicate-of` contains a cycle. |
| Derived consistency | A stored `blocked` value disagrees with recomputing it (§ 4.1). |
| Identity | Two items share an ID, or an ID's prefix is outside its project's prefix set. |
| Enumerations | A status, kind, priority or visibility value is off-list. |
| Export size | A project's export exceeds 10 MB (§ 8.3). |
| Cross-project *(on-demand)* | Informational: links whose target project is not present. |

---

## 11. Relationship to roadmap-format.md

That standard governs the markdown format and keeps doing so. Three points
where this document touches it, stated so neither is silently overridden:

- **Optional fields.** Its § 3.5 files `Layman:` and `Source:` as optional and
  § 3.5.3 gives defaults for absent `Kind:` / `Source:`. This document does not
  change that for markdown; § 3.3 *adopts* those defaults, and § 3.1's stricter
  obligations apply only to writes through the store.
- **Status vocabulary.** Its § 3.11 makes a fifth status emoji an
  anti-pattern, so `dropped` (§ 7.2) has no markdown form and is excluded from
  the render. Adding one is that standard's decision, not this one's.
- **Archive rotation.** Its § 3.9 floors ID allocation to the highest ID across
  `ROADMAP.md` + `CHANGELOG.md` + `docs/roadmap/*.md`. A layman-only render no
  longer carries IDs, so after cutover the export replaces the render in that
  floor computation. ANTS-3749 and ANTS-3751 own the rotation change; this is
  the interaction they must account for.

---

## 12. Deferred to the implementation spec

Named so they are decided rather than discovered:

- Whether the published render lists closed items at all, or only open work
  plus recent releases. The CHANGELOG already carries release history. The
  render-fidelity check (§ 10) needs this pinned before it is implementable.
- What the tooling **does** after detecting a hand-edit to a generated artifact
  — refuse, or accept and regenerate with a warning. That it must detect one is
  INV-3 and is not deferred.
- Concurrency tuning. The *contract* is not deferred: writes are serialised,
  and a writer blocked beyond a bounded wait refuses with a named code rather
  than queueing indefinitely. Only the timeout value and journal settings are
  spec-level. The store must sit on local disk — file locking is unreliable on
  network shares.
- Whether `sort_order` is renumbered densely on insert or kept sparse.

---

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-30 | 3 (model coherence, corpus drift, failure modes) | 6 / 12 / 14 / 18 / 1 | Structural rewrite: obligations split into three tiers, export scope and record schema defined, INV-1 given its missing leg, identity grammar corrected after the survey regex was found wrong about two projects, migration source shapes corrected (no project uses pass headings), § 11 added to reconcile with roadmap-format.md. |
