# Wrapped-quotation matching — ANTS-4547 / ANTS-4550

**Status:** implemented (2026-08-20)

## Problem

Prose in this corpus is hard-wrapped at ~70 columns, so a quotation of
ordinary sentence length spans a line break. Every line-oriented matcher
then reports a miss on text that is present and exact:

- `workspace_search` runs ripgrep line by line, so a review gate
  verifying a cold reviewer's quotation gets a miss it cannot
  distinguish from a real absence — and dismisses a true finding
  (ANTS-4547). Measured by the reporter at 18% of quotations.
- `roadmap_log op:"amend_body"` matches within one physical line, so a
  wrapped phrase refuses `body_match_not_found` and the caller edits one
  paragraph with N single-line calls whose joint result is checked by
  nothing — the hazard ANTS-4097 names (ANTS-4550).

Both want the same normalisation, and the reference implementation of it
(a `sed | tr | grep` pipeline each review skill carries) was one-sided
and wrong for months. So the rule gets one owner.

## The rule

**A run of whitespace in the pattern matches a run of whitespace and
markdown blockquote markers in the text.** Everything else matches
literally. Normalisation is two-sided: a quotation pasted straight out of
a blockquote, newlines and `>` markers included, matches unmarked text.

`src/wrapmatch.{h,cpp}` owns it, in two forms driven by one string —
`WrapMatch::find()` for in-process matching, `WrapMatch::toRegex()` for
handing to ripgrep.

## Invariants

- **INV-1** — `find()` locates a needle whose whitespace spans a
  newline, and reports the span in the ORIGINAL text.
- **INV-2** — two-sided: a needle carrying its own newlines, indentation
  and `>` blockquote markers matches text that has none.
- **INV-3** — `find()` reports EVERY occurrence; deciding what a second
  one means is the caller's. An empty or whitespace-only needle finds
  nothing rather than everything.
- **INV-4** — the needle is literal: a regex metacharacter in it matches
  itself and nothing else.
- **INV-5** — `toRegex()` returns a pattern accepted by both PCRE2 and
  ripgrep's Rust regex that matches across the break; empty needle in →
  empty string out, which a caller must treat as a refusal.
- **INV-6** — `op:"amend_body"` amends a phrase spanning a hard-wrapped
  break in ONE call. The fallback is tried only after the exact
  single-line match finds nothing, so no currently-succeeding call
  changes behaviour.
- **INV-7** — the uniqueness guard survives the normalisation: a wrapped
  phrase occurring twice refuses `body_match_ambiguous` rather than
  clobbering the first.
- **INV-8** — `workspace_search match_wrapped:true` finds a quotation
  spanning a hard wrap and reports the line where the matched span
  STARTS. Off by default: the same search without it still misses.
- **INV-9** — `match_wrapped` is literal-mode only; with `regex:true` it
  refuses `bad_args` rather than re-flowing a caller's regex.
- **INV-10** — the wrapped pass DECLINES a span whose continuation lines carry
  structure a re-flow would destroy: column alignment (an internal run of 2+
  spaces) or the opening of a new list item. `Patch::structuredBlock` is set,
  `text` is left empty so no caller can half-apply, and `hits` stays 1 — the
  phrase *was* found, and calling it absent sends the caller hunting for text
  that is there.

  **Differing indentation is NOT the discriminator**, though that is what the
  report asked for (ANTS-4612). ANTS-3467's own fixture disproves it:

  ```
    - **Auto-lock timeout (the priority):** make it
      user-configurable (e.g. 1/5/10/30 min) so users tune it.
  ```

  is 2 spaces then 4 and is an ordinary hard wrap — the deeper indent is the
  bullet's hanging indent. The naive rule was written, and `Ants3467WrapSpan‑
  NowAmends` caught it in the same session.

- **INV-11** — end to end, `op:"amend_body"` refuses such a span with
  `body_match_wrapped_block` and leaves the file byte-identical. A distinct
  code because `wrapped_match:true` is *also* true on the benign case the pass
  was built for, so it cannot be branched on.

  Why it matters more than an ordinary refusal: the damage was **cumulative**.
  Measured on CFG-0196, three successive amend calls — each `ok:true` with the
  correct text echoed in `body_paragraph` — walked one row from 4 to 6 to 8 to
  12 leading spaces. On a store-backed project the file is a render, so hand
  repair is reverted by the next write; `amend_body` was both the only route
  back and the thing causing the damage.
