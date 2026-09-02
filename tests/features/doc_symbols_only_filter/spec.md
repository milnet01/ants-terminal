# Feature spec: `doc_symbols only=` row filter (ANTS-3689)

## Problem

`doc_citations` has `only:"stale"`, which narrows the rows a caller acts
on while leaving `counts` whole-document. `doc_symbols` had no
equivalent, so every occurrence shipped — including every `resolved`
row, the one class nobody reads. Measured: a single-document run over
`docs/specs/ANTS-3663.md` returned over 100 KB, past the response cap,
and had to be offloaded to a file and sliced before it could be used.

## Contract

`only` accepts `"all"` (default), `"unresolved"` and `"not_checked"`.
It narrows `symbols[]` only.

`counts` stays whole-document under every value — it answers what the
scan found, not what this call chose to print, which is what makes a
narrowed reply readable at all. `symbols_filtered_out` reports how many
rows the filter withheld, so a short list is distinguishable from a
document with little in it. The applied value is echoed as `only`.

An unrecognised value refuses `bad_args` with the accepted list, rather
than being ignored — a filter that silently means "all" returns the
oversized payload the caller passed it to avoid.

## Invariants

- **INV-1 default unchanged.** Omitting `only` emits every row, echoes
  `only:"all"`, and reports `symbols_filtered_out: 0`.
- **INV-2 narrows rows.** `only:"unresolved"` emits only unresolved rows.
- **INV-3 counts survive.** `counts` is identical under every accepted
  value, including `total`.
- **INV-4 withheld count.** `symbols_filtered_out` equals the rows the
  filter dropped, and rows emitted plus rows withheld equals
  `counts.total`.
- **INV-5 not_checked.** `only:"not_checked"` selects that class alone.
- **INV-6 unknown refused.** An unrecognised value refuses `bad_args`
  and names what is accepted.
