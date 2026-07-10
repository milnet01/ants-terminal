# Feature spec: bad_id_format guard for nonconforming bracket-token ids (ANTS-3387, extended by ANTS-3492)

Cross-session pain (Vestige, 3D_E-NNNN scheme, 2026-06-30):
`roadmap_query {ids:["3D_E-0022"]}` returned `found:false` although
`[3D_E-0022]` is a well-formed, actionable bullet in the file.

**ANTS-3387** (interim, safe) kept the letter-led canonical gate but made a
digit-led id-shaped token return a NAMED `bad_id_format` instead of a silent
`bullet_not_found`. **ANTS-3492** (2026-07-10) went further and RELAXED the
canonical gate itself from "letter-leading prefix" to "prefix contains ≥1
letter": `3D_E-0022` is now canonical and resolves/flips normally
(INV-7), so a `3D_E-NNNN` project is fully addressable by id. The
`bad_id_format` guard now fires only on a LETTER-FREE id-shaped token (a date
bracket like `2026-07`), which stays non-canonical (INV-2/INV-3). The
canonical gate is `^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+$`
(roadmap-format.md § 3.5.1, ANTS-3492).

## Surface

- `roadmap_query(id:"3D_E-0022")` → `{ok:false, code:"bad_id_format", id,
  hint}`.
- `roadmap_query(ids:[…])` with any nonconforming token → `{ok:false,
  code:"bad_id_format", bad_format_ids:[…], hint}` (whole batch refused).
- `roadmap_log op:"flip"|"annotate" id:"2026-07"` (letter-free) → `{ok:false,
  code:"bad_id_format"}` (offending token + canonical-form guidance in the
  `error` message; consistent with the sibling `rlErr` refusals).
- `roadmap_log op:"flip" id:"3D_E-0022"` (digit-led, letter-containing) →
  resolves + flips normally (ANTS-3492).

## Invariants

- **INV-1 / classifier.** `rcIsNonconformingIdToken(tok)` returns true iff
  `tok` matches the loose id-ish shape `^[A-Za-z0-9][A-Za-z0-9_-]*-\d+$` but
  NOT the canonical, letter-containing gate
  `^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+$` (ANTS-3492 —
  only `kCanonical` widened; `kIdIsh` stays digit-permissive). Source anchor:
  `rcIsNonconformingIdToken` + the canonical lookahead literal in
  `remotecontrol.cpp`.
- **INV-2 / write flip → bad_id_format.** `cmdRoadmapLogFlip` with a
  nonconforming `id` locator returns `{ok:false, code:"bad_id_format"}`.
  The guard is a single format-independent early check (before the
  GFM/ants-v1 split), so it fires regardless of roadmap format. Behavioural
  via `cmdRoadmapLogFlipForTest`.
- **INV-3 / write annotate shares the guard.** `op:"annotate"` (routed to
  the same handler) with a nonconforming `id` returns `bad_id_format`.
  Behavioural.
- **INV-4 / conforming-absent id is NOT bad_id_format.** A canonical but
  genuinely-absent id (`ANTS-9999`) still returns the ordinary
  not-found refusal (`bullet_not_found`), never `bad_id_format` — the guard
  keys on *shape*, not presence. Behavioural regression.
- **INV-5 / read id branch.** `cmdRoadmapQuery`'s singular `id` branch emits
  `bad_id_format` via `rcIsNonconformingIdToken(idArg)` after the `bad_case`
  check. Source anchor: `bad_id_format` + `rcIsNonconformingIdToken(idArg)`.
- **INV-6 / read ids branch.** The `ids[]` branch refuses the whole batch
  with `bad_id_format` + `bad_format_ids[]` when any requested id is
  nonconforming. Source anchor: `bad_format_ids`.
- **INV-7 / digit-led, letter-containing id flips (ANTS-3492).** The seed's
  `[3D_E-0022]` bullet (canonical since ANTS-3492) resolves and
  `op:"flip"` succeeds — the original Vestige repro is now fully addressable
  by id, not refused. Behavioural via `cmdRoadmapLogFlipForTest`.
