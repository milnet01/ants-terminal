# Roadmap Data Model Standard (ANTS-3753)

> **Status:** Draft 2026-07-30 — not yet implemented. Supersedes nothing;
> `roadmap-format.md` remains the authority for the *markdown* format until
> the implementation spec lands.
>
> **Source:** user-request-2026-07-30 (roadmap moves to a shared database).

This standard defines **what a roadmap item is**, independent of how it is
stored. `roadmap-format.md` describes one *serialisation* of it (markdown).
This describes the model those bytes carry, so a database, an export file and
a published page can all be checked against one definition rather than against
each other.

It is written to be implementable, not aspirational: every field below is
either already in the corpus, or named here as a deliberate addition with the
reason it is required.

---

## 1. Three artifacts, three jobs

| Artifact | Role | In git? |
|---|---|---|
| `roadmap.sqlite` | The working store. All reads and writes go here. | **No** |
| `roadmap-export.jsonl` | The durable record and backup. One item per line. | **Yes** |
| `ROADMAP.md` | Published render for humans. Layman content only. | **Yes** |

The split exists because the three have incompatible requirements. The working
store needs indexes and transactions; the durable record needs to diff in git
and rebuild the store; the published page needs to be readable by someone who
does not work on the project.

**INV-1 — Lossless round-trip.** Rebuilding the store from the export and
re-exporting MUST produce a byte-identical export. Every field in §3 and §4,
every relationship in §6, and the ordering in §5 survives the trip. This is
the property that makes it safe to keep the database out of git: the export is
a complete copy, not a summary.

**INV-2 — The published render is derived and lossy, and that is deliberate.**
`ROADMAP.md` carries §3's `layman` text and nothing technical. It is never
parsed back. It is therefore NOT a backup, and must never be treated as one —
this is the distinction that decides INV-1's target.

**INV-3 — One writer of record.** The store is written only through the
roadmap verbs. The export and the render are generated artifacts; a hand-edit
to either is lost at the next generation, and the tooling MUST detect that
case rather than silently discard the edit (§9).

---

## 2. Scope

Applies to every project indexed by the shared store, including projects that
have no roadmap yet. The model MUST NOT assume the shape of any one project's
corpus — most installations will have one project, not nine.

---

## 3. Required fields

An item missing any of these cannot be reported on, and MUST be rejected at
write time.

| Field | Type | Notes |
|---|---|---|
| `project` | ref | The owning project. Items are never global. |
| `id` | text | `PREFIX-NNNN`, unique within the store (§7.1). |
| `status` | enum | §7.2. |
| `headline` | text | One line, technical. Ends with a period. |
| `layman` | text | One sentence, non-technical. **The only text a public reader sees** — an item without it is invisible on the published page. |
| `kind` | enum | §7.3. |
| `source` | text | Provenance. The cross-session workflow is unanswerable without it: "what did project X ask for" is a query on this field. |
| `priority` | int 1–5 | §7.4. **Required for open items only** — priority on finished work is meaningless, and backfilling it across completed items would be invention. |
| `created` | date | §7.5. |
| `last_modified` | date | §7.5. |

### 3.1 Required on close

| Field | Type | Notes |
|---|---|---|
| `shipped` | date | Required when `status` is shipped. |
| `resolution` | text | Required when `status` is shipped or dropped. What was done and why, or why it was not done. |

`body` is deliberately **not** required (§4). `resolution` is, because the
institutional value of a roadmap sits in closed items: a shipped item with no
resolution has recorded that something happened and nothing about what.

Specs do not substitute for this. There are ~218 spec documents against ~3,216
items across the current corpus, so for the large majority the item itself is
the only technical record.

---

## 4. Optional fields

| Field | Type | Notes |
|---|---|---|
| `body` | text | Free-form technical detail. Optional because many legitimate items are complete in one line, and a mandatory body produces filler that reads like content. |
| `lanes` | list | Subsystems touched. |
| `evidence` | list | Paths to screenshots, logs, repros. |
| `visibility` | enum | §7.6. Defaults to public. |
| `blocked` | bool | Derived from an unresolved `blocked-by` link (§6), never set by hand. |
| `milestone` | text | Target release. Distinct from `section`, which is where the item is *filed*. |
| `extras` | key/value | §4.1. |

### 4.1 The extension mechanism is required, not a convenience

