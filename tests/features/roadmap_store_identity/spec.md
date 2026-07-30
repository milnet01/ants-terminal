# roadmap_store_identity — item identity in the roadmap store

Feature contract for **ANTS-3756** INV-3 and INV-4.
Parent spec: [`docs/specs/ANTS-3756-roadmap-store-schema.md`](../../../docs/specs/ANTS-3756-roadmap-store-schema.md)

## What this locks

**INV-3 — item identity is case-folded *within a project*.**

- `Sh-1` then `SH-1` in **one** project raises a uniqueness violation.
- The same pair in **two different** projects does not.
- An `INSERT` naming `id_fold` explicitly is **refused**.

The second leg is not padding. `roadmap-data-model.md` § 7.1 says the same id
may legitimately exist in two projects, so a store keyed on `id_fold` alone
rejects a corpus the model requires.

The third leg is what makes the folding structural rather than a habit of the
current writer. `id_fold` is `GENERATED ALWAYS AS (lower(id)) VIRTUAL`, so
SQLite refuses to let any writer store an unfolded identity key. As a plain
column the uniqueness constraint stays satisfiable while the writer quietly
fails to fold — it fires on whatever the writer chose to put there.

**INV-4 — an off-grammar id is stored verbatim and never rewritten.**

`[Cl9]` does not match `roadmap-format.md` § 3.5.1's grammar. It is stored with
`id_origin = 'quarantined'`, and `id` round-trips byte for byte — no dash
inserted, no case changed, no brackets stripped.

`id_origin` has **three** values, not a boolean: a synthesised `PASS-N-M[-S]`
id also fails the grammar, yet the model's § 7.1 says it "**is** an ID for every
purpose". A boolean conflates it with genuinely off-grammar ids, and would bury
144 live items.

## Must fail first

Verified against a deliberately wrong store before the real one:

- `UNIQUE (project_id, id_fold)` written against `id` → leg 1 passes `SH-1`.
- `id_fold TEXT NOT NULL` as a plain column → leg 3 accepts a bogus fold.
- `id_origin` as a boolean, or ids normalised on the way in → INV-4 fails.
