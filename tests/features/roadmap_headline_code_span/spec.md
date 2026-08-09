# roadmap_headline_code_span — a bold marker inside backticks is not emphasis

**Contract:** ROADMAP.md ANTS-4066. No design spec — the defect and its fix are
one matcher, and `spec-format.md` § 1's triggers are all absent (one subsystem,
obvious in shape, cheap to redo).

## The defect

`rxBold` (`src/roadmapparse.cpp`) is `\*\*(.+?)\*\*` with no notion of Markdown
code spans, so a headline that legitimately *quotes* a bold marker inside
backticks ends at the first marker **inside that code span** and every character
after it is dropped from the stored headline. Nothing is reported — the
truncation is silent, which is the same failure class as ANTS-4065 § 1 reached
through a different matcher.

Measured across all 14 projects: 12 bullets, 11 in this one and 1 in
Music_Production. The clearest is ANTS-1702, whose headline quotes the C
signature `runMain(int argc, char **argv)` — those two asterisks are C syntax,
not emphasis, and the parser stops dead on them.

**The source is correct in all 12.** The fix belongs in the reader, never in the
text: rewriting the markdown would mean corrupting a C signature to suit a regex.

## The fix

Mask code spans before matching, so the bold run is located outside code. The
mask is **length-preserving** — only `*` inside a span becomes `x` — so the
match offsets index the original string and every capture is sliced from it. The
stored headline is therefore the author's bytes, mask or no mask.

## Cases

| Case | Asserts |
|---|---|
| `CSignatureInBackticksSurvives` | the ANTS-1702 shape keeps its whole headline |
| `QuotedBoldMarkerSurvives` | a bullet *about* the `**Layman:**` trailer keeps its whole headline |
| `PlainBoldHeadlineUnchanged` | the ordinary case is untouched — the containment claim |
| `RealBoldAfterACodeSpanStillCloses` | a code span that carries no `*` changes nothing |
| `UnterminatedBacktickDoesNotSwallowTheHeadline` | a stray backtick leaves the tail alone rather than masking to end-of-string |

## Verified red first

Against pre-change source the first two cases fail with a truncated headline;
the last three pass and are the regression half — they are what makes "only
these 12 change" testable rather than asserted.

## Corpus check

`tools/roadmap-conformance.py` counts the truncations directly. 11 before the
fix in this project, 0 after. That count is the acceptance gate, and it is
outside this test because it reads the live roadmap rather than a fixture.
