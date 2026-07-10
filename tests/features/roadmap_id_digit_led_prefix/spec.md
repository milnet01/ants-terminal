# roadmap_id_digit_led_prefix — ANTS-3492 conformance

The roadmap ID recognisers required a **letter-leading** prefix
(`^[A-Za-z]…`), so the Vestige/3D_Engine `3D_E-NNNN` scheme was invisible to
`roadmap_query id/ids` and `roadmap_log op:flip` — the whole project's IDs
couldn't be fetched or flipped. The rule is relaxed to **"prefix contains
at least one ASCII letter"** (design + full site inventory in
`docs/specs/ANTS-3492.md`).

## Invariants covered

- **INV-1** — `isCanonicalId("3D_E-0042")` / `("3D_E-42")` true: a digit-led,
  letter-containing prefix is canonical.
- **INV-2** — `isCanonicalId("2026-07")` / `("3-2")` / `("1-1")` false: a
  letter-free prefix is never an id, so a date/version bracket in prose
  can't be mistaken for one.
- **INV-3** — `isCanonicalId("ANTS-0001")` / `("RETRO-1234")` true: no
  regression on conventional letter-led ids.
- **Boundary** — `_-1` (underscore lead), `A-` (no digits), `-42` (empty
  prefix), `3D_E` (no `-NNNN`) all reject.
- **INV-4** — `RoadmapFoldIn::sniffIdPrefix` returns `3D_E` on a
  `3D_E-`-dominant roadmap (exercises the widened `sniffPrefixFromText`),
  and a letter-free bracket like `[2026-07]`-shaped prose never wins.

## Notes

`RoadmapIndex::isCanonicalId` and `RoadmapFoldIn::sniffPrefixFromText` are
the cleanly unit-testable surfaces sharing the widened grammar; the
`rxAntsV1IdBracket` parser and the `roadmap_log` flip guard
(`rcIsNonconformingIdToken`) use the identical pattern and are exercised
end-to-end via a live `roadmap_query id=3D_E-NNNN` round-trip during
verification (see the spec's Tests section).
