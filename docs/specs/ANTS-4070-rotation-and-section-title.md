# ANTS-4070 — move a closed minor's sections to its archive, and let a section be retitled

**Status:** accepted (2026-08-10) — 25 findings verified across five passes, all
fixed. **Loop 4 falsified this document's own headline claim**: rotation does
need a new store primitive, and its archive path was anchored in the wrong
place. Review is closed in favour of building test-first, on the evidence in the
loop log — the cold reads were returning document-internal findings while the
defects that mattered came from checking the spec's claims against
`roadmapmigrate.cpp`. INV-13 is the instrument that settles what is left.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-4070 (in-session-2026-08-09, § 9 decision pass,
user-requested).
**Blocker for:** ANTS-4065 Phase D (the re-import), per the user's ordering that
all code lands before migrating.
**Pairs with:** ANTS-4078 (the roadmap's 3.2 MB, split out of this item after
measurement — see § 1).

**Layman:** After a version ships, the finished work is meant to move out of the
main roadmap file into a per-version archive. Since the roadmap moved into a
database nothing does that any more, and there is also no way to rename a
section heading. This adds both.

---

## 1. Problem

**Rotation lost its owner at cutover, and nothing reports that it stopped.**
`roadmap-format.md` § 3.9 puts rotation in `/bump` as a snip-and-create on the
markdown. On a store-migrated project that hand edit is discarded at the next
render — silently, with no error, which `roadmap-data-model.md` § 10 states as
the general rule for hand-editing generated output. So rotation does not fail;
it stops happening and says nothing.

Both standards already decide what should replace it.
`roadmap-format.md` § 3.9's last paragraph and `roadmap-data-model.md` § 8's
last bullet say the same thing in the same words: **reassign the closed minor's
sections to the archive path and re-render.** This spec is the mechanism for
that sentence, not a re-decision of it.

**A second, smaller hole shares the cause.** `roadmap-format.md` § 4.3 step 4
changes a released block's heading from `(target: YYYY-MM)` to
`shipped (YYYY-MM-DD)`. That is a *section title*, and § 4.3 records that no
store operation for changing one is defined — so a cut-over project cannot
perform step 4 at all. § 4.3 names this item as the one that owes it. Both
operations move or mutate a whole section rather than a bullet, which is why
they are one spec.

**What is NOT the problem, measured before drafting.** The roadmap bullet leads
on size — 3.2 MB against § 3.9's ~150 KiB review trigger — and rotation cannot
address it. Measured 2026-08-09 with the script recorded in ANTS-4078, over
`ROADMAP.md`'s 3,300,473 bytes under `##` headings: **3,029,307 of them (92%)
sit under minor 0.7, which is the open minor** (`project(ants-terminal VERSION
0.7.104)` in `CMakeLists.txt`), and § 3.9 forbids within-minor rotation on
purpose. Minors 0.5 and 0.6 are already archived — `docs/roadmap/` holds exactly
`0.5.md` and `0.6.md`. So rotation built exactly to the standard has **nothing
eligible to move today** and is correct anyway: it runs the moment 0.7 closes,
and that bump then moves ~3 MB in one step. The size question is ANTS-4078's;
the user settled the split on 2026-08-09 (§ 2.5).

> **"Nothing eligible" is a claim about § 2.2's rule, and it only holds because
> of that rule's third case.** `ROADMAP.md` still carries a live
> `## 0.5.x and 0.6.x — archived` heading — 303 bytes of signpost pointing at
> the two archive files. A selection that accepted any non-digit after the
> minor would claim it on `rotate_minor 0.5` and file the pointer inside its own
> target. § 2.2 excludes it, INV-4 tests it, and without both this paragraph
> would be false.

## 2. Surface

### 2.1 Both operations are callers over one new store primitive

The single most useful thing found while sizing this: **almost every write
these operations need already exists**, and the roadmap bullet's "build it as a
store write" overstates the work.

**The exception is a slug setter, and BOTH operations need it** — for one
underlying reason. A section's slug is not stored data the render round-trips;
it is *derived on import* from the section's heading **and from which file the
section was found in** (§ 2.2's re-slug rule, § 2.3's). So an operation that
changes either input must rewrite the slug, or the next re-import derives a
slug the store does not hold, `findSection()` misses, and a **second** section
is added beside the first. Retitle changes the heading; rotation changes the
file. INV-11 and INV-13 are the same invariant aimed at the two operations.

- `RoadmapStore::setSectionSource(sectionId, std::optional<QString> sourcePath)`
  (ANTS-3782 § 2.2) reassigns a section to a file. `nullopt` means the live
  roadmap.
- `RoadmapStore::updateSection(sectionId, title, level, position, parentId)`
  (ANTS-3765 § 2.6) changes a section's title.
- `RoadmapRender::render()` already buckets sections by
  `sourcePath.value_or(liveRoadmapPath)` and emits one file per bucket, so an
  archive re-emits to its own path with no render change at all.
- `RoadmapWrite::commitAndRender(store, projectId, projectRoot, liveRoadmapPath,
  dryRun, mutate, outcome, error)` (ANTS-3809) takes the mutation as a callable
  and owns the whole sequence — begin, mutate, dry render, commit, real render —
  including **ANTS-3758 INV-5's per-project publish gate**. Every reference to a
  `§ N` in this list belongs to the spec named beside it, never to this
  document.
- **`RoadmapStore::setSectionSlug(sectionId, slug)` — the one addition**, or
  `slug` added to `updateSection()`'s tuple. There is no slug setter today:
  `addSection()` takes one at INSERT and `updateSection()`'s tuple is
  `(title, level, position, parentId)`. Verified against `src/roadmapstore.h`,
  whose public surface carries `addSection`, `setSectionIntro`,
  `setSectionSource`, `updateSection`, `findSection`, `readSection` and
  `listSections`, and no slug setter among them.

So each operation below is a **caller**: validation, a section selection, and a
`mutate` lambda over the setters above. No new column, no new render path, and
no second markdown writer — which is ANTS-3809 INV-2's rule. The one store
addition is the slug setter, and its absence is a compile failure rather than
anything a test could catch (§ 9).

### 2.2 `roadmap_log op:"rotate_minor"`

```jsonc
{ "op": "rotate_minor",
  "caller_cwd": "<project root>",
  "minor": "0.7",            // <MAJOR>.<MINOR>, the minor that just closed
  "dry_run": true }          // optional; previews without writing
```

**The archive path is derived, never passed, and it is anchored on the PROJECT
ROOT.** It is **`docs/roadmap/<minor>.md`, relative to the project root** —
which is the only form the migration can read back, fixed by two independent
mechanisms:

- `RoadmapMigrateLoad`'s `isPlaceableSourcePath()` matches the
  **project-root-relative** path against
  `\Adocs/roadmap/(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md\z` and **refuses the
  whole load** with `source_unplaceable` for anything else — including an
  in-project `docs/archive/0.6.md`, which its own comment names as the quieter
  route to the same silently-unplaceable section.
- `findRoadmaps()` discovers archives by building `QDir(projectRoot)` and
  reading `<projectRoot>/docs/roadmap`.

