# ANTS-4504 — a trailer key inside an inline code span is a mention, not a declaration

## Contract

`RoadmapParse::trailerValuesIn()` reads the five trailer keys — `Kind:`,
`Lanes:`, `Layman:`, `Evidence:`, `Source:` — out of a bullet's body. A bullet
that *quotes* one of those keys inside backticks is discussing the format, not
declaring a value, and the parser must not read it as one.

The guard shipped for that was three FIXED-LENGTH negative lookbehinds on
`rxSource()` / `rxLanes()` / `rxKind()`:

    (?<!`)(?<!`\*)(?<!`\*\*)

Each sees only the one to three characters immediately before the label, so it
catches a span that OPENS immediately before the key and nothing else. A span
that opened several words — or several lines — earlier is invisible to it, and
the key inside it parses as a declaration.

Observed on this project's own `ANTS-3808`, whose body quotes `rxTrailerKey()`'s
corpus note: a backticked example carrying **two** keys, wrapped by the source
across two lines. The second key sits at end-of-line preceded by a space, no
lookbehind fires, and the bullet imported a `Lanes:` value it never declared.
The parser's own comment predicts exactly this: *"the corpus most likely to
write that sentence is the one documenting the format."*

**The rule.** Before matching, the body is reduced to a length-preserving
MASKED view in which every inline code span — its delimiters included — is
blanked. The matchers run against the mask; every captured value is sliced from
the ORIGINAL body at the mask's offsets, so a value that legitimately contains
backticks is stored verbatim. This is the same shape ANTS-4066 already uses for
the bold-headline matcher, one level up.

**Span boundaries are `MarkdownScan::codeSpans()`'s**, not a fourth hand-rolled
pairing. That primitive already states CommonMark's rules once for the project:
a run pairs only with an equal-length run; a run with no partner is literal
text and opens nothing; a span may cross a newline but never a blank line or a
fence line; a backtick run inside a fenced block is not a delimiter. The
per-bullet mask ANTS-4066 added to `parseBullets()` pairs backticks one at a
time and says so — it is a known miss, not a rule, and it is not the thing to
copy.

**Fenced blocks are deliberately NOT masked.** A fence mask is computed because
`codeSpans()` requires one, but a key on a fenced line keeps parsing as it does
today. That is a sibling defect of the same class with no measurement behind
it, and widening the blast radius of a corpus-moving change to cover it is not
this item's job.

## Invariants

| ID | Invariant |
|----|-----------|
| INV-1 | A trailer key inside an inline code span yields no value for that key, however far ahead of it the span opened. |
| INV-2 | The rule holds when the span WRAPS across a line break — the case the fixed-length lookbehinds cannot express at any width. |
| INV-3 | A real declaration outside any span still parses, on the same body that carries a quoted one. |
| INV-4 | A captured value containing a code span is stored VERBATIM: the mask decides where a match is, never what it says. |
| INV-5 | A backtick run with no equal-length partner masks nothing — one stray backtick cannot swallow the rest of the body and silence its real trailers. |
| INV-6 | An ordinary bullet, carrying no backtick at all, parses byte-identically. |

## Test

`tests/features/roadmap_trailer_code_span/test_roadmap_trailer_code_span.cpp`
(bundle `test_core`). Drives `RoadmapParse::trailerValuesIn()` and
`RoadmapParse::parseBullets()` on hand-written markdown — the level the defect
lives at, before any store or migration is involved.
