# roadmap_query_bullet_fields — the caller-chosen row shape

Feature contract for ANTS-4837.

## Problem

Two reported shapes were expressible by no argument.

`mode:"headline_only"` emits four keys fixed by contract (ANTS-4699), and
`kind` is not one of them — so the cheap mode cannot answer a triage question
that sorts by kind. The wide mode a caller falls back to emits `headline` and
`headline_oneline` as byte-identical strings whenever the headline is one
line, so the fallback pays twice for the same text.

`fields` does not close the gap: it selects top-level envelope keys, not keys
within each bullet.

## Contract

- **INV-1** — `bullet_fields` keeps exactly the named keys on each bullet, in
  the order named. A key no row carries is omitted, never emitted null: these
  rows are already gated, so absence is the normal case.
- **INV-2** — `kind` is obtainable. That is the reported gap.
- **INV-3** — a requested name that no row carries is reported in
  `bullet_fields_unmatched`, beside `bullet_fields_available`. One without the
  other cannot separate a misspelling from a key that is merely gated off.
- **INV-4** — refused `bad_mode_combo` on `headline_only`, `section_index`,
  `bundles` and `report`. Each owns its row shape; an ignored projection would
  return full rows, which is the one result a caller cannot tell from an
  answer.
- **INV-5** — on the `ids` path, `input_index` survives a projection that does
  not name it. Results come back in document order, so a caller zipping them
  against its own array mis-pairs without it (ANTS-4712), and a lean shape is
  where that bug is most likely.
- **INV-6** — present but empty, or present and not a string or array of
  strings, refuses `bad_args` rather than being ignored.

## Notes

The projection runs before pagination on the list paths, on ANTS-3577's
reasoning: the soft-cap measure then weighs the rows that will actually be
emitted, so a narrow projection fits more rows per page. It also suppresses
the automatic downshift to the lean four-key shape, which would otherwise
discard the projection the caller asked for.