**An earlier draft anchored the path on `dir(liveRoadmapPath)` instead, on the
grounds that the migration's archive discovery "looks beside the live file". It
does not** — both mechanisms above are anchored on the root, and a
`sub/docs/roadmap/0.7.md` is refused by the first and never seen by the second.

**§ 3.9 anchors archives at `<dir(ROADMAP.md)>/docs/roadmap/…`, and the two
coincide because the migration requires the roadmap at the root anyway.**
`findRoadmaps()` accepts a live roadmap only as a file lying **directly in the
project root** whose name case-folds to `roadmap.md` — its own comment notes
that a `docs/ROADMAP.md` "is not a candidate". So a project whose
`.ants/project.json` moves the roadmap out of the root is one the migration
cannot discover, and therefore one neither of these operations can reach: they
refuse `op_unsupported` on a project that is not store-migrated (§ 2.4). Nothing
here needs to handle that case, and an invariant asserting a path for it would
be asserting behaviour no fixture can produce.

**§ 3.9's case-sensitive regex `^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$` validates
the FILENAME**, not the whole path, and is applied before anything is written. Deriving it is what keeps the naming rule in one place; accepting a
path would let a caller write `docs/roadmap/v0.7.md`, which § 3.9 forbids and
which the migration's own archive discovery would then refuse to read back.

**Selection — which sections move.** A top-level (`level == 2`) section is in
the minor when its title, after an optional leading `v`, begins with
`<major>.<minor>` and what follows that prefix is one of exactly three things:

1. **end of title** — `## 0.7`;
2. a character that is neither a digit **nor a `.`** — `## 0.7 — …`, `## 0.7: …`;
3. a **`.` followed by a digit** — `## 0.7.0 — …`, `## 0.7.50–0.7.59 — …`.

**Case 2 excludes `.` on purpose, and the three cases are disjoint.** Written as
one expression against the title, which is the contract:

```
^v?<major>\.<minor>(?:$|[^0-9.]|\.[0-9])
```

An earlier draft said case 2 was any "non-alphanumeric" character. A `.` is
non-alphanumeric, so case 2 swallowed case 3 and the rule claimed everything the
third case exists to reject — adding a disjunct cannot exclude anything.

Every descendant section moves with its parent, resolved through `parentId`,
because § 3.9 rotates "the closed minor's heading **and its sub-headings**".

**Each of the three exists to exclude something real in this corpus, and the
third is the one a plain prefix match gets wrong.** Case 3 rejects `## 0.70.0`,
which a bare `startsWith("0.7")` claims. It also rejects
`## 0.5.x and 0.6.x — archived` — a 303-byte **signpost** that lives in the live
roadmap and points at `docs/roadmap/0.5.md` and `0.6.md`. That heading names two
minors, belongs to neither, and must stay where a reader looking for the
archives will find it; moving it into the archive it points at would delete the
pointer and duplicate its text inside its own target. `.x` is a `.` followed by a
non-digit, so the rule excludes it without a special case. Case 1 exists because
a title that *is* the minor has no following character at all, and a rule
phrased only over "the next character" is undefined for it.

Verified against the corpus 2026-08-09 by `grep -nE '^## 0\.7' ROADMAP.md`,
which returns **nine** headings and the expression selects all nine:
`## 0.7.0 — …`, `## 0.7.7 — …`, `## 0.7.12 — …`, `## 0.7.50–0.7.59 — …`,
`## 0.7.65 — …`, `## 0.7.78 — …`, `## 0.7.79 — …`, `## 0.7.80–0.7.84 — …` and
`## 0.7.92 — …`. It rejects `## 0.8.0 — …`, `## 0.9.0 — …`, `## 1.0.0 — …` and
the `0.5.x` signpost.

**A section whose `sourcePath` already EQUALS the derived archive path is
skipped; one matching the minor but filed under a *different* path is
reassigned.** The equality is the rule, not "already archived" — under the looser
reading a `0.7` section mistakenly sitting in `0.6.md` would be left there, so
the parent heading would render into `0.7.md` while a child stayed in `0.6.md`,
which is INV-5's failure mode arriving from the other side. Equality is also all
the idempotency argument needs: a re-run finds every section already at the
derived path and moves none.

**A moved section is RE-SLUGGED, because the importer prefixes every archive
slug with its file's minor.** This is the half a reassignment-only reading of
§ 3.9 misses, and it is why rotation needs § 2.1's slug setter too.

`planFrom()` gives each archive source a prefix taken from its own filename —
`ctx.prefix = "<M>-<N>"` — and `walkSource()` writes
`s.slug = ctx.prefix + "-" + uniqueSlug(seen, slugifyHeading(title))`, with
`seen` scoped to **one source's slug namespace**. So the same heading carries
an un-prefixed slug while it lives in `ROADMAP.md` and a `0-7-…` one once it
lives in `docs/roadmap/0.7.md`. Rotation that reassigned `source_path` and left
the slug alone would leave the store holding a slug no re-import derives:
`findSection()` misses, `RoadmapMigrateLoad` calls `addSection()`, and a
**second** section appears beside the first — INV-11's silent duplicate,
reached through rotation instead of retitle. Nothing deletes the original,
because the loader retains a section the plan stopped carrying.

**The rule, and it is a whole-set computation rather than a per-section one.**
For the move set in `sectionOrderLess()` order, rotation assigns each section
`<major>-<minor>-` + `uniqueSlug()` over that set alone, seeded empty — which
reproduces exactly what the next import's walk of that archive will derive,
because the archive will contain exactly this set in exactly this order. A
section's new slug therefore cannot be computed in isolation: it depends on
which of its siblings move with it, which is why the mutation takes the
selection as a whole.

**The live side frees the slugs it gives up, and that is refused, not
absorbed** — the same hazard § 2.3 states for retitle, arriving from the other
direction. If removing the move set would leave any **remaining** live section
holding a `uniqueSlug()`-disambiguated slug whose un-suffixed base the move
frees, the next import re-derives a different slug for a section rotation never
touched. Rotation refuses with `bad_args`, naming the slugs, and writes
nothing. INV-13 tests both halves.

**A rotation must not empty the live file.** The render assembles content only
for paths that still have sections — a file with none never enters `byFile` and
is never written, so it would be left on disk with its old content while the
store said otherwise. `rotate_minor` therefore refuses with `bad_args` when the
move set would leave `liveRoadmapPath` with zero sections. Reachable only on a
project whose every section is one closed minor, which is not this one, but
INV-1 asserts bullets end up *out of* `ROADMAP.md` and that assertion would
otherwise be false in exactly that case.

### 2.3 `roadmap_log op:"retitle_section"`

```jsonc
{ "op": "retitle_section",
  "caller_cwd": "<project root>",
  "section": "0-7-92-indie-review-4-ants-mcp-roadmap-pass",   // slug
  "title": "0.7.92 — indie-review #4 + Ants MCP roadmap pass — shipped (2026-08-09)",
  "dry_run": true }
```

