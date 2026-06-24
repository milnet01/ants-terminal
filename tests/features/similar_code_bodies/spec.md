# Feature: similar_code `include_bodies` (ANTS-2156)

Test contract for the exemplar-retrieval enhancement. Vestige Obs #16:
before writing a new subsystem the agent did ~13 read/grep round-trips,
the expensive half being "show me this codebase's canonical example of
pattern X" — `similar_code` / `workspace_search` return MATCH LINES
(pointers), so to copy an idiom the agent opened each whole file anyway.

Per the reporter's offered alternative (reuse-first, CLAUDE.md §3), this
is implemented as an `include_bodies` option on the existing
`similar_code` verb rather than a new verb: when true, each (already
top-N, score-ranked) match also carries the FULL enclosing definition —
`symbol`, `body` lines, `body_start_line`/`body_end_line`,
`body_truncated` — extracted via the existing `ReadRegion` symbol-body
slicer. One call returns the complete idiom.

The verb glue (`cmdSimilarCode`) needs `RemoteControl`, so the
behavioural invariant drives the two pure libs it composes
(`SimilarCode::findSimilar` + `ReadRegion::extract`) and the wiring
invariant source-greps the handler + schema.

## Invariants

- **INV-1** — a shape query locates a function and its signature resolves
  (via the same parse the verb uses) to the full multi-line body through
  the symbol extractor.
- **INV-2** — `cmdSimilarCode` honours `include_bodies`, reuses
  `ReadRegion::extract`, emits `body_unavailable` on a non-resolving
  signature, and the `similar_code` schema advertises the option.

## Out of scope (v1)

Canonical-ness ranking beyond structural similarity (call-count /
recency / convention-proximity, named in the roadmap) — v1 ranks by the
existing similarity score. A concept-STRING query (vs a code shape) stays
`workspace_search`'s job; this reuses the shape matcher.

## Pre-fix check

Against pre-fix code the `include_bodies` arg, `scSymbolFromSignature`,
and the schema property are absent → INV-2's greps fail. Verified before
wiring.

Label: `features;fast`.
