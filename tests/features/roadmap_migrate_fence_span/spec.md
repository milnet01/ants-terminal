# ANTS-4403 — a multi-backtick inline span must not open a fence in the migration walk

## Contract

`RoadmapMigrate::planFrom()` walks each source line by line and must not read a
line at face value inside a fenced code block: a `##` in a fence is not a
heading, and a `- ✅ …` in a fence is not a bullet (ANTS-3757 § 2.11).

The fence rule is CommonMark's, and the project already states it once, in
`MarkdownScan` (ANTS-3603, ANTS-3655): a fence opener is a run of three or more
backticks or tildes with up to three leading spaces — re-based on the content
column of any enclosing list item (ANTS-3638) — **and a backtick fence's info
string may hold no backtick** (CommonMark § 4.5).

That last clause is what makes a line like

    ```` ```python ```` because that is what `ruff format` formats

a *paragraph containing an inline code span*, not a fence opener. Quoting fence
syntax with a longer backtick run is the standard way to write about fences, and
a roadmap that discusses tooling does it.

`walkSource()` did not use `MarkdownScan`. It hand-rolled
`line.trimmed().startsWith("```")`, which:

1. accepts a multi-backtick inline span as a fence delimiter (no info-string
   rule), and
2. accepts **any** indent, where CommonMark allows at most three spaces past the
   enclosing content column.

Clause 1 is a silent whole-document data-loss bug. One such line flips fence
parity for the rest of the file, so every following line masks as fence content:
its bullets are never recorded, its headings never open a section, and the plan
simply ends early. Nothing is reported — the plan is well-formed, just short.

Measured on this project's own `ROADMAP.md` (43,816 lines, 2,030 top-level
bullets) at the time of filing: one such line at 31,081 dropped the plan from
2,040 items to 1,559 and from 218 sections to 135. **481 items — 24% of the
roadmap — vanished with no note.** Every store row those bullets would have
matched was then reported as an orphan, which is where `roadmap_migrate`'s
446-orphan figure came from; it was read as a policy question about rows the
file no longer explains, and it was this defect.

## Invariants

- **INV-1** — a line whose backtick run is followed by a backtick (a multi-backtick
  inline span) does not open a fence, and bullets after it are still planned.
- **INV-2** — a real fenced block still masks: a bullet *inside* ``` … ``` is not
  planned.
- **INV-3** — the loss is silent, so the guard is a count: for a source whose only
  difference is such a line, the planned item count is unchanged.
- **INV-4** — a fence indented past the top-level three-space allowance, but within
  an enclosing list item's content column, still masks (ANTS-3638). This project's
  `ROADMAP.md` carries fences at indent 5 and 6 under bullets, so a fix that
  narrowed the indent rule would trade one silent loss for another.
- **INV-5** — a fence opened with ``` is not closed by a `~~~` line.

## Verify

`ctest -R roadmap_migrate_fence_span` — and, before the fix, the same test
failing on assertions rather than on compile.
