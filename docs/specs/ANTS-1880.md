# ANTS-1880 — `spec_query` + `project_layout` recognise `docs/phases/`

**Status:** spec draft, cold-eyes loops 1 + 2 folded (2026-05-26).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-1880 (vestige-feedback-2026-05-26 Obs
#23 + Wishlist #6).

**Layman:** Some projects (like Vestige) keep per-phase design docs
at `docs/phases/phase_<NN>_<topic>_design.md` instead of the Ants-
Terminal convention `docs/specs/ANTS-NNNN.md`. The spec-query MCP
tool only knows the second shape today; teach it (and the
project-layout cache) about the phases shape too so phase docs
become a first-class spec surface.

## 1. Problem

Vestige's CC session, asking `spec_query{id:"phase_10_audio_design"}`,
gets a `not_found` because `cmdSpecQuery` resolves only
`docs/specs/<id>.md`. The same file at `docs/phases/<id>.md` is
invisible to the MCP layer; the session falls back to a raw Read
(losing the parsed-metadata / invariants surface
`spec_query` provides).

`project_layout` likewise misses `docs/phases/` because its
candidate list (`kSpecsCandidates` in
`src/projectlayoutengine.cpp`) covers `docs/specs`,
`docs/private/specs`, `docs/internal/specs`, `docs/fork/specs` —
not the phases-layout used by some projects.

## 2. Surface

### 2.1 `spec_query` — per-id-shape routing

`cmdSpecQuery` (`src/remotecontrol.cpp`) currently builds the path
as `docs/specs/<id>.md` and rejects any id that fails
`isValidSpecId` (regex `^ANTS-[0-9]+$`). After this change, the
validator accepts two **disjoint** id shapes that each route to a
single dir — no cross-dir fallback:

| id regex                              | Routes to                       | Example         |
|---------------------------------------|---------------------------------|-----------------|
| `^ANTS-[0-9]+$` (existing)            | `docs/specs/<id>.md`            | `ANTS-1881`     |
| `^phase_[0-9]+_[a-z0-9_]+$` (new)     | `docs/phases/<id>.md`           | `phase_10_audio_design` |

The two regexes are disjoint by construction (`ANTS-` prefix vs
`phase_` prefix), so the routing is unambiguous. Any other id
shape returns `code:"bad_id"` with the existing error envelope.
The response shape is unchanged — `id`, `path`, `size_bytes`,
`mtime_ms`, parsed metadata + invariants — but `path` reflects the
resolved dir (`docs/specs/<id>.md` or `docs/phases/<id>.md`).
A new `source` field on the response signals which dir fed the
lookup (`"specs"` or `"phases"`) so a caller scanning results can
branch on layout without re-parsing the path. The field is always
emitted on `ok:true` (INV-3 makes it a hard invariant).

### 2.2 `project_layout` — phases_dir field

`LayoutEnvelope` gains a `phasesDir` field (JSON key `phases_dir`
on the wire — same naming convention as `specsDir` ↔ `specs_dir`).
The scan probes `docs/phases/`, `docs/private/phases/`,
`docs/internal/phases/`, `docs/fork/phases/` in that order;
first-hit wins. The field is the empty string when no phases
directory is present (back-compat for ants-terminal itself, which
doesn't use this layout). `fromJson` treats a missing
`phases_dir` key as the empty string (pre-1880 cache entries
deserialize cleanly). `isStale` compares `phasesDir` the same way
it compares `specsDir` (any mismatch invalidates the cache).

**Probe-set version bump.** Widening the candidate dir set
violates the `kProbeSetVersion` cache-invalidation contract at
`src/projectlayoutengine.h:40` ("Bump whenever the candidate path
list widens"). Bump `kProbeSetVersion` from 3 → 4 AND append a
matching line to the `// History:` comment block immediately
above the constant declaration (lines 36-39 today, naming v1 /
v2 / v3 entries); the new line cites ANTS-1880 + the
`phases_dir` addition. Every pre-1880 cached envelope (stored
`probeSetVersion: 3`) will then fail the version check and be
re-scanned on next access.

The phases probe order mirrors the existing `kSpecsCandidates`
ordering convention at `src/projectlayoutengine.cpp:176-181`
(`docs/<dir>` → `docs/private/<dir>` → `docs/internal/<dir>` →
`docs/fork/<dir>`).

### 2.3 `invariant_check` — phases dir parity

`cmdInvariantCheck` currently scans `docs/specs/<ANTS-*>.md`.
After this change it ALSO scans `docs/phases/<phase_*>.md`
(when the dir exists) and merges the hits into the existing
`matched_specs[]` array (one envelope, one array). Each entry
keeps its own `path` so the caller distinguishes spec vs phase
hits without a schema change.

**Back-compat preservation.** `specs_scanned` retains its
existing semantics (count of files scanned under `docs/specs/`
only). A new sibling field `phases_scanned` carries the
phases-dir scan count (default 0 when no phases dir). A new
`total_scanned = specs_scanned + phases_scanned` field is also
emitted for callers that want the combined diagnostic in one
read. `matched_count` becomes the combined total (specs + phases
hits) — this is the field's *intent* (how many spec-class docs
matched), so the combined count is the right shape; the field
was always "count of `matched_specs[]`", which now naturally
covers both dirs. Both dirs are always scanned (preserves the
existing no-short-circuit walk in `cmdInvariantCheck`'s
`docs/specs/` loop at `src/remotecontrol.cpp`).

### 2.4 Out-of-scope

The Vestige bullet also suggested `path_prefix` + `id_pattern`
config args on `spec_query`. This spec covers the more common
case (the docs/phases convention) with zero new args, keeping the
caller surface identical. Configurable path prefixes are deferred
under a follow-up to ANTS-1880 (no separate ID until needed).

## 3. Invariants

- **INV-1** — `cmdSpecQuery` accepts an id matching either
  `^ANTS-[0-9]+$` (routes to `docs/specs/`) OR
  `^phase_[0-9]+_[a-z0-9_]+$` (routes to `docs/phases/`). Any
  other id format returns `code:"bad_id"`. The two regexes are
  disjoint — no overlap, no fallback between dirs. Example
  accepted ids: `ANTS-1881`, `phase_10_audio_design`,
  `phase_01_intro`. *Test:*
  `tests/features/spec_query_phases_layout/test_spec_query_phases_layout.cpp::Inv1IdValidatorAcceptsBothShapes`.
- **INV-2** — Per-id-shape routing is deterministic: an
  `ANTS-NNNN` id NEVER reads `docs/phases/<id>.md`; a `phase_*`
  id NEVER reads `docs/specs/<id>.md`. *Test:*
  `…::Inv2NoCrossDirFallback`.
- **INV-3** — `cmdSpecQuery` response carries a `source` field
  with value `"specs"` (ANTS-NNNN ids) or `"phases"` (phase_*
  ids) so a caller can branch on layout without re-parsing
  `path`. *Test:* `…::Inv3SourceFieldEchoed`.
- **INV-4** — `LayoutEnvelope` carries a `phasesDir` field (wire
  key `phases_dir`); first-hit-wins probe across
  `docs/phases/` + `docs/private/phases/` +
  `docs/internal/phases/` + `docs/fork/phases/`. `fromJson`
  treats a missing key as the empty string (pre-1880 cache
  compat); `isStale` compares the field like `specsDir`. *Test:*
  `…::Inv4LayoutEnvelopeCarriesPhasesDir`.
- **INV-5** — `kProbeSetVersion` bumps from 3 to 4 in this
  change. Pre-1880 cached envelopes (storing `probeSetVersion:3`)
  are re-scanned on next access. *Test:*
  `…::Inv5ProbeSetVersionBumped`.
- **INV-6** — `cmdInvariantCheck` scans BOTH `docs/specs/` and
  `docs/phases/` (when present); hits merge into `matched_specs[]`
  (each entry carries its own `path`). `specs_scanned` retains
  the existing specs-only count (back-compat); two new fields
  `phases_scanned` and `total_scanned` carry the phases-dir
  count + combined sum. `matched_count` is the combined total.
  *Test:* `…::Inv6InvariantCheckScansBothDirs`,
  `…::Inv6SpecsScannedRetainsSpecsOnlyCount`.

## 4. Tests

`tests/features/spec_query_phases_layout/`:

- **`spec.md`** — mirrors INV-1..INV-6 in test-anchor form (each
  `*Test:* …::InvN…` cite resolves to a function in the .cpp
  below).
- **`test_spec_query_phases_layout.cpp`** — source-grep style
  against `cmdSpecQuery`, `cmdInvariantCheck`,
  `projectlayoutengine.cpp`, and `projectlayoutengine.h`. Anchors
  the validator regex extension, the per-id-shape routing,
  the `source` echo, the `phasesDir` JSON shape, the
  `kProbeSetVersion` bump (3 → 4), and the
  `cmdInvariantCheck` merged scan + new `phases_scanned` /
  `total_scanned` fields.
- Added to the `test_claude` bundle (next to other
  `roadmap_query_*` / `spec_query_*` tests).
- Per project convention, each INV is first verified to FAIL
  against pre-fix code (`phasesDir` / `phases_scanned` literals
  absent), then GREEN after the fix.

## 5. RAM / build cost

Zero new state. ~25 LoC in `cmdSpecQuery` (validator + fallback
probe), ~30 LoC in `cmdInvariantCheck` (parallel scan + merge),
~30 LoC in `projectlayoutengine.cpp` (phases probe + JSON
emit), ~25 LoC tool-descriptor schema, ~120 LoC test → **~230
LoC** total.

## 6. Migration

None. Additive — back-compat preserved (existing `ANTS-NNNN` ids
resolve via the unchanged primary path; default
`project_layout` callers see an extra `phases_dir:""` field —
documented in the tool descriptor).

## 7. Tool-descriptor update

- `spec_query` descriptor: extend the `id` description to name
  both accepted shapes + the fallback dir.
- `project_layout` descriptor: extend the response-shape
  description to mention `phases_dir`.

## 8. Cross-doc impact

- `docs/standards/specs.md` — append a section noting the
  `docs/phases/` alternative layout is recognised by `spec_query`
  + `project_layout` for projects whose per-phase design docs
  live there.

## 9. Cold-eyes loop log

- **Loop 2 (2026-05-26).** Reviewer flagged 0 CRITICAL, 3 HIGH
  (`source` field "optional" vs INV-3 "always emitted"
  contradiction; kProbeSetVersion bump didn't pin the History:
  comment-block location; §2.3 silently redefined `specs_scanned`
  — back-compat hazard), 4 MEDIUM (INV-1 example `phase_01_intro`
  vs §Layman `_design` suffix; phases probe-order cross-ref
  unstated; no-short-circuit reads as new contract not
  preservation; §4 Tests undercounted INVs at 1..4 vs 1..6). All
  verified against `projectlayoutengine.h:36-40` (History
  comment), `remotecontrol.cpp:6934` (specs_scanned emit). Fixes:
  drop "optional" qualifier from §2.1; pin the History:
  comment block location for the bump; keep `specs_scanned`
  semantics, add `phases_scanned` + `total_scanned` siblings;
  clarify INV-1 examples; cite `kSpecsCandidates` ordering
  precedent; reword §2.3 no-short-circuit as preservation;
  rewrite §4 Tests with full INV-1..INV-6 anchor coverage +
  bundle-placement note.
- **Loop 1 (2026-05-26).** Reviewer flagged 0 CRITICAL, 4 HIGH
  (probe-set version bump not specified — silent cache contract
  drift; JSON schema for new `phases_dir` field undefined;
  INV-1 regex contradicted dir routing — needed disjoint regex per
  dir, not fallback; `cmdInvariantCheck` merged response shape
  unspecified), 5 MEDIUM (regex too permissive — tightened to
  `^phase_[0-9]+_[a-z0-9_]+$`; standards.md impact understated;
  cold-eyes log empty; etc.). All verified against
  `src/projectlayoutengine.h:40` (kProbeSetVersion contract),
  `src/projectlayoutengine.cpp` (scanDir / first-hit), Vestige's
  example file pattern. Fixes: bump `kProbeSetVersion` 3→4 in
  INV-5; pin JSON shape (`phasesDir`/`phases_dir`, empty-default,
  isStale parity); tighten regex; split INV-2 into per-shape
  routing with explicit "no cross-dir fallback"; add `source`
  field response echo (INV-3); pin `cmdInvariantCheck` merged
  shape with `phases_scanned` sibling diagnostic; renumber INVs
  to accommodate the additions.
