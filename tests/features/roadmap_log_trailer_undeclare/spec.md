# A body that stops declaring a trailer key — ANTS-4576 / ANTS-4548

**Status:** implemented (2026-08-20)

## Problem

§ 2.6 re-derives the five trailer columns from a body every write
replaces. Where the OLD body declared a key and the new one does not, it
cleared the column — `clearItemField()`, i.e. SQL `NULL`.

Four of the five columns cannot hold NULL. `kind` and `source` are
`TEXT NOT NULL`; `lanes` and `evidence` are `TEXT NOT NULL DEFAULT
'[]'`. Only `layman` is nullable. So deleting a declaration from a body
reached the engine and came back as a raw SQLite string:

    roadmap_log op:"amend_body" id:"ANTS-4546"
      old_text:"Kind: fix."  new_text:"The kind is unchanged."
    -> {"code":"store_failed",
        "error":"NOT NULL constraint failed: item.kind Unable to fetch row"}

The write refused atomically and nothing was lost — but a caller who
asked to edit one sentence was handed a constraint string naming a
column they never mentioned, and the one line keeping that column
populated could not be edited by the verb that exists for editing body
lines.

## Contract

**Un-declaring is not clearing.** What "the new body no longer yields
this key" means is decided by the column's own storage contract, which
is stated once, in the DDL:

| Column | Storage | On un-declare |
|---|---|---|
| `layman` | nullable | cleared to `NULL` — unchanged |
| `lanes`, `evidence` | `NOT NULL DEFAULT '[]'` | set to `[]` — the empty list IS their absent state |
| `kind`, `source` | `NOT NULL`, no default | KEPT — there is no absent state, so "the body stopped saying it" cannot mean "it has none" |

Keeping is lossless rather than a fudge: the render emits a trailer line
from the column exactly when the body does not declare that key at a
line start (`shadows()`), so the deleted line comes back canonically,
below the body, and the file still declares the same value. The column
and the file agree — which is all § 2.6 ever wanted.

**The refusal keeps the code it had, `store_failed`, and that is now the
weakest part of this path** — it reads as an engine fault where the fault
is the caller's text. Filed rather than widened here: the plumbing is a
per-op code override at three sites, and ANTS-4576 asked for the message.

**And prose reaching a column is guarded on this path too.** ANTS-4549
routes a `note` through `rlNoteDeclaresTrailer()`; `new_text` is the same
caller prose arriving through a different argument, and was unguarded.
It is guarded now, by the same function, with the argument named in the
message.

**`dry_run` already shares the real write path** — `commitAndRender()`
runs the mutation inside the transaction and rolls it back — so a
preview and the real call agree, refusal for refusal. ANTS-4548 asked
for that; the measurement says it holds, and INV-6 is what keeps it
holding.

## Invariants

- **INV-1** — deleting a body's `Kind:` declaration succeeds; the column
  keeps its value and the render re-emits the line canonically.
- **INV-2** — the same for `Source:`.
- **INV-3** — deleting a `Lanes:` declaration succeeds and empties the
  column to `[]`, never `NULL`; the file stops declaring lanes.
- **INV-3b** — the same for `Evidence:`.
- **INV-4** — deleting a `Layman:` declaration still clears the column,
  which is the one column that can hold absence.
- **INV-5** — no path in this file surfaces a raw SQLite constraint
  string: a refusal says which key, which value, and what to do.
- **INV-5b** — a DELIBERATE declaration whose value is outside `kind`'s
  closed vocabulary refuses in words, quoting the rejected value. The
  position guard cannot catch this one: the shape is exactly what the
  render writes.
- **INV-5c** — and the two shapes the parser accepts but the column
  would refuse — `Kind: Fix.` (case) and `Kind: bug.` (a § 7.4 alias) —
  are canonicalised with the migration's own mapper rather than refused.
  The capture arrives raw; the column CHECKs the canonical form.
- **INV-6** — `dry_run:true` and the real call agree — same `ok`, same
  `code` — and the dry run leaves both the store and the file untouched.
- **INV-7** — `new_text` naming a trailer key mid-line refuses
  `body_shadowed`, names the key and names `new_text` as the argument;
  the store is unchanged.
- **INV-8** — a DELIBERATE declaration through `new_text` still works:
  the label first on its line rewrites the column.
