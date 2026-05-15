# combining_at_wide_right_edge — ANTS-1334

See `docs/specs/ANTS-1334.md` for the full contract.

## Test scope

Asserts that a zero-width combiner arriving immediately after a
wide char lands on the LEAD cell, not the continuation cell, even
when the wide char straddles the right edge (`cursorCol = cols-2`).

## Invariants checked

- **INV-1.** Combiner attaches to lead column (cols-2) after a
  delayed-wrap wide write at the right edge.
- **INV-3.** Combiner still attaches correctly to interior lead
  cells (no regression on the m_cursorCol > 0 branch).

## Repro before the fix

Pre-fix `handlePrint` chose `targetCol = m_cursorCol` when
`m_wrapNext` was set, which is the continuation cell after a
right-edge wide write. Renderer looks combiners up by lead column
⇒ diacritic invisible.
