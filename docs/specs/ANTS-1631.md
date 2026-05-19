# ANTS-1631 — cold_eyes_partition probe-set widening (App-Build doc shapes)

**Status:** shipped
**Kind:** enhancement
**Source:** cross-session-reports-2026-05-18 (Vestige 3D_Engine #13, MAME
Curator /cold-eyes #d, RetroArch /cold-eyes, RetroDB Obs A)
**Pull:** 22 (MCP cold-eyes partition fold-in)
**Composes with:** [[ANTS-1619]] (contracts visibility — same engine, same
pull), [[ANTS-1634]] (UX polish — same engine, same pull), [[ANTS-1411]]
(generalised spec-lane walker this builds on), [[ANTS-1493]] (`project_layout`
probe-set widening — this spec mirrors that widening into cold-eyes).

## 1. Problem

`cold_eyes_partition`'s spec-lane discovery walks `docs/specs/` only.
Across four cross-session reports, four real-world doc layouts hit the
sparse-partition wall:

- **Vestige 3D_Engine.** 16 nested per-subsystem spec dirs at
  `docs/engine/<sub>/spec.md` — partition returns one lane (contracts).
- **MAME Curator /cold-eyes.** Per-feature `src/mame_curator/<module>/spec.md`
  files invisible — same one-lane fallback.
- **RetroArch /cold-eyes.** Date-prefixed specs at
  `docs/private/specs/2026-04-27-*.md` not surfaced.
- **RetroDB Obs A.** 8 `docs/specs/*.md` files dropped silently —
  partition returned only the contracts lane.

`project_layout`'s [[ANTS-1493]] widening already probes
`docs/{private,internal,fork}/specs/`, but `cold_eyes_partition` has its
own walker (`listSpecLaneCandidates`) that only knows `docs/specs/`.

## 2. Fix

### 2.1 Mirror `project_layout`'s spec-dir candidate list

`listSpecLaneCandidates` walks every spec-dir candidate from a static
table:

```c++
static const QStringList kSpecDirs = {
    QStringLiteral("docs/specs"),
    QStringLiteral("docs/private/specs"),
    QStringLiteral("docs/internal/specs"),
    QStringLiteral("docs/fork/specs"),
};
```

Order matches `projectlayoutengine.cpp`'s `kSpecsCandidates` (canonical →
private → internal → fork). Each dir is resolved case-insensitively via
the existing `caseInsensitiveResolve` helper. A `QSet<QString> seenRel`
dedups across roots so a project that symlinks `docs/private/specs` to
`docs/specs` doesn't produce duplicate lanes.

### 2.2 App-Build per-feature shapes

Two new walkers run after the spec-dir loop:

- `walkLevel2SpecMd(parentRel)` — for `parentRel` in
  `{"docs/engine", "src"}`, walk one level deep, for each subdir try
  `<parentRel>/<sub>/spec.md`. Catches both Vestige 3D Engine
  (`docs/engine/audio/spec.md`) and MAME Curator
  (`src/mame_curator/<module>/spec.md`) cases.
- `docs/phases/*.md` direct walk — for projects that keep phased design
  docs (`phase_1_design.md`, `2026-04-27-architecture.md`). No
  `spec.md` suffix requirement here; the `*.md` glob already accepts
  date-prefixed names automatically (ANTS-1411's walker already accepts
  any `*.md` shape; only the ANTS-NNNN active-set filter is restrictive).

One-level-deep cap is deliberate: it covers the documented App-Build
shapes without slurping vendor trees (`node_modules/**/spec.md` is the
worst case if we went recursive).

### 2.3 Date-prefix `YYYY-MM-DD-*.md`

No code change needed — the walker already accepts any `*.md` name and
only the ANTS-NNNN-shape regex is restrictive. Verified by
`Ants1631DatePrefixedSpecsSurface` test below.

## 3. Invariants

- **INV-1** (canonical dir still works). A `docs/specs/ANTS-9999.md` in
  the workspace still surfaces as a `spec/ANTS-9999` lane (active gate
  intact). No regression of [[ANTS-1411]].
- **INV-2** (private/internal/fork mirrored). A
  `docs/private/specs/PRIVATE-1.md` in the workspace surfaces as a
  `spec/PRIVATE-1` lane. Same for `docs/internal/specs/INTERNAL-1.md`
  and `docs/fork/specs/FORK-1.md`.
- **INV-3** (dedup across roots). When `docs/specs/A.md` and
  `docs/private/specs/A.md` both exist (rare, but observed when a
  project moves specs in-flight), the lane appears once (no duplicate
  `spec/A` lanes).
