# ANTS-3764 — `RoadmapParse::BulletRecord` carries what migration needs

## Background

ANTS-3764 step 1 moved the roadmap reader into `ants_core_lib` verbatim.
The record it returns was shaped by what `RoadmapDialog` needed to draw a
card, and `docs/specs/ANTS-3757-roadmap-migration-read.md` § 2.3 lists five
things the migration needs that it does not carry. Each would otherwise be
re-derived by hand from `BulletRecord::body` — which is the second bullet
parser § 2.3 exists to forbid, and whose disagreements with the first would
be silent and would be about the corpus itself.

All five are **additive**. No existing caller reads a field that does not
yet exist, so `RoadmapDialog`, `roadmap_query` and `roadmap_log` see no
behaviour change; the existing 3116-test suite is the evidence for that half.

## Invariants

### INV-1 — `sourceStatus` is the verbatim `- **Status**:` value

On the pass-headings path the record carries the whole remainder of the
winning Status line, exactly as written: the qualifier tail is kept
(`done (v3.20.0, …). Adds catalogs for …`) and nothing is stripped, so
`**un-gated (2026-07-05).**` keeps its asterisks. Matching still folds case
and absorbs the leading `*` — the reader already did both; **storage strips
nothing** (ANTS-3757 § 2.7). Empty on the emoji and checkbox paths, which
carry status in the marker itself.

### INV-2 — `source` is the `Source:` value

Populated from a `Source:` line, empty when absent. Measured against the
corpus (`ROADMAP.md`, 2026-07-31) rather than assumed:

- **Capture runs to end of line, not to the first period.** 61 of 1282
  values carry an internal period — `in-session-2026-07-29 (rpmlint.log,
  first successful build).` — and `Kind:`'s stop-at-first-period rule would
  cut 4.8% of the corpus mid-value. `Evidence:` already runs to end-of-line
  for the same reason.
- **The value stops at a following trailer key** (`Kind:` / `Lanes:` /
  `Layman:` / `Evidence:`, optionally bold). 10 lines write two keys on one
  line: `Source: regression. Lanes: packaging.`
- **Un-anchored, with the ANTS-3722 backtick guard.** 157 occurrences are
  inline in a prose trailer rather than at a line start, which is exactly
  what ANTS-2058 found for `Lanes:`; 22 are backticked mentions of the key
  itself, which the guard excludes.
- A single trailing sentence period is dropped, as `Evidence:` does.

### INV-3 — `firstLine` / `lastLine` are a 1-based inclusive span

Every record reports the source lines it was built from: the bullet line
through its last continuation line, or the `####` heading through the last
non-blank line of its block. Trailing blank lines are outside the span, and
two records never overlap — ANTS-3757 INV-11 is a partition over these
spans, so without them it is unimplementable.

### INV-4 — `passDesignator` survives the heading regex

The pass path records the designator it consumed (`43.5`, `43.5.B`). It is
the one field with no consumer in ANTS-3757 § 2.9 — that section resolved
to take the reader's synthesised id rather than re-derive one — but it is
also the only field here that is **unobtainable afterwards**: by the time a
record exists the heading has been consumed, and recovering it would mean
re-matching the heading regex. Keeping it makes ANTS-3757 INV-10 assertable
directly: `passIdFromDesignator(rec.passDesignator) == rec.id`, reader and
writer agreeing on the same string rather than by assumption. Empty on
every non-pass path.

### INV-5 — `idToken` is the leading-slot token as written

The token in the item's leading slot, verbatim and **without its brackets**
(ANTS-3757 § 2.6), before any acceptance test: `ANTS-1234` and the
off-grammar `Cl9` both reach migration, which is what lets § 2.5 apply the
id-shaped grammar and § 2.6 quarantine what fails it. Empty when the
leading slot holds no token.

Two properties `rec.id` cannot supply, and both are why the field exists:

- `rec.id` is **positionless** — it is filled from the first
  `[<PREFIX>-NNNN]` token anywhere in the body, so a bullet whose leading
  slot says `[Cl9]` and whose prose cites `[ANTS-9999]` reports the
  citation. `roadmap-data-model.md` § 7.1 recognises an id only at the
  leading position.
- `rec.id` **conflates** a grammar-conforming id with an off-grammar one
  (ANTS-1987 fills both), so `id_origin` cannot be decided from it.

A leading-slot token immediately followed by `(` or `:` is a markdown link,
not an id (ANTS-3757 § 2.5), and is not recorded.

## Test plan

Behavioural, against `RoadmapParse::parseBullets` in `ants_core_lib` — no
Widgets, so it lives in the `test_core` bundle beside ANTS-3756's store
tests. Two synthetic fixtures (one ants-v1, one pass-headings) with the
line numbers written out, so INV-3 asserts exact spans rather than a
relation that any monotonic numbering would satisfy. INV-4 asserts against
`PassHeadingWrite::passIdFromDesignator()` rather than a literal.
