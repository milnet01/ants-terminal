# ANTS-4070 — move a closed minor's sections to its archive, and let a section be retitled

**Status:** spec draft (2026-08-09).
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
purpose. Minors 0.5 and 0.6 are already archived. So rotation built exactly to
the standard moves **zero bytes today** and is correct anyway: it runs the
moment 0.7 closes, and that bump then moves ~3 MB in one step. The size
question is ANTS-4078's; the user settled the split on 2026-08-09 (§ 2.5).

## 2. Surface

### 2.1 Neither operation needs a new store primitive

The single most useful thing found while sizing this: **both writes already
exist**, and the roadmap bullet's "build it as a store write" overstates the
work.

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
  and owns dry-render, the § 2.4 publish gate, the real render and the commit.

So each operation below is a **caller**: validation, a section selection, and a
`mutate` lambda over an existing setter. No new column, no new render path, and
no second markdown writer — which is ANTS-3809 INV-2's rule.

### 2.2 `roadmap_log op:"rotate_minor"`

```jsonc
{ "op": "rotate_minor",
  "caller_cwd": "<project root>",
  "minor": "0.7",            // <MAJOR>.<MINOR>, the minor that just closed
  "dry_run": true }          // optional; previews without writing
```

**The archive path is derived, never passed.** `docs/roadmap/<minor>.md`,
relative to the project root, and it is validated against § 3.9's own
case-sensitive regex `^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$` before anything
is written. Deriving it is what keeps the naming rule in one place; accepting a
path would let a caller write `docs/roadmap/v0.7.md`, which § 3.9 forbids and
which the migration's own archive discovery would then refuse to read back.

**Selection — which sections move.** A top-level (`level == 2`) section is in
the minor when its title, after an optional leading `v`, begins with
`<major>.<minor>` followed by a character that is **not** a digit. The
non-digit requirement is the whole rule: without it, rotating `0.7` also claims
`0.70`. Every descendant section moves with its parent, resolved through
`parentId`, because § 3.9 rotates "the closed minor's heading **and its
sub-headings**".

This matches the corpus as authored: `## 0.7.0 — …`, `## 0.7.12 — …` and
`## 0.7.50–0.7.59 — …` are all minor 0.7, and `## 0.8.0 — …` is not.

**Sections already carrying an archive path are skipped, not re-written.** A
re-run is therefore a no-op, which is what makes the operation safe to put in
`/bump` where a retry is ordinary.

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

**The slug is not recomputed from the new title.** A section's slug is its
address: `roadmap_log` ops, `roadmap_query` and every cross-reference key on it,
and re-slugging on a retitle would break all of them at once for a cosmetic
gain. § 4.3 step 4 is a heading edit, not a re-identification.

### 2.4 Guards, and what a dry run shows

Three refusals, each returning the canonical envelope shape
(`mcp-error-codes.md`):

| Condition | Code |
|---|---|
| `minor` is not `<MAJOR>.<MINOR>`, or the derived filename fails § 3.9's regex | `bad_args` |
| No section matches the minor | `section_not_found` |
| A matched section still holds an **open** item (📋 or 🚧) | `minor_not_closed` |

**`minor_not_closed` is the guard that matters, and it is derived from the data
rather than from a version string.** The store does not know the project's
version, and asking it to read `CMakeLists.txt` would give a migration engine an
opinion about a build file. "Closed" is instead exactly what it means on the
roadmap: nothing under that minor is still open. That also makes the guard
testable without a fixture version file.

`dry_run: true` returns the same envelope the real run would, plus the resolved
`archive_path` and the ordered list of section slugs that would move, and writes
nothing. The selection in § 2.2 is an inference over titles, so a caller must be
able to see what it inferred before it commits — this is that.

### 2.5 Scope decisions (agreed with the user, 2026-08-09)

- **Mechanism only.** Presented with the measurement in § 1, the user chose to
  build § 3.9's rotation as the standard defines it and to split the size
  question out rather than widen rotation to trim inside an open minor. The
  rejected option and its cost are in § 8.
