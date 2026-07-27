# doc_citations_scan — the `path:line` grammar and scrape rule (ANTS-3653)

Contract: [`docs/specs/ANTS-3653.md`](../../../docs/specs/ANTS-3653.md). This
file is the test-surface map; the spec owns the grammar and the reasoning.

`src/doccitations.{h,cpp}` — `DocCitations::scan(lines, opts)` is a **pure
function**: document text in, citation tokens out. No filesystem, no path
resolution, no status taxonomy, no response caps. Qt6::Core-only, in
`ants_core_lib` beside `MarkdownScan`, whose `fenceMask` and `codeSpans`
(ANTS-3649) it consumes.

**Every fixture here is a string literal and an expected token list — no
filesystem access at all.** That is the point of the split from ANTS-3636 and
should stay true: a scan fixture that needs a temp dir has drifted into the read
path's lane.

## Contract

The invariant ids are **non-contiguous by design** — they keep the numbers they
held in ANTS-3636, where each is now a tombstone, so every existing citation of
them still resolves. A gap is not a missing invariant.

- **INV-1** — a citation inside a **fenced** block is not harvested; one in an
  **indented** block is (`fenceMask` models no indented code blocks). *Test:*
  `` `src/a.cpp:1` `` inside a ```` ```cpp ```` fence, `src/a.cpp:2` in prose and
  a 4-space-indented `src/a.cpp:6` → **two** citations, the fenced one and only
  the fenced one dropped.
- **INV-2** — a citation **is** harvested from an inline code span (the inverse
  of `doc_integrity`'s link rule), from bold, from link text and from bare
  prose. *Test:* `` `src/a.cpp:2` ``, ``**`src/a.cpp:3`**``,
  ``[`src/a.cpp:4`](x)`` and a bare `src/a.cpp:5` → four citations.
- **INV-3** — both range separators parse: `:10-12` and `:10–12` (en dash).
  *Test:* the two forms → two citations with identical loci.
- **INV-9** — trailing loci are dropped, not guessed. *Test:*
  `src/a.cpp:262/265/286` and `src/a.cpp:5669+` → `startLine` 262 and 5669, both
  `partial`.
- **INV-10** — `~` on a **kept** locus sets `approximate` and changes nothing
  else; on a **dropped** trailing locus it does not. *Test:* two fixtures
  identical but for the tilde → equal in every field but `approximate` and
  `raw`; `src/a.cpp:208-~228` → set; `src/a.cpp:262/~265` → `partial` set,
  `approximate` clear.
- **INV-24** — a malformed locus is recognised (stage 1) and then rejected
  (stage 2). *Test:* `src/a.cpp:0` and `src/a.cpp:10-5` → two `unparsed`
  entries with reason `bad_locus`, zero citations. Plus a `` `~:11985` `` in the
  same fixture → **neither** array grows: the tilde precedes the colon, which
  matches no production, so the token is invisible to both (the measured gap,
  ANTS-3646). Asserted because "invisible" is otherwise indistinguishable from
  "not yet implemented".
- **INV-29** — two citations on one line, and two occurrences of the same
  citation, are two entries. *Test:* `src/a.cpp:1` twice on one line → two
  tokens.
- **INV-32** — a bare `:N` is a continuation **only when it fills a whole inline
  code span**, tested after CommonMark's one-space strip. *Test:*
  `` `src/a.cpp:1` `` then `` `09:45` `` (contains `:45` but does not fill the
  span) and a bare-prose ratio `3:1` → one token, no continuation; a second
  fixture writing `` ` :2 ` `` → two tokens, the second marked a continuation.
- **INV-33** — `citations` and `unparsed` are both in document order: ascending
  `docLine`, then ascending column, stable across runs. *Test:* two citations on
  one line and one on an earlier line → line-then-column order, identical on a
  second run. Plus one inside a code span that **opens on one line and closes on
  the next**, its own text beginning on the second → `docLine` is that second
  line, not the span's opening line.
- **INV-36** — an unterminated fence sets `unterminatedFence` to the 1-based
  line of the unclosed opener, **from `MarkdownScan::fenceMask(lines, &opener)`
  and never inferred from the mask**; `-1` otherwise. *Test:* an unclosed
  ```` ``` ```` at line 4 with a citation at line 9 → `4`, zero citations; the
  same fixture closed → `-1`, one citation; plus a fence opening at line 4 and
  closing on the doc's **final** line → `-1`.
- **INV-39** — `partial` is set by a `/`-separated extra locus or a trailing
  `+`, and by nothing else — `trailing` is exhaustive, so every other character
  ends the token. *Test:* `src/a.cpp:262/265`, `src/a.cpp:5669+` and a
  **bare-prose** `see src/a.cpp:12.` → the first two `partial`, the third clear
  with `endLine` 12. The third must be bare prose: inside a code span the token
  ends at the closing backtick, the `.` is never adjacent, and the assertion
  would hold against an implementation with no exhaustiveness rule at all.
- **INV-40** — a digit run longer than `maxLocusDigits` is **recognised and then
  rejected**: stage 2 tests the run's *length* before converting, so no
  over-large value is ever computed. *Test:* `src/a.cpp:99999999999999` →
  exactly one `unparsed`/`bad_locus` and zero citations; plus a run of exactly
  `maxLocusDigits` digits → a citation, fixing the boundary from both sides.

## Trap cases

What a plausible implementation gets away with if the fixture is dropped:

| INV | What passes without it |
|---|---|
| 36 | an implementation deriving `unterminatedFence` from the mask — that mask also ends in a run of `true`, so it passes both the unclosed and the closed case and fails only the closes-on-the-final-line one |
| 40 | a strict `[0-9]{1,maxLocusDigits}(?![0-9])` production, which also yields zero citations — it just emits nothing at all, reporting neither |
| 32 | a scan that accepts any bare `:N` — it still passes every path-bearing fixture, and only the `09:45` / `3:1` row separates it |

## Test

`tests/features/doc_citations_scan/test_doc_citations_scan.cpp`, compiled into
the `test_core` bundle (Core-only, no Widgets/RemoteControl link) per
`tests/features/README.md` — not a standalone `add_executable`. Verified RED
against feature-absent code before the implementation landed.

## Out of scope

Path resolution, the basename ladder, `status`, the line cache, response caps
and paging — all ANTS-3636. Which antecedent a continuation inherits from is
that spec's sticky rule; this layer only marks the continuation. Anchor-symbol
drift is ANTS-3654.