Locates by slug through `RoadmapStore::findSection()`, exactly as
`create_section` and `bundle_row` already do (ANTS-3809 § 2.2), then calls
`updateSection()` with the new title and the section's existing level, position
and parent — the setter takes the whole tuple and a partial update has no
meaning, which is its own comment's rule.

**The slug IS recomputed from the new title, and the store gains a setter for
it.** Decided 2026-08-09 (§ 2.5) after cold-eyes loop 3 surfaced the question.

A section's slug is **derived, not round-tripped**: the render emits
`## <title>` and never the slug, and the import re-derives it with
`RoadmapIndex::slugifyHeading()` / `uniqueSlug()`. ANTS-4065 § 2.6's governed
set carries no section slug either. So *keeping* the stored slug does not keep
it stable — it makes the store disagree with what any re-import derives, and
because `RoadmapMigrateLoad` § 2.6 resolves sections through
`findSection(projectId, slug)`, that re-import would miss and **add a second
section rather than update the first**. Re-slugging is what keeps the store
equal to its own regeneration.

The cost is re-addressing, and it is smaller than it looks: the slug has always
been a function of the heading, so every heading edit has always moved it. What
was stable was that nothing edited headings.

Three consequences, each load-bearing:

- **The slug setter § 2.1 adds is what makes this possible** —
  `setSectionSlug(sectionId, slug)`, or `slug` added to `updateSection()`'s
  tuple. **Rotation needs the same primitive for the same reason** (§ 2.2): it
  changes the other input the slug is derived from. Neither operation is the
  exceptional one.
- **A collision is refused, never auto-suffixed, and the check runs in BOTH
  directions.** `section` is `UNIQUE (project_id, slug)`, and `uniqueSlug()`
  disambiguates in *document order* — so a suffix invented here could differ from
  the one a re-import assigns, which is the divergence this decision exists to
  remove. Two conditions, each returning `bad_args` naming the slugs involved and
  writing nothing:
  - **Forward** — `slugifyHeading(title)` already belongs to another section.
  - **Backward** — the retitled section is the **un-suffixed head of a family
    another section was disambiguated out of.** Precisely: its stored slug
    equals `slugifyHeading(its own title)`, **and** some other section's stored
    slug differs from `slugifyHeading(that section's own title)` while that
    section's own `slugifyHeading()` equals the retitled section's current slug.
    Retitling the head **frees** the base, so the next import hands it to the
    sibling — whose stored slug is the `-2` form — and INV-11 fails on a section
    this operation never touched.
    **The scope is the retitled section's own family and nothing wider.**
    Retitling the *suffixed* member is safe and must succeed: the head keeps
    deriving the base, and the member derives whatever its new heading gives.
    A condition phrased over "any section anywhere holds a disambiguated slug"
    would refuse every retitle on any project containing one repeated heading —
    and `uniqueSlug()`'s own comment uses repeated `Performance` headings as its
    worked example, so on this corpus that reading refuses
    `roadmap-format.md` § 4.3 step 4 outright, which is the operation's whole
    purpose. INV-12 tests the head (refused), the member (succeeds) and an
    unrelated section on a project that holds such a family (succeeds).
- **The rendered section order can change.** `sectionOrderLess()` sorts on
  `(position, slug)` with the slug as tiebreak, so retitling a section that
  shares a position with a sibling can reorder the two in the output file. That
  is the render's existing rule applied to a new input, not a new rule.

The envelope reports `slug` (the new one) and `previous_slug`, so a caller
holding the old address learns it moved rather than discovering it at the next
call that fails.

### 2.4 Guards, and what a dry run shows

**Five CALLER-SIDE refusal CODES over six conditions, checked before the
mutation**, each returning the canonical envelope shape
(`mcp-error-codes.md`). They are additional to whatever the seam refuses — see
the inherited set below. **The `bad_args` row carries the slug-collision checks
of §§ 2.2 and 2.3**; a guard list built from this table alone would otherwise
ship without them and hit `UNIQUE (project_id, slug)` at write time instead of
refusing cleanly.

| Condition | Code | Status in the taxonomy |
|---|---|---|
| `minor` is not `<MAJOR>.<MINOR>`; the derived filename fails § 3.9's regex; the move set would leave `liveRoadmapPath` with zero sections; or `retitle_section`'s `title` is empty, whitespace-only, contains a newline, or **slugifies to the empty string** | `bad_args` | existing |
| **A slug collision** — `retitle_section`'s forward or backward check (§ 2.3), or `rotate_minor` freeing a base a remaining live section was disambiguated out of (§ 2.2) | `bad_args` | existing; the envelope names the slugs involved |
| **No section's TITLE matches the minor** per § 2.2 (`rotate_minor`), or `retitle_section`'s `section` slug does not resolve through `findSection()` | `section_not_found` | existing; already what `roadmap_log` emits for an unresolvable `section` slug, though `mcp-error-codes.md`'s entry documents only `read_region`'s use (§ 7) |
| A required argument is wholly absent — `minor`, or `retitle_section`'s `section` / `title` | `missing_field` | existing; `mcp-error-codes.md` makes it the absent-arg specialisation of `bad_args`, and the pervasive `roadmap_log` field-guard code |
| **Any section in the move set** — a matched section *or a descendant* — still holds an **open** item | `minor_not_closed` | **new** (§ 7) |
| The project is not store-migrated | `op_unsupported` | **new** (§ 7) |

