# A `note` is opaque prose — ANTS-4549

**Status:** implemented (2026-08-20)

## Problem

`op:"annotate"` (and `op:"flip"` / `op:"flip_batch"` carrying a note) did
not treat `note` as opaque. The note is appended to the bullet's body and
§ 2.6 then RE-DERIVES every trailer column from the new body, so a bare
trailer keyword in caller prose — `Kind:`, `Layman:`, `Source:`,
`Lanes:`, `Evidence:` — is read as metadata and the words after it are
written to the matching column.

Reported against AI_Prompts/AIPR-0033, where it produced an invalid kind
and the store's CHECK constraint refused the whole write with
`store_failed`. **The refusal was luck, not a guard**: had the following
word been one of the 21 accepted kinds, the write would have SUCCEEDED
and silently re-kinded the item.

`op:"append"` already refuses exactly this, as `body_shadowed`, naming
the shadowed column and quoting the text that would win the re-parse. So
this is not "add a guard" but "route the note through the guard the
sibling op already has".

## Contract

A `note` may declare a trailer key only in the shape the RENDER writes
one: the label first on its line (leading whitespace allowed). Anywhere
else the key sits in running prose, and reading it as metadata is the
defect.

Position, not the value comparison `op:"append"` uses, because a note has
no column argument to compare against — and blanket-refusing every
declaration would close § 2.6's own reason for existing: an annotate
whose note carries a `Layman:` line is the only way to fill that column
on a migrated item, and the render gates on it. The two shapes are
distinguishable without asking the caller.

The check runs once, where the note is scrubbed — before the locate,
before the format split, and before the backend split — so all three
note-carrying ops and both backends give one answer.

## Invariants

- **INV-1** — `op:"annotate"` with a note naming a trailer key mid-line
  refuses `body_shadowed`; the file is NOT modified.
- **INV-2** — the same note with the key wrapped in backticks is
  accepted (ANTS-4504 masks code spans), so the refusal is
  self-correctable in one edit.
- **INV-3** — `op:"flip"` carrying such a note refuses too, and the
  status does NOT flip: one op, one outcome.
- **INV-4** — under `op:"flip_batch"` the offending locator lands in
  `skipped[]` with code `body_shadowed` while the other locators still
  apply — the per-locator failure shape the op already uses.
- **INV-5** — every key whose pattern can match mid-line is guarded
  there (`Kind:`, `Source:`, `Lanes:`), not just `kind`, which is the
  only one the store's CHECK constraint happens to catch.
- **INV-5b** — and nowhere else: `Layman:` / `Evidence:` are anchored
  patterns and cannot match mid-line, so prose naming them is accepted.
  A guard stricter than the parser refuses notes that were never at risk.
- **INV-6** — the refusal names the key, quotes the offending text, and
  names both remedies (backticks, or a line of its own).
- **INV-7** — a deliberate declaration survives: the label first on its
  own line, indented or not. `roadmap_write_half` INV-4 depends on it.
