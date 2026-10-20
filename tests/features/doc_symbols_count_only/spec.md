# Feature spec: `doc_symbols count_only` (ANTS-5265)

## Problem

`counts` answers "how many names resolve, and how many do not", and the
verb builds it whole-document on every call. But the reply also carries
`symbols[]` and `findings[]`, so asking for the totals costs the rows.
Measured 2026-09-21 by ut-monsterhunt: `doc_symbols` over a 21-document
`docs/` returned 106,500 characters, past the response cap, to deliver
five numbers. `only` does not help: it filters rows, and the rows were
the problem.

`workspace_search` already has `count_only` for this question.

## Contract

`count_only: true` returns the reply without `symbols[]` and
`findings[]`, and adds `findings_count`, the number of findings the full
reply would have carried. Everything else is unchanged, `counts`
included. Omitted or false, the reply is as before.

`count_only` with `mode:"locator"`, or with an `only` other than
`"all"`, refuses `bad_args`. Neither has rows to act on under
`count_only`, so accepting them would ignore an argument silently.

## Invariants

- **INV-1 default unchanged.** Without `count_only`, `symbols[]` and
  `findings[]` are present and `findings_count` is absent.
- **INV-2 rows dropped.** With `count_only`, `symbols[]` and
  `findings[]` are absent.
- **INV-3 counts survive.** `counts` is identical with and without
  `count_only`.
- **INV-4 findings counted.** `findings_count` equals the length of the
  `findings[]` the full reply carries.
- **INV-5 conflicts refused.** `count_only` with `mode:"locator"` or a
  narrowing `only` refuses `bad_args`.