**"Empty" is tested on the SLUG, not only on the title, because
`slugifyHeading()` keeps only letters and digits.** A title of `———` or `!!!`
is neither empty nor whitespace-only and passes a naive guard, then slugifies
to `""` — an unaddressable section, stored in a `UNIQUE (project_id, slug)`
column where a second one violates the constraint and surfaces as
`store_failed` rather than a caller-side refusal. INV-9's *breaks when* clause
already names that outcome ("a heading with no text to slug, which the
migration cannot read back and no later op can address"); the guard has to
actually exclude it. `uniqueSlug()` does not save this case — it returns an
empty base **without** inserting it into `seen`, so it never disambiguates one
empty slug from another.

**The openness guard is scoped to the whole MOVE SET, not to the matched
sections.** § 2.2 moves every descendant with its parent, so a guard reading only
the `level == 2` matches would archive an in-progress item living under a `###`
child while reporting success. The set the guard tests and the set the mutation
writes are the same set, by construction.

**`section_not_found` is keyed on the TITLE match, not on the set that survives
filtering**, and the distinction is the whole reason the row says so. § 2.2 skips
sections whose `sourcePath` already equals the derived archive path, so on a
re-run every matched section is skipped and the move set is empty — which is a
**success envelope with
`sections_moved: 0`**, never a refusal. Reading it the other way makes the second
of two identical calls fail, and `/bump` retrying a step is ordinary.

**"Open" is `roadmap-data-model.md` § 3.4's sense — 📋 planned, 🚧 in-progress
**and** 💭 considered** — which is exactly what `RoadmapRender::isOpen()`
implements. An earlier draft of this row said "📋 or 🚧", inventing a narrower
notion of closed than the codebase's own; a minor holding only 💭 items would
have passed the guard and archived work nobody has committed to.

**Four further codes are inherited from the seam and are reachable here**, so the
five above are not the whole refusal set. `RoadmapWrite::Result` maps them
(`src/roadmapwrite.h`): `GateUnmet → render_gate_unmet`,
`RenderFailed → render_failed`, `StoreFailed → store_failed`,
`PublishFailed → write_failed`. **`render_gate_unmet` is the one to design
around**: it fires when the project holds a public open item with no `Layman:`
line, and the gate is **per project**, so an offending item under the *open*
minor blocks rotating a *closed* one. Nothing is written when it fires. § 6's
fixtures must therefore give every open bullet a `Layman:` line, or INV-1 and
INV-3 pass and fail for reasons that have nothing to do with rotation.

**`op_unsupported` exists because the alternative is silence.** Verified
2026-08-09: on a project that is not migrated, `roadmapWriteTarget()` returns
`nullopt` with `ReadError::None`, and `rcRoadmapSourceRefused()` returns false —
so the seam does not refuse, it **falls through to the op's markdown path**. The
existing ops all have one. These two do not, and without an explicit refusal the
fall-through reaches an op with nothing behind it. The seam's three real codes
(`too_large`, `read_failed`, `unrecognised_format`) all describe a *migrated*
project whose read failed, so none of them fits; `locator_unsupported` is the
nearest precedent in spirit — valid request, this backend cannot serve it — and
is per-locator by definition, which these are not.

**`minor_not_closed` is derived from the data rather than from a version
string.** The store does not know the project's version, and asking it to read
`CMakeLists.txt` would give a migration engine an opinion about a build file. The
guard tests exactly one thing: nothing in the move set is still open. That makes
it testable without a fixture version file.

**It is NOT § 3.9's rotation-event rule, and this op deliberately does not
enforce that rule.** § 3.9 says rotation happens "at `/bump` time on a minor or
major bump only", and that "every section under the open minor stays put". A
minor can hold zero open bullets while still being the current one — routinely,
just after a patch release and before the next item is filed — so this guard
would let `rotate_minor 0.7` succeed today on a project at `VERSION 0.7.104` and
archive the minor it is still shipping from.

**`/bump` owns that check, because `/bump` is the only caller that knows a minor
just closed.** It is the release step; the version transition is its input, not
something a store operation can infer. Stating the split rather than silently
leaving the hole is the point — an implementer reading only § 2.4 would
otherwise believe the op is safe to call with any minor. § 5's deferred `/bump`
wiring is therefore not merely plumbing: **until it lands, nothing enforces
§ 3.9's rotation event**, and the operation must not be exposed as a routine
verb before it does.

This is also what § 1's "nothing eligible to move today" rests on: it holds
because 0.7 is open *and* holds open bullets. The first clause is the one that
matters, and it is `/bump`'s to check.

**The dry-run envelope, with its fields named.** `dry_run: true` writes nothing
and returns what the real run would, plus — for `rotate_minor` — three fields:

- **`archive_path`** — string, the derived path relative to the project root.
- **`sections`** — an array of section slug strings listing **exactly the
  sections that are (or would be) reassigned**: matched sections *and* their
  descendants, minus any whose `sourcePath` already **equals the derived archive
  path**. Equality, not "already archived" — § 2.2 owns why. Ordered by the
  render's own document order, `sectionOrderLess()`'s `(position, slug)`, so two
  builds cannot disagree about the sequence. **The slugs are the NEW,
  prefixed ones** — the address each section carries *after* the move, on the
  same reasoning `retitle_section` reports the new `slug` rather than the old:
  once the call returns, every pre-move slug is a dead address. The dry run
  reports the same strings, which is what lets INV-8 compare the two.
- **`sections_moved`** — integer, and **always `sections.length`**. It is stated
  rather than left implied because the two obvious readings differ: a count of
  matched `##` sections would exclude descendants, and on the idempotent re-run
  a count of *matched* sections would be non-zero while nothing moved.

On a re-run, therefore, `sections` is `[]` and `sections_moved` is `0`. The real
run returns the same three fields, so a caller can compare them directly; INV-8
does exactly that. Naming them is not pedantry — § 2.2's selection is an
inference over titles, and a caller must be able to see what it inferred before
committing to it.

**`retitle_section`'s dry run returns the same envelope its real run does** —
`slug` (the new one), `previous_slug` and `title` — and no `sections` /
`sections_moved` / `archive_path`, there being no set to preview. Saying it
returns "the resolved section slug and nothing else" would contradict both
INV-8 and § 2.3, and would withhold from the preview the single field a caller
most needs to see before committing: the address its section is about to move
to. INV-8 asserts the dry and real envelopes match for **both** operations.

### 2.5 Scope decisions (agreed with the user, 2026-08-09)

- **Mechanism only.** Presented with the measurement in § 1, the user chose to
  build § 3.9's rotation as the standard defines it and to split the size
  question out rather than widen rotation to trim inside an open minor. The
  rejected option and its cost are in § 8.
- **Both operations in one spec**, because § 4.3 already names this item as the
  owner of the section-title write and both are section-scoped callers over the
  same seam.
- **A retitle re-slugs** (§ 2.3). Surfaced by cold-eyes loop 3 as an open
  decision and returned to me to make; the user delegated it rather than picking
  an option. The two rejected alternatives are in § 8, and the reason this one
  wins is that the other two fail on their own terms — refusing a slug-changing
  retitle would refuse `roadmap-format.md` § 4.3 step 4, which is the operation's
  whole purpose, and rendering the slug into the file would put a machine
  artefact into a document § 3.5 governs for humans, which ANTS-4065 § 2.4
  already rejected for the defaulted-`Kind:` marker.

## 3. Invariants

- **INV-1** — A rotated minor's sections render into
  `docs/roadmap/<M>.<N>.md`, and out of `ROADMAP.md`, with their content
  unchanged. *Test:* `tests/features/roadmap_rotate_minor/` — migrate a
  two-minor fixture, rotate the closed one, assert every bullet id under it
  appears in the archive and none in the live file, and that each bullet's
  rendered text is byte-identical to its pre-rotation text. *Breaks when:* the
  mutation writes the path but the render is not re-run, which leaves the store
  correct and both files stale — the exact failure mode a hand edit has today,
  reintroduced from the other side.
- **INV-2** — Rotation is idempotent, and the second run **succeeds**. *Test:*
  rotate twice; assert the second run returns `ok` with `sections_moved: 0` and
  `sections: []` — **not** `section_not_found` — and that the two renders are
  byte-identical. Then rotate a minor no title matches at all and assert that
  **does** return `section_not_found`, so the two empty cases are shown to be
  distinguishable rather than assumed to be. *Breaks when:* the emptiness check
  runs over the set that survives the already-archived skip rather than over the
  title match, which turns every retry into a refusal.
- **INV-3** — A minor holding an open item is refused, and nothing is written.
  *Test:* four fixtures — a closed-minor section carrying one 📋, one 🚧 and one
  💭 bullet respectively, plus a fourth whose `##` is entirely shipped and whose
  `###` **child** carries a 🚧; each asserts `code == "minor_not_closed"` and
  that both files are byte-identical to before the call. *Breaks when:* the
  guard is written as a warning, runs after the mutation rather than before it,
  omits 💭 — which `RoadmapRender::isOpen()` counts as open and an earlier draft
  of § 2.4 did not — or tests only the matched `level == 2` sections, which
  archives an in-progress item that lives one level down.
- **INV-4** — The § 2.2 match admits only a release designator. *Test:* one
  fixture carrying `## 0.7.0 — …`, `## 0.70.0 — …`, `## 0.7 — …` and
  `## 0.5.x and 0.6.x — archived`; rotate `0.7` and assert the first and third
  move and the second does not, then rotate `0.5` and assert the signpost does
  not. *Breaks when:* the title match is a plain `startsWith("0.7")` — which
  claims `0.70.0` — or accepts any non-digit after the prefix, which claims the
  `0.5.x` signpost and files a pointer inside the archive it points at.
- **INV-5** — Every descendant of a rotated section moves with it, **including
  one misfiled under a different archive**. *Test:* fixture with a `###` under
  the closed minor's `##`, plus a second `###` whose `sourcePath` is
  `docs/roadmap/0.6.md`; rotate `0.7` and assert **all three** sections carry
  `docs/roadmap/0.7.md` afterwards. *Breaks when:* selection filters on
  `level == 2` and stops there, leaving children pointing at the live file — or
  skips on *any* non-null `sourcePath` rather than on equality with the derived
  path, which strands the misfiled child in `0.6.md` while its parent renders
  into `0.7.md`.
- **INV-6** — The archive path is derived, **project-root-relative**, and
  § 3.9-conforming; and a rotation never empties the live file. *Test:*
  `rotate_minor` with `minor: "v0.7"`, `"0.7.0"` and `"00.7"` each return
  `bad_args`; the accepted `"0.7"` produces exactly `docs/roadmap/0.7.md`, and
  that string is asserted to satisfy
  `\Adocs/roadmap/(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md\z` — the same predicate
  `RoadmapMigrateLoad::isPlaceableSourcePath()` applies, so the assertion is
  against the rule that actually gates a re-import rather than against a
  restatement of it; and a fixture whose every section belongs to the rotated
  minor returns `bad_args` with both files unchanged. **That fixture must begin
  on its `##` line, with no preamble** — content before the first heading
  becomes a level-0 preamble section which stays on the live path, so a fixture
  carrying one can never reach zero live sections and the clause would be
  unreachable (§ 6). *Breaks when:* the path is
  taken from the caller, built by string concatenation without re-validating the
  filename, anchored on `dir(liveRoadmapPath)` rather than the project root —
  which for a roadmap outside the root yields a path the loader refuses
  `source_unplaceable` and discovery never enumerates — or allowed to leave the
  live file with no sections, which the render then never rewrites, stranding
  its old content on disk.
- **INV-7** — `retitle_section` changes the title and the slug, and nothing
  else. *Test:* retitle a section, then assert its level, position, parent and
  every item filed under it are unchanged; that the rendered heading carries the
  new text; that the stored slug now equals `slugifyHeading(newTitle)`; and that
  the envelope reports both `slug` and `previous_slug`. *Breaks when:* the
  section's items are re-filed or its parent link is dropped — `updateSection()`
  takes the whole tuple, so a caller passing a default for a field it did not
  mean to change silently rewrites it.
- **INV-8** — A dry run writes nothing and reports what a real run would do,
  **for both operations**. *Test:* two legs. `rotate_minor` with
  `dry_run: true`; assert both files are byte-identical afterwards and that the
  envelope's `sections`, `sections_moved` and `archive_path` equal the three the
  real run then returns. Then `retitle_section` with `dry_run: true`; assert
  both files unchanged and that its `slug`, `previous_slug` and `title` equal
  the three its real run then returns. The second leg is the one that catches a
  retitle dry run built to return only what it was asked, since the new `slug`
  is computed rather than passed. *Breaks
  when:* the dry run is implemented as "do it and roll back the transaction"
  but the render has already published the files — the render is outside the
  transaction, which is why `commitAndRender` takes `dryRun` rather than the
  caller wrapping it.
- **INV-9** — `retitle_section`'s arguments are validated, and each failure has
  one code a caller can branch on. *Test:* `""`, `"   "`, a value containing
  a newline, **and a punctuation-only title such as `"———"`** each return
  `bad_args`; a `section` slug that does not resolve returns
  `section_not_found`; a wholly absent `section` or `title` returns
  `missing_field`; and the section's stored title is unchanged after every one.
  The punctuation case is the one a guard written against the *title* passes and
  a guard written against `slugifyHeading(title)` catches.
  *Breaks when:* the title is passed straight through to
  `updateSection()`, so the next render emits a bare `##` — a heading with no
  text to slug, which the migration cannot read back and no later op can
  address.
- **INV-10** — The publish gate is inherited, not bypassed. *Test:* fixture
  whose **open** minor holds a public item with no `Layman:` line; rotate the
  closed minor and assert `code == "render_gate_unmet"` with both files
  byte-identical to before, even though the offending item is in a different
  minor. *Breaks when:* the op reports success off its own mutation without
  consulting the render's outcome — the store then says rotated while both
  files say otherwise.
- **INV-11** — A retitle leaves the store equal to its own regeneration. *Test:*
  retitle, render, re-import, and assert the project has the **same number of
  sections** as before and that the retitled section's stored `section_id` is
  unchanged. *Breaks when:* the slug is left at its old value — the re-import
  derives a new one from the rendered heading, `findSection()` misses, and a
  **second** section appears beside the first. This is the invariant the whole
  § 2.3 decision exists to satisfy, and the only one that fails silently: both
  sections render, so the file merely grows a duplicate heading.
- **INV-12** — A retitle is refused when either direction of the slug-collision
  check fires. *Test:* two cases. **Forward** — two sections; retitle the first
  to a heading that slugifies to the second's slug; assert `bad_args`, both
  slugs unchanged, both files byte-identical. **Backward** — two sections whose
  headings slugify alike, so one holds `foo` and the other `foo-2`; retitle the
  `foo` one to something unrelated and assert `bad_args`, because succeeding
  would free `foo` and a re-import would then hand it to the `foo-2` section,
  changing the slug of a section this call never touched. **Two SAFE cases on
  that same fixture, both of which must succeed** — retitling the `foo-2`
  member, and retitling an unrelated third section. Without them the test
  passes against a check phrased over "any section anywhere holds a
  disambiguated slug", which refuses every retitle on any project containing
  one repeated heading. *Breaks when:* the implementation auto-suffixes with
  `uniqueSlug()` — which disambiguates in document order, so the suffix chosen
  here can differ from the one a re-import assigns — checks only the forward
  direction, which leaves INV-11 failing on an untouched sibling, or scopes the
  backward check wider than the retitled section's own family, which makes the
  operation refuse its own motivating use case.
- **INV-13** — A rotation leaves the store equal to its own regeneration. The
  rotation counterpart to INV-11, and the invariant this spec was missing while
  it claimed rotation needed no slug write. *Test:* rotate, render, re-import,
  and assert that every moved section's stored `section_id` is unchanged **and
  that no moved heading is carried by two sections**; then assert each
  moved section's stored slug equals `"<major>-<minor>-" +` the slug
  `RoadmapIndex::uniqueSlug()` derives for its heading within the move set
  alone. A **second leg** covers the live side: a fixture where a remaining
  live section holds a `uniqueSlug()`-disambiguated slug whose base the move
  would free asserts `bad_args` with both files unchanged. *Breaks when:*
  rotation is built as `setSectionSource()` alone — the importer prefixes every
  archive slug with its file's minor (§ 2.2), so the re-import derives a slug
  the store does not hold, `findSection()` misses and `addSection()` adds a
  **second** section beside the first while the loader retains the original.
  Like INV-11 it fails silently: both sections render, so the archive merely
  grows a duplicate heading.

## 4. RAM / build cost

**No new target, no new stored state, no new dependency.** Both operations are
callers in `src/remotecontrol_roadmap_log.cpp`, which already links everything
they need, plus one method on `RoadmapStore` (§ 2.1) that writes an existing
column; the store gains no column and the render gains no path. Peak memory
is one render of the project, which `RoadmapWrite::commitAndRender()` already
performs on every existing `roadmap_log` write — rotation makes it no larger.

The one figure worth stating is the rotation that eventually runs here: minor
0.7 currently holds 3,029,307 bytes across its sections, and rotating it moves
that from `ROADMAP.md` to `docs/roadmap/0.7.md` in a single render. Both files
are written whole, as every render already writes them, so the transient cost is
the same one the project pays on each roadmap write today.

## 5. Out of scope

- **Shrinking the current 3.2 MB** — ANTS-4078 carries it, with the measurement
  that separates the two. Deferred, not excluded.
- **Rotating inside an open minor** — § 3.9 forbids it deliberately and the user
  declined to reverse that (§ 2.5). Changing it is an amendment to that
  standard, not to this spec. Permanent exclusion unless § 3.9 changes.
- **Wiring rotation into `/bump`** — this spec builds the operation and its
  guards; which release step calls it is `.claude/bump.json`'s recipe, and
  changing that is a separate edit with its own verification. Deferred; no id
  yet, because it is one line in a recipe once this ships. **It is not optional
  plumbing:** § 2.4 puts § 3.9's rotation-event rule — rotate only on a minor or
  major bump — on the caller, so until this lands nothing enforces it and the op
  should not be exposed as a routine verb.
- **A markdown fallback path.** On a non-migrated project rotation stays
  `/bump`'s snip-and-create, and these ops refuse with `op_unsupported` (§ 2.4)
  rather than growing a second implementation. Permanent exclusion: a markdown
  writer for an op that exists to move store rows would be two writers for one
  file, which is what ANTS-3809's no-second-writer rule forbids.

## 6. Tests

`tests/features/roadmap_rotate_minor/`, label `features;fast`, covering
INV-1 … INV-13. Behavioural against a real store in a `QTemporaryDir` — and per
the standing trap, `RoadmapStore` is constructed with an **explicit path**,
since the default resolves under `XDG_DATA_HOME` and would run the suite against
the live store.

The fixture is a two-minor project: one closed minor with a `##` heading, a
`###` child and several bullets, one open minor, and — for INV-4 — `## 0.70.0`,
`## 0.7` and `## 0.5.x and 0.6.x — archived` headings, none of which may be
claimed by the wrong call.

**The fixture's live roadmap sits directly in the project root**, because
`findRoadmaps()` accepts no other placement (§ 2.2) — a fixture at
`sub/ROADMAP.md` cannot be migrated at all, so a case built on one would fail
before reaching anything this spec governs.

**INV-11 and INV-13 need a real round trip, not a store assertion.** Both claim
the store equals its own regeneration, so both run render → re-import against
the same store. A test that asserted the expected slug directly would pass
against an implementation whose slug rule is self-consistent and disagrees with
the importer's — which is the exact defect INV-13 exists to catch.

**They do NOT compare section counts the same way, and only INV-11 may.** A
migrated file gains a **level-0 preamble section** for whatever precedes its
first `##` heading. A retitle creates no file, so INV-11's count is stable and
a change in it is the duplicate. A rotation creates the archive, whose preamble
section is therefore added by the **first** re-import of that new file — the
project legitimately gains one section, and a count cannot tell that from the
duplicate INV-13 is about. INV-13 asserts the duplicate directly instead: each
moved slug still resolves to the same `section_id`, and no moved heading is
carried by two sections. Found at implementation on 2026-08-10; the clause had
read "the same number of sections" and failed for a reason that was not a bug.

**Every open bullet in every fixture carries a `Layman:` line, except INV-10's,
which exists to omit one.** The publish gate is per project (§ 2.4), so a single
`Layman:`-less open bullet anywhere refuses the whole render — and a fixture
that trips it accidentally makes INV-1 and INV-3 pass or fail for a reason that
has nothing to do with rotation. INV-10 is the deliberate case, and it is why
the accident is worth guarding against.

Per the project test convention, **each case is verified to fail against
pre-change source first**; for this item that is the whole set, since neither
operation exists.

INV-1's byte-identical clause compares the *rendered bullet text* before and
after, not the source markdown, because the first render of a hand-written
fixture legitimately materialises trailers the source omitted — the settling
ANTS-4065 § 2.6 measures. Rotating after that first render is what makes the
comparison meaningful.

## 7. Cross-doc impact

- **`docs/standards/roadmap-format.md` § 3.9** — its store-migrated paragraph
  says rotation "is a store write" without naming one. Gains the op name and
  the derived-path rule once this ships.
- **`docs/standards/roadmap-format.md` § 4.3** — the step-4 paragraph says no
  store operation for changing a section title is defined and names this item.
  That sentence is replaced by the operation.
- **`docs/standards/roadmap-data-model.md` § 8** — its last bullet calls
  rotation "a piece of unbuilt code (ANTS-4070)". Same correction.
- **`docs/standards/mcp-error-codes.md`** — three edits, and the first two are
  the ones a reviewer should check hardest, since a spec that mints codes
  without adding them is how a taxonomy stops being one. **Add
  `minor_not_closed`** and **add `op_unsupported`** (§ 2.4 states both, with the
  evidence that no existing code fits). **Amend `section_not_found`**, whose
  entry describes only `read_region`'s section-mode although
  `roadmap_log`'s store path already emits it for an unresolvable `section`
  slug — a pre-existing gap this spec surfaces rather than creates.
- **`CHANGELOG.md`** — one `Added` entry; a project on the store gains the
  ability to archive a released version at all.
- **`CLAUDE.md`** — no change. It names subsystems, not verb ops; the catalogue
  is `tool_info {catalog:true}`.

## 8. Alternatives considered (and rejected)

- **Widen rotation to trim finished patch ranges inside an open minor.** Would
  actually move the 3 MB today. Rejected by the user on 2026-08-09: § 3.9
  forbids within-minor rotation on purpose, and reversing that is a change to
  the standard with a wider blast radius than this item. ANTS-4078 may reopen it
  with evidence.
- **Add a `RoadmapStore::rotateMinor()` primitive.** Rejected: both writes it
  would need already exist (§ 2.1), and a primitive that only wraps two setters
  puts selection logic — which is title parsing, a markdown concern — inside the
  store.
- **Take the archive path as an argument.** Rejected: it moves § 3.9's naming
  rule out of the one place that states it, and a non-conforming name renders a
  file the migration's own discovery then refuses to read.
- **Derive "closed" from `CMakeLists.txt`'s version.** Rejected: it gives the
  migration engine an opinion about a build file, and the roadmap already
  carries the answer in a form the guard can test (§ 2.4). Note this leaves
  § 3.9's rotation-event rule to `/bump` — § 2.4 says so explicitly rather than
  letting the gap stand unremarked.
- **Keep the stored slug on a retitle, and render it so it survives the round
  trip.** Rejected: the render is `roadmap-format.md` § 3.5's human-facing file,
  and ANTS-4065 § 2.4 rejected the same shape when it considered marking a
  defaulted `Kind:` for the parser. One machine artefact admitted here makes the
  next one an argument about degree.
- **Refuse any retitle that would change the derived slug.** Rejected: it
  refuses `roadmap-format.md` § 4.3 step 4 — `(target: YYYY-MM)` →
  `shipped (YYYY-MM-DD)` — which is the reason the operation exists. An
  operation that cannot perform its own motivating use case is not a
  conservative choice; it is a broken one.

## 9. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 … INV-13 | `tests/features/roadmap_rotate_minor/` |
| § 2.2 derived path is project-root-relative and conforms to § 3.9's regex | INV-6, asserting against `isPlaceableSourcePath()`'s own predicate |
| § 2.2 the three-case title match | INV-4 |
| § 2.2 a moved section is re-slugged, and the store equals its own regeneration | INV-13 |
| § 2.2 the live file is never emptied | INV-6's `bad_args` set, extended with the zero-sections case |
| § 2.3 the slug is recomputed, and only the title and slug change | INV-7 |
| § 2.3 the store equals its own regeneration after a retitle | INV-11 |
| § 2.3 a collision is refused in both directions, and no wider than the retitled section's family | INV-12, whose two safe cases are what pin the scope |
| § 2.4 a dry run equals the real run, for both operations | INV-8 |
| § 2.4 the five refusal codes over six conditions | INV-3 (`minor_not_closed`), INV-6 (`bad_args`), INV-9 (`bad_args`, `section_not_found`, `missing_field`), INV-2 (`section_not_found` on a title that matches nothing), INV-12 / INV-13 (`bad_args` on a collision); `op_unsupported` **nothing** — it needs a non-migrated fixture, which this suite has no harness for |
| § 2.4 the four inherited seam codes | INV-10 covers `render_gate_unmet`; `render_failed` / `store_failed` / `write_failed` **nothing** — they are ANTS-3809's to test and it does |
| § 2.1 no second markdown writer is added | **nothing here** — ANTS-3809's no-second-writer rule owns it, and its source scrape already covers `src/` |
| § 2.4 § 3.9's rotation-event rule (rotate only on a minor/major bump) | **nothing** — deliberately delegated to `/bump` (§ 5), and nothing in this spec's code can observe a version transition |
| § 2.1 the new `setSectionSlug` primitive existing | **nothing** — a compile failure, not a test; it is the one store addition this spec makes |
| § 5's `/bump` wiring landing at all | **nothing** — no test can assert a recipe step exists until it does; tracked by the deferred line in § 5 |

Fifteen rows, **six** carrying a bolded `nothing` (two of them partial, naming
what *is* covered beside what is not). The uncovered budget did not grow when
loop 4 found rotation needs the slug write: that finding **closed** coverage
gaps rather than opening them, adding INV-13 and two rows and leaving the six
`nothing` rows exactly as they were. The `setSectionSlug` row is no longer
retitle's alone — it is the one store addition both operations rest on.

## Cold-eyes loop log

**This document did not converge, and loop 4 changed the diagnosis of why.**
Five passes returned 8 → 5 → 4 → 2 → 6 verified findings. The count going back
up is the signal: the earlier reading — that rotation had settled by loop 2 and
only `retitle_section` was still costing rounds — was **false**, and loop 4
falsified it with two Q1 defects in rotation, one of them introduced by loop 3's
own fix.

**What the passes were actually producing is the finding worth keeping.** Loop
4's two cold lanes returned four findings between them, every one of them the
document disagreeing with itself. The two that changed the design came from the
orchestrator resolving the lanes' *open questions* against source, and they were
the same defect twice: the spec reasoned about `docs/roadmap/` from an imagined
model of the importer and never opened `roadmapmigrate.cpp`. One read of that
file's archive path yields both; no number of cold document reads yields either.

**So review is closed here, and not because a cap was reached.** Cold reading
had stopped being the instrument that finds things in this document. What
remains uncertain is behavioural — what slug a rotated section must carry — and
INV-13 answers it by measurement where a fifth lane could only argue. The
`retitle_section` split is **not** taken: the premise it rested on is gone, both
operations now rest on the same new primitive (§ 2.1), and the two Q1s were in
the half the split would have shipped first.

| Loop | Date | Lanes | Findings | Outcome |
|------|------|-------|----------|---------|
| 4-impl | 2026-08-10 | **None — no reviewer was dispatched.** These are fold-backs from building the thing, per `/write-spec` Step 8 | 3 amended, all found by a test failing for a reason that was not a bug | **A cold reader has no compiler, and all three of these needed one.** (1) INV-13 asserted the project keeps "the **same number of sections**" across rotate → render → re-import. It does not, and correctly so: a migrated file gains a **level-0 preamble section** for whatever precedes its first `##`, and a rotation *creates* the archive, so that file's preamble is added by the first re-import of it. The count is off by one for a reason INV-13 is not about, and it cannot tell that from the duplicate it *is* about — so the clause now asserts the duplicate directly (same `section_id`, no heading carried twice). INV-11 keeps its count: a retitle creates no file. (2) INV-6's zero-live-sections leg was **unreachable as written** — the same preamble section stays on the live path, so a fixture built the ordinary way can never reach zero and the clause tested nothing; the fixture must begin on its `##` line. (3) § 2.4 never said whether `sections` reports the pre- or post-move slugs, and a builder needs one: it reports the **new** ones, matching `retitle_section`'s `slug`. **Not re-gated.** Rule 14 would send an amendment back through `/cold-eyes`; the user closed review on this document at loop 4 on the evidence that cold reading had stopped finding things in it, and every one of these three came from running the code instead. Flagged rather than skipped silently. |
| 4 | 2026-08-10 | 2, cold — same shared packet, rebuilt from disk; no mention of loops 1–3 | Q1 2 · Q2 2 · Q3 2 — verified 6, dismissed 1 | **Run because loop 3-impl had closed with 2 verified findings, which under the convergence rule cannot be a final pass.** Both lanes again led on the same defect — § 2.3's backward collision check was phrased project-wide ("any other section") while its rationale was family-scoped, so a literal build refuses every retitle on any project holding one repeated heading, which is `roadmap-format.md` § 4.3 step 4, the operation's whole purpose. **The two that mattered came from verifying the lanes' open questions, not from the lanes.** § 2.2's archive path was anchored on `dir(liveRoadmapPath)` — loop 3's own fix — on the stated grounds that "the migration's archive discovery looks beside the live file". It does not: `findRoadmaps()` builds `QDir(projectRoot)` and reads `<projectRoot>/docs/roadmap`, and `isPlaceableSourcePath()` refuses any project-root-relative path outside `docs/roadmap/<M>.<N>.md` with `source_unplaceable`. INV-6 had been written to assert the refused path. And `planFrom()` prefixes every archive slug with its file's minor (`ctx.prefix`), so rotation reassigning `source_path` without re-slugging makes the next re-import miss on `findSection()` and **add a second section** — INV-11's silent duplicate, reached through rotation, which the document claimed needed no slug write at all. § 2.1's headline was false in both halves. Also fixed: the retitle dry run said it returned "nothing else" against INV-8; § 2.4 counted five caller-side refusals and omitted the collision check; a punctuation-only title passed the empty-title guard and slugifies to `""`. **1 dismissed** — a citation naming "`RoadmapMigrateLoad` § 2.6" whose stated behaviour is correct, so it changes nothing built. INV-13 added. 624 → 700 lines. |
| 3-impl | 2026-08-09 | 1, cold — **re-gate of the § 2.5 slug amendment**, per `/write-spec` Step 8, after the user delegated loop 3's surfaced decision back to me. Not a review loop of the original run, which had already exited at its cap | Q2 1 · Q3 1 — verified 2, fixed 2 | **Both were collateral from the amendment and from loop 3's own fixes**, which is the pattern this run never escaped. Loop 3 changed § 2.2's skip rule to *equality with the derived archive path*, and left two restatements in § 2.4 at the old "already carrying an archive path" — under which a `0.7` section misfiled in `0.6.md` is stranded rather than reassigned; INV-5 gained that case. The amendment's collision check was one-directional: retitling *out of* a `uniqueSlug()`-disambiguated family **frees** the un-suffixed slug, so a re-import re-derives a different slug for a section the call never touched, and INV-11 then fails on the untouched sibling. Both directions now checked, INV-12 tests both. Three Open questions resolved as no-defect: `RoadmapMigrateLoad` does resolve by `findSection(projectId, slug)` and *retains* a section the plan stopped carrying — confirming INV-11's duplicate rather than a merge; INV-10's fixture is buildable because `load()` does not render; and `RoadmapWrite::Result` is a distinct enum from the seam's. **Not converged, and stopping here on purpose** — see the note below. 537 → 624 lines. |
| 3 | 2026-08-09 | 2, cold — same shared packet, rebuilt from disk; no mention of loops 1–2 | Q2 2 · Q3 2 — verified 4, fixed 4; **1 surfaced, not fixed** | **Exited at the `--max-loops` cap with the tail empty but one open decision.** Both lanes again led on the same defect, the third loop running: § 2.4 defined a closed minor as "nothing under it is still open", which contradicts § 3.9's "rotation happens at `/bump` time on a minor or major bump only". A minor holds zero open bullets routinely — just after a patch release — so the guard would have let `rotate_minor 0.7` archive the minor this project is still shipping from, and § 8 had already rejected the version check that would distinguish them. Resolved by stating that the op deliberately does not enforce the rotation-event rule and `/bump` owns it, which also promotes § 5's deferred wiring from plumbing to a precondition. Three more: the archive path was anchored on the project root while § 3.9 anchors it beside `ROADMAP.md` (they differ whenever `.ants/project.json` moves the roadmap, and the migration would never rediscover the file); "already carrying an archive path" was ambiguous between *any* path and *the derived* one; and the render never rewrites a file left with zero sections, so a rotation that emptied the live file would strand its old content while INV-1 claimed otherwise. **Surfaced, not fixed:** a section's slug is derived from its heading, not round-tripped, so `retitle_section` diverges the stored slug from any re-import's — three candidate answers with different costs, so it is the user's call, and until it is answered the op is unsafe on a project facing ANTS-4065 Phase D. 463 → 537 lines. |
| 2 | 2026-08-09 | 2, cold — same shared packet, rebuilt from disk after loop 1's edits; no mention of loop 1's findings or fixes | Q1 1 · Q2 1 · Q3 3 — verified 5, dismissed 0 | **Both lanes again led on the same two, and the worst of them was loop 1's own collateral.** § 2.2's three-case rule, added by loop 1 to fix the `0.5.x` signpost, made case 2 "a non-alphanumeric character" — and `.` is non-alphanumeric, so case 2 swallowed case 3 and the rule re-admitted exactly what it was written to reject. Adding a disjunct cannot exclude anything. Restated as one expression, `^v?<major>\.<minor>(?:$\|[^0-9.]\|\.[0-9])`, with the three cases disjoint. Loop 1's corpus enumeration also omitted `## 0.7.7` of nine headings — re-derived from `grep -nE '^## 0\.7'` and the command recorded beside it. Three draft defects loop 1 had not reached: the openness guard was scoped to matched sections while § 2.2 moves descendants too (an in-progress item one level down would have been archived silently), `sections` / `sections_moved` left three readings open, and `retitle_section` had no refusal for an unresolvable slug or an absent argument. Both lanes' Open questions resolved as no-defect: the three seam codes are all in the taxonomy, `ANTS-3765 § 2.6` is "Re-run matching", and both ctest labels exist. 424 → 463 lines. |
| 1 | 2026-08-09 | 2, cold — one shared byte-stable packet: the scrubbed doc, bounded windows of `roadmapstore.h` / `roadmapwrite.h` / `roadmaprender.cpp`, the quoted passages of `roadmap-format.md` §§ 3.9/4.3, `roadmap-data-model.md` § 8, `ANTS-4065` § 2.6 and four `mcp-error-codes.md` entries, plus eleven verified source facts | Q1 1 · Q2 3 · Q3 4 — verified 8, dismissed 0. Plus **3 found while BUILDING the packet**, before a lane ran | **Both lanes independently led on the same two defects** — § 2.4's `section_not_found` row contradicting INV-2's idempotent re-run, and a bare "§ 2.4" in § 2.1 that resolves to this document instead of ANTS-3809. All 8 fixed. Two of the three packet-phase findings were mine minting error codes: `not_migrated` does not exist (the seam maps only `too_large` / `read_failed` / `unrecognised_format`, and on a non-migrated project does not refuse at all), and `minor_not_closed` was used without being filed. Both lanes also routed the same two items to Open questions rather than guessing, and both were real: `isOpen()` counts 💭 as open, so § 2.4's "📋 or 🚧" invented a narrower closed than the codebase's; and `## 0.5.x and 0.6.x — archived` — a 303-byte signpost — would have been claimed by `rotate_minor 0.5` and filed inside the archive it points at, which also falsified § 1's "moves zero bytes today". § 2.2's match became three explicit cases; INV-9 and INV-10 added. 308 → 424 lines. |
