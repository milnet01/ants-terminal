# ANTS-4500 — a synthesised id has its own namespace and its own counter

Contract for `tests/features/roadmap_synth_id/test_synth_id.cpp`.
Design: [`docs/specs/ANTS-4500-synthesised-id-namespace.md`](../../../docs/specs/ANTS-4500-synthesised-id-namespace.md).

## Problem

`Loader::allocateId()` invents an id for an id-less bullet by taking the next
value in the project's own counter space. Two consequences.

An invented id is indistinguishable from an allocated one once written, and it
is positional — it depends on where the bullet sits and on what the source
carried that run. ANTS-4493 records one moving between runs. So an invented id
cannot be cited in a commit message, a spec or another item.

Synthesis also spends the live id space. Every invented id advances the counter
`roadmap_log op:"append"` allocates from, so numbers nobody chose are consumed.

## Fix

An invented id renders as `<prefix>-S<NNNN>` and draws from a counter kept at
`<prefix>#S` in `id_prefix`. `#` cannot appear in a declared prefix, so the key
cannot collide with a real one. The id grammar widens to admit the `-S` infix,
so a synthesised item stays reachable by every locating read and write.

Items already holding an invented id keep it. This is new synthesis only.

## Invariants

- **INV-1** — a migration that invents an id renders it with the `-S` infix.
  *Test:* `rendersSSuffix`.
- **INV-2** — inventing an id does not advance the real prefix's counter.
  *Test:* `realCounterUntouched`.
- **INV-3** — two synthesised ids in one project never collide, and the floor
  survives a lost counter row and a store restored behind its file.
  *Test:* `synthIdsUnique`.
- **INV-4** — a synthesised id is reachable by every locating read and write.
  *Test:* `synthIdAddressable`.
- **INV-5** — an item already holding a synthesised id keeps it across a
  re-migration of an unchanged, still-id-less source bullet.
  *Test:* `existingIdsUnchanged`.
- **INV-6** — an id-bearing item whose id matches no stored id is matched on
  the id-less key before being treated as new, and the source's id wins.
  *Test:* `reMatchFallsBackToKey`.
- **INV-7** — a synthesised id never becomes the project's allocation prefix.
  *Test:* `synthPrefixNotProjectPrefix`.
- **INV-8** — a synthesised id is addressable by the parser, not merely by a
  better error message.
  *Test:* `synthIdParsesAsId`.
- **INV-9** — a wholly-synthesised project still resolves its real prefix.
  *Test:* `realPrefixRowEnsured`.

## Notes

INV-5 and INV-7 are regression guards rather than red-first cases. INV-5 passes
before the change and must keep passing. INV-7 guards the `#` exclusion in
`RoadmapStore::idPrefixFor()`; it goes red only when that exclusion alone is
reverted, which is how it was proved to fail.

Label: `features`. Bundle: `test_core`.
