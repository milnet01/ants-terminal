# Roadmap Data Model Standard (ANTS-3753)

> **Status:** Adopted 2026-07-30; amended 2026-08-03
> (§ 1 INV-2, §§ 3.3, 4.1, 5, 8) and 2026-08-05 (§ 1 INV-3, § 3.2, § 4.1,
> § 7.1, § 8, § 9, § 10, *What checks this*). **Partly
> implemented** — the store, the migration, the export and the published
> render have shipped
> ([ANTS-3756](../specs/ANTS-3756-roadmap-store-schema.md),
> 3757, 3758, 3761, 3764, 3765, 3766, 3767, 3782, 3796, 3797), as has
> `roadmap_log`'s write half (ANTS-3809). The consumer cutover (ANTS-3793)
> has **shipped**: the read seam, `roadmap_query`, and `RoadmapDialog`, which
> reads its bullets through `RoadmapSource::bulletsFor()` and its legend through
> `storeLegend()`, falling back to a markdown parse only where there is no store
> row. What INV-3 still ends is their **markdown writing**, not their reading.
> The publish + health checks (ANTS-3794) have not started. Defines *what a roadmap item is*. The schema, export
> serialisation, migration algorithm and check implementations live in the
> implementation specs (§ 9), not here.
>
> **Who conforms:** the store implementation and its migration; and, once a
> project has cut over, anyone writing roadmap items in it. Until that project
> cuts over, nothing here binds it and
> [`roadmap-format.md`](roadmap-format.md) alone governs.
>
> **Source:** user-request-2026-07-30 (roadmap moves to a shared database).

**Citation convention:** a bare `§ N` is a section *of this document*. A cite to
another standard names it — `roadmap-format.md § 3.5` — or carries a pronoun
whose antecedent is that standard in the same sentence. Both documents have a
§ 3.3 and they say different things, so an unanchored cite is a defect here, not
a shorthand.

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
what the visibility flag exists to withhold. One private home also means
cross-project relationships resolve from a single checkout, and a project that is not a git repository at all can still be
backed up.

**The file's name and shape are ANTS-3761 § 2.1's, not this document's**:
`roadmap-export/<export_slug>.jsonl`, one RFC 8785-canonical JSON object per
line. `export_slug` is the project's slug column, carrying a `UNIQUE` constraint
precisely so two projects cannot overwrite one another's backup. The one rule
that belongs to neither document is that the backup repo's `.gitignore` is an
**allowlist**: a `roadmap-export/` directory it does not name is written and
never committed, and `git status` stays clean while that happens. Naming it is
part of publishing, so ANTS-3794 owns it, and the first export is verified with
`git log -- roadmap-export/` rather than by looking at the working tree.

**INV-1 — The export is a complete copy of the store.** Exporting the live
store, rebuilding from that export, and re-exporting produces byte-identical
files — **per project**, and for the corpus as a whole, since the export is one
file per project and a whole-corpus rebuild must not lose the cross-project
relationships of INV-4. **Two legs are required, and the second is the one that
bites**: (a) export → rebuild → re-export is byte-identical, and (b) the
committed export equals a fresh export of the **live store**. Leg (a) alone is
satisfied by an export that already lost half the model, since an empty file is
a fixed point.

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

