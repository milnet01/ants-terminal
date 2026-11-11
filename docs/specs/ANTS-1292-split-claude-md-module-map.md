# ANTS-1292 — Split CLAUDE.md: module map → `docs/subsystems.md` (served via `subsystem` MCP)

**Status:** shipped 2026-05-22.
**Kind:** refactor.
**Source:** indie-review-2026-05-13 (token + perf bundle).
**Siblings:** ANTS-1251 (`subsystem` tool), ANTS-1258 (`indie_review`).

## 1. Problem

`CLAUDE.md` is loaded into every Claude session preamble. Its
`## Module map (src/)` subsection — one bullet per `src/` subsystem —
was ~130 lines (~2-3 K tokens) of lane summaries that most sessions never
need verbatim. The same data is already returned on demand by the
`subsystem` MCP tool (`op=map`) and consumed by `indie_review_partition`.
Keeping it in CLAUDE.md taxed every session for content that has an
on-demand home.

## 2. Surface

- The canonical module map lives in **`docs/subsystems.md`**, keeping the
  same parseable bullet format (`## Module map (src/)` H2 + ``- `name` —
  summary`` bullets) so `SubsystemMap::parse` is reused unchanged.
- `CLAUDE.md` keeps a short **stub** `## Module map (src/)` section that
  points at `docs/subsystems.md` and the `subsystem` tool.
- `SubsystemMap::resolveSource(claudeMdPath)` resolves the source for a
  project: prefers `<root>/docs/subsystems.md` when it exists, else falls
  back to `claudeMdPath` unchanged.
- Both module-map consumers route through `resolveSource`:
  `RemoteControl::cmdSubsystem` and `IndieReviewEngine::derivePartition`.
- `subsystem op=map` reports the resolved source relative to root in its
  `source` field (e.g. `"docs/subsystems.md"`), no longer hardcoded
  `"CLAUDE.md"`.

## 3. Invariants

- **INV-1** — `resolveSource` returns `<root>/docs/subsystems.md` when that
  file exists; otherwise returns the input `claudeMdPath` unchanged.
- **INV-2** — Empty input → empty output (no crash, no fabricated path).
- **INV-3** — Both `cmdSubsystem` and `derivePartition` parse lanes from
  `resolveSource(claudeMdPath)`, so a migrated project serves lanes from
  `docs/subsystems.md` and an un-migrated project still serves from
  `CLAUDE.md` (back-compat).
- **INV-4** — `docs/subsystems.md` parses to ≥ 15 lanes including
  `vtparser` (parity with the pre-split CLAUDE.md catalogue). The `15` is a
  deliberate drift-tripwire floor, not the live count — the catalogue
  currently parses to ~30 lanes.
- **INV-5** — Parsing the (post-split) `CLAUDE.md` directly yields zero
  lanes — the catalogue is no longer duplicated in the always-loaded file.
- **INV-6** — `op=map` `source` field reflects the file actually parsed,
  relative to the project root.

## 4. Tests

`tests/features/mcp_subsystem/test_mcp_subsystem.cpp`, mapped to the
invariants above:

- **INV-1, INV-4** → assertion `INV-12` (the pre-existing ANTS-1251-era
  label kept verbatim in the shared mcp_subsystem test; the later ANTS-1292
  assertions below use the `ANTS-1292/INV-N` prefix): `resolveSource(CLAUDE.md)`
  then `cachedLanes` yields ≥ 15 lanes incl. `vtparser` (the resolved source
  is `docs/subsystems.md`).
- **INV-1 (preference clause)** → `ANTS-1292/INV-13`: `resolveSource` returns
  a path ending `docs/subsystems.md` when that file is present.
- **INV-5** → `ANTS-1292/INV-14`: parsing the (stub) CLAUDE.md directly
  yields zero lanes. Pre-split this fails (CLAUDE.md carried the
  catalogue); post-split it passes.
- **INV-2** → `ANTS-1292/INV-2`: `resolveSource("")` returns empty.
- **INV-1 (fallback clause) and INV-3** → `ANTS-1292/INV-3` (one shared
  assertion): a path with no `docs/subsystems.md` sibling resolves to the
  input CLAUDE.md path unchanged — this is both INV-1's else-branch and
  INV-3's back-compat guarantee.
- **INV-6** has no isolated unit assertion — the relative-path stripping +
  empty-root `"CLAUDE.md"` fallback live inline in `cmdSubsystem`, which
  needs a live `MainWindow` the source-grep test can't construct. It is
  exercised end-to-end by the live `op:"map"` `source` field. Tracked as a
  testability follow-up (extract the relativiser for an isolated assertion).

## 5. Notes

`docs/subsystems.md` is not auto-loaded by any session; it is fetched only
when a session calls the `subsystem` tool. The mtime cache (ANTS-1251
INV-2) keys on the resolved source file, so edits to `docs/subsystems.md`
invalidate correctly.
