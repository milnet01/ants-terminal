# roadmap_import_mapping — the markdown→store import mapping

**Contract:** [`docs/specs/ANTS-4065-import-mapping-contract.md`](../../../docs/specs/ANTS-4065-import-mapping-contract.md)
(accepted 2026-08-08), INV-1 … INV-11. Build order:
[`docs/plans/ANTS-4065-import-mapping-contract.md`](../../../docs/plans/ANTS-4065-import-mapping-contract.md)
Phase C.

## What this covers

One case per invariant, all behavioural — the spec carries no source-grep
invariant. Three layers are exercised, because the contract spans three:

- **parse / plan** (`RoadmapMigrate::planFrom`) — INV-1, INV-2, INV-3, INV-4,
  INV-8, INV-9, INV-11.
- **path validation** (`RoadmapMigrate::validatePaths`) — INV-7.
- **render** (`RoadmapRender::bulletText`) — INV-5, INV-10.
- **round trip** (migrate → load → render → re-import) — INV-6.

## Fixtures

Inline markdown, not committed files. Every case is a few-line roadmap and the
document under test is more useful beside its assertion than one directory
away; the two cases that need a project on disk (INV-6's round trip, INV-7's
path resolution) write their inline text into a `QTemporaryDir`.

`RoadmapStore` is constructed with an **explicit path** throughout. The default
resolves under `XDG_DATA_HOME`, which is the developer's live store.

## Must-fail-first

Verified against pre-change source before the fix landed. The spec's § 6 names
six cases that must red; **seven did**, and the seventh is not a defect in
either the test or the code:

| Case | Why it reds on pre-change source |
|---|---|
| INV-1 | the empty-`rawKind` branch assigns a default and emits no note |
| INV-2 | `rxKind()` is `^`-anchored, so an inline `Kind:` is invisible |
| INV-5 | `bulletText()` renders `Source:` from the value, not the provenance |
| INV-7 | § 2.5's path validation does not exist in current source at all |
| INV-9 | `CaseInsensitiveOption` is still set, so `kind:` parses |
| INV-10 (equal-value) | the anchor leaves `offset == -1`, so `shadows()` is false and the trailer is emitted |
| **INV-4 (the four § 2.1 additions)** | **not in § 6's list.** `bug`, `performance`, `process + tooling` and `audit` are § 2.1's four mechanical map additions, which by construction cannot pass before C3 adds them. § 6 enumerates the six *named invariants* that red and does not claim to be exhaustive over cases; recorded here rather than silently absorbed |

INV-3, INV-6, INV-8 and INV-11 pass on pre-change source and must keep passing
— they are the regression half.

## INV-6 and the deferred columns

§ 2.6 defers `headline`, `layman` and `lanes`: the round trip is known to move
them and the cause is undiagnosed (§ 5, Phase D4). So the assertion is over the
**six** governed columns that are not deferred, and the three that are get a
measured drift count printed rather than asserted. A test that is expected to
stay red is not a gate, and a suite with a permanently-red member stops being
read; the printed counts are the instrument D4 works from.