Surveyed across all nine project roadmaps, four field keys dominate — `Kind:`
(2,077), `Source:` (1,971), `Layman:` (1,717), `Lanes:` (657) — followed by a
long tail of roughly eighteen that individual projects invented: `Fix:`,
`Scope:`, `Why:`, `Goal:`, `Theme:`, `Dependencies:`, `Spec:`, `Test count:`,
`Repro:`, `Framework:`, `Root cause:`, `Prerequisites:`, `Risk:` and others.

A model with only the fields in §3 and §4 would **silently delete that tail on
the first regeneration**. `extras` is what stops that, and it is therefore a
correctness requirement rather than a nicety.

Three tail keys are promoted out of `extras` because they are *relationships*
rather than facts: `Spec:`, `Dependencies:` and `Prerequisites:` become links
(§6). The remainder stay as extras — they describe how someone chose to
structure a write-up, which is prose organisation, not queryable metadata.

---

## 5. Structure and ordering

Items live in a section tree and their order carries meaning.

| Field | Notes |
|---|---|
| `section` | ref into the section tree. |
| `sort_order` | Position within the section. **Priority is positional in the current corpus** and that ordering is real information; losing it on migration would discard the project's accumulated prioritisation. |

`priority` (§7.4) and `sort_order` are complementary, not redundant:
`sort_order` is an exact total order within one project; `priority` is a coarse
band that is comparable *across* projects, which position can never be.

### 5.1 Sections

A section has `slug`, `title`, `level`, optional `intro` prose, an optional
parent, and its own order. Sections nest — release block (`##`) containing
themed sub-sections (`###`) is the dominant shape, at 116 and 420 occurrences
respectively across the corpus.

### 5.2 Structures the model must survive

Measured across the nine-project corpus. Each must round-trip under INV-1:

| Count | Structure |
|---|---|
| 1,209 | sub-bullets under an item |
| 610 | narrator bullets — prose bullets carrying a status marker but **no ID** |
| 182 | markdown table rows (bundle progress tables) |
| 38 | fenced code blocks inside bodies |

Narrator bullets are the awkward case: they are not items (no ID, nothing to
report on) but they are not section prose either. They are modelled as an
ordered element of the section, not as an item, so they never appear in item
counts and never require §3's fields.

---

## 6. Relationships

Typed, directed links between items. This is where the model earns its keep:
several currently-open problems become queries rather than conventions.

| Type | Meaning | Replaces |
|---|---|---|
| `splits-from` | This item was carved out of a parent. | ANTS-3748 — today a split leaves feedback files citing only the parent, whose completion then over-claims. |
| `blocked-by` | Cannot start until the target closes. | Prose markers parsed out of body text. |
| `duplicate-of` | Same work as the target. | Manual dedup. |
| `supersedes` | Replaces an earlier decision. | Nothing — currently unrecorded. |
| `relates-to` | Untyped association. | `Dependencies:` / `Prerequisites:` (26 uses). |
| `specified-by` | Target is a spec document. | `Spec:` (17 uses). |

**INV-4 — Links are cross-project.** A link's target may belong to another
project. This is new capability: one project's work being blocked by another's
is currently expressible only as prose.

**INV-5 — A citation is not a link.** Roadmap prose frequently mentions IDs
belonging to other projects or other documents — this project's own roadmap
contains `FIBR-` and `ADR-` references in body text. An ID appearing in prose
MUST NOT create a link. Links are declared, never inferred from text.

### 6.1 Adjacent tables

- `feedback_refs` — which cross-session feedback file cites which item. Makes
  "does this file cite an ID that no longer covers its finding" answerable.
- `citations` — item or spec → file + symbol. Makes `documentation.md` § 1.7
  (cite symbols, not lines) machine-checkable across the ~1,956 citations in
  this project's docs, instead of a manual sweep.
- `history` — one row per field change, written automatically.

### 6.2 Why `history` is required, not optional

Today git carries the full history of every roadmap edit, because the roadmap
*is* a tracked text file. Under this model the store is untracked and the
published render is lossy, so **that history has nowhere to live**. The export
is committed per push, which gives coarse snapshots but not per-edit change.

The `history` table restores it, and improves on it: structured and queryable
where a git diff must be read. Items are amended routinely as understanding
improves, so this is a normal path, not an audit corner case.

---

## 7. Enumerations

### 7.1 Identity

`PREFIX-NNNN`. The prefix is per project (`ANTS`, `DOOM`, `FIBR`, `CL`,
`ONEUP`, `ROLO` are current). **The store owns allocation.** Today each project
keeps a gitignored per-machine counter that is explicitly not the source of
truth, with a floor recomputed by scanning the corpus so a wiped counter cannot
reissue a live ID. A shared store allocates directly and that entire failure
mode disappears.

