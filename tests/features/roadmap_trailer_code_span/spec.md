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
pairing. That primitive states the pairing rules once for the project:
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
| INV-7 (ANTS-4598) | A line whose runs do not all pair does not unmask a key quoted BELOW it. The leftover run stays literal rather than taking the opener of a balanced line further down, which would invert the mask for the rest of the body. |
| INV-8 (ANTS-4598) | INV-5's other half: a stray run does not swallow the trailer block below it merely because some later line quotes something. |
| INV-9 (ANTS-4598) | A run INSIDE an already-paired span is span content, never a delimiter — a ``` quoted within a `…` span opens nothing. |
| INV-10 (ANTS-4526) | A trailer key on a FENCED line is a quoted example, not a declaration — INV-1's sibling for the block form. |
| INV-11 (ANTS-4526) | The same holds for a TILDE fence, which carries no backtick and so must not be skipped by the early-out. |
| INV-12 (ANTS-4526) | Containment, the fenced twin of INV-3: quoting keys inside a fence does not cost the bullet its own declarations written plainly below. |

## Test

`tests/features/roadmap_trailer_code_span/test_roadmap_trailer_code_span.cpp`
(bundle `test_core`). Drives `RoadmapParse::trailerValuesIn()` and
`RoadmapParse::parseBullets()` on hand-written markdown — the level the defect
lives at, before any store or migration is involved.

## ANTS-4598 — the pairing order

INV-1 to INV-6 assume `codeSpans()` puts the span where the author wrote it.
Until 2026-08-20 it paired runs in one forward sweep over the whole body, so a
run left over on one line took as its closer the OPENER of a balanced line
further down. Past that point the mask is INVERTED: text the author quoted
reads as prose, and prose reads as quoted — so a quoted key downstream parses
as a declaration, which is INV-1 failing on a body that satisfies its shape.

The repair is two passes: pair within a line first, then join what is left over
across lines. A hard-wrapped span leaves a leftover on BOTH lines and still
joins; a line that balances on its own has nothing to donate. Measured over the
machine-global store's 4291 bodies — four bullets parse differently, all four
toward the declaration their author wrote, none loses a value, and the corpus's
409 legitimately wrapped spans are unchanged.

INV-9 is the no-regression half and is not hypothetical: the sweep consumed
runs inside a span by resuming at the closing run, and a first draft that
collected runs up front without saying so turned the ``` quoted inside a `…`
span into a cross-line opener, silencing the declarations between them.

INV-10 to INV-12 close the half ANTS-4504 deferred. It masked inline spans
only and recorded why: nothing had measured how many corpus bullets carry a
fenced trailer key, and masking one whose only declaration is fenced would
lose a real value. That measurement has now been taken across every body in
the machine-global store, and the answer is none — so masking costs the
corpus nothing and guards the shape going forward.

It was taken by LINKING `MarkdownScan::fenceMask` rather than reimplementing
its rule. A hand-rolled fence test written for the measurement reported a
false positive on a multi-backtick inline span — the identical mistake
ANTS-4403 was filed for, made again in the tool measuring it. The real
primitive treats that span as a paragraph, as CommonMark § 4.5 requires.

INV-12 is the control: it passes both before and after the fix, so the fix
cannot have been bought by masking everything.
