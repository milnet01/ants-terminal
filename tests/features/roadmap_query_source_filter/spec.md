# Feature: `roadmap_query` filters by `Source:` provenance

Test contract for ANTS-4985.

## Problem

`Kind:` was selectable; `Source:` was not. `roadmap-format.md` § 3.5.3 states
the cause in the `code-quality-review-YYYY-MM-DD` row — *"nothing reads
`Source:` values, so this is traceability, not a format break"* — a sentence
written to justify accepting two spellings, whose side effect was that the
field every item fills in was unreadable by every query surface.

So "how many review fixes remain?" had no answer. The only clean count was by
`kind`, and `kind` under-reports it.

## Why provenance is not a `kind`

Measured 2026-09-09 against the live store, this project's active items: **36
are review-derived by `Source:`, and only 6 carry `review-fix` or
`audit-fix`.** The other 30 are `perf`, `doc`, `refactor`, `marketing`,
`implement`, `enhancement`. That is not misfiling — a review legitimately
produces work of any kind. `kind` answers *what sort of work*; `Source:`
answers *where it came from*; the two are orthogonal and one field cannot
carry both. Adding more provenance-shaped `kind` values deepens the overload
that produced the under-count.

## Why prefix, and why an array

The recognised values are DATED (`indie-review-2026-05-13`), so equality
answers nothing anyone asks. The column is free text — 2599 distinct values
over 6706 items, measured the same day — so there is no `accepted` list to
refuse against the way `kind` has one.

The array is not convenience. § 3.5.3 adopted `code-quality-review-*` on
2026-08-12 and says existing `indie-review-*` bullets keep theirs, so two
spellings for one provenance are live by the standard's own decision. A
scalar-only filter cannot ask "from any review" in one call.

## Invariants under test

- **INV-1** — `source` narrows the list by case-insensitive PREFIX, and
  composes with `status`.
- **INV-2** — an ARRAY matches if ANY prefix hits, which is what the two live
  review spellings require.
- **INV-3** — `source` composes with `kind`. The two filters are independent
  axes; applying both narrows to their intersection.
- **INV-4** — a present-but-empty `source` REFUSES with `bad_args`. An empty
  prefix matches everything, so returning the full set reads as "everything
  came from there" and cannot be told from no filter at all. This is the
  `bad_kind` reasoning applied to a filter that has no enum to check.
- **INV-5** — the envelope echoes the applied prefixes as `source` and reports
  `source_filtered_out`, so a zero-row answer is distinguishable from a filter
  that did nothing.
- **INV-6** — each bullet carries `source` as a field, beside `kind` and
  `lanes`. It was the only trailer column with a stored value and no field of
  its own.

## Test shape

Drives `RemoteControl::cmdRoadmapQuery` against a seeded temp roadmap, the
same harness `roadmap_query_kind_filter` uses (the null `m_main` is never
dereferenced on this path). Behavioural throughout — the verb is pure enough
to call directly, so nothing here is a scrape.

Label: `features;fast`.
