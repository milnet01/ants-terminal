# ANTS-4596 — a layman value is not truncated at an internal full stop

Status: implemented

## Problem

`rxLayman()` captures its value with a NON-GREEDY `(.+?)` closed by
`[\.\n]` (`src/roadmapparse.cpp:348`), so the value ends at the FIRST
full stop inside it. The comment above that pattern says the character
class is there to strip the TRAILING sentence period, keeping the stored
value punctuation-free per ANTS-1154 INV-4.

Those are two different rules. They agree on a value with no internal
dot, which is why the corpus hid the difference, and they diverge on
`e.g.`, `etc.`, a decimal, or a leading `.deb`.

Verified against the parser at HEAD before the fix — a throwaway TU
linked against `build/*.a` calling `parseAntsV1Bullet`:

    Layman line: ... two Claude tabs open (e.g. Ants Terminal + Vestige) ...
      stored:    "When you have two Claude tabs open (e"
    Layman line: Sub-numbered items like 41.5 and 41.5.B are merged.
      stored:    "Sub-numbered items like 41"

The two neighbouring keys already have the correct shape and their
comments already carry this reasoning. ANTS-3764 gave `Source:` a
capture that runs to end-of-line, stops at a following trailer key, then
chops one trailing period. ANTS-3382 gave `Evidence:` an end-of-line
capture, its comment noting that a `[^\.\n]` stop "would truncate at the
extension". Neither was back-applied here.

Measured 2026-08-20 across the machine-global store (16 projects, 5049
items): 403 truncated trailer values, ~29,000 characters lost, the large
majority of them this key. This defect is the reason ANTS-4585's repair
pass cannot run first — a repaired value is re-truncated by the next
parse.

## Surface

- `src/roadmapparse.cpp` — `rxLayman()`, and the `layman` extraction in
  `parseAntsV1Bullet()`.

## Rule

The layman value runs to the end of its line, continues across a wrap on
`matchLastIn()`'s existing terms, and then stops at the first following
trailer declaration. Exactly ONE trailing full stop is removed, so the
stored value stays punctuation-free.

The stop set for this key names `Kind`, `Lanes`, `Source` and `Evidence`
and NOT `Layman` itself — `rxTrailerKey()` is the mirror set, built to
serve `Source:`, and reusing it here would let a value run through a
following `Source:` while ending at a second `Layman:` that cannot occur.

## Invariants

- **INV-1** — A value carrying `(e.g. …)` is stored whole, parenthesis
  balanced.
- **INV-2** — A value carrying a decimal (`41.5`, `41.5.B`) keeps every
  digit.
- **INV-3** — ANTS-1154 INV-4 still holds: a value's single trailing full
  stop is removed and the stored value ends without punctuation.
- **INV-4** — A second key on the same line still ends the value:
  `Layman: x. Kind: chore.` yields the layman AND `kind: chore`. Before
  the fix the first-period stop produced this by accident; an
  end-of-line capture has to do it deliberately.
- **INV-5** — ANTS-4542 still holds for this key: a value hard-wrapped
  mid-phrase is rejoined rather than truncated at the wrap.
- **INV-6** — A file extension inside the value survives, the same
  guarantee ANTS-3382 states for `Evidence:` and ANTS-4542 INV-5 states
  for `Source:`.

## Tests

`test_roadmap_trailer_layman_internal_period.cpp` drives the pure static
`RoadmapDialog::parseBullets` and asserts the parsed `layman` and `kind`
fields.