- **Both operations in one spec**, because § 4.3 already names this item as the
  owner of the section-title write and both are section-scoped callers over the
  same seam.

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
- **INV-2** — Rotation is idempotent. *Test:* rotate twice; the second run
  reports zero sections moved and the two renders are byte-identical.
  *Breaks when:* the selection matches on title alone without skipping sections
  that already carry an archive path, so a re-run rewrites rows and bumps the
  outcome counters.
- **INV-3** — A minor holding an open item is refused, and nothing is written.
  *Test:* fixture whose closed-minor section carries one 📋 bullet; assert
  `code == "minor_not_closed"` and that both files are byte-identical to before
  the call. *Breaks when:* the guard is written as a warning, or runs after the
  mutation rather than before it.
- **INV-4** — `0.7` does not claim `0.70`. *Test:* fixture carrying
  `## 0.7.0 — …` and `## 0.70.0 — …`; rotate `0.7` and assert only the first
  moves. *Breaks when:* the title match is a plain `startsWith("0.7")`, which is
  the obvious implementation and is wrong.
- **INV-5** — Every descendant of a rotated section moves with it. *Test:*
  fixture with a `###` under the closed minor's `##`; assert both carry the
  archive path afterwards. *Breaks when:* selection filters on `level == 2` and
  stops there, leaving children pointing at the live file — which renders the
  parent heading into the archive and its content into `ROADMAP.md`.
- **INV-6** — The archive filename is derived and § 3.9-conforming. *Test:*
  `rotate_minor` with `minor: "v0.7"`, `"0.7.0"` and `"00.7"` each return
  `bad_args`; the accepted `"0.7"` produces `docs/roadmap/0.7.md`. *Breaks
  when:* the path is taken from the caller, or built by string concatenation
  without re-validating against the regex.
- **INV-7** — `retitle_section` changes the title and nothing else. *Test:*
  retitle a section, then assert its slug, level, position, parent and every
  item filed under it are unchanged, and that the rendered heading carries the
  new text. *Breaks when:* the slug is recomputed from the new title, which
  silently re-addresses every reference to that section.
- **INV-8** — A dry run writes nothing and reports what a real run would do.
  *Test:* `dry_run: true`; assert both files are byte-identical afterwards and
  the envelope's section list equals the list the real run then moves. *Breaks
  when:* the dry run is implemented as "do it and roll back the transaction"
  but the render has already published the files — the render is outside the
  transaction, which is why `commitAndRender` takes `dryRun` rather than the
  caller wrapping it.

## 4. RAM / build cost

**No new target, no new stored state, no new dependency.** Both operations are
callers in `src/remotecontrol_roadmap_log.cpp`, which already links everything
they need; the store gains no column and the render gains no path. Peak memory
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
  yet, because it is one line in a recipe once this ships.
- **A markdown fallback path.** On a non-migrated project rotation stays
  `/bump`'s snip-and-create and these ops refuse with the seam's existing
  `not_migrated` refusal. Permanent exclusion: two writers for one file is
  precisely what ANTS-3809 INV-2 forbids.

## 6. Tests

`tests/features/roadmap_rotate_minor/`, label `features;fast`, covering
INV-1 … INV-8. Behavioural against a real store in a `QTemporaryDir` — and per
the standing trap, `RoadmapStore` is constructed with an **explicit path**,
since the default resolves under `XDG_DATA_HOME` and would run the suite against
the live store.

The fixture is a two-minor project: one closed minor with a `##` heading, a
`###` child and several bullets, one open minor, and — for INV-4 — a `## 0.70.0`
heading that must not be claimed. Per the project test convention, **each case is
verified to fail against pre-change source first**; for this item that is the
whole set, since neither operation exists.

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
  carries the answer in a form the guard can test (§ 2.4).

## 9. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 … INV-8 | `tests/features/roadmap_rotate_minor/` |
| § 2.2 derived path conforms to § 3.9's regex | INV-6 |
| § 2.3 slug is not recomputed | INV-7 |
| § 2.1 no second markdown writer is added | **nothing here** — ANTS-3809 INV-2 owns it, and its source scrape already covers `src/` |
| § 5's `/bump` wiring landing at all | **nothing** — no test can assert a recipe step exists until it does; tracked by the deferred line in § 5 |

Five rows, **two** with a bolded `nothing`.

## Cold-eyes loop log

| Loop | Date | Lanes | Findings | Outcome |
|------|------|-------|----------|---------|
