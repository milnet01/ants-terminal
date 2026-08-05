# roadmap_render — feature-conformance contract

Locks [ANTS-3758](../../../docs/specs/ANTS-3758-roadmap-render.md), the render
that generates `ROADMAP.md` from the store at full fidelity. That spec's § 6
assigns all fourteen invariants to this one directory.

Behavioural, against a real store in a `QTemporaryDir` — never source-scrape.
The invariants are about what the render *emits* and what it *refuses*, and a
grep over `roadmaprender.cpp` would assert that the implementation looks like
itself.

## What each case locks

| Case | INV | Breaks when |
|---|---|---|
| `Inv1ExportsMatch` | 1 | a non-defaultable field is dropped from the bullet, or an element is emitted out of order |
| `Inv1TableRendersAsGfm` | 1 | a `table` element's canonical JSON payload is emitted verbatim instead of serialised, the separator row is omitted, or a literal `\|` in a cell is left unescaped (ANTS-3832) |
| `TableRefusesShapelessPayload` | 1 | a payload with no `header` renders a malformed table rather than refusing |
| `Inv2SectionOrder` | 2 | sections are sorted by slug, by `section_id`, or by walking `parent_id` |
| `Inv3ArchiveRouting` | 3 | a rotated archive's sections fold back into `ROADMAP.md` |
| `Inv4Membership` | 4 | the filter reads "open items only", `visibility` is ignored, or an unfiled item is skipped silently |
| `Inv5PublishGate` | 5 | the gate is applied per item rather than per project, or a refusal returns `nullopt` and loses the ids |
| `Inv6AllOrNothing` | 6 | files are written in a loop with no staging |
| `Inv7Idempotent` | 7 | an ordering falls back to an unstable comparison, or a timestamp is emitted |
| `Inv8FormatMarker` | 8 | the marker is emitted on top of the one the root intro already carries (duplicating it), or a markerless store publishes silently |
| `Inv9LevelAgreesWithParent` | 9 | heading depth is derived by walking parents instead of read from `level` |
| `Inv10ElementInterleaving` | 10 | items are emitted first and narration/tables after |
| `Inv11SingleElementReader` | 11 | `roadmapexport.cpp` keeps its own `FROM element` / `FROM project` SQL |
| `Inv12RequiredPiecesPresent` | 12 | `Kind:` is skipped for items whose kind equals § 3.5.3's default |
| `Inv13PathContainment` | 13 | either path is joined to the root without canonicalising, or the check covers `source_path` alone |
| `Inv14DryRunWritesNothing` | 14 | dry-run is implemented as write-then-delete |

## Verifying RED

Per the project convention every case is shown failing against its *Breaks
when* mutation before the implementation is restored. Where that is scripted,
restore files with `write_text` and never `shutil.copy2`: `copy2` preserves
mtime, ninja then skips the rebuild, and the mutation accumulates silently in a
binary that still links green.

`Inv11SingleElementReader` is the one source-scrape case, and deliberately so —
it asserts a *refit* rather than a behaviour, and the thing it must prove is
that no second reader exists anywhere under `src/`.
