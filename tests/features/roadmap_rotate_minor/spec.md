# roadmap_rotate_minor — feature-conformance contract

Design contract: [`docs/specs/ANTS-4070-rotation-and-section-title.md`](../../../docs/specs/ANTS-4070-rotation-and-section-title.md).
This file states only what the test asserts; the reasoning lives in the spec.

## Subject

`roadmap_log op:"rotate_minor"` and `roadmap_log op:"retitle_section"`, driven
through their `*ForTest` seams against a store the migration actually built.
INV-1 … INV-13 are the design spec's, numbered identically.

## Fixture rules

- **Behavioural, not hand-built.** Every case writes markdown into a
  `QTemporaryDir`, redirects `XDG_DATA_HOME`, and migrates it —
  `findRoadmaps` → `planFrom` → `load`. A hand-built store can hold rows the
  loader never writes, and an invariant asserted against one is asserted
  against a state the product cannot reach.
- **`RoadmapStore` is constructed with an explicit path**, never
  default-constructed: `defaultPath()` resolves under `XDG_DATA_HOME` and a
  default construction runs the suite against the developer's live store.
- **The live roadmap sits directly in the project root.** `findRoadmaps()`
  accepts no other placement, so a fixture at `sub/ROADMAP.md` cannot be
  migrated at all.
- **Every open bullet carries a `Layman:` line, except INV-10's.** Since
  ANTS-4628 the publish gate is scoped to the items a write touches and a
  rotation touches none, so a stray `Layman:`-less bullet no longer refuses a
  rotation. The convention is kept anyway: these fixtures double as examples of
  a well-formed roadmap, and one that relies on the gate ignoring it is a worse
  fixture than one that says what it means.

## What each case asserts

| Case | Assertion |
|---|---|
| INV-1 | A rotated minor's bullets render into `docs/roadmap/0.7.md` and out of `ROADMAP.md`, each bullet's rendered text byte-identical to its pre-rotation rendered text. |
| INV-2 | The second rotation succeeds with `sections_moved: 0` / `sections: []` and renders byte-identically; a minor no title matches returns `section_not_found`. |
| INV-3 | 📋, 🚧 and 💭 each refuse `minor_not_closed`, as does a 🚧 living under a `###` child of an all-shipped `##`; nothing is written. |
| INV-4 | `0.7` claims `## 0.7.0` and `## 0.7` but not `## 0.70.0`; `0.5` does not claim the `## 0.5.x and 0.6.x — archived` signpost. |
| INV-5 | Every descendant moves, including one misfiled under `docs/roadmap/0.6.md`. |
| INV-6 | `v0.7`, `0.7.0` and `00.7` refuse `bad_args`; `0.7` derives exactly `docs/roadmap/0.7.md`, satisfying `isPlaceableSourcePath()`'s own predicate; a rotation that would empty the live file refuses. |
| INV-7 | A retitle changes title and slug and nothing else, and reports `slug` + `previous_slug`. |
| INV-8 | A dry run writes nothing and reports what the real run reports — for both operations. |
| INV-9 | `""`, `"   "`, a newline, and a punctuation-only title each refuse `bad_args`; an unresolvable slug refuses `section_not_found`; an absent argument refuses `missing_field`. |
| INV-10 | The render's publish gate is inherited but SCOPED (ANTS-4628, 2026-08-24): a rotation touches no item row, so an offending item under a *different* minor no longer refuses it. The rotation still runs the full `commitAndRender` sequence — the gate is not bypassed, it judges the empty set the rotation touched. |
| INV-11 | After a retitle, render → re-import leaves the section count and the retitled section's `section_id` unchanged. |
| INV-12 | Forward and backward slug collisions refuse; retitling the suffixed member and an unrelated section on the same fixture both succeed. |
| INV-13 | After a rotation, render → re-import leaves the section count and every moved `section_id` unchanged, each moved slug equal to `"0-7-"` + the slug derived within the move set alone; a move that would free a base a remaining live section was disambiguated out of refuses. |

## Not covered here, and why

- **`op_unsupported`** — needs a non-migrated fixture, which this suite has no
  harness for.
- **`render_failed` / `store_failed` / `write_failed`** — ANTS-3809's to test,
  and it does.
- **`setSectionSlug()` existing at all** — a compile failure, not a test.
- **§ 3.9's rotation-event rule** — delegated to `/bump`; nothing in this code
  can observe a version transition.
