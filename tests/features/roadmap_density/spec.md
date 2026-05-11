# roadmap_density — ANTS-1238

> Feature contract for the RoadmapDialog density toggle.
> Companion to `docs/specs/ANTS-1238.md`. This file is the
> human-readable test promise; the test source asserts each
> invariant exit-0 on success.

## What this locks

ANTS-1238 ships a per-user density toggle for the RoadmapDialog
(compact / cozy / comfortable) that selects per-CSS-class px
values at `renderCardsHtml` time. The test bundle in this
directory locks the 9 spec invariants — ANTS-1238-INV-1 through
ANTS-1238-INV-9.

## How the test reaches the contract

- **Renderer-layer** (no QApplication needed for the renderer
  itself; the bundle provides QApplication so all tests build
  together): static `RoadmapDialog::renderCardsHtml(...)` is
  called against a small markdown fixture with three different
  `CardRenderOptions::density` values. Output strings are
  compared, scanned for tier-unique sentinels, and stripped of
  the `<style>` block to assert content-equality across tiers.
- **Config layer** (no GUI): a fresh `Config` instance backed by
  a `QTemporaryDir`-pointed `XDG_CONFIG_HOME` exercises the
  setter / getter round-trip and the graceful-fallback path.
- **Dialog layer** (offscreen QPA): a `RoadmapDialog` instance
  with the bundle's QApplication tests combo accessibleName,
  state-preservation across density changes, and the
  persistence-failure path (chmod 0400 on the temp config).

## Invariants tested

The test source uses one `TEST(RoadmapDensity, Main)` block that
calls `runMain()`; each invariant lives behind a `fail("<label>", …)`
exit point. The label is what surfaces on stderr on failure and
is the grep handle for re-implementers.

| INV    | Statement                                                                                                                                                                                | `fail()` label |
|--------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------|
| INV-1  | `CardRenderOptions{}` produces byte-equal HTML to an explicit-cozy render.                                                                                                               | `INV-1`        |
| INV-2  | Three densities → three pairwise-distinct outputs + tier-unique sentinels: Compact ↔ `font-size:9px`, Cozy ↔ `font-size:16px`, Comfortable ↔ `font-size:18px`. Each sentinel absent from the other two tiers. | `INV-2a` (pairwise distinct) + `INV-2b` (sentinel uniqueness) |
| INV-3  | `Config::roadmapDensity()` round-trip for `"compact"` / `"cozy"` / `"comfortable"`.                                                                                                      | `INV-3`        |
| INV-4  | Unknown / missing / invalid value → `roadmapDensity() == "cozy"`.                                                                                                                        | `INV-4`        |
| INV-5  | Density change does not mutate the search box text or the `roadmap-filter-done` checkbox state. (Probes the contract via `findChild<QLineEdit*>` / `findChild<QCheckBox*>`.)             | `INV-5`        |
| INV-6  | Rendered HTML outside the `<style>` block is byte-equal across all three densities.                                                                                                      | `INV-6`        |
| INV-7  | Density combo's `accessibleName()` equals `tr("Roadmap card density")` after RoadmapDialog construction.                                                                                 | `INV-7`        |
| INV-8  | No tier emits a `font-size:Npx` declaration with N < 9 (regex `font-size:([0-9]+)px` scan across all three rendered outputs).                                                            | `INV-8`        |
| INV-9  | Persistence-write failure path: with the parent config directory at mode 0500 (Config::save uses tmpfile+rename, which respects directory perms not file perms), calling `setRoadmapDensity("compact")` advances the in-memory state to `"compact"` AND a freshly-constructed `Config` reading the same on-disk file still returns the pre-chmod value. | `INV-9`        |

Full design rationale + tier-table set-difference analysis for
INV-2's sentinels: `docs/specs/ANTS-1238.md` § 2.f.

## Exit shape

Exit 0 if all invariants hold. Non-zero exit with a `FAIL: <why>`
diagnostic to stderr on the first failed invariant — the
diagnostic names the invariant ID and the specific failure mode
so a re-implementer can diagnose without re-reading the test
source.

## Re-open conditions

If a future tier-table refactor in `roadmapdialog.cpp` breaks any
of the 9 invariants:

1. The test will surface the specific INV that failed.
2. Check whether the spec's tier table changed too — if so, the
   spec is the source of truth; update the test to match.
3. If the tier table is unchanged, the implementation regressed —
   fix the renderer.

Add a new INV / sub-test when adding a new density tier (e.g.
"ultra-compact"), a new persistence dimension, or a new
publicly-observable dialog property that should not mutate on
density change.