**INV-3 — Once a project is *store-migrated*, the store is its only writer of
record.** **"Store-migrated" is a store project row *plus* a roadmap the format
detector classifies `ants-v1`** — the pair `RoadmapSource::migratedProject()`
tests, defined in `roadmap-format.md` § 3.5.1. **"Cut over" names the first
conjunct and nothing beyond it.** That function's only project-level test is
`RoadmapStore::readProjectByRoot()` returning a row, and no cutover flag exists
anywhere in the code — so no project is store-rowed and not-yet-cut-over, and
this document's phrasing and § 3.5.1's ("a store row *and* a roadmap this
format's detector classifies as `ants-v1`") are the same test stated twice, not
two tests that could disagree about which ID carrier a project reads.
The second conjunct is load-bearing and is used in that sense throughout this
document: a project with a row whose roadmap is a GFM or pass-headings dialect
fails that function's closing `ants-v1` test, is still written by the markdown
splice paths, renders nothing, and so is **not** covered by this invariant.
Retiring those paths is § 9's.
The export and the render are generated; a hand-edit to either is lost at the
next generation, so both must be fidelity-checked. *What checks this* has the
current state; the one distinction worth making here is that the render's
**shape** fidelity is checked while its **losslessness** oracle is ANTS-3810's
and does not exist yet — so a render that fails is caught, a render that
succeeds is not yet proved faithful. Before cutover the
markdown remains authoritative and neither check applies (§ 9).

**A failed publish leaves the store ahead of the file, and that is the
sanctioned direction — but only a *publish* does.** A write runs the render
twice: a **validating** dry render before the store commit, whose failure (a
render error, or a success carrying gate failures) rolls the store write back
and commits nothing; and the **publishing** render after the commit, which is
the one this paragraph is about. So "the store is left ahead" is never the
answer to a validation failure — *What checks this*' `Inv1RenderFailureRollsBack`
row is the rollback leg, and the two rows are not in tension. A write whose
store commit succeeded but whose publishing render did not land refuses
`write_failed` (`mcp-behavioural-notes.md`) with the store committed and the
file stale-behind. The recovery is to re-run the render —
never a hand-edit to close the gap, which § 10 forbids for exactly the reason
the next generation would discard it. `mcp-behavioural-notes.md` owns the
mechanics, being the caller-facing home for a refusal code.

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
installations have one project, not a dozen. Corpus measurements below are evidence
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
is required at this tier. Its parenthesised qualifiers say what kind of
requirement: `write (open)`, `write (closed)` and `write (status shipped)` are
**conditional** — the field is required only for items in that state; `write
(store-populated)` is **unconditional but not author-supplied** — the field must
be present on every item and the store is what puts it there, so a write path
that rejects a call for omitting one has misread the row. One field carries a
slash form, `write (open) / publish (open, public)` — `layman` is owed at both
tiers, for open items at write and for **public** open items at publish, since
§ 7.5 keeps `internal` items out of the render the gate protects. **`write (migration-populated)`** is § 4.1.1's project-level
analogue of `store-populated`: migration is what supplies it, no author ever
does, and a project that has never been migrated — created in the store after
cutover — carries `''`, which § 4.1.1 defines as "not recorded" rather than as a
breach. The list is
not repeated here: one field→tier mapping in two encodings is what
`documentation.md` § 1.5 forbids, and a field added to one and not the other is
the drift it predicts. What the table cannot express follows.

`sort_order` is **not** in this tier — § 5 derives it and § 4.1 marks it so. A
curating write still places the item; it does so by position in the section's
element list, which is where the order lives.

`shipped` is required only for `shipped`, not for every closed item — a
`dropped` item has no ship date, and demanding one would be a nonsense state.

**On a migrated item, this tier requires only the fields the item already
carries.** Requiring the full set on legacy data would reject the commonest
operations there — a status flip, a headline correction, a body edit — for
fields the item was never obliged to have. A status flip is the clearest case
and is **not** a curating write at all; the rule above covers the rest, so an
editor need not back-fill `priority` **on the item being edited** in order to
fix a typo. (`layman` was named here too until 2026-08-24; the paragraph below
now overrides it for a public, open item on a store-migrated project, and
leaving it listed here meant a reader who stopped at this line built a write
path that accepts the fix and is then surprised by `render_gate_unmet`.) An item *created* after cutover is at the full tier from
its first write.

**This is a rule about the item, and § 3.2's publish gate is a rule about the
write — the second still applies.** On a store-migrated project every write
validates by rendering, so a write is refused `render_gate_unmet` when a public
open item **it touches** lacks a `layman`. Nothing above exempts a write from
that gate; § 3.2 owns it and names the remedy.

**So the tier exemption above does NOT extend to `layman` on a store-migrated
project — for a PUBLIC, OPEN item.** An editor fixing a typo on such an item
need not back-fill `priority`, but does have to give it a one-sentence
`layman`, because the write puts it in the gate's scope. A closed item or an
`internal` one is never gated (§ 3.2), so the exemption holds there unchanged;
demanding `layman` on a closed item is § 10's anti-pattern and this paragraph
does not create an exception to it. The line can ride along in the same call
— a `roadmap_log` note whose first line declares the `Layman:` trailer sets the
column — so the cost is one sentence on an item already being edited, and
legacy debt is repaired as the corpus is touched rather than in one sweep.
Amended 2026-08-24 with § 3.2's scope decision; before it, that write was
refused anyway, by every *other* item's missing line as well as its own.

### 3.2 Required before publish

`layman`, **on open items only**. A public open item lacking one **fails the
publish gate**, and the gate is the whole mechanism: the render is not
generated at all, rather than generated with that item omitted. It does not
fail migration.

**The gate judges a scope of items, and the scope is the caller's to state.**
Asked with no scope it judges every item — the whole-project rule as originally
written, and what a direct `RoadmapRender::render()` still gets. Every write
states a scope, and it is the items that write touched. Decided by the user
2026-08-24, superseding the 2026-08-03 whole-project decision and closing the
exemption question this section had been carrying as unassigned.

**`op:render` is a write for this purpose, not an unscoped publish.** It goes
through the same `commitAndRender` sequence with a mutation that writes no
item, so it states an EMPTY scope, the gate judges nothing, and it publishes.
Reading it as "a bare publish, therefore unscoped, therefore whole-project"
inverts the amendment on the one operation it was made for — Music_Production's
publish was the case that could not be unblocked any other way. No caller on
the verb surface passes an unscoped render.

**On a store-migrated project (INV-3's sense — cut over, and a roadmap
`roadmap-format.md` § 3.1's detector classifies `ants-v1`, from the marker
where present and a best-effort parse otherwise), the gate blocks every store
write, not only publication.** Each write op on such a project validates
by rendering (ANTS-3809 INV-1), so an unmet gate refuses it
`render_gate_unmet` — including a `dry_run` preview, which the same render
produces. Because the write's scope is the items it touched, a status flip on
a blameless item is **not** refused by other items' missing `layman` values;
only its own would refuse it, and the refusal's `gate_failures[]` names them.
That is a deliberate consequence of making the render the only writer **of the
markdown files**, not a separate rule.

**Whole-project scoping was tried first and was withdrawn because it is
self-blocking.** A gate evaluated after the mutation refuses every write on a
project carrying legacy debt, the repairs included, so repairing one item at a
time is refused by the offenders that remain and rolled back (ANTS-4434). Taken
to its end that is a project no call can edit at all: Music_Production reached
it, where the one op that would publish the ids a repair must name was itself
gate-refused, with no route out through the verb surface (ANTS-4628). A rule
whose enforcement removes the ability to comply is not enforcing anything. A cut-over project whose roadmap is a
GFM or pass-headings dialect still splices markdown, renders nothing, and so
meets no gate on write. A closed item publishes without
`layman`: § 3.3 leaves the field empty on migrated items, so gating closed
items too would make the gate unsatisfiable for every project with published
history. This is the **only** publish-gating field; `priority` and `resolution`
gate neither publishing nor migration.

**Settled 2026-08-24, by scope rather than by exemption.** The question this
paragraph carried was whether `layman` should get the "written after cutover"
exemption § 7.5 gives `priority` — because § 3.3 leaves the field empty on
every migrated item that did not declare one (a declared `Layman:` line is
preserved, not dropped; § 3.3 measures the share), and read literally no
project could publish until all of those were hand-curated. Neither answer was
taken. `layman` gets **no exemption**: an item that is public, open and in a
write's scope owes its sentence, migrated or not. What changed is the scope, so
the corpus is no longer asked all at once. Every write — `op:render` included —
asks only what it touched. Only an unscoped `render()` asks every item, and
nothing on the verb surface issues one.

The practical effect is that the backlog is repaid as the corpus is edited,
one sentence at a time, by whoever is already in that item — and never demanded
as a precondition for unrelated work. The whole-project FIGURE is no longer
reported by anything on the verb surface, since every dry run reachable there is
now scoped; measuring it needs an unscoped `render()`, which only a direct
library caller can make. That is a gap, recorded as one. ANTS-3821 still tracks filling
this project's own missing lines in; it is now a tidy-up rather than the thing
standing between the project and its first render.

### 3.3 Accepted at migration — historical items

Migration MUST NOT reject an item for a field the source format never required.
Where `roadmap-format.md` § 3.5.3 defines a default, migration applies it
(`kind` → `implement`, `source` → `planned` — that is § 3.5.3's default *source*,
unrelated to the `planned` **status** of § 7.3) and records that the value was
defaulted. Where no default exists — `layman`, `priority`, `resolution`, and any
date not derivable from history — the field is left empty.

**"No default" is not "never harvested", and `priority` is where the two get
confused.** Leaving a field empty is what migration does when the *source* holds
nothing for it; it is not licence to discard a value the source declares. Where
an item carries a `Priority:` line, migration harvests it by § 7.5's rule — which
is total, and states what an unreadable value does — and only an item carrying
none is left empty. The same applies
to any field this tier says has no default: no default means nothing is
*invented*, never that a declared value is dropped.

This is the only arrangement the corpus permits. Across the surveyed corpus,
**two in five** items carry no `Kind:` (41%), and close to half carry no
`Source:` (46%) or `Layman:` (49%). A write-time-only reading would refuse every
project.

Figures here and below come from `tools/roadmap-corpus-survey.py`, which finds
every project under the shared root; it reported 13 of them on 2026-08-03, up
from 10 four days earlier — which is the drift this paragraph is about.
Proportions and approximate counts are used on purpose: the corpus grows every
time anyone files an item, so an exact count written into a standard is wrong
within the day. Exact figures appear only where an argument turns on the
exact value — § 7.1's three off-grammar IDs, § 7.4's `Kind:` tally, § 8's
pass-headings counts among them — and the script prints most of them. **Two are
derived rather than printed** and a re-runner must not expect to find them in
its output: § 7.5's split of the 88 `Priority:` lines into 86 integers and two
words, and § 7.3.1's eight-token tally of one project's status values. The
script prints the totals those refine. Re-run it
rather than trusting a number in this file: per § 2 these measurements only evidence that a requirement is
*satisfiable*.

**The survey finds roadmaps case-insensitively, and that is load-bearing.** One
project names its file `roadmap.md`; an uppercase-only glob excluded it, and
this document consequently asserted that no project used the § 8 pass-headings
format when one tracks 154 items in it. A measurement that cannot see a
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
| `id` | write (store-populated) | § 7.1 — **the store owns allocation**, so a post-cutover author never supplies one; migration is the only path that carries an id in from outside, **except** `id_strategy: "stable_prefix"`, under which a caller supplies a stable string id (`Ts20-SP6`) that the store records rather than allocates — § 7.1 states what that is worth. **Unique within its project**, not within the store — the same ID may legitimately exist in two projects. |
| `id_origin` | write (store-populated) | § 7.1. How the ID came to exist: `parsed` (it matched `roadmap-format.md` § 3.5.1's grammar in source), `synthesised` (the model made it — § 7.1's `PASS-N-M` ids, § 7.2's migration allocations, **every id the store allocates after cutover, and a caller's `stable_prefix` id**, which § 7.1 explains), `quarantined` (off-grammar, recorded verbatim). Distinct from `provenance.id` (§ 7.7), which records *who supplied* a value; an item can be `synthesised` and `migrated` at once, and § 7.1 needs a place to record that an ID is unparseable without rewriting it. |
| `status` | write | § 7.3. |
| `headline` | write | One line, technical. |
| `layman` | write (open) / publish (open, public) | One sentence, non-technical, for a non-programmer reader. **Not** the only published text — INV-2 publishes the full bullet; this is what a reader is shown first (`roadmap-format.md` § 3.5 puts it on the card face, the headline behind it). § 3.2 gates publish on it. |
| `kind` | write | § 7.4. |
| `source` | write | Where the item came from (`roadmap-format.md` § 3.5.3's `Source:`). "What did project X ask for" is a query on this field. Not to be confused with `provenance` (§ 7.7), which records how each field's *value* was obtained. |
| `priority` | write (open) | § 7.5. Meaningless once closed. |
| `created`, `last_modified` | write (store-populated) | § 7.6. The store stamps them; migration fills them per § 7.7 (`git-derived`, or `asserted` from a dated `Source:`). |
| `shipped` | write (status `shipped`) | § 7.6. |
| `resolution` | write (closed) | What was done and why, or why it was not. |
| `section` | write | § 5. Where the item is filed — realised by the item's row in that section's element list, not by a second column on the item, for the same reason `sort_order` is not stored. |
| `body` | optional | Free-form technical detail. Optional because many items are complete in one line, and a mandatory body produces filler that reads like content. |
| `lanes`, `evidence` | optional | Subsystems touched; paths to screenshots, logs, repros. Both are already first-class in `roadmap-format.md` § 3.5, so neither is part of § 4.3's invented tail. |
| `visibility` | optional | § 7.5. Defaults to `public`. |
| `milestone` | optional | Target release, as the version string the project releases under (`0.7.55`, not a section heading). Distinct from `section`, which is where the item is *filed*. |
| `sort_order` | derived | § 5. The item's position within its section, computed from that section's ordered element list. **An author never writes it**, and it is not stored: § 5 makes the element list the only place that order lives, so a stored copy would be a second encoding with nothing to reconcile it against. Readers that want an integer rank get one computed at read. |
| `blocked` | derived | True iff a `blocked-by` relationship targets a **resolvable, same-project** item that is not closed. *Resolvable* = the target exists in the store being read. Cross-project targets are excluded — a partial rebuild cannot see them, and deriving from what is absent would make the value depend on which projects happen to be present. **So `blocked` under-reports by design** (see INV-4): an item blocked only from another project reads `blocked: false`, and a consumer that needs the true answer queries the `blocked-by` relationships themselves rather than this field. |
| `extras` | optional | § 4.3. |
| `provenance` | derived | § 7.7. Per field, never silently promoted to `asserted`. |

`resolution` is required at close and `body` is not, because the institutional
value sits in closed items: a shipped item with no resolution records that
something happened and nothing about what. Specs do not substitute — this
project holds a couple of hundred spec documents against nearly two thousand
items, so for the large majority the item is the only technical record.

### 4.1.1 Project fields

Almost everything above is per *item*. One field is per **project** and worth
naming here because a reader looking for it would otherwise only find it in the
schema:

| Field | Obligation | Notes |
|---|---|---|
| `source_format` | write (migration-populated) | ANTS-3815. Which roadmap dialect the migration read this project's **live** roadmap in — one of `roadmap-format.md`'s three (`ants-v1`, `github-task-list`, `pass-headings`). Per project and not per source file: an archive is parsed under its own grammar, but this records index 0 only, because the live file is the one a consumer's dispatch asks about. **`''` means "not recorded" and is not a format** — it is what a project carries when no migration ever wrote the column, whether because the project was migrated before the column existed or because it was created in the store after cutover and has no source dialect to record. Neither is an error: such a project is served exactly as it was before. **A project whose live file changes dialect after migration is refused, not silently re-classified** — the stored value is compared against the detector's live answer, and a mismatch refuses rather than falling through to `roadmap-format.md` § 3.5.1's counter row, which would strand the `id_high_water` row and reissue live IDs. Re-running the migration is the route back, and it is what § 3.10.3's sanctioned GFM→`ants-v1` conversion ends with; that conversion is not the case this rule refuses, because it does not run behind the store's back. |

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
instead — `Spec:` and `Dependencies:`; § 6 owns the conversion rule.

---

## 5. Structure

Items live in a section tree, and their order carries meaning: an item's rank
within its section is the current corpus's prioritisation, so that order is
preserved exactly. (Below, `position` is a **section's** field — its place in
the project's document order. The two orderings are different facts.)

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
**position**, a **source**, and an **ordered element list**. The `position` and
the `source` are not bookkeeping:

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
such a block (§ 5.2's table row counts the lines), and they are the document's
*metadata* — not items, and not narration.

They are therefore their own per-project structure: status value → that
project's wording, which is what would let one renderer serve every project.
`RoadmapDialog` does that only for a project it read from the store:
`src/roadmapdialog.cpp` still holds the four status emojis and their labels as
compile-time constants, guarded by a `static_assert` on the count, and those are
the words a project parsed from markdown is rendered in whatever its own legend
says. ANTS-3793 built the other path — `RoadmapDialog::storeLegend()` reads the
stored `legend` and hands it to the renderer, but only when the render came from
the store. So holding the legend as data has already let one renderer speak a
cut-over project's vocabulary; what is left to § 9 is whether the markdown path
should reach the same legend, which the compile-time constants currently
prevent.

### 5.2 Structures the model must survive

Measured across the surveyed corpus, in descending order of how much of the
corpus each accounts for. **These are approximations and they drift** —
`tools/roadmap-corpus-survey.py` prints the live values, and § 3.3 says why a
standing number in this file is the wrong thing to trust:

| Approx. count | Structure | Home |
|---|---|---|
| ~1,800 | sub-bullets | item `body` |
| ~180 | markdown table **data** rows | `table` element |
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
| `relates-to` | Untyped association. | converted from `Dependencies:` (98 occurrences; see the conversion rule below) | — |
| `specified-by` | Target is a spec **document**, addressed by path. | converted from `Spec:` (~20 occurrences) | — |

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

**The stored direction is the lower-sorting endpoint, and writing the reverse
pair is a no-op rather than a second row.** Each endpoint is keyed by
`export_slug || 0x1f || id_fold` — the stable identity, deliberately not
`item_pk`, which moves when the store is rebuilt — and the pair is ordered on
that key, so `src` is fixed by the two items and not by which caller wrote
first. `RoadmapStore::relateItems()` already implements exactly this. **A
cross-project edge is never normalised**: there the local item is always `src`,
because the remote endpoint has no `item_pk` to order against
(`relateCrossProject()`).

Saying this is not optional bookkeeping, because **the schema does not enforce
it**. The `relationship` table declares no `UNIQUE`; the constraint is the
partial index `rel_item_uq` on `(type, src_pk, dst_pk)`, which is
orientation-sensitive — `(A,B)` and `(B,A)` are distinct keys it will happily
hold at once. The normalisation above is the only thing preventing the
duplicate, so **any writer that inserts a relationship row without applying it
breaks INV-1**, and `relateItems()` being the sole writer today is what makes
that safe rather than the database.

`splits-from`, `blocked-by`, `duplicate-of` and `supersedes` are acyclic —
`supersedes` included, because "A replaces an earlier decision B" that also
holds in reverse names no current decision. Acyclicity is checked over the
**full store only**: a partial checkout can break a cycle by not containing
part of it, so checking there would report a pass that the whole store fails.

Three adjacent record types exist for one reason — each turns a question
currently answered *outside* the model, by grepping prose or by reading git log,
into a query: **`feedback_ref`** (which
cross-session feedback file cites which item — making ANTS-3744 a query),
**`citation`** (item or document → file and symbol, making `documentation.md`
§ 1.7 machine-checkable), and **`history`** (one row per field change).

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

Two of `roadmap-format.md` § 3.5.1's rules are not identity rules, and the model
must not read them as such. Its `-\d+` regex is the acceptance test; its
"zero-padded to 4 digits" prose is a **write-side convention**, so `CL-9` is a
well-formed existing ID and not an unparseable one. And that standard's
§ 3.10.4 says id *handling* — parsing,
fetching, allocation — is case-insensitive, which is a statement about the
tooling's grammar, not about when two IDs denote the same item.

**Identity is therefore this document's own decision, stated here: IDs are
compared case-INSENSITIVELY within a project.** `Sh-1` and `SH-1` are one
item, and § 4.1's per-project uniqueness is enforced on the case-folded value.
**The prefix is folded with it, everywhere the prefix is a key.** The
`id_high_water` row keyed per `(project, prefix)` (§ 8) and the committed-corpus
scan that floors it both fold the prefix, or `Sh` and `SH` become two allocators
over one namespace and the second issues `SH-9` while `Sh-9` exists — a
duplicate under the rule this paragraph just stated, and one nobody can see
they have created. `roadmap-format.md` § 3.10.4's case-insensitive `id_prefix`
grammar validates an argument; this is about the lookup.
This follows the tooling rather than fighting it — a store that treated them as
two items would allocate an ID `roadmap_log` then refuses as a duplicate.

**Some live IDs do not match that grammar, and the model must say what happens
to them rather than assume they are absent.** `roadmap-format.md` § 3.5.1
requires a dash between prefix and number; 3D_Engine writes three items as
`[Cl9]`, `[Cl10]` and `[CE18]`, with no dash. A migration built on the grammar
alone drops them silently — and silent loss is what § 3.3 exists to prevent,
which is why it is the tier that governs here. They are **items with an
unparseable ID**: migration MUST NOT invent a dash (rewriting an ID breaks that
standard's append-only rule and every citation of it), and MUST NOT treat them as
ID-less (§ 7.2's bulk allocation would issue a second identity for an item that
already has one).

Instead migration **quarantines** them: the item is imported with its ID
recorded verbatim and `id_origin` set to `quarantined` (§ 4.1), the project's
migration completes
rather than blocking, and the run reports them. Three items in the corpus are
affected, all in one project.

**Quarantine is a hand-off, not a resting state, and the owning project is what
clears it.** Three routes exist and the third is preferred: the project
**renumbers its own items** to conforming IDs and fixes its own citations in the
same change, after which a re-import parses them normally and the quarantine
report empties; or it amends `roadmap-format.md` § 3.5.1 to admit the shape; or
it accepts the ID as opaque and lives with the report. The first is preferred
because only that project knows which of its commits, changelog entries and
sibling items cite the old ID — which is the whole reason **migration** must not
rewrite one. The two are not in tension: the reader stays faithful to the bytes
on disk, and standardising is a deliberate edit made by the project that owns
the references, not a silent rewrite made by a tool that cannot see them.
Nothing in this model performs the renumber, and ANTS-3757 § 2.6 owns when the
quarantine clears.

**A caller-supplied `stable_prefix` id is `synthesised`, not `parsed` and not
`quarantined` (ANTS-3809).** `roadmap_log`'s `id_strategy: "stable_prefix"` is a
live argument under which the caller passes a stable string id — `Ts20-SP6`,
`Sh4` — and the store records it verbatim, allocating nothing and raising no
high-water. It is the one post-cutover write that carries a **complete, unallocated** id in
from outside, so § 4.1's "the store owns allocation" holds for every other
write and names this as its exception. `id_hint` is the near case and not the
same one: it pins the *number* an allocation will use, is refused `id_taken`
when it sits at or below the allocation floor (§ 8 — the `max()` of the store
row and the committed corpus, not either alone), and still runs through the
allocator —
so the id it produces is `synthesised` like any other allocation. Of the three `id_origin` values it is
`synthesised`: `parsed` would claim the id matched the § 3.5.1 grammar *in
source*, which is a statement about a file nobody read, and `quarantined` is
migration's verdict on an id it could not honour — filing a deliberately-chosen
id under it would make a first-class identity indistinguishable from salvaged
junk. Such an id need not match § 3.5.1's `-\d+` shape, and does not become
`quarantined` by failing it; the shape rule governs the ids this model
*allocates*, not the ones a caller pins.

**An ID is recognised only at an item's leading position, never mid-text.** A
bracketed token matching the grammar anywhere else is prose: `DOOM_Ants`
contains `players[idx-1]` and `row[right-1]`, both of which match the grammar,
so a grammar-anywhere rule would refuse that project outright.

"Leading position" resolves per source shape, because the corpus has three and
only one of them has bullets:

| Shape | Where the ID is | Recognition rule |
|---|---|---|
| Emoji bullet (`roadmap-format.md` § 3.5) | `- ✅ [ANTS-1234] **…**` | Immediately after the status emoji. |
| GFM task list (`roadmap-format.md` § 3.10.1) | `- [x] [3D_E-0007] **…**` | Immediately after the checkbox. |
| Pass heading (`roadmap-format.md` § 3.10.5) | nowhere in the text | **Synthesised** from the heading as `PASS-<major>-<minor>[-<sub>]`. |

A synthesised `PASS-N-M` **is** an ID for every purpose in this document: it
satisfies § 3.1's `id` obligation, so those items are not ID-less and § 7.2
must not allocate a second identity for them. It is not, however, an
*allocated* ID — it is derived from the heading, so renumbering a pass changes
it, which is the one place this model's identity is not append-only. That
tension is real and belongs to § 9 along with the rest of migration.

**A sub-designated pass id does not match § 3.5.1's `-\d+` grammar, and is
`synthesised` rather than `quarantined` anyway.** The sub is alphabetic — the
designator `43.5.B` yields `PASS-43-5-B` — so the grammar rule above, which
governs ids this scheme *allocates*, does not reach it. `quarantined` is
reserved for an off-grammar id **found in source text**, where the model must
record something it did not choose and cannot rewrite; a pass id is one the
model built itself to a shape it defines, so `synthesised` is the only honest
value. Deciding this by the grammar alone would file 154 well-formed items as
malformed.

The store owns allocation. Each project currently keeps a gitignored per-machine
counter — **one file, holding one integer, with no per-prefix form and no
per-prefix filename defined anywhere**, so on the multi-prefix projects
`roadmap-format.md` § 3.10.4 permits it carries the first prefix and the rest
fall through to the corpus floor alone. It
is explicitly not the source of truth, with a floor recomputed by scanning the
corpus so a wiped counter cannot reissue a live ID. A shared store moves the
carrier but **not** the floor: its `id_high_water` row is still keyed per
`(project, prefix)` and is still `max()`-ed against the recomputed corpus
high-water, so neither the floor nor the per-prefix bookkeeping goes away
(`roadmap-format.md` § 3.5.1). What the store removes is the *file* — a
gitignored counter that a fresh clone can arrive without.

### 7.2 Allocating IDs to items that have none

Every item needs an ID, and roughly 1,600 in the corpus have none — about 40%
of it. (Pass-heading items are **not** among them: § 7.1 synthesises their ids,
so they arrive already identified.) Allocation splits by whether anyone will
ever need to cite the item:

| Items | Approx. count | Rule |
|---|---|---|
| **Closed** | ~1,020 | Allocated in bulk, in document order. Nobody cites a finished item, so no curation is required and none is invented. |
| **Open** | ~600 | Allocated into the project's normal sequence and treated as a real item — it will be cited, worked on, and referenced in commits. § 3.2's publish gate then applies, so it must be curated before a write that TOUCHES it can land (§ 3.2's scope rule, 2026-08-24) — not before the project publishes, which is no longer gated on it. |

The two rows differ in *obligation*, not in ID shape and not in provenance
(§ 7.7 states where both stand). A separate archival prefix would add a second
prefix per project to reconcile, and an ID's text should never encode metadata
that can change.

**Only bullets that are items get an ID, and itemhood resolves per source
shape** — the same three the recognition table above uses, because what marks a
bullet differs in each and a single rule would be wrong for two of them.

| Source shape | A line is an item when | Headline comes from |
|---|---|---|
| Emoji bullet (`roadmap-format.md` § 3.5) | it carries **both** a status emoji and the bold headline § 3.5 requires | the bold span |
| GFM task list (§ 3.10.1) | it carries a checkbox — **a bold headline is not required** | the bullet's own first line |
| Pass heading (§ 3.10.5) | it is a `#### Pass N.M` heading — **never an ordinary bullet** | the heading |

**On the emoji shape both halves are needed.** § 3.3 allows plain narration
bullets with no status marker, which § 5 models as `narration`, so the bold
headline alone would promote them to items. A bullet carrying a marker but
neither an ID nor a bold headline is not an item, and these are not one thing
either — most are detail lines belonging to a parent item's `body`, the rest are
status-legend lines (§ 5.1). § 5.2's table carries the live proportions. Neither
gets an ID.

**On the GFM shape a bold headline is not required, and requiring one would be
the silent loss § 3.3 forbids.** § 3.10.1's own example is
`- [ ] (WIP) Build login screen`, so a project written entirely in plain
checkboxes would file every top-level bullet as body prose of no parent and
issue no ID for any of them. The checkbox is the marker; the bullet's own text
is the headline, and such a bullet is ID-less rather than ID-bearing, so § 7.2
allocates for it like any other. `RoadmapParse`'s GFM branch already does this.
A checkbox bullet with no text at all remains `narration`, having no headline to
take.

**On the pass-headings shape only the headings are items, and every other line
in a pass block is that pass's body — deliberately, not as a gap.** The one such
project in the corpus is RetroDB (154 passes), and its ordinary bullets are the
per-file breakdown of what a completed pass did (`- **README.md** — 5 fixes: …`),
its own table of contents, and a legend. Promoting them would file a
table-of-contents link as a roadmap item and attach hundreds of identities to
prose that already belongs to a pass; § 3.1's `id` obligation is not breached,
because these are not items. So a pass-headings project's ID-less bullets are
neither allocated for nor an error, and an implementer has nothing to invent
here. `RoadmapParse::parsePassHeadingBullets()` emits records for the headings
alone, and the rest is absorbed as section intro or `narration`.

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

#### 7.3.1 Normalising a pass-headings status

A pass-headings project writes status as a free-text `- **Status**:` line rather
than an emoji, so its values must be mapped onto the five above. **The unit
classified is the leading token, not the whole line** — the same shape § 7.5
harvests `Priority:` with, for the same reason: the value is a word followed by a
qualifying tail, and matching whole strings makes an ordinary vocabulary look
unmappable.

| Source word | Status | Provenance |
|---|---|---|
| `done` · `shipped` · `completed` | `shipped` | `asserted` |
| `in-progress` · `in_progress` · `inprogress` · `doing` · `wip` · `partial` | `in-progress` | `asserted` |
| `deferred` · `considered` · `parked` | `considered` | `asserted` |
| `todo` · `planned` | `planned` | `asserted` |
| anything else | `planned` | `defaulted` |

**The mapping is total, and the fallback is a default rather than a rejection.**
Refusing an unrecognised word would halt a whole project on its author's private
vocabulary, which is the § 10 anti-pattern that rejects a migrated item for a
field its source format never required. An item whose block declared a status
keeps that line's **value** — the text after the colon, qualifier tail included,
but not the `- **Status**:` label itself — in `extras.source_status`, so nothing
is lost and the write-back stays a right inverse. A word outside the table
additionally records `defaulted` provenance and a `status_defaulted` note, which
is what makes the guesses countable rather than invisible. A block carrying **no**
`- **Status**:` line at all is the third case: it is `defaulted` with no
`extras` key, because there was no value to keep.

**`deferred` → `considered` is not an invention.** `considered` is the status for
work that is recorded and not committed to, which is what a deferral is; the
"not now" that distinguishes it from `planned` is a **priority**, and § 7.5 owns
that. Nothing here needs a sixth status, so `roadmap-format.md` § 3.11's
four-emoji anti-pattern stands unamended.

Measured against the surveyed pass-headings project (2026-08-09): its 164 status
values reduce to eight leading tokens with no remainder — `done` 128, `planned`
18, `deferred` 11, `shipped` 2, `partial` 2, `un-gated` 1, `in-progress` 1,
`considered` 1 — so **163 map as `asserted` and one (`un-gated`) takes the
default**, correctly, since that item's blocking gate had been met and the work
had not begun.

### 7.4 Kind

The canonical set is `roadmap-format.md` § 3.5.3's 21-value enum. **Writes
accept canonical values only.**

**The live corpus is already canonical.** `tools/roadmap-corpus-survey.py` over
all 14 projects reports **21 distinct values — 21 canonical, zero others**
(2026-08-08, after ANTS-4065 Phase B2 normalised every off-taxonomy value at
source). This section previously read "32 distinct values — all 21 canonical
ones plus 11 others"; both halves of that figure are now historical.

**The mapping below stays normative regardless, because import does not only
read the live corpus** — an older git revision, a `docs/roadmap/` archive, a
fork, or a project joining later can still carry an off-taxonomy value, and a
mapping is only useful if it is fixed before it is needed. The counts are the
pre-Phase-B2 measurement, retained as the evidence each row was derived from and
**not** as a current figure:

| Corpus value | Canonical | Corpus value | Canonical |
|---|---|---|---|
| `bug` (29) | `fix` | `enhance` (3) | `enhancement` |
| `improve` (7) | `enhancement` | `docs` (2) | `doc` |
| `bugfix` (6) | `fix` | `performance` (2) | `perf` |
| `spike` (5) | `research` | `perf / optimize` (2) | `perf` |
| `testing` (1) | `test` | `feat` (1) | `feature` |
| `perf / fix` (1) | `perf` | `tooling` (1) | `chore` |
| `behaviour-change` (1) | `enhancement` | `process + tooling` (1) | `chore` |
| `audit` (1) | `audit-fix` | | |

The counts have two different scopes, because the rows were added by two
different measurements: the eleven rows this section carried first were counted
over all 14 projects' `ROADMAP.md`, and the four ANTS-4065 § 2.1 added — `bug`,
`performance`, `process + tooling`, `audit` — over this project's `ROADMAP.md`
plus its two archives, that being the only project they occurred in.

Three entries are compound. `perf / optimize` and `perf / fix` map to their
**first** term; a rule that picked the second would silently reclassify
performance work. `process + tooling` is not an exception to that: its first
term is not canonical, and both terms independently mean § 3.5.3's housekeeping,
so the row is settled by the value's meaning rather than by position. **There is
no general compound rule** beyond these three named rows — ANTS-4065 Phase B3
met three further compounds (`feature/fix`, `design + implement`, `design + fix`)
and resolved them by correcting the four bullets that carried them, deliberately
adding no rule.

**`mappedKind()` (`src/roadmapmigrate.cpp`) implements all fifteen rows** — the
four ANTS-4065 § 2.1 added landed with that spec's Phase C on 2026-08-09. A
value in neither the canonical enum nor this table still falls through the
unmapped branch (`implement`, with `extras.source_kind` preserved and a
`kind_unmapped` note emitted, alongside § 2.3's `field_defaulted`), which is
visible rather than silent.

The table is generated, not recalled: `tools/roadmap-corpus-survey.py` prints
every non-canonical value with its count. A project joining later re-runs it
and extends the table by amendment.

### 7.5 Priority and visibility

> **The `Priority:` harvest described below is SPECIFIED, NOT IMPLEMENTED.**
> Nothing reads a `Priority:` line: `Priority:` does not occur anywhere under
> `src/`, and `roadmapmigrateload.cpp`'s reconciled field list deliberately
> omits `priority` (INV-3 there — a re-run must never clear a value a human set
> through `roadmap_log`). So on a migrated item `priority` is left empty
> **whether or not the source carries the line**, and the mapping below states
> what a harvest would mean rather than what one does.
>
> **Do not build the harvest from this section alone.** Implementing it as
> written re-derives `priority` on every re-run and overwrites values set
> through `roadmap_log`, which is exactly what INV-3 exists to prevent — so an
> implementation owes INV-3 a first-write-wins or source-vs-store rule that
> neither document currently states. Tracked as ANTS-4440.
>
> Note also that INV-3's own stated *reason* — "a source file cannot express
> them" — is inaccurate for `priority` specifically: `roadmap-format.md`
> § 3.5.2 defines the carrier. The behaviour it protects is right; the
> rationale needs narrowing to the other four fields.

`priority` is `1` (highest) to `5` (lowest), required on open items **written
after cutover**. On a migrated item § 3.3 leaves it empty **only where the
source declares no `Priority:` line**; where one exists it is harvested by the
rule below, exactly as any field with a source-side counterpart is. Five bands
rather than ten: ten levels are not reliably distinguishable, so they collapse
in practice to three with the rest defaulting to the middle, and the number
stops carrying information. The prose severity vocabulary — CRITICAL, HIGH, MEDIUM, LOW — is
the one `roadmap-format.md` § 3.8 puts in a finding's headline (its
*Severity in the headline* rule), which enumerates all four itself — **that
section owns the vocabulary and this one only maps it onto bands**, so a fifth
severity word is added there and reflected here, never the reverse; and `INFO` has no band — an INFO finding is an
observation rather than work, so it becomes an item only once someone gives it a
priority;
its § 3.5.2 is the carrier that puts one in a `Priority:` body line. The
vocabulary maps CRITICAL → 1, HIGH → 2, MEDIUM → 3, LOW → 4. Band 5 is reserved
for someday-maybe work that no severity word expresses.

**The `Priority:` line is the only carrier migration would read — see the
not-implemented note at the top of this section. A severity word in the
headline does not set `priority`.** § 3.8 requires that word on every
fold-in finding, so harvesting it would assign a priority to a large class of
items from a value *inherited* from whichever review raised them — where the
`Priority:` line is one an author set deliberately. It would also be an
inference from prose, which INV-5 refuses for relationships and this model
refuses here for the same reason: the mapping above is what the four words
*mean* when an author writes one into the field, not a second place to look for
them. § 3.3 is therefore exact — an item with no `Priority:` line is left empty,
however its headline reads.

**The harvest rule is total, because most of the corpus does not write a
severity word at all.** Of the 88 `Priority:` lines in the corpus, **86 already
hold an integer**; the two that do not are `medium` and `LOW`, and `CRITICAL`
and `HIGH` never appear in the field. So a rule matching only the four bare
uppercase words would leave almost every declared priority empty. Reading a
`Priority:` value:

1. An integer 1–5 is taken as itself.
2. Otherwise the **leading token** is matched **case-insensitively** against the
   four severity words — `roadmap-format.md` § 3.5.2's own shape is
   `Priority: CRITICAL — security blocker`, so whatever follows the word is a
   comment and not part of the value.
3. Anything else — an integer outside 1–5, an unrecognised word — leaves
   `priority` empty and the raw string in `extras`. Nothing is guessed.

`visibility` is `public` or `internal`. The published render includes only
`public` items, and excludes `dropped` items regardless. Today everything is
published because the file *is* the record — including security findings that
are still open.

**Those two exclusions are the whole of the render's membership rule: a closed
item is published.** Size is managed by rotating closed minors into
`docs/roadmap/*.md` (§ 8), never by hiding them, so "open work only" is not a
curation this model offers. Omitting them would break two things at once —
`roadmap-format.md` §§ 3.6.2–3.6.3 match CHANGELOG entries and commit subjects
against bullet **headlines**, and a released item whose bullet left the corpus
can no longer be matched; and § 8's committed-corpus ID floor would narrow while
it is still the floor under the store's `id_high_water` row.

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
| `defaulted` | **No usable source value at migration** — either absent, or present and outside the field's recognised set — so the default this model or `roadmap-format.md` § 3.5.3 defines was applied. **The discriminator is per field, and the two fields do not use the same one.** For `kind`, `extras.source_kind` is written *only* on the unrecognised branch, so its presence separates the two cases and a conformer counting "items that declared no kind" must exclude the rows carrying it. For `status`, `extras.source_status` is written whenever the block declared one — recognised or not (§ 7.3.1) — so it does **not** separate them; there, `defaulted` provenance is itself the discriminator, and the absent case is the one carrying no key at all. |
| `git-derived` | Recovered from commit history, subject to § 4.2's limitation. |
| `migrated` | Generated by migration itself rather than chosen by an author — every ID migration **allocates** (§ 7.2, **both** rows) *or* **synthesises**. The second case is a pass-headings item, whose id is derived from the author's own heading (§ 7.1): it has a source-side counterpart, but no author wrote an id, so `asserted` would be a false claim and `migrated` is correct. |
| `store-generated` | Stamped by the store on a post-cutover write — the `write (store-populated)` fields of § 4.1. Distinct from `asserted`, which is what the *author's* fields on that same write carry. |

A value is **never silently promoted to `asserted`.** Editing a field through
the store sets `asserted` for that field only; the item's other fields keep
what they had.

Because it is per field, `provenance` is not an item-level label — an item
routinely carries an `asserted` headline beside a `defaulted` kind and a
`migrated` id, and a post-cutover item carries `asserted` beside
`store-generated`.

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
  closed by INV-2; the conformance check is § 9's.

  **A quarantined ID is the one stated exception, and it is not a defect in the
  render.** § 7.1 requires an off-grammar ID to be imported verbatim and forbids
  inventing a dash, so a render of a project holding one emits `[Cl9]` where
  § 3.5 asks for `[PROJ-NNNN]`. Rendering it any other way would be the silent
  rewrite § 7.1 forbids, and refusing to render would punish the project for a
  defect in its own source. So the render carries the ID as stored, the file is
  conforming **except** at the quarantined items, and the quarantine report — not
  the render — is what says so. The exception is self-clearing: it exists only
  while an item is quarantined, and § 7.1's preferred route has the owning
  project renumber, after which the render conforms with nothing amended here.
  A conformance check (§ 9's) must therefore read the quarantine set before
  reporting, or it will fail a render that is behaving exactly as specified.

- **Optional fields.** Its § 3.5 files `Layman:` and `Source:` as optional and
  § 3.5.3 gives defaults for absent `Kind:` / `Source:`. This document does not
  change that for markdown; § 3.3 *adopts* those defaults, and § 3.1's stricter
  obligations apply only to writes through the store.
- **ID allocation.** Its § 3.5.1 keeps a `.roadmap-counter` under a flock, but
  is explicit that the counter is a derived per-machine cache and **not**
  source: the true high-water mark is the highest ID across the committed
  corpus, which it defines as `ROADMAP.md` + `CHANGELOG.md` +
  `docs/roadmap/*.md`. Once a project is **store-migrated** (INV-3's sense —
  cut over *and* `ants-v1`) the store owns allocation and the counter is
  retired for it; a cut-over project whose roadmap is a GFM dialect keeps its
  counter, and a pass-headings one derives its ids from the headings and uses
  neither. § 3.5.1's carrier table is the authority on all three.
  The render keeps carrying IDs
  (INV-2), so `ROADMAP.md` does not stop being a floor input merely by becoming
  generated. **For published items that input does not narrow**: § 7.5 keeps
  closed items on the render, and the bullet below has the archives re-rendered
  from the store rather than frozen, so both sets of IDs stay in the committed
  corpus. **For `internal` and `dropped` items it narrows to nothing**, and that
  is the reason the corpus floor cannot stand alone after cutover: § 7.5 excludes
  them from the render by policy, so their IDs appear in no committed file, and a
  fresh clone — where the store is machine-local and may not exist — would
  reissue a live ID from a corpus scan that cannot see them. The store's
  `id_high_water` row covers them today (ANTS-3809), which is why § 3.5.1 keeps
  the corpus as a floor *under* that row rather than as a substitute for it. The
  export supersedes both as the authoritative floor, being subject to neither
  exclusion — which is what
  § 3.5.1's definition needs amending to say for cut-over projects. **The
  interim half of that amendment has landed** (ANTS-3809): § 3.5.1 now names the
  store's `id_high_water` row as a cut-over project's carrier and keeps the
  committed-corpus floor under it. What is still owed is the end state above —
  the export as the floor — which waits on the publish cadence (ANTS-3794).
- **Status vocabulary.** Its § 3.11 makes a fifth status emoji an anti-pattern,
  so `dropped` has no markdown form and is excluded from the render. Adding one
  is that standard's decision, not this one's.
- **Pass headings.** Its § 3.10.5 documents `#### Pass N.M` as a supported read
  *and* write format, and one surveyed project tracks **154 items** in it —
  counted as `#### Pass N.M` headings; its `- **Status**:` lines are counted
  separately and are more numerous, so the two figures below are not two
  measurements of one population. It
  is therefore squarely **in** migration scope. Two consequences the model must
  carry: its items are identified by synthesised `PASS-N-M` ids (§ 7.1), and
  its status is a free-text `- **Status**:` line rather than an emoji —
  **142 of that project's 164 status values fall outside § 7.3's five-value
  enum** as whole strings (`deferred`, `partial`, `un-gated`, `shipped in
  v3.6.15`, and a long tail carrying prose). That figure measures whole lines,
  which is why it reads as a large gap; classified on the **leading token** —
  the unit § 7.3.1 actually maps — the same 164 values reduce to eight words
  with no remainder. § 7.3.1 owns the mapping and this is no longer an open
  question.

- **Archive rotation and the release flow.** Its § 3.9 rotates closed bullets
  byte-identically into `docs/roadmap/*.md`, and its § 4.3 moves released work
  into the CHANGELOG. **Neither the archives nor the CHANGELOG is lost at
  cutover, and only one narrow thing is.**

  The archives are **in the store**: migration reads every
  `docs/roadmap/<M>.<N>.md` alongside the live file, and each section records
  the file it came from, so the render buckets sections by that path and
  re-emits each archive to its own file. The store's own schema comment gives
  the reason the column exists — without it the render re-emits a rotated
  archive back into `ROADMAP.md`. So archives are generated outputs after
  cutover, not frozen ones, and hand-editing one is lost exactly as § 10 says.

  The CHANGELOG is unaffected: § 4.3's first two steps touch `CHANGELOG.md`,
  which no render generates, and its status-flip steps are store writes on a
  cut-over project (ANTS-3809).

  **What is genuinely lost is the rotation *trigger*.** The renderer never moves
  an item from the live file to an archive; § 3.9 puts that step in `/bump` as a
  snip-and-create on the markdown, and that hand edit is what the next render
  discards. **Rotation therefore becomes a store operation** — reassign the
  closed minor's sections to the archive path and re-render, which lands both
  files with content preserved and nothing snipped. That is a change to
  `roadmap-format.md` § 3.9, and it is built: `roadmap_log
  op:"rotate_minor"` (ANTS-4070). Not an open question in this model.

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
  is already fixed by §§ 3.3, 7.1, 7.2, 7.3.1 and 7.4; what is open is how to
  execute it.
- The **remaining** checks — their inputs, pass conditions, scheduling, and
  behaviour on a machine where the store does not yet exist. INV-1 already fixes
  the export round-trip check; § 7.7 already fixes what `provenance` must record.
- Concurrency across projects sharing one store, and the auto-publish cadence to
  the backup repo — including that a push conflict means two stores diverged and
  must surface rather than auto-merge, and that a silent backup failure is worse
  than no backup because it stops anyone checking.
- How the render **demonstrates** § 8's conformance rather than asserting it. The
  filename and the two-standard amendment question are closed by INV-2; what is
  open is the check that catches a render silently missing a required piece, and
  whether a cut-over project re-reads its own render at all.
- The rotation **operation** § 8 settles the shape of: which sections move to
  which archive path, when it runs, and what it does with a minor that was never
  rotated before cutover. The policy is fixed there (rotation is a store write,
  archives stay generated); this is its execution, and it is filed as ANTS-4070.
- The fate of `roadmap_query`'s and `RoadmapDialog`'s remaining **markdown write**
  paths. ANTS-3793 cut over their reading; a full-fidelity render keeps them able
  to *parse* the file, so what INV-3 ends is the writing alone — which is why
  they still need a cutover rather than none. This includes whether
  `RoadmapDialog`'s markdown **fallback** path should reach each project's
  legend; its store path already does (§ 5.1, ANTS-3793).
  **`roadmap_log` is no longer among them**: ANTS-3809 landed its write half, so
  on a cut-over project in the emoji-bullet shape its eight ops mutate the store
  and re-render (*What checks this*). Its splice paths remain for every other
  project, and retiring those is not yet filed.

ANTS-3754 carries the verified finding list that is those specs' input.

---

## 10. Anti-patterns

The prohibitions above, collected — a conformer arrives holding a defect rather
than a question.

- ❌ **Rejecting a migrated item for a field its source format never required**
  (§ 3.3). Two in five corpus items have no `Kind:`; a write-time reading refuses every
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
- ❌ **Hand-editing the render or the export on a store-migrated project**
  (INV-3) — the export always, the render wherever it owns the file (§ 3.2; on
  a cut-over GFM or pass-headings project the roadmap is spliced, not
  generated, so this bullet does not reach it). Both are
  generated; the edit is lost silently.

---

## What checks this

| Rule | What catches a breach |
|---|---|
| **Every** corpus figure in this document | `tools/roadmap-corpus-survey.py` — re-run it rather than trusting a standing number (§ 3.3) |
| INV-1 leg (a), round-trip byte-identity | `tests/features/roadmap_export_roundtrip` — `Inv1RoundTripIsByteIdentical`, `Inv2EveryRowAndColumnSurvives`. Those `InvN` names are ANTS-3761's spec invariants, not this document's. |
| INV-1 leg (b), committed export == live store | **nothing yet** — it needs the publish cadence, so it is ANTS-3794's |
| INV-2 render fidelity | `tests/features/roadmap_render`, since ANTS-3758 shipped. `tests/features/roadmap_read_seam`'s `Inv2BackendsAgree` goes further on the shape half: it renders a migrated store back to markdown, parses that file, and compares record-for-record against the store read. **It is not the losslessness oracle § 1 says is still owed** — its equality is over the 20 fields of the *bullet* record, so a store column that record does not carry (`extras` and `provenance` among them) is never compared. ANTS-3810's oracle is the whole-row one. |
| Read budgets — a whole-project store read stays under its item ceiling and its p95 | `tests/features/roadmap_read_seam` — `Inv3Ceiling` (default suite) and `Inv3Latency` (`perf` label). ANTS-3793's INV-3, not this document's numbering. A budget nothing measures is a comment: the p95 case is what forced the batched `RoadmapStore::readItems()`, the N+1 having been 83 of 101 ms. |
| INV-3, the store-is-the-only-writer leg, over `roadmap_log`'s eight ops (`mcp-behavioural-notes.md` lists them) on a store-migrated project | `tests/features/roadmap_write_half` — ANTS-3809's `Inv1RenderFailureRollsBack` (a validating render that does not succeed, including one succeeding *with* gate failures, rolls the store write back) and `Inv2RenderIsTheOnlyWriter` (every op writes markdown only through `RoadmapRender::render()`). `InvN` here is ANTS-3809's numbering, not this document's. |
| § 7.1's `stable_prefix` allocates nothing, and § 8's interim carrier rule | `tests/features/roadmap_write_half` — `Inv3Allocation`, which pins both the store-row floor and the committed-corpus floor under it |
| INV-3 hand-edit detection | **nothing yet** — § 9's |
| INV-4 cross-project relationships | the `relationship` table carries them; that they are *used* is not checkable |
| INV-5 no relationship inferred from prose | **nothing** — a prohibition on authors and on migration, enforced by § 6 giving migration only two structured fields to read |
| § 7.7 provenance never silently promoted | **nothing yet.** ANTS-3809 landed the write path that stamps it, but no test asserts a value is never promoted; the check is still § 9's |
| §§ 3.1–3.2 obligations, §§ 7.3–7.5 enums | **partly.** § 3.2's gate is enforced on every store write; `Inv1RenderFailureRollsBack` checks that a refused write rolls back, and since 2026-08-24 it does so against the write's OWN offender rather than an unrelated one. The SCOPE rule is checked by `Ants4628UntouchedDebtDoesNotBlockAWrite`, `Ants4628WriteIsStillRefusedByItsOwnOffender` and `Ants4628RenderPublishesPastLegacyDebt` (same bundle), and at the render library by `Inv5GateScopeLimitsOffenders`. § 3.1's write-time obligations and the §§ 7.3–7.5 enums are validated nowhere, and remain § 9's |
| § 7.1 identity grammar | `roadmap-format.md` § 3.5.1's regex, already in `RoadmapIndex::isCanonicalId` |
| § 8 reconciliation | **nothing** — prose agreement between two standards; § 8's ID-allocation bullet records how far the amendment it owes has got. |

---

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-30 | 3 (model coherence, corpus drift, failure modes) | 6 / 12 / 14 / 18 / 1 | Structural rewrite: obligations split into tiers, export scope defined, INV-1 given its missing leg, identity grammar corrected after the survey regex was found wrong about two projects, migration source shapes corrected. |
| 2 | 2026-07-30 | 3 (same partition, cold) | 13 / 19 / 17 / — / — | **Stopped and split.** ~8 of the 13 CRITICALs were collateral from loop 1's own fixes; the findings were overwhelmingly schema-level, i.e. this document was a standard carrying an implementation spec. Split per ANTS-3754: the model stays here, the schema goes to a spec. Backup relocated to the private config repo, closing a leak the draft shipped. ID allocation for the corpus's ID-less items decided (user, 2026-07-30). |
| 15 | 2026-08-09 | 3, cold — identical packet rebuilt from disk; dispatched only after 4b was re-run over loop 14's rows, since half that loop was collateral | **Q1 1 · Q2 6 · Q3 1** — verified 8, dismissed 1, **6 fixed and 2 filed** | **Exited at the 3-loop cap.** Two of the eight were loop 14's own collateral and both are the same shape — a fix that was right about its own sentence and wrong about the sentence it implied. § 4.1.1 gained "re-running the migration … is what § 3.10.3's sanctioned conversion ends with", but § 3.10.3 is five passes ending at `Layman:` summaries and never re-runs anything: a conformer following it to the letter finishes with `source_format` recording `github-task-list` against a file that classifies `ants-v1`, i.e. permanently refused by the very rule that named it sanctioned. `roadmap-format.md` now has a step 5. And loop 14's `internal`/`dropped` caveat landed in § 8 only, leaving § 3.5.1 still promising a corpus floor that "can never reissue a live or migrated id" — the caveat is now on both sides. **Four were pre-existing.** § 7.5 claimed "enumerating the four values is this document's doing, not that one's" while `roadmap-format.md` § 3.8 enumerates all four verbatim — two homes for one vocabulary, with no rule saying which governs; § 3.8 now owns it and this document only maps it onto bands. § 7.1 said an item ID matches § 3.5.1's grammar "not a narrower one", but a sub-designated pass id is `PASS-43-5-B` — the sub is alphabetic (`^\d+\.\d+(?:\.[A-Za-z][A-Za-z0-9]*)?$`) — so it fails the `-\d+` shape and a builder had to choose between `synthesised` and `quarantined` for 154 items; `quarantined` is now stated as reserved for off-grammar ids *found in source*, which a self-built id never is. § 3.10.5 glossed the emoji format as `- **headline** [ID]`, putting the ID after the headline where § 3.5 and § 7.1 both pin it immediately after the emoji — a parser author reading it would scan mid-text, which § 10 names an anti-pattern. And § 3's preamble, rewritten last loop, said closed bullets carry "their IDs, which is what § 3.6.2's and § 3.6.3's matching resolve against" — both sections **fuzzy-match headlines**, not ids. **Dismissed on evidence:** a lane read `roadmapmigrate.cpp:755`'s comment ("the name regex is deliberately tighter") as making § 8's "reads every `docs/roadmap/<M>.<N>.md`" false — but loop 14 corrected § 3.9's published regex to the code's own `(0\|[1-9][0-9]*)` form, so the two now agree and "every" is true; what is stale is the code comment, surfaced as a code-side note. **Filed, not fixed — two, both decisions:** ANTS-4072 (a bullet-form item inside a pass-headings project has no id carrier — 73 such items exist) and ANTS-4073 (a pre-1.0 project told to use `## P01` phase blocks can never satisfy § 3.9's rotation trigger). The cap binding here is evidence about the pairing, not about either document: eight of this loop's findings still crossed the two files, and `roadmap-format.md` entered the run never having been gated at all. |
| 14 | 2026-08-09 | 3, cold — identical packet, rebuilt from disk after loop 13's edits; ANTS-4070's § 4.3 step-4 half added to the already-surfaced list | **Q1 2 · Q2 6 · Q3 2** — verified 10, dismissed 0 | **Five of the ten were loop 13's own collateral, which is the loop working.** The sharpest: loop 13 rewrote § 7.7's `defaulted` row to be per-field and wrote "only the *unrecognised-value* case carries [an `extras` key]" — true of `kind`, false of `status`, because `roadmapmigrate.cpp:287` holds `source_status` "for both word rows, not only the lossy one". A conformer using key-presence to find guessed statuses would have called all 163 recognised RetroDB values guesses; the row now states that `kind` discriminates by key and `status` by provenance. Loop 13's § 3.10.3 ordering rule rested on "the marker is what makes the file classify `ants-v1`" — but § 3.5.1 says the detector falls back to a **best-effort parse**, and step 1 alone (emoji bullets) can satisfy it, so marker-last narrows the window without closing it; the rule now says so and forbids a commit or store write between step 1 and step 0. Its sibling, step 2, still said the counter goes unread on a store-row project, which under marker-last is backwards — the counter *is* the carrier until step 0 lands. § 8's "that input does not narrow either" ignored § 7.5's own `internal`/`dropped` exclusions: those IDs reach no committed file at all, so the corpus floor cannot stand alone and it is the store row that covers them. And § 3.3's "the script prints every one" was false of the two figures loop 13 added, neither of which the survey emits. **Three were pre-existing.** § 3.9 published the archive regex as `^[0-9]+\.[0-9]+\.md$`, which **accepts `00.07.md`** — the padding the sentence beside it forbids; the shipped `archiveNameRx()` is `\A(0\|[1-9][0-9]*)\.(0\|[1-9][0-9]*)\.md\z`, so the code was right and the standard's own regex was the defect, now corrected and executed against twelve names before landing. § 4.1.1's "refused, not silently re-classified" had no counterpart in § 3.5.1's carrier table, which simply falls a dialect-changed project through to the counter row — stranding `id_high_water`. And IDs compare case-insensitively while nothing said the **prefix** half of the `(project, prefix)` key folds too, so `Sh` and `SH` would allocate over one namespace as two. **Also fixed:** § 5.2's `~170` table data rows against a measured 184 (siblings round correctly, so this was drift, not the stated convention), and § 7.7's `migrated` row, which described allocation only although migration also *synthesises* pass ids — `roadmapmigrate.cpp:302` stamps `migrated` for them, so the value was right and the gloss incomplete. **Collateral caught by 4c, not by a lane:** the new loop-log section was missing from this file's sibling's Contents list. |
| 13 | 2026-08-09 | 3, cold — first run gating this document **paired with `roadmap-format.md`**, one shared byte-stable packet (~10.6k tok) carrying the live survey, the RetroDB status tally, the pass-headings status reader, archive discovery, render routing and the `section`/`project` DDL; ANTS-3838, ANTS-4068, ANTS-4070 and ANTS-4071 listed as already-surfaced | **Q1 1 · Q2 7 · Q3 3** — verified 11, dismissed 1 | **The pairing is what earned the loop: 8 of the 11 were cross-document, and none was reachable from either file alone.** The § 9 decision pass that preceded this run closed four open items, and the cold read found that two of its answers contradicted the neighbouring standard. `roadmap-format.md` § 3's preamble said released work "moves out of the roadmap into the CHANGELOG" while the new § 7.5 rule publishes closed items — a renderer author reading the format standard would have omitted every ✅ item and broken §§ 3.6.2–3.6.3 headline matching; the preamble now scopes itself to CHANGELOG authoring and points at § 3.9 rotation as the size mechanism. And § 4.3's new post-cutover paragraph claimed "steps 3–4 edit roadmap bullets", but **step 4 rewrites a release-block `##` heading** — a section title, for which no store op exists — so a cut-over project cannot perform it at all; filed onto ANTS-4070, which moves sections for the same reason. **Three more were this run's own collateral**, all from § 7.3.1: "keeps its **raw line** in `extras.source_status`" is wrong twice over (`roadmapparse.cpp` stores `peek.mid(valueStart)`, the value *after* the colon, and only where a Status line existed at all); and § 7.7's `defaulted` row defined itself purely in `kind` terms while the new table assigns `defaulted` to `status`, so a conformer counting defaulted rows by `extras.source_kind` mis-counts every status row — the row is now per-field. **Five were pre-existing and older than the migration.** `Kind:` is "**Required as of v1.1**" in § 3.5 and "two optional metadata fields … the format stays terse for it" in § 3.5.3 — found independently by two lanes, and it decides whether a conformance checker rejects a `Kind:`-less bullet; the default is now stated as a reader-side legacy fallback. § 3.10.3's conversion recipe adds the format marker at **step 0**, which on a project that already has a store row makes it store-migrated before steps 1–4 run, so those four steps become hand edits to a generated file that the next render discards — while the store path refuses them too, its publish gate being unmet until step 4 fills the `Layman:` lines; the marker now goes last. § 3.9's ~150 KiB threshold and its minor-bump-only rotation event cannot both be breached-or-not at 0.7.104 with a 3.2 MB file and no closed minor — the size is now stated as a review trigger. § 3.10.4 told multi-prefix repos to keep "one counter per prefix — one file each" while **no per-prefix filename exists in this standard or in any source file**; rather than invent one (two tools inventing different names each read zero for the other's prefix, reissuing live IDs) both documents now say such a project has no counter carrier and allocates from the corpus floor. And § 3.3 vs § 7.5 disagreed on whether a headline severity word sets `priority`: § 7.5's tie-break ("where an item carries both, the `Priority:` line wins") only means anything if it does, while § 3.3 says only a `Priority:` line is harvested — since § 3.8 mandates that word on every fold-in finding, the difference decides `priority` for a large class of items. Resolved toward § 3.3: the headline word is inherited from whichever review raised the finding, so reading it would be an inference from prose. **Dismissed:** § 3.5.1's "a roadmap **SHOULD** carry that marker" against § 8's "its § 3.1 requires the format marker" — § 3.1 governs *conforming* files and § 3.5.1 addresses arbitrary files the detector meets, and INV-2 already entails the render emits one. |
| 12 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loops 10–11; ANTS-3838 listed as already-surfaced so it would not be re-raised | **Q1 1 · Q2 4 · Q3 2** — verified 7, dismissed 1, **3 fixed and 4 filed** | **Exited at the 3-loop cap.** Two fixed items were loop 11's own collateral, and one of them is the run's sharpest lesson: loop 11 corrected § 1's header to say `RoadmapDialog` "reads the store on one path (§ 5.1's legend)", which is *still false* — `roadmapdialog.cpp:575` reads its **bullets** through `RoadmapSource::bulletsFor()` and falls back to a markdown parse only where there is no store row, and `ROADMAP.md:25311` carries ANTS-3793 as ✅ shipped. A fix aimed at one sentence inherited the stale premise beside it. § 1 and § 9 now say the consumer **read** cutover shipped and what INV-3 ends is the markdown **writing**. Also fixed: § 7.5 ¶1 still opened "§ 3.3 leaves it empty on migrated ones, like every other field with no source-side counterpart" after loop 11 gave § 3.3 the opposite rule — a migration implementer reading that first sentence discards 86 declared integers; and § 3.2 gated publish on "any **public** open item" while § 4.1's canonical obligation cell and § 3.1's gloss both said "open items" with no such qualifier. **Dismissed on evidence:** a lane read `roadmapdialog.cpp`'s "legendText is the RAW stored text" as raw markdown and called § 5.1's status-value→wording claim false; `RoadmapStore::setLegend()` takes a `QJsonObject` and stores `canonicalJson()`, and `legendByEmoji()` parses that JSON keyed by status word — § 5.1 is right, and "raw" means un-reparsed JSON. **Filed, not fixed — four, each a design call rather than a wording defect**, listed in ANTS-4068. The cap binding here is evidence about the document: at ~960 lines two cold reads are still reaching parts of it for the first time, and every one of the four is a rule this standard has never actually stated. |
| 11 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loop 10; packet repaired first (it had claimed to carry "every section this doc cites" while omitting `roadmap-format.md` §§ 3.6.2/3.6.3/3.10.4/3.11, and its DDL window cut before `feedback_ref` and `citation`, which both lanes then asked about) | **Q1 1 · Q2 3 · Q3 1** — verified 5, dismissed 1 | **Two of the five were loop 10's own collateral**, which is what the loop is for: § 1's header still listed `RoadmapDialog` wholesale as not cut over after § 5.1 was corrected to say its store path ships, and § 3.3's new "migration harvests it by § 7.5's mapping" pointed at a mapping that is total only over four bare uppercase words — while 86 of the corpus's 88 `Priority:` values are already integers and the other two are `medium` and `LOW`, so the rule as written would have left almost every declared priority empty. § 7.5 now states a total rule (integer taken as itself; leading token matched case-insensitively; anything else left empty with the raw string in `extras`). **Three were pre-existing and older than this run.** § 7.7 defined `defaulted` as "absent at migration", but `roadmapmigrate.cpp` stamps it on the *unmapped* branch too, where the source did declare a `Kind:` — so any "how much of the corpus declared a kind" count taken from provenance is wrong by the unmapped population; the row now names both cases and the `extras.source_kind` that separates them. § 1 said the render's losslessness oracle "does not exist yet" while *What checks this* called `Inv2BackendsAgree` "the stronger check of the same property" — its equality is over the 20 fields of the **bullet** record, so `extras` and `provenance` are never compared, and the row now says so. And § 1's "a failed publish leaves the store ahead of the file" read as flatly contradicting `Inv1RenderFailureRollsBack` until you know a write renders **twice** — a validating dry render before the commit (failure rolls back) and a publishing render after it (failure leaves the store ahead); neither passage said so, and § 1 now does. **Dismissed:** a lane asked what provenance a caller-supplied `stable_prefix` id carries — real, but it is ANTS-3838, filed at loop 9 and deliberately left open as a design call. |
| 10 | 2026-08-08 | 2, cold — identical byte-stable shared packet (~13k tok) carrying the live `roadmap-corpus-survey.py` output, the `item`/`element`/`relationship` DDL, `mappedKind()`, three `src/roadmap*.cpp` outlines and every cited section of `roadmap-format.md` / `documentation.md`; genre pinned `standard` | **Q1 5 · Q2 2 · Q3 2** — verified 9, dismissed 0 | **First loop under the four questions** (the C/H/M/L/I column above is the retired scale; these are Q-counts). ANTS-4067's re-sync was the trigger, and the cold read found six defects it had not gone looking for. The one an implementer would have built wrong: **§ 3.3 said migration leaves `priority` empty while § 7.5 gives a full harvest mapping** (`CRITICAL → 1` … plus a tie-break) that has no purpose unless migration reads `Priority:` lines — 88 items declare one, 86 already as integers. One builder leaves the column NULL corpus-wide; another populates it; both cite this document. § 3.3 now distinguishes *no default* (nothing is invented) from *never harvested* (a declared value is dropped), which is the confusion underneath it. **Two claims had simply gone stale under shipped code:** § 5.1's "nothing reads a project's legend at all" — ANTS-3793's `RoadmapDialog::storeLegend()` reads it on the store path (`roadmapdialog.cpp:589`, wired at `:3046`), so § 9's open question narrows to the markdown path; and § 3.1's curating-write carve-out covered only a status flip, leaving a headline edit on a migrated item demanding fields § 7.5 exempts. **`write (migration-populated)` was used in § 4.1.1 and defined nowhere**, and `''` was defined only for pre-column projects, so a project created in the store after cutover had no stated value — both closed. Four figures were stale against the corrected survey: pass-headings 144 → 154 items and 136-of-154 → 142-of-164 status values, `Dependencies:` ~21 → 98, sub-bullets ~1,500 → ~1,800, and "over half carry no `Layman:`" was false in direction (49%). **One fix landed outside this document:** `roadmap-format.md` § 3.5.1 named the detector's task-list literal `gfm` where the shipped detector (`roadmapparse.cpp:1033`), the `source_format` CHECK and every consumer use `github-task-list` — this document was right and its neighbour wrong, so the neighbour was corrected and now owes its own gate. |
| 9 | 2026-08-05 | 3 (one per host doc, cold; no prior-loop briefing) | 3 / 6 / 10 / 15 / 0 | **Converged by cap (3 loops this run).** 34 raised, 32 verified and fixed, 1 dismissed on evidence, 1 re-found already-filed. Dimension tally: dim 7×5, dim 5×5, dim 6×5, dim 4×5, dim 2×4, dim 1×4, dim 8×3, dim 12×2, dim 11×2. **Two lanes independently converged on one root cause**, which is the finding of the run: "cut over" and "store-migrated" are different sets — the second is the first *plus* an `ants-v1` roadmap — and loop 8 had qualified § 3.2, § 9 and the *What checks this* row while leaving INV-3, § 8's allocation bullet and § 10's anti-pattern speaking of cutover alone. An implementer reaching INV-3 first would have built a store-only writer for every cut-over project, including the GFM and pass-headings ones that still splice. INV-3 now carries the definition and every dependent passage points at it. Also fixed, and pre-existing rather than collateral: § 7.1 claimed a shared store makes the wiped-counter failure mode "disappear, along with the per-prefix bookkeeping" — the shipped allocator keeps both (`rlStoreIdHighWater()` `max()`es the store row over `corpusHighWater()`, keyed per `(project, prefix)`), so that sentence is the one that would have talked an implementer out of the floor it exists to protect. `Inv3Allocation` gained the *What checks this* row it never had. **Dismissed on evidence:** a lane read "a refusal lands in `skipped[]`" as over-broad because `bad_op_combo` runs ahead of the store dispatch; `src/remotecontrol.cpp:10269` shows it too is `skip(li, …)`, per locator. **Filed, not fixed:** ANTS-3838 — the store `append` path stamps `provenance.id = "asserted"` on every branch while § 7.7 reserves `store-generated` for exactly that write. Verified as a divergence; which side is canonical is a design call, so neither doc was bent to match the other. |
| 8 | 2026-08-05 | 3 (one per host doc, cold; no prior-loop briefing) | 1 / 7 / 9 / 9 / 1 | 27 raised, 25 verified and fixed, **1 dismissed on evidence**, 1 INFO. Dimension tally: dim 4×5, dim 6×4, dim 8×4, dim 7×3, dim 2×3, dim 1×3, dim 5×2, dim 12×1, dim 13×1. Roughly half were collateral from loop 7's own fixes, which is what the loop is for: the pass-headings case had been folded into loop 7's carrier table as a prose caveat that flatly contradicted the row above it ("read and written" vs "left untouched") — now its own row; the framing sentence loop 7 added keyed on "has no store row" while the table it introduces includes store-row projects; and § 3.2's new paragraph said "after cutover" where the trigger is cutover **and** the emoji-bullet shape, which this document already scoped correctly in two other places. Genuine draft defects loop 7 missed: `dry_run` was documented as always previewing, but `commitAndRender()` checks the gate *before* the dry-run return, so on a gate-failing project — this project, today — a preview refuses `render_gate_unmet` instead; § 7.1's `stable_prefix` carve-out claimed to be "the one post-cutover write that carries an id in from outside" while `id_hint` also does; and INV-3's "the export's check has shipped" is leg (a) only, contradicted by the table's own "leg (b) — nothing yet". **The dismissal is the reason findings are verified rather than applied:** a lane argued that if every store bullet's `firstLine` is 0 then a range from line 1 matches *nothing*, so the stated reason for refusing `line_range` was backwards. The envelope reports `line` as `firstLine + 1` (`src/remotecontrol.cpp:8944`), so every store bullet reports line 1 and `[1,10]` matches all of them — the original wording was right. **Filed, not fixed:** ANTS-3837, the neighbouring pass-headings bullet's op list predating `amend_body` and `bundle_row`. |
| 7 | 2026-08-05 | 3 (one per host doc, cold; genre pinned `standard`) | 2 / 6 / 11 / 14 / 0 | **ANTS-3809 § 7 gate**, run on the three cross-doc rows the write half owes. 33 verified, 0 dismissed, all fixed; 1 collateral self-caught by the sweep, 1 surfaced. Dimension tally: dim 2×7, dim 4×9, dim 5×7, dim 8×5, dim 6×3, dim 7×2, dim 1×1. This document's own two led: § 4.1 said "**the store owns allocation**, so a post-cutover author never supplies one" while `id_strategy: "stable_prefix"` — a live argument the shipped schema accepts — has the caller supply one, and `id_origin` had no value for it (now `synthesised`, with § 7.1 stating why `parsed` and `quarantined` are both wrong). The Status header still said the published render "**ha[s] not**" shipped while three rows of the *What checks this* table it sits above cite ANTS-3758 as shipped. Also fixed: § 3.2 stated the publish gate's whole consequence as publication not happening, when after cutover it refuses **every** write op project-wide; § 9 still listed `roadmap_log` among the consumers that write markdown; and the new row's own "migrated project" / "`ants-v1`" were this document's only uses of either term against 30 uses of "cut over". The added row and § 8's bullet stated the same three facts twice — the § 8-reconciliation row is now a pointer, per the delete-N−1 rule rather than a reconcile. The self-caught collateral is the shape the sweep exists for: a fix citing the emoji-bullet row as "§ 5" when it is in § 7.1. **Surfaced, not fixed:** the store fills `firstLine`/`lastLine` with 0 on the read side too, so `mcp-behavioural-notes.md`'s `roadmap_query` entry may owe the same caveat — ANTS-3793's, not this run's. |
| 6 | 2026-08-03 | 2 (same partition, cold; no prior-loop briefing) | 1 / 2 / 10 / 8 / 1 | **Converged by cap (3 loops this run).** 21 verified, 1 dismissed, 20 fixed, 1 surfaced. Both lanes independently led on the same line, and it is the ANTS-3795 defect the two earlier loops missed: § 4.1's `layman` row still read "**The only text a public reader sees**" — the layman-only premise surviving inside the canonical field table, contradicting the INV-2 this whole amendment rewrote. Loops 4 and 5 swept for the phrase and not for the *claim*, which is why grepping a premise finds only the wording that states it. Also fixed: `provenance`'s four-value enum had no value for a post-cutover store-stamped field — the commonest case there will ever be — now `store-generated`; the INV-1 row implied both of that invariant's legs were checked when the shipped tests cover only leg (a); and § 7.5's severity vocabulary was attributed wholesale to `roadmap-format.md` § 3.8, which owns the headline convention but enumerates no values (one lane over-claimed here and was corrected on the evidence). **Surfaced, not fixed:** § 3.2 gates publish on `layman` while § 3.3 leaves it empty on every migrated item, so read literally no project can publish its first render until hundreds of items are hand-curated. That is a cost decision, not a defect, and § 3.2 now says so and hands it to ANTS-3758. |
| 5 | 2026-08-03 | 2 (same partition, cold; no prior-loop briefing) | 0 / 4 / 12 / 13 / 1 | 29 verified, 0 dismissed, all fixed. **Roughly half were collateral from loop 4's own fixes** — deleting a sentence in § 5.1 stranded the verb after it ("Today `RoadmapDialog` cannot:"), § 3.1's rewrite and § 4.1's new `write (store-populated)` qualifier landed in one batch without being reconciled, and the ID-floor bullet was left referring to "the layman-only premise" the same loop had deleted. The remedy taken was a **harder blast-radius sweep, not another dispatch**: re-reading each edited passage in context rather than grepping it, which is what found them. Two genuine draft defects an implementer would have built wrong: `id` was marked a plain `write` obligation while § 7.1 gives allocation to the store, and § 3.1 declared itself canonical while enumerating three of the four obligation qualifiers. The sweep also generalised this loop's top finding — a bare `§ 3.3` resolving to **this** document's § 3.3 instead of `roadmap-format.md`'s — into the class it belongs to: eight further unanchored cites, now fixed, with a citation convention stated in § 1 so the next one is a defect rather than a shorthand. |
| 4 | 2026-08-03 | 2 (single doc, cold; genre pinned `standard`) | 0 / 4 / 7 / 9 / 0 | **ANTS-3795 amendment gate**, run on the edit that made the render full-fidelity. 20 verified, 3 dismissed, all 20 fixed, plus 2 collateral the blast-radius sweep caught. Both lanes independently led on the same three: the **Status header still said "not yet implemented"** while the store, migration and export had shipped; **§ 5's section definition omitted `position` and `source_path`**, the two columns ANTS-3796/3797 added and the render depends on, so the model as written could not round-trip what the store already stores; and the *What checks this* table still said **nothing yet** for INV-1 when `roadmap_export_roundtrip` tests it. Also fixed: § 7.1 mandated flagging an unparseable ID with no field in § 4 to carry it while the store had shipped `item.id_origin` — now a § 4.1 field. **Every corpus figure was re-derived rather than reasoned about**, and the survey now reports 13 projects and 4,080 items against the draft's 10 and its counts of 86 / 78 / ~90 / 8 / 280 / 218; the exact counts are gone in favour of the proportions § 3.3's own policy asks for. One finding landed on THIS amendment: the ID-floor claim that "all three inputs survive" ignored § 9's still-open render-curation question. |
| 3 | 2026-07-30 | 3 (model coherence, identity/migration, cross-doc) — genre pinned `standard` | 6 / 9 / 14 / 15 / 2 | Every verified finding fixed. Corpus scope was wrong: the survey globbed `ROADMAP.md` and missed a tenth project whose file is lowercase, so the document asserted no project used pass headings when one tracks 144 items in it, 136 of whose statuses fall outside § 7.3's enum. Also fixed: § 5.1's justification was false against `roadmapdialog.cpp`; § 8 gained the render-conformance and archive-rotation touch points; identity semantics, `provenance` (new § 7.7), obligation-tier vocabulary and the `blocked-by`/INV-5 conflict all settled. Added § 10 anti-patterns and a *What checks this* table. Genre pinning is what changed the run: loops 1–2 graded a standard against spec shape. **Exited at the 3-loop cap.** |
