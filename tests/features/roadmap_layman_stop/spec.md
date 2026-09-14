# ANTS-4955 — a Layman summary is stored bare and rendered with a full stop

Status: implemented

## Problem

The parse dropped one trailing full stop from a `Layman:` value and the
render never put it back. `roadmap_log` stored its `layman` argument
verbatim. So the column's content depended on which route wrote it, and
re-migrating a store-backed project rewrote every verb-written value.

## Rule

`docs/standards/roadmap-format.md` § 3.5, the `Layman:` piece:

- Every route that writes the store's `layman` column drops ONE trailing
  `.`. The import already did; `roadmap_log op:"amend_field"` now does too.
- The render appends `.` unless the stored text ends in `!` or `?`. It
  does not reuse `withStop`, which exempts `.` only.
- `roadmap_log` writing to a roadmap with no store emits that same
  rendered line.

## Invariants

- **INV-1** — The render ends a bare stored value with `.`.
- **INV-2** — The render adds nothing after a value ending in `!` or `?`.
- **INV-3** — A stored value ending in `..` renders `...`, and parsing that
  line gives back `..`. Parse after render is identity for all three shapes.
- **INV-4** — `amend_field` drops one trailing `.`: `Faster start.` stores
  `Faster start`, and the published line carries exactly one stop.
- **INV-5** — A markdown-backed append writes `**Layman:** Faster start.`
  whether the caller passed the stop or not, and `Wow!` unchanged.
- **INV-6** — A migrated value whose file line carried a stop renders with
  one after the next write, not bare.
- **INV-7** — `roadmap_log op:"append"` on a store-backed project drops one
  trailing `.` from its `layman` argument (`rlFillItemBody`), and the
  published line carries exactly one stop.

## Tests

`test_roadmap_layman_stop.cpp`: INV-1..3 call `RoadmapRender::bulletText`
and `RoadmapParse::trailerValuesIn` directly. INV-4 and INV-6 migrate a
fixture into a sandboxed store and drive `amend_field`. INV-5 drives
`cmdRoadmapLogAppendForTest` against a temp `ROADMAP.md` with no store.
