# roadmap_body_element_roundtrip — the element vocabulary survives the store

**Item:** ANTS-4616 · **Bundle:** `test_claude` ·
**Suite:** `roadmap_body_element_roundtrip` · **Label:** `features`

## Why this exists

Once a project is store-backed the whole-file re-render is unavoidable and
fires on any write, so **every element type a bullet body can carry has to
survive it.** Nested sub-bullet indentation was measured and does survive
(ANTS-4558) — the reporter withdrew that half of their finding after checking.
Markdown tables, fenced code blocks and blockquotes were *assumed* and never
exercised.

The argument for not assuming is in the same batch: ANTS-4596 was a real prose
loss that shipped, and was caught only because a contributor grepped for a
sentence they remembered writing.

## What the run found

**All four invariants passed on the first run.** The store holds these three
element types faithfully and the renderer emits them unchanged. That is the
answer the item asked for; what changes is that it is now a gate rather than an
assumption, so a future change to the render or the parse cannot quietly break
one.

## Method — and the trap it avoids

Each case writes markdown, migrates it, **deletes the seed file**, renders the
store back out, and asserts against the file that lands.

Deleting the seed is the load-bearing step. Without it the case cannot
distinguish a faithful round trip from a render that never ran — both leave the
author's own bytes on disk, and the suite would pass vacuously **for exactly
the reason it exists to rule out**. With the file gone, anything the assertions
find came out of the store.

## Invariants

- **INV-1 — a markdown TABLE survives:** header row, separator row, every data
  row, in order. A table whose separator is lost stops being a table and
  renders as prose with stray pipes.
- **INV-2 — a FENCED CODE BLOCK survives, fences included.** Asserted as a
  *count* of exactly two: an unbalanced fence is the worst outcome of the
  three, because it swallows everything after it when the file is next read.
- **INV-3 — a BLOCKQUOTE survives with its markers,** not merely its text. The
  wrapped-match rule reads `>` as whitespace by design (ANTS-4547), which makes
  a blockquote the shape most likely to be normalised away somewhere here.
- **INV-4 — all three in one body keep their relative order.** Each case above
  proves its element in isolation; a pipeline can still lose one only in the
  presence of another, and a real body is not a single-element fixture.
