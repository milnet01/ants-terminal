# ANTS-5315 — roadmap by SemVer release: handoff

Genre: record

Date: 2026-09-24. Written at a forced stop (terminal relaunch). Asked for by
claude-ab (v2 orchestrator) on the user's behalf.

## Where it stands

- **Built: nothing.** No code, no schema change, and no edit to the
  standard. The investigation below is the whole of the work.
- **No other project's roadmap has been written.** That rule stands: a
  dry run goes to claude-ab before any write to another project.
- **Authorisation:** build the schema, the verbs and the render (upgraded
  from "propose only"). The order is schema, then standard, then verbs and
  render, then migration dry runs, then writes. Ship the migration with a
  measured wrong-placement rate.

## Why it is urgent

Games_Hub adopted v2. Its `check-doc.py` milestone check reports "no
version headings, 193 unplaced", and that check gates. The store
re-renders `ROADMAP.md` on every write, so every roadmap commit there is
refused. Its sections today: `P01 — Shipped`, `P02 — Queued`,
`P03 — Considered`, with themed `###` subsections. Its
`.claude/workflow.json` declares `version_files`.

## What already exists — read before building

**The standard already decides most of the questions.** In
`~/.claude/draft/v2/standards/roadmap.md`, under "The roadmap is organised
by version" and the sections after it:

- The heading form is `## 0.1.0 — Theme`: no `v`, SemVer
  `major.minor.patch`.
- Versions order by SemVer, not by text: `1.10.0` follows `1.9.0`.
- 1.0.0 is where "a minor may break things" stops being true. That answers
  claude-ab's question 6.
- "A version heading is a section. Where a store serves the roadmap it
  needs nothing new to hold one."
- An explicit unscheduled group, named as such, not an empty value sorted
  last.
- Non-shipping work, never-completing work, and work gated on someone
  else's queue each take no version and get their own named group.
- Sections are a gate or a stream: "say which in a way a tool can read".
- A project without `version_files` groups by theme, not by version.

claude-ab asked that no third hand edit that file carelessly: it changed it
twice on 2026-09-24. Re-read it in full before adding anything.

**The assignment half already exists in the store verbs.** Do not add an
op.

- `roadmap_log op:"amend_field" field:"section"` moves an item, and takes
  `locators[]` to move several in one render (ANTS-4948).
- `create_section`, `move_section` and `delete_section` exist.
- Check the batch size limit before relying on one call for 193 items.

**The checker's milestone check** is `check_milestones` in
`~/.claude/draft/v2/tools/check-doc.py`. `VERSION_HEADING_RE` is
`^#{2,3}\s+v?(\d+\.\d+\.\d+)\b`. Any other heading resets `version` to
None, so every item under a named group counts as unplaced. The check also
never reads `version_files`. That file is claude-ab's; propose changes to
it, do not edit it.

## Decisions so far, and why

1. **No schema bump, as the leading option.** `kSchemaVersion` is a
   one-way door for every project on this machine: the first binary to
   upgrade locks every older build out (CLAUDE.md, ANTS-4462). Ask whether
   each field can be DERIVED first.
   - The version is the section title, parsed.
   - The theme is the title's suffix, or the section intro.
   - Released or not, and the release date, come from the git tag `vX.Y.Z`
     or `X.Y.Z`.
   - claude-ab argued for a version *entity* with a date, a state and a
     theme. My position: the section already is that entity (title,
     position, intro), and the rest derives from tags. **Say this to
     claude-ab before building. It is an open disagreement, not a settled
     point.**
2. **Pre-release suffixes are refused in headings.** `1.0.0-rc1` is a cut
   of release 1.0.0, not a release. This project already keeps `-rcN` at
   the tag only (CLAUDE.md, ANTS-1318).
3. **Non-version groups need a fixed, parseable leading word**, so the
   checker counts their items as placed. Proposal: `## Unscheduled — …`,
   `## No release — …` and `## Standing — …`. Gate against stream stays
   open. One option is a stream suffix such as `## 0.4.x — Patches`, where
   an `x` patch field marks a stream.
4. **Unplaced is a normal state.** Items go in `Unscheduled`, which the
   checker accepts. A project's own checker then cannot fail on the day it
   migrates. That is Games_Hub's way out: create `Unscheduled`, bulk-move
   with `amend_field`, and have the checker accept named groups.
5. **Numeric order is enforced where sections are made, and checked.** The
   store orders sections by stored position. When a SemVer title is created
   with no `after_section`, `create_section` should place it numerically.
   The checker, or a render-time test, reports version headings out of
   SemVer order. The required test case is `0.9.0` then `0.10.0`.
6. **Querying without a schema change:** add a `version:"0.3.0"` filter to
   `roadmap_query` that resolves the section by its parsed title. Unplaced
   items are `section=unscheduled`.
7. **Migration source: the CHANGELOG first, git tags second, measured.**
   - A CHANGELOG `## [X.Y.Z]` section citing an id is the project's own
     statement of what shipped where.
   - A tag is descriptive: what landed between two tags. The standard
     separates that from a heading's predictive meaning. Also, "work that
     ships in no artifact takes no version", and tags cannot tell the
     difference.
   - Measure how often tag-derived and CHANGELOG-derived placements agree,
     on Ants Terminal, which has both. The disagreement is the
     wrong-placement estimate claude-ab wants.
   - Open items are never placed; they go in `Unscheduled`.
   - Idempotent, never overwrites a human placement, dry run first:
     `backfill_dates` discipline.

## Next concrete step

1. Send claude-ab decision 1 (no schema bump, derive from sections and
   tags) and decision 3 (group grammar), and get a yes or no. Both change
   what gets built.
2. Then extend `create_section` with numeric placement, add the
   `roadmap_query` `version:` filter, and add a render-order test covering
   `0.9.0` then `0.10.0`.
3. Propose the `check_milestones` change to claude-ab: accept the three
   group words; read `version_files`.
4. Build the migration as a dry-run-first `roadmap_log` op (the
   `backfill_dates` shape). Run it on Ants Terminal first, then send
   per-project dry runs to claude-ab.

## Rulings (claude-ab, 2026-09-24, after this record was written)

- **Decision 1 accepted: no schema change.** The version is the section
  title. The theme, the release date and the released state derive from
  the title and the git tag.
- **Decision 3 accepted, and the checker half is built** on claude-ab's
  side. `check_milestones` counts an item as placed under `## Unscheduled`,
  `## No release`, `## Standing`, `## Backlog`, or a patch stream
  `## 0.4.x`. Games_Hub is no longer blocked.
- **Accepted:** the CHANGELOG as the primary migration source and tags
  second, measured against each other. Pre-release suffixes refused in
  headings.
- **Open:** whether `check_milestones` should read `version_files` (see
  below).
- **Do not build on these before the relaunch.** Next: step 2 of § Next
  concrete step. Step 3 is done.

**On `version_files`.** The standard says a project WITHOUT
`version_files` groups by theme and must carry no version headings. A
checker that never reads the declaration therefore demands version
headings of a project the standard tells not to have them. That project
fails unless it files everything under a group word. Reading the
declaration would skip the check for such a project, and would enforce
the standard's own breach: "a version heading in a project declaring no
version-bearing files".
