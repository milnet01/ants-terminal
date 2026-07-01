# Feature spec: bad_id_format guard for nonconforming bracket-token ids (ANTS-3387)

Cross-session pain (Vestige, 3D_E-NNNN scheme, 2026-06-30):
`roadmap_query {ids:["3D_E-0022"]}` returned `found:false` although
`[3D_E-0022]` is a well-formed, actionable bullet in the file. Root cause:
the canonical id gate (`idTokenPattern`, roadmap-format.md § 3.5.1 / ANTS-1405
INV-4) is letter-led `[A-Za-z][A-Za-z0-9_-]*-\d+`, so a digit-leading prefix
(`3D_E` starts with `3`) never parses as a project id — the parser assigns
that bullet a synthetic content-hash and the authored token is unaddressable
on both the read (`id`/`ids`) and write (`flip`/`annotate`) locator paths. A
bare `found:false` / `bullet_not_found` reads as "the item vanished".

Fix (primary, safe): when an `id`/`ids`/locator token is id-token *shaped*
but fails the canonical gate, return `bad_id_format` with a canonical-form
hint instead of a silent miss. NOT changed: whether § 3.5.1 should admit
digit-leading prefixes — ANTS-1405 INV-4 rejected them on purpose.

## Surface

- `roadmap_query(id:"3D_E-0022")` → `{ok:false, code:"bad_id_format", id,
  hint}`.
- `roadmap_query(ids:[…])` with any nonconforming token → `{ok:false,
  code:"bad_id_format", bad_format_ids:[…], hint}` (whole batch refused).
- `roadmap_log op:"flip"|"annotate" id:"3D_E-0022"` → `{ok:false,
  code:"bad_id_format"}` (offending token + canonical-form guidance in the
  `error` message; consistent with the sibling `rlErr` refusals).

## Invariants

- **INV-1 / classifier.** `rcIsNonconformingIdToken(tok)` returns true iff
  `tok` matches the loose id-ish shape `^[A-Za-z0-9][A-Za-z0-9_-]*-\d+$` but
  NOT the canonical `^[A-Za-z][A-Za-z0-9_-]*-\d+$`. Source anchor:
  `rcIsNonconformingIdToken` + both regex literals in `remotecontrol.cpp`.
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
