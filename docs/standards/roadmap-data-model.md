# Roadmap Data Model Standard (ANTS-3753)

> **Status:** Adopted 2026-07-30; amended 2026-08-03 (§ 1 INV-2, § 8). **Partly
> implemented** — the store, the migration and the export have shipped
> ([ANTS-3756](../specs/ANTS-3756-roadmap-store-schema.md),
> 3757, 3761, 3764, 3765, 3766, 3767, 3782, 3796, 3797); the published render
> (ANTS-3758), the consumer cutover (ANTS-3793) and the publish + health checks
> (ANTS-3794) have not. Defines *what a roadmap item is*. The schema, export
> serialisation, migration algorithm and check implementations live in the
> implementation specs (§ 9), not here.
>
> **Who conforms:** the store implementation and its migration; and, once a
> project has cut over, anyone writing roadmap items in it. Until that project
> cuts over, nothing here binds it and
> [`roadmap-format.md`](roadmap-format.md) alone governs.
>
> **Source:** user-request-2026-07-30 (roadmap moves to a shared database).

[`roadmap-format.md`](roadmap-format.md) describes one *serialisation* of a
roadmap item (markdown). This describes the model those bytes carry, so a store,
a backup and a published page can each be checked against one definition rather
than against each other. Where the two standards touch the same rule, § 8
records which governs.

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
- [10. Anti-patterns](#10-anti-patterns)
- [What checks this](#what-checks-this)
- [Cold-eyes loop log](#cold-eyes-loop-log)

---

## 1. Three artifacts

| Artifact | Role | Home |
|---|---|---|
| Store | Working store. All reads and writes go here. | Local, untracked |
| Backup export | Durable record. One file per project. | The **private** `claude-config` repo |
| Published render | For human readers. Full detail per item; lossy in *membership*. | Each project's own repo, as `ROADMAP.md` |

**The backup lives in a private repo, and that is a safety requirement rather
than a convenience.** The export carries every item's full technical body,
including items marked `internal` (§ 7.5), and `ants-terminal` is public — so a
backup inside each project's own repository would publish in full detail exactly
what the visibility flag exists to withhold. One private home also means cross-project relationships resolve from a
single checkout, and a project that is not a git repository at all can still be
backed up.

**INV-1 — The export is a complete copy of the store.** Exporting the live
store, rebuilding from that export, and re-exporting produces byte-identical
files — **per project**, and for the corpus as a whole, since the export is one
file per project and a whole-corpus rebuild must not lose the cross-project
relationships of INV-4. **Both legs are required**: re-export equality alone is
satisfied by an export that already lost half the model, since an empty file is
a fixed point. The check therefore compares the committed export against a fresh
export of the **live store**, not only against itself.

**INV-2 — The published render is lossy in *membership*, not in detail.** Per
included item it is written in full `roadmap-format.md` § 3.5 bullet form — § 8
enumerates the required pieces and states the obligation — so the generated file
is the file that exists today, written by the store instead of by hand (user
decision, 2026-08-03). What it
drops is *which* items appear: § 7.5 excludes `internal` and `dropped` ones,
which is why the full-detail backup stays in a private repo and why INV-1
targets the export instead. The render is **not** a source of record (INV-3),
but neither is it write-only — carrying ids is what would let ANTS-3765
§ 2.6.1's id-less re-run matching retire; whether a cut-over project re-reads
its own render is § 9's.

**INV-3 — After a project cuts over, the store is its only writer of record.**
The export and the render are generated; a hand-edit to either is lost at the
next generation, so both are fidelity-checked. Before cutover the markdown
remains authoritative and neither check applies (§ 9).

**INV-4 — Relationships may cross projects.** One project's work being blocked
by another's is currently expressible only as prose.

**INV-5 — A mentioned ID is not a relationship.** An ID appearing in body or
intro prose never creates one; roadmaps routinely mention other projects' IDs,
and this project's own contains `FIBR-` and `ADR-` references in prose. A
relationship is declared by a dedicated field, never inferred from text.

---

## 2. Scope

Every project indexed by the shared store, including projects with no roadmap
yet. The model MUST NOT assume the shape of any one project's corpus — most
installations have one project, not ten. Corpus measurements below are evidence
that a requirement is *satisfiable*; they never define what the model must hold.

---

## 3. Obligations

Three tiers of *obligation* — required at write, required before publish,
accepted at migration. Conflating them is what makes a data model unmigratable:
a corpus assembled under laxer rules cannot retroactively satisfy stricter ones,
and a standard that demands it simply never runs.

Two further values appear in § 4.1's Obligation column and are **not** tiers,
because nothing is obliged to supply them: `optional` means an author may write
the field and nothing rejects its absence, and `derived` means the store
computes it and an author may never write it at all. A conformer's duty is
discharged by the three tiers alone.

### 3.1 Required at write — items created or curated after cutover

**§ 4.1's Obligation column is the canonical list.** Every field marked `write`
is required at this tier, and its parenthesised qualifiers carry the conditional
cases — `write (open)`, `write (closed)`, `write (status shipped)`. The list is
not repeated here: one field→tier mapping in two encodings is what
`documentation.md` § 1.5 forbids, and a field added to one and not the other is
the drift it predicts. What the table cannot express follows.

`sort_order` is **not** in this tier: § 5 derives it from the element list, and a
write obligation on it as well would give one fact two authored encodings with
no rule naming which is right when they disagree. A
curating write still places the item — it does so by position in the section's
element list, which is where the order actually lives.

`shipped` is required only for `shipped`, not for every closed item — a
`dropped` item has no ship date, and demanding one would be a nonsense state.

A status flip on a migrated item is **not** a curating write. Requiring the full
set there would reject the commonest operation on legacy data, for fields the
item was never obliged to carry.

### 3.2 Required before publish

`layman`, **on open items only**. An item without it cannot appear on the
published render, so a project with any public open item lacking one fails the
publish gate — it does not fail migration. A closed item publishes without
`layman`: § 3.3 leaves the field empty on migrated items, so gating closed
items too would make the gate unsatisfiable for every project with published
history. This is the **only** publish-gating field; `priority` and `resolution`
gate neither publishing nor migration.

### 3.3 Accepted at migration — historical items

Migration MUST NOT reject an item for a field the source format never required.
Where `roadmap-format.md` § 3.5.3 defines a default, migration applies it
(`kind` → `implement`, `source` → `planned` — that is § 3.5.3's default *source*, unrelated to the `planned` **status** of § 7.3) and records that the value was
defaulted. Where no default exists — `layman`, `priority`, `resolution`, and any
date not derivable from history — the field is left empty.

This is the only arrangement the corpus permits. Across the surveyed corpus,
**half** the items carry no `Kind:`, **half** carry no `Source:`, and **over
half** carry no `Layman:`. A write-time-only reading would refuse every project.

Figures here and below come from `tools/roadmap-corpus-survey.py`, which finds
every project under the shared root; it reported 13 of them on 2026-08-03, up
from 10 four days earlier — which is the drift this paragraph is about. They are quoted as proportions and orders of
magnitude on purpose: the corpus grows every time anyone files an item, so an
exact count written into a standard is wrong within the day. Re-run the script
rather than trusting a number in this file — per § 2 these measurements only
evidence that a requirement is *satisfiable*.

**The survey finds roadmaps case-insensitively, and that is load-bearing.** One
project names its file `roadmap.md`; an uppercase-only glob excluded it, and
this document consequently asserted that no project used the § 8 pass-headings
format when one tracks 144 items in it. A measurement that cannot see a
non-conforming project cannot be evidence about the corpus.

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
| `id_origin` | write | § 7.1. What *shape* the ID arrived in: `parsed` (matched `roadmap-format.md` § 3.5.1's grammar), `synthesised` (the model derived or allocated it — § 7.1's `PASS-N-M` ids and § 7.2's allocations), `quarantined` (off-grammar, recorded verbatim). Distinct from `provenance.id` (§ 7.7), which records *who supplied* a value; an item can be `synthesised` and `migrated` at once, and § 7.1 needs a place to record that an ID is unparseable without rewriting it. |
| `status` | write | § 7.3. |
| `headline` | write | One line, technical. |
| `layman` | write (open) / publish (open) | One sentence, non-technical. The only text a public reader sees. |
| `kind` | write | § 7.4. |
| `source` | write | Provenance. "What did project X ask for" is a query on this field. |
| `priority` | write (open) | § 7.5. Meaningless once closed. |
| `created`, `last_modified` | write (store-populated) | § 7.6. Required to be *present*, never supplied by an author: the store stamps them, and migration fills them per § 7.7 (`git-derived`, or `asserted` from a dated `Source:`). A write path that rejects a call for omitting them has misread this row. |
| `shipped` | write (status `shipped`) | § 7.6. |
| `resolution` | write (closed) | What was done and why, or why it was not. |
| `section` | write | § 5. Where the item is filed. |
| `body` | optional | Free-form technical detail. Optional because many items are complete in one line, and a mandatory body produces filler that reads like content. |
| `lanes`, `evidence` | optional | Subsystems touched; paths to screenshots, logs, repros. Both are already first-class in `roadmap-format.md` § 3.5, so neither is part of § 4.3's invented tail. |
| `visibility` | optional | § 7.5. Defaults to `public`. |
| `milestone` | optional | Target release, as the version string the project releases under (`0.7.55`, not a section heading). Distinct from `section`, which is where the item is *filed*. |
| `sort_order` | derived | § 5. The item's position within its section, computed from that section's ordered element list. **An author never writes it**, and it is not stored — storing it as well would be a second encoding of the element list's own ordering, and § 5's precedence rule exists because the two would drift. Readers that want an integer rank get one computed at read. |
| `blocked` | derived | True iff a `blocked-by` relationship targets a **resolvable, same-project** item that is not closed. *Resolvable* = the target exists in the store being read. Cross-project targets are excluded — a partial rebuild cannot see them, and deriving from what is absent would make the value depend on which projects happen to be present. **So `blocked` under-reports by design** (see INV-4): an item blocked only from another project reads `blocked: false`, and a consumer that needs the true answer queries the `blocked-by` relationships themselves rather than this field. |
| `extras` | optional | § 4.3. |
| `provenance` | derived | § 7.7. Per field, never silently promoted to `asserted`. |

`resolution` is required at close and `body` is not, because the institutional
value sits in closed items: a shipped item with no resolution records that
something happened and nothing about what. Specs do not substitute — this
project holds a couple of hundred spec documents against nearly two thousand
items, so for the large majority the item is the only technical record.

### 4.2 Dates have a known limitation

`created`, `last_modified` and `shipped` backfilled from git history are marked
`git-derived` (§ 7.7), and are **not** reliable for items that have been
through archive rotation:
`roadmap-format.md` § 3.9 moves bullets byte-identically into an archive file,
so the commit touching such an item is the rotation, not the work. Cycle-time
reporting is therefore sound for items created after cutover and best-effort
before it. Saying so is better than publishing a figure nobody can trust.

### 4.3 The extension mechanism is a correctness requirement

Four field keys dominate the corpus — `Kind:`, `Source:`, `Layman:` and
`Lanes:`, together accounting for the overwhelming majority of all field lines
— followed by a **long tail of several hundred distinct keys** that individual
projects invented. About twenty of those are in real use rather than one-off:
`Fix:`, `Scope:`, `Why:`, `Dependencies:`, `Spec:`, `Test:`, `Repro:`,
`Root cause:`, `Risk:`, `Design:`, `Related:`, `Verified:`, `Problem:`,
`Impact:`, `Acceptance:` and others.

A model with only § 4.1's fields would **silently delete that tail at the first
regeneration**. `extras` prevents it. Two tail keys are read as relationships
instead (§ 6): `Spec:` always, and `Dependencies:` only where its free-text value
resolves to an item ID — one that does not stays in `extras` unconverted.

---

## 5. Structure

Items live in a section tree, and their order carries meaning: position is the
current corpus's prioritisation, so that order is preserved exactly.

`priority` and `sort_order` are complementary. `sort_order` is an exact order
**within a section**; `priority` is a coarse band comparable *across* projects,
which position can never be. **The section's element list is where that order
lives, and it is the only place it lives** — `sort_order` is recomputed from the
list rather than stored beside it (§ 4.1, `derived`). The two therefore cannot
disagree, because there is nothing for the list to disagree with.

A **project** holds its sections, plus the status legend of § 5.1 — the legend
belongs to the project rather than to any section, because it describes the
whole document's vocabulary.

A section has a slug, title, level, optional intro prose, an optional parent, a
**position**, a **source**, and an **ordered element list**. The last two are
not bookkeeping:

- **`position`** is the section's place in its project's document order.
  Sibling sections have no other ordering fact to derive it from, so without it
  the published render cannot reproduce the order of the file it replaces — and
  a rebuild from the export re-files prose sections among version blocks.
- **`source`** records which file the section was read from: the live roadmap,
  or one of `roadmap-format.md` § 3.9's rotated archives. Without it a rebuild
  folds a rotated archive back into `ROADMAP.md`, un-rotating every archive at
  the first render.

Both are part of the store, so both are inside INV-1's "complete copy" and must
survive the export round-trip. Elements are the ordered contents of a section:
references to items, plus the content that belongs to no single item.

| Element | Carries |
|---|---|
| `item` | A reference to a § 4 item. |
| `narration` | Section-summary prose that belongs to no single item. |
| `table` | A markdown table: header plus ordered rows. |

**Prose belongs to its item.** Sub-bullets, step lists and detail lines beneath
an item are that item's `body`; fenced code blocks belong to a `body` or an
`intro`. Neither is a section element, because both are subordinate to something
specific rather than interleaved at section level.

That distinction is load-bearing rather than tidy. Most of the corpus's
status-marked lines carrying no bold headline are item detail (§ 5.2's table
row) —
MAME Curator's `` `tests/api/test_fp09_fixes.py:362` — wrapped the SSE
history-replay `` and Music Production's `Step 3 — failing tests for
TC-06-17/18/19` are sub-steps of a parent item, not free-standing content.
Modelling them as section elements would detach them from the item they
describe, and the item would then be published without the prose that explains
it — § 4.3's failure by another route.

### 5.1 The status legend is structured, not prose

Where a project documents its status vocabulary, it does so in its own words:
this project writes `In progress (active commit work — usually direct-to-main
on this project; rarely a branch / PR)` where `roadmap-format.md` § 3.3's own
table says `In progress (being tackled now)`. Two of the surveyed projects carry
such a block, eight lines in total, and they are the document's *metadata* —
not items, and not narration.

They are therefore their own per-project structure: status value → that
project's wording. Today `RoadmapDialog` cannot: `src/roadmapdialog.cpp` holds the four status
emojis and their labels as compile-time constants, guarded by a
`static_assert` on the count, and nothing reads a project's legend at all. A
project whose legend words differ is rendered in the dialog's words, not its
own. Holding the legend as data is what would let one renderer speak each
project's vocabulary; whether the dialog should then do so is § 9's call, not
this document's.

### 5.2 Structures the model must survive

Measured across the surveyed corpus, in descending order of how much of the
corpus each accounts for. **These are orders of magnitude and they drift** —
`tools/roadmap-corpus-survey.py` prints the live values, and § 3.3 says why a
standing number in this file is the wrong thing to trust:

| Order of magnitude | Structure | Home |
|---|---|---|
| ~1,500 | sub-bullets | item `body` |
| ~170 | markdown table **data** rows | `table` element |
| ~80 | status-marked detail lines | item `body` |
| ~20 | fenced code blocks | `body` or `intro` |
| ~10 | status-legend lines | § 5.1 legend, on the project |
| residual | section-summary prose | `narration` element |

A table's separator row (`|---|---|`) is delimiter, not content: it carries no
cell values and is regenerated by any renderer, so it is not a row the model
stores. Counting it as one inflates the table figure by roughly a tenth and
would have the store round-tripping a line that means nothing.

---

## 6. Relationships

Typed and directed. The last column separates two different things, because
conflating them would breach INV-5: **converted** means migration reads an
existing structured field and writes the relationship; **supersedes going
forward** means the relationship replaces a practice, with nothing harvested.

| Type | Meaning | Migration | Replaces |
|---|---|---|---|
| `splits-from` | Carved out of a parent. | authored | ANTS-3748 — a split currently leaves feedback files citing only the parent, whose completion then over-claims. |
| `blocked-by` | Cannot start until the target closes. | **authored** | Prose block markers, which are *not* harvested — see below. |
| `duplicate-of` | Same work as the target. | authored | Manual dedup. |
| `supersedes` | Replaces an earlier decision. | authored | Nothing — currently unrecorded. |
| `relates-to` | Untyped association. | converted from `Dependencies:` (21 uses) | — |
| `specified-by` | Target is a spec **document**, addressed by path. | converted from `Spec:` (20 uses) | — |

**`blocked-by` is authored-only, and migration harvests nothing for it.** Prose
saying an item is blocked stays prose in the item's `body`. INV-5 is the reason:
inferring a relationship from text is precisely what it forbids, and a
best-effort parse of "blocked on the parser work" would manufacture edges that
look declared. A pre-cutover block is therefore recorded as history, not as a
relationship, until someone declares it.

`Dependencies:` values are free text, so conversion is best-effort in a
different sense: a value that resolves to an item ID becomes a `relates-to`
edge, and one that does not stays in `extras` unconverted. That is a
structured-field read, not a prose inference, so INV-5 holds.

A relationship's target is an item *or* a document, and the two are addressed
differently — items by the `(project, id)` pair, since § 4.1 makes `id` unique
only within its project; documents by path. `supersedes` may therefore target an
ADR or any other decision record, not only a spec.

`Spec:` values are paths rather than IDs, and that does not weaken INV-5:
what INV-5 forbids is harvesting a relationship from *prose*, and `Spec:` is a
declared field whatever shape its value takes.

`relates-to` is the one **symmetric** type: A relates-to B implies B relates-to
A, stored once and rendered both ways. Storing it twice would make INV-1's
byte-identical round-trip depend on which direction happened to be written first.

`splits-from`, `blocked-by`, `duplicate-of` and `supersedes` are acyclic —
`supersedes` included, because "A replaces an earlier decision B" that also
holds in reverse names no current decision. Acyclicity is checked over the
**full store only**: a partial checkout can break a cycle by not containing
part of it, so checking there would report a pass that the whole store fails.

Three adjacent record types exist for the same reason: **`feedback_ref`** (which
cross-session feedback file cites which item — making ANTS-3744 a query),
**`citation`** (item or spec → file and symbol, making `documentation.md` § 1.7
machine-checkable), and **`history`** (one row per field change).

**`history` is exported.** It is worth saying explicitly because it is the one
record type whose bulk invites an exception, and INV-1 admits none: git
currently carries the history of every roadmap edit because the roadmap *is* a
tracked file, and under this model the store is untracked and the render lossy,
so that history would otherwise have nowhere to live. Exporting it is the point,
not an optimisation.

---

## 7. Enumerations and identity

### 7.1 Identity

An item ID matches `roadmap-format.md` § 3.5.1's grammar, **not a narrower
one**. Live prefixes include `ANTS`, `DOOM`, `FIBR`, `CL`, `ONEUP`, `ROLO`,
`3D_E` and `mame-curator` — so a prefix may contain hyphens, underscores and
digits, and a project may declare **several**.

Two of § 3.5.1's rules are not identity rules, and the model must not read them
as such. Its `-\d+` regex is the acceptance test; its "zero-padded to 4 digits"
prose is a **write-side convention**, so `CL-9` is a well-formed existing ID and
not an unparseable one. And its § 3.10.4 says id *handling* — parsing,
fetching, allocation — is case-insensitive, which is a statement about the
tooling's grammar, not about when two IDs denote the same item.

**Identity is therefore this document's own decision, stated here: IDs are
compared case-INSENSITIVELY within a project.** `Sh-1` and `SH-1` are one
item, and § 4.1's per-project uniqueness is enforced on the case-folded value.
This follows the tooling rather than fighting it — a store that treated them as
two items would allocate an ID `roadmap_log` then refuses as a duplicate.

**Some live IDs do not match that grammar, and the model must say what happens
to them rather than assume they are absent.** § 3.5.1 requires a dash between
prefix and number; 3D_Engine writes three items as `[Cl9]`, `[Cl10]` and
`[CE18]`, with no dash. A migration built on the grammar alone drops them
silently, which is the one outcome § 3.3 forbids. They are **items with an
unparseable ID**: migration MUST NOT invent a dash (rewriting an ID breaks
§ 3.5.1's append-only rule and every citation of it), and MUST NOT treat them as
ID-less (§ 7.2's bulk allocation would issue a second identity for an item that
already has one).

Instead migration **quarantines** them: the item is imported with its ID
recorded verbatim and `id_origin` set to `quarantined` (§ 4.1), the project's
migration completes
rather than blocking, and the run reports them. The project then either amends
`roadmap-format.md` § 3.5.1 to admit the shape, or accepts the ID as opaque —
both are decisions with consequences beyond one project, which is why the model
declines to pick one. Three items in the corpus are affected.

**An ID is recognised only at an item's leading position, never mid-text.** A
bracketed token matching the grammar anywhere else is prose: `DOOM_Ants`
contains `players[idx-1]` and `row[right-1]`, both of which match the grammar,
so a grammar-anywhere rule would refuse that project outright.

"Leading position" resolves per source shape, because the corpus has three and
only one of them has bullets:

| Shape | Where the ID is | Recognition rule |
|---|---|---|
| Emoji bullet (§ 3.5) | `- ✅ [ANTS-1234] **…**` | Immediately after the status emoji. |
| GFM task list (§ 3.10.1) | `- [x] [3D_E-0007] **…**` | Immediately after the checkbox. |
| Pass heading (§ 3.10.5) | nowhere in the text | **Synthesised** from the heading as `PASS-<major>-<minor>[-<sub>]`. |

A synthesised `PASS-N-M` **is** an ID for every purpose in this document: it
satisfies § 3.1's `id` obligation, so those items are not ID-less and § 7.2
must not allocate a second identity for them. It is not, however, an
*allocated* ID — it is derived from the heading, so renumbering a pass changes
it, which is the one place this model's identity is not append-only. That
tension is real and belongs to § 9 along with the rest of migration.

The store owns allocation. Each project currently keeps a gitignored per-machine
counter — one per prefix, for the multi-prefix projects § 3.10.4 permits — that
is explicitly not the source of truth, with a floor recomputed by scanning the
corpus so a wiped counter cannot reissue a live ID. A shared store allocates
directly and that whole failure mode disappears, along with the per-prefix
bookkeeping.

### 7.2 Allocating IDs to items that have none

Every item needs an ID, and roughly 1,600 in the corpus have none — about 40%
of it. (Pass-heading items are **not** among them: § 7.1 synthesises their ids,
so they arrive already identified.) Allocation splits by whether anyone will
ever need to cite the item:

| | Share | Rule |
|---|---|---|
| **Closed** | ~1,020 | Allocated in bulk, in document order. Nobody cites a finished item, so no curation is required and none is invented. |
| **Open** | ~600 | Allocated into the project's normal sequence and treated as a real item — it will be cited, worked on, and referenced in commits. § 3.2's publish gate then applies, so it must be curated before that project publishes. |

The two rows differ in *obligation*, not in ID shape and not in provenance
(§ 7.7 states where both stand). A separate archival prefix would add a second
prefix per project to reconcile, and an ID's text should never encode metadata
that can change.

**Only bullets that are items get an ID**, and a bullet is an item when it
carries **both** a status marker and the bold headline `roadmap-format.md` § 3.5
requires. Both halves are needed: § 3.3 allows plain narration bullets with no
status marker, which § 5 models as `narration`, so the bold headline alone would
promote them to items. The status-marked bullets carrying a marker but neither
an ID nor a bold headline are not items, and they are not one thing either —
most are detail lines belonging to a parent item's `body`, the rest are
status-legend lines (§ 5.1). § 5.2's table carries the live proportions. Neither
gets an ID.

### 7.3 Status

`planned` · `in-progress` · `shipped` · `considered` · `dropped`

`dropped` is new — abandoning an item currently means deleting its line, erasing
the decision with it. It has **no markdown serialisation**: `roadmap-format.md`
§ 3.11 makes a fifth status emoji an anti-pattern.

Dropped items are excluded from the published render as **policy**, not as a
consequence of that gap. A reader of the roadmap wants to know what is being
worked on, and abandoned work is noise to them; the decision is preserved in the
store and the export, which is where it has value. So adding a fifth emoji to
`roadmap-format.md` would not by itself put dropped items on the render — that
would be a separate decision, and § 7.5 states the exclusion unconditionally.

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

`priority` is `1` (highest) to `5` (lowest), required on open items **written
after cutover** — § 3.3 leaves it empty on migrated ones, like every other field
with no source-side counterpart. Five bands
rather than ten: ten levels are not reliably distinguishable, so they collapse
in practice to three with the rest defaulting to the middle, and the number
stops carrying information. The prose severity vocabulary — CRITICAL, HIGH, MEDIUM, LOW — is
`roadmap-format.md` § 3.8's, where findings carry it in the headline;
its § 3.5.2 is the carrier that puts one in a `Priority:` body line. Both map
CRITICAL → 1, HIGH → 2, MEDIUM → 3, LOW → 4. Where an item carries both, the
`Priority:` line wins: it is the field an author set deliberately, where the
headline word is inherited from whichever review raised it. Band 5 is reserved
for someday-maybe work that no severity word expresses.

`visibility` is `public` or `internal`. The published render includes only
`public` items, and excludes `dropped` items regardless. Today everything is
published because the file *is* the record — including security findings that
are still open.

### 7.6 Dates

`created`, `last_modified`, `shipped`, ISO 8601 (`documentation.md` § 1.3).

**No per-item _closure_ date field exists today** — closure is prose
(`Resolved (2026-07-30): …`). Release blocks and CHANGELOG sections carry dates
at *release* granularity, so "what shipped in version X" is already answerable;
"when did this item close", "how long do items stay open" and "is the backlog
growing" are not. Those three are the gain, subject to § 4.2's limitation.

One per-item date **does** exist and is worth harvesting: `roadmap-format.md`
§ 3.5.3's dated `Source:` values (`user-YYYY-MM-DD`, `audit-YYYY-MM-DD`,
`indie-review-YYYY-MM-DD`, and the rest) record when an item was raised. For a
folded-in finding that is a better `created` than the git-derived value, because
it survives the archive rotation § 4.2 warns about. Migration prefers it where
present, and marks the result `asserted` rather than `git-derived`.

Effort and size estimates are deliberately excluded: they are guesses that
attract false precision, and `created` + `shipped` yield real cycle time.

### 7.7 Provenance

`provenance` records **per field** where a value came from. It is the model's
honesty mechanism: without it a defaulted `kind` and an author's considered
`kind` are indistinguishable, and every later reader over-trusts the corpus.

| Value | Meaning |
|---|---|
| `asserted` | An author supplied it. |
| `defaulted` | Absent at migration; § 3.3 applied `roadmap-format.md` § 3.5.3's default. |
| `git-derived` | Recovered from commit history, subject to § 4.2's limitation. |
| `migrated` | Generated by migration itself, with no source-side counterpart — the case § 7.2's bulk-allocated IDs fall into. |

A value is **never silently promoted to `asserted`.** Editing a field through
the store sets `asserted` for that field only; the item's other fields keep
what they had.

Because it is per field, `provenance` is not an item-level label — an item
routinely carries an `asserted` headline beside a `defaulted` kind and a
`migrated` id. § 7.2's two allocation rows differ in *obligation*, not in
provenance: **both** get `provenance.id = migrated`, since neither ID was
chosen by an author.

---

## 8. Relationship to roadmap-format.md

That standard governs the markdown format and keeps doing so — except at the
one point below where, after cutover, it stops describing the file at all.
These are the points where the two touch, stated so neither is silently
overridden:

- **The render is a conforming `ROADMAP.md`, and that is a requirement on the
  render rather than a happy accident.** Its § 3.5 makes the status emoji, the
  `[PROJ-NNNN]` ID, the bold headline and `Kind:` *required pieces* of every
  bullet; its § 3.1 requires the format marker; its §§ 3.6.2–3.6.3 match
  CHANGELOG entries and commit subjects against bullet **headlines**.
  `documentation.md` § 3 binds the same file independently, to status emojis and
  stable per-bullet IDs. A full-fidelity render (INV-2) satisfies all of them,
  so neither standard is amended and the filename does not move. **The
  obligation runs the other way**: a render that dropped any required piece
  would break commit and CHANGELOG matching in every cut-over project. The
  choice this once forced — amend both standards, or retire `ROADMAP.md` — is
  closed by INV-2; what remains is § 9's check that catches a render dropping a
  required piece silently.

- **Optional fields.** Its § 3.5 files `Layman:` and `Source:` as optional and
  § 3.5.3 gives defaults for absent `Kind:` / `Source:`. This document does not
  change that for markdown; § 3.3 *adopts* those defaults, and § 3.1's stricter
  obligations apply only to writes through the store.
- **ID allocation.** Its § 3.5.1 keeps a `.roadmap-counter` under a flock, but
  is explicit that the counter is a derived per-machine cache and **not**
  source: the true high-water mark is the highest ID across the committed
  corpus, which it defines as `ROADMAP.md` + `CHANGELOG.md` +
  `docs/roadmap/*.md`. After a project cuts over the store owns allocation and
  the counter is retired **for that project**. The render keeps carrying IDs
  (INV-2), so the layman-only premise no longer removes that leg by itself — but
  whether the floor narrows is still open, on two counts: § 9 has yet to decide
  whether the render lists closed items at all, and the bullet below leaves the
  archives' and the CHANGELOG's futures unsettled. The export is subject to
  neither and supersedes all three as the authoritative floor, which is what
  § 3.5.1's definition needs amending to say for cut-over projects.
- **Status vocabulary.** Its § 3.11 makes a fifth status emoji an anti-pattern,
  so `dropped` has no markdown form and is excluded from the render. Adding one
  is that standard's decision, not this one's.
- **Pass headings.** Its § 3.10.5 documents `#### Pass N.M` as a supported read
  *and* write format, and one surveyed project tracks **144 items** in it —
  counted as `#### Pass N.M` headings; its `- **Status**:` lines are counted
  separately and are more numerous, so the two figures below are not two
  measurements of one population. It
  is therefore squarely **in** migration scope. Two consequences the model must
  carry: its items are identified by synthesised `PASS-N-M` ids (§ 7.1), and
  its status is a free-text `- **Status**:` line rather than an emoji —
  **136 of that project's 154 status values fall outside § 7.3's five-value
  enum** (`deferred`, `partial`, `un-gated`, `shipped in v3.6.15`, and a long
  tail carrying prose). Normalising them is a per-value mapping decision of the
  same kind as § 7.4's, and it is **not made here**: § 9 owns it, because
  unlike `Kind:` the corpus offers no canonical target for `deferred` or
  `partial` and inventing one is design work.

- **Archive rotation and the release flow.** Its § 3.9 rotates closed bullets
  byte-identically into `docs/roadmap/*.md`, and its preamble moves released
  work into the CHANGELOG. Both are **hand edits to a file that becomes
  generated**, so under INV-3 both are lost at the next render. Whether the
  archives freeze at cutover or the store takes over rotation is § 9's call;
  what matters here is that the two size-management rules stop working and
  neither standard currently says so.

---

## 9. What the implementation spec owns

This document deliberately stops at the model. The following are design work
with real decisions in them, and belong in a spec that goes through the
implementation gate rather than in a standard:

- The schema: entity keys, cardinalities, per-element columns, and how `extras`
  and `provenance` are stored.
- The export: field order, sort collation, encoding, and every other rule that
  makes INV-1's "identical file" testable rather than aspirational. Its
  *membership* is already fixed — § 6 rules `history` in, INV-1 rules the whole
  store in.
- Migration: the algorithm, per-project atomicity, re-run matching, what happens
  to an item deleted from source, and the cutover transition — including the
  interim in which some projects are migrated and others are not. Its **policy**
  is already fixed by §§ 3.3, 7.1, 7.2 and 7.4; what is open is how to execute
  it. One policy question is deliberately left here rather than decided above:
  **how the pass-headings status vocabulary normalises.** § 7.4 could map `Kind:`
  because every stray value had an obvious canonical target; `deferred` and
  `partial` have none in § 7.3's enum, so the choice is between adding statuses
  and losing information, and that is a decision with consequences rather than a
  lookup table.
- The **remaining** checks — their inputs, pass conditions, scheduling, and
  behaviour on a machine where the store does not yet exist. INV-1 already fixes
  the export round-trip check; § 7.7 already fixes what `provenance` must record.
- Concurrency across projects sharing one store, and the auto-publish cadence to
  the backup repo — including that a push conflict means two stores diverged and
  must surface rather than auto-merge, and that a silent backup failure is worse
  than no backup because it stops anyone checking.
- Whether the published render lists closed items at all, or only open work plus
  recent releases. (Which items are *eligible* is already fixed by § 7.5: public,
  not dropped. This is the curation question inside that set.)
- How the render **demonstrates** § 8's conformance rather than asserting it. The
  filename and the two-standard amendment question are closed by INV-2; what is
  open is the check that catches a render silently missing a required piece, and
  whether a cut-over project re-reads its own render at all.
- Whether archive rotation and the CHANGELOG release flow (§ 8) freeze at
  cutover or move into the store.
- The fate of `roadmap_query`, `roadmap_log` and `RoadmapDialog`, all of which
  parse and write `ROADMAP.md` today. A full-fidelity render keeps them able to
  *parse* it, so what INV-3 ends is their **writing** — which is why they still
  need a cutover rather than none. This includes whether `RoadmapDialog` should
  render each project's legend (§ 5.1 makes it *possible*, not mandatory).

ANTS-3754 carries the verified finding list that is those specs' input.

---

## 10. Anti-patterns

The prohibitions above, collected — a conformer arrives holding a defect rather
than a question.

- ❌ **Rejecting a migrated item for a field its source format never required**
  (§ 3.3). Half the corpus has no `Kind:`; a write-time reading refuses every
  project.
- ❌ **Demanding `layman` on closed items before publish** (§ 3.2). Migration
  leaves it empty, so the gate becomes unsatisfiable for any project with
  history.
- ❌ **Dropping the long tail of project-invented field keys** (§ 4.3). Several
  hundred exist; a model with only § 4.1's fields deletes them at the first
  regeneration.
- ❌ **Modelling an item's sub-bullets or code blocks as section elements**
  (§ 5). They are subordinate to one item, and detaching them publishes the item
  without the prose that explains it.
- ❌ **Counting a table's separator row as a row** (§ 5.2). It is a delimiter,
  regenerated by any renderer.
- ❌ **Inferring a relationship from prose** (INV-5, § 6). A mentioned ID is a
  mention. `blocked-by` in particular is authored-only.
- ❌ **Recognising an ID anywhere but an item's leading position** (§ 7.1).
  `players[idx-1]` matches the grammar and is not an ID.
- ❌ **Treating a pass-headings roadmap as ID-less** (§§ 7.1, 8). Its ids are
  synthesised; allocating over them mints a second identity.
- ❌ **Rewriting an off-grammar ID to make it parse** (§ 7.1). IDs are
  append-only; rewriting one breaks every citation of it.
- ❌ **Promoting a value to `provenance: asserted` because it was touched**
  (§ 7.7). Only an author's write asserts.
- ❌ **Publishing a `dropped` or `internal` item** (§ 7.5). The visibility flag
  exists to hold open security findings.
- ❌ **Hand-editing the render or the export after cutover** (INV-3). Both are
  generated; the edit is lost silently.

## What checks this

| Rule | What catches a breach |
|---|---|
| § 2, §§ 3.3, 5.2, 7.2, 7.4 corpus claims | `tools/roadmap-corpus-survey.py` |
| INV-1 export round-trip | `tests/features/roadmap_export_roundtrip` — `Inv1RoundTripIsByteIdentical`, `Inv2EveryRowAndColumnSurvives` |
| INV-2 render fidelity | **nothing yet** — there is no render before ANTS-3758 |
| INV-3 hand-edit detection | **nothing yet** — § 9's |
| §§ 3.1–3.2 obligations, § 7.3–7.5 enums | **nothing yet** — the store's write path, § 9's |
| § 7.1 identity grammar | `roadmap-format.md` § 3.5.1's regex, already in `RoadmapIndex::isCanonicalId` |
| § 8 reconciliation | **nothing** — prose agreement between two standards. The one amendment it still owes (`roadmap-format.md` § 3.5.1's counter definition) is ANTS-3793's, with the rest of the cutover. |

---

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-30 | 3 (model coherence, corpus drift, failure modes) | 6 / 12 / 14 / 18 / 1 | Structural rewrite: obligations split into tiers, export scope defined, INV-1 given its missing leg, identity grammar corrected after the survey regex was found wrong about two projects, migration source shapes corrected. |
| 2 | 2026-07-30 | 3 (same partition, cold) | 13 / 19 / 17 / — / — | **Stopped and split.** ~8 of the 13 CRITICALs were collateral from loop 1's own fixes; the findings were overwhelmingly schema-level, i.e. this document was a standard carrying an implementation spec. Split per ANTS-3754: the model stays here, the schema goes to a spec. Backup relocated to the private config repo, closing a leak the draft shipped. ID allocation for the corpus's ID-less items decided (user, 2026-07-30). |
| 4 | 2026-08-03 | 2 (single doc, cold; genre pinned `standard`) | 0 / 4 / 7 / 9 / 0 | **ANTS-3795 amendment gate**, run on the edit that made the render full-fidelity. 20 verified, 3 dismissed, all 20 fixed, plus 2 collateral the blast-radius sweep caught. Both lanes independently led on the same three: the **Status header still said "not yet implemented"** while the store, migration and export had shipped; **§ 5's section definition omitted `position` and `source_path`**, the two columns ANTS-3796/3797 added and the render depends on, so the model as written could not round-trip what the store already stores; and the *What checks this* table still said **nothing yet** for INV-1 when `roadmap_export_roundtrip` tests it. Also fixed: § 7.1 mandated flagging an unparseable ID with no field in § 4 to carry it while the store had shipped `item.id_origin` — now a § 4.1 field. **Every corpus figure was re-derived rather than reasoned about**, and the survey now reports 13 projects and 4,080 items against the draft's 10 and its counts of 86 / 78 / ~90 / 8 / 280 / 218; the exact counts are gone in favour of the proportions § 3.3's own policy asks for. One finding landed on THIS amendment: the ID-floor claim that "all three inputs survive" ignored § 9's still-open render-curation question. |
| 3 | 2026-07-30 | 3 (model coherence, identity/migration, cross-doc) — genre pinned `standard` | 6 / 9 / 14 / 15 / 2 | Every verified finding fixed. Corpus scope was wrong: the survey globbed `ROADMAP.md` and missed a tenth project whose file is lowercase, so the document asserted no project used pass headings when one tracks 144 items in it, 136 of whose statuses fall outside § 7.3's enum. Also fixed: § 5.1's justification was false against `roadmapdialog.cpp`; § 8 gained the render-conformance and archive-rotation touch points; identity semantics, `provenance` (new § 7.7), obligation-tier vocabulary and the `blocked-by`/INV-5 conflict all settled. Added § 10 anti-patterns and a *What checks this* table. Genre pinning is what changed the run: loops 1–2 graded a standard against spec shape. **Exited at the 3-loop cap.** |
