# roadmap_composed_trailers — feature contract

`composed_trailers` names the trailer lines the **render wrote**, so a caller can
tell a value that is in the file because the author typed it from one that is
there because the store composed it. `roadmap_log op:"amend_body"` depends on
that distinction: it cannot reach a line the render composed, however plainly
that line reads in `body`.

Filed as ANTS-5087, from the performance pass on the roadmap lane.

## The defect

The field is computed twice. `RoadmapRender::bulletText()` decides which trailer
lines to emit; `RoadmapSource`'s store-built record decided the same thing again
from its own copy of the five predicates. Two of the copies had drifted:

| Key | The render's rule | The read seam's rule |
|---|---|---|
| `source` | has a value, the body does not declare it, **and the provenance does not say `defaulted`** | has a value and the body does not declare it |
| `kind` | the body does not declare it **with a value the vocabulary recognises** | the body does not declare it |

So the field could name a `Source:` line the render withheld for an imported
default (ANTS-4065 § 2.4), and miss a `Kind:` line the render wrote past an
unrecognised declaration. The read seam's own comment claimed the record "cannot
disagree with the renderer about which lines it wrote", which is the claim that
was false.

The repair is one owner: `RoadmapRender::trailerLines()` makes the five
decisions, `bulletText()` emits from it, and the record reports its keys.

## Cases

Every case **publishes first** (`roadmap_log op:"render"`) and asserts the field
against the file the render wrote. A case that re-states the predicate would
pass by agreeing with itself, which is how the two copies drifted unnoticed.

| Case | Asserts |
|---|---|
| `Ants5087DefaultedSourceIsNotComposed` | `CTR-0001` declares no `Source:`, so the import supplies one and marks the provenance `defaulted`. The published block carries no `Source:` line, and `source` is not in the field. |
| `Ants5087ColumnOnlyKeysAreStillComposed` | The same item's `layman` and `kind` values live in the columns alone, the render wrote both lines, and both keys ARE in the field. Without this the case above would pass on an empty list. |
| `Ants5087UnrecognisedBodyKindIsStillComposed` | `CTR-0002`'s body declares `Kind:` with a value outside the vocabulary. The render writes its own line as well, so the block carries both — and `kind` is in the field. |
| `Ants5087FullyDeclaredBodyComposesNothing` | `CTR-0003` declares every key in its body, all recognised. The render writes no trailer line and the field is empty. This is what stops a predicate that simply says yes from passing the two cases above. |

## Fixture

Three shipped items, so the render's Layman gate judges none of them and each
case is about trailer lines alone. `CTR-0001` ends on its trailer run, which
ANTS-4506 strips from the stored body, so its values live in the columns;
`CTR-0002` and `CTR-0003` end on prose, so their declarations stay in the body
and shadow their columns.

## Must-fail-first — run, not asserted

Run against pre-fix source (2026-09-18). `Ants5087DefaultedSourceIsNotComposed`
and `Ants5087UnrecognisedBodyKindIsStillComposed` failed on their assertions;
the two that hold on either rule passed, which is what makes them the control.

## Would break this

- Computing the field from the store row again, anywhere, instead of asking
  `trailerLines()` → the two copies can drift a second time.
- Deriving it by DIFFING the rendered text against the body → a value that
  appears in both reads as composed in neither.
- Dropping the `defaulted` rider from the render → the import's own default is
  written back into the file, which is the loss ANTS-4065 § 2.4 measured.
- Letting an unrecognised `Kind:` declaration suppress the render's line → the
  recognised column is dropped and the next parse adopts the fragment.
