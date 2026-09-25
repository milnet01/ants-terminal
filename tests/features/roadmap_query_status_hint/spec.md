# roadmap_query's bad_status names the union a list was asking for (ANTS-5351)

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

INV-1 fails against the pre-fix code, which emitted no `hint`.

## Build

Compiled into `test_claude`. XDG_DATA_HOME is redirected into a temp dir, so
the verb never opens the machine's real roadmap store.