- **INV-4** (App-Build engine shape). A workspace with
  `docs/engine/audio/spec.md` and `docs/engine/graphics/spec.md`
  surfaces both as lanes (`spec/spec` from `audio/spec.md` and
  `spec/spec` from `graphics/spec.md` — note: lane names from the
  walker pass through `QFileInfo::completeBaseName`, which strips
  only the extension; both lanes share the basename `spec`. Caller-
  facing test asserts on `doc_paths` not `name` to avoid this
  ambiguity).
- **INV-5** (App-Build src shape). A workspace with
  `src/parser/spec.md` surfaces as a lane with
  `docPaths == ["src/parser/spec.md"]`.
- **INV-6** (docs/phases). A workspace with
  `docs/phases/phase_1_design.md` and `docs/phases/2026-04-27-arch.md`
  surfaces both as lanes.
- **INV-7** (date-prefix). A workspace with
  `docs/specs/2026-04-27-rfc.md` surfaces as a `spec/2026-04-27-rfc`
  lane (no ANTS-NNNN filter applies — the walker accepts any non-
  ANTS-shape filename).
- **INV-8** (vendor depth bound). A workspace with
  `docs/engine/audio/sub/spec.md` does NOT surface the deeper file
  (walker is one level deep by design).
- **INV-9** (INV-2 cap still applies). Total spec-lane count across all
  probe roots is capped at `kMaxSpecLanes`; `truncated == true` when
  the cap fires. The cap counts after dedup.

## 4. Test

Extended `tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp`:

- `Ants1631PrivateInternalForkSpecsSurface` (INV-2) — workspace with
  three specs in `docs/private/specs/`, `docs/internal/specs/`, and
  `docs/fork/specs/`. Asserts each surfaces as a `spec/<basename>` lane.
- `Ants1631DedupAcrossSpecRoots` (INV-3) — workspace with
  `docs/specs/A.md` and `docs/private/specs/A.md`. Asserts the lane
  appears once.
- `Ants1631DocsEngineSpecMdSurfaces` (INV-4) — workspace with
  `docs/engine/audio/spec.md`. Asserts the file surfaces in some
  lane's `docPaths`.
- `Ants1631SrcModuleSpecMdSurfaces` (INV-5) — workspace with
  `src/parser/spec.md`. Asserts the file surfaces.
- `Ants1631DocsPhasesSurfaces` (INV-6) — workspace with two files
  under `docs/phases/`. Both surface.
- `Ants1631DatePrefixedSpecsSurface` (INV-7) — workspace with
  `docs/specs/2026-04-27-rfc.md` (no ROADMAP, no active filter
  applies). Asserts surfaces.
- `Ants1631DeepNestedNotSurfaced` (INV-8) — workspace with
  `docs/engine/audio/sub/spec.md`. Asserts the deeper file does NOT
  appear in any lane (walker is one level deep by design).

## 5. Out of scope

- **Recursive descent under `docs/engine/` or `src/`.** Bounded one-level
  walk by design — vendor trees and `node_modules` would balloon the
  partition without payoff. If a project keeps specs at level 3+, the
  `.cold-eyes/partition.json` override (ANTS-1412) is the right tool.
- **Custom probe dirs configurable via the override file.** Override
  already accepts arbitrary `doc_paths[]`; the new probe dirs cover the
  observed App-Build conventions without an extra config surface.
- **Composes with [[ANTS-1633]]** but doesn't subsume it. ANTS-1633 is
  about `cold_eyes_brief` extracting cited code paths from non-spec
  doc bodies; this spec is about *which docs surface as lanes*. Separate
  concerns.

## 6. RAM / build budget

- Engine: 4 spec-dir probes (was 1) + 2 level-2 walks (`docs/engine`,
  `src`) + 1 `docs/phases` walk. Worst-case extra fs cost: 7
  `caseInsensitiveResolve` calls on a project that has none of them
  (each resolve is one `QDir::entryList` on the parent, which
  caches OS-side). Bounded.
- Cache: same `m_coldEyesCache` storage; new probe set doesn't expand
  `PartitionResult`'s shape (only `lanes[]` length, which is already
  capped at `kMaxSpecLanes`).
- Walker dedup: `QSet<QString> seenRel` per call, freed on return.
  Capped above by the total spec count walked × ~50 byte avg path =
  trivial.
- Build: no new source files. `coldeyesengine.cpp` grows by ~80 lines
  (one widened walker function + the audit-infra helper from
  [[ANTS-1634]]).

## 7. Sources

- Vestige 3D_Engine cross-session report 2026-05-18 — Observation #13.
- MAME Curator /cold-eyes cross-session report 2026-05-18 —
  Observation #d.
- RetroArch /cold-eyes cross-session report 2026-05-18 —
  date-prefix specs note.
- RetroDB cross-session report 2026-05-18 — Observation A (8 specs
  dropped).
- [[ANTS-1411]] — generalised spec-lane walker (this builds on).
- [[ANTS-1493]] — `project_layout` probe-set widening (this mirrors).
