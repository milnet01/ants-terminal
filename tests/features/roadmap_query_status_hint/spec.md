# roadmap_query's status: a list's hint, and the array form (ANTS-5351, ANTS-5376)

`status` takes one value. `status:"planned,in-progress"` refused `bad_status`
with an `accepted` list that included `active` without saying `active` is
exactly that union, so the caller's first orienting call was wasted.

## Invariants

- **INV-1** — a `status` value holding a separator (comma, pipe or
  whitespace) still refuses `bad_status`, and the envelope carries a `hint`
  naming `active` (planned + in-progress) and `all`.
  *Test:* `Inv1ListValueGetsTheHint`.
- **INV-2** — a single unknown value refuses `bad_status` with no `hint`:
  there is no list to explain.
  *Test:* `Inv2SingleUnknownValueHasNoHint`.

- **INV-3** (ANTS-5376) — a `status` ARRAY is the union of its elements,
  each read as the same value would be alone (case-insensitive; an aggregate
  such as `active` expands). The envelope echoes `filter` as the lower-cased
  array. *Test:* `Inv3ArrayIsTheUnion`.
- **INV-4** (ANTS-5376) — an array element outside the accepted set refuses
  `bad_status` naming that element, and an empty array refuses `bad_status`
  rather than reading as `all`. *Test:* `Inv4BadArrayRefuses`.
- **INV-5** (ANTS-5376) — INV-1's hint also names the array form.
  *Test:* `Inv5ListHintNamesTheArrayForm`.

- **INV-6** — a granular status filters the same bullets with and without
  `section`: both branches share one predicate (ANTS-3408's defect was a
  filter reaching one branch only). *Test:* `Inv6GranularFilterOnBothPaths`.

A `section_index` call takes the union of the elements' aggregates, as a
single granular value collapses to its aggregate there.

INV-1 fails against the pre-fix code, which emitted no `hint`; INV-3 to INV-5
fail against the code before ANTS-5376, which read an array as no status.

## Build

Compiled into `test_claude`. XDG_DATA_HOME is redirected into a temp dir, so
the verb never opens the machine's real roadmap store.