**Three of the nine surveyed projects have no IDs at all** — 3D_Engine uses 408
GitHub-style checkboxes, and MAME Curator and Music Production use neither
scheme. Migration must therefore **allocate** identity, not merely parse it
(§8).

### 7.2 Status

`planned` · `in-progress` · `shipped` · `considered` · `dropped`

`dropped` is new. Abandoning an item currently means deleting its line, which
erases the decision along with the item. A decision *not* to do something is
exactly the kind of thing that is wanted six months later, so it is recorded
rather than removed.

### 7.3 Kind

The canonical set is `roadmap-format.md` § 3.5.3's enum. The corpus currently
holds **34 distinct values against that ~21-value enum**: `bug` / `bugfix`
alongside `fix`, `docs` alongside `doc`, `feat` alongside `feature`, `enhance`
alongside `enhancement`, `performance` alongside `perf`, `testing` alongside
`test`, plus `spike`, `design` and `tooling`.

Thirteen values normalise to canonical ones at migration; the mapping is
recorded in the migration spec so it is reviewable rather than silent. An
unmapped value is a migration refusal, never a guess.

### 7.4 Priority

`1` highest … `5` lowest, required on open items.

Five bands rather than ten: ten levels are not reliably distinguishable, so in
practice they collapse to a three-point scale with the rest defaulting to the
middle, and the number stops carrying information. Each band must be
argued for to be assigned.

### 7.5 Dates

`created`, `last_modified`, `shipped`. ISO 8601 (`documentation.md` § 1.3).

**No date field exists in the current model at all** — closure is recorded as
prose (`Resolved (2026-07-30): …`). Consequently "what shipped last month",
"how long do items stay open" and "is the backlog growing" are not currently
answerable without parsing English. These three fields are the largest single
reporting gain in this document.

Effort and size estimates are deliberately **excluded**. They are guesses,
they attract false precision, and `created` + `shipped` yield real cycle time
from observed data instead.

### 7.6 Visibility

`public` · `internal`

The published render includes only `public` items. This is new capability:
today everything is published because the file *is* the record, including
security findings that are still open. It is consistent with the existing
posture in which audit artifacts stay local while the roadmap is public by
design.

---

## 8. Migration requirements

Derived from the survey, not assumed. Migration MUST:

1. **Read all three source shapes** — emoji bullets, GFM task lists, and
   pass headings (`#### Pass N.M`). Parsers for all three already exist.
2. **Allocate IDs** for the three projects that have none, without disturbing
   the six that do.
3. **Normalise the 13 non-canonical `Kind` values** via a reviewed mapping.
4. **Backfill dates** from git history where derivable, and mark them as
   derived rather than asserted.
5. **Refuse rather than guess.** An unparseable item, an unmapped kind, or a
   duplicate ID stops the migration for that project. Partial migration of a
   project is not permitted.
6. **Report what it could not fill.** Currently 33% of items carry no layman
   line, and 66 of 215 open items lack one — those must be written before a
   project's first publish, or a third of its roadmap ships invisible.

Migration is re-runnable and read-only against the source markdown until an
explicit cutover.

---

## 9. Health checks

The store is verifiable, and `verify` is what gives the standard teeth. It runs
in a pre-commit or pre-push hook, mirroring the existing test gate.

| Check | Fails when |
|---|---|
| Schema integrity | The store's own consistency check fails. |
| **Round-trip** | Rebuild-from-export does not reproduce the export byte-for-byte (INV-1). |
| Required fields | Any §3 field is absent; `resolution` absent on a closed item. |
| Referential | A link, feedback reference or citation points at a missing target. |
| Identity | Two items share an ID, or an ID is outside its project's prefix. |
| Enumerations | A status, kind, priority or visibility value is off-list. |
| Render fidelity | The committed `ROADMAP.md` differs from a fresh render — i.e. someone hand-edited a generated file (INV-3). |
| Corpus regression | Any indexed project fails to re-index cleanly. |

---

## 10. Deferred to the implementation spec

Named here so they are decided rather than discovered:

- Whether the published page lists completed items at all, or only open work
  plus recent releases. The CHANGELOG already carries release history.
- What happens to a hand-edit of `ROADMAP.md` — detect and refuse, or accept
  and document. §9's render-fidelity check assumes refuse; the spec confirms.
- Concurrency settings for multi-project writes, and the constraint that the
  store must sit on local disk (file locking is unreliable on network shares).
- Whether `sort_order` is renumbered on insert or kept sparse.
