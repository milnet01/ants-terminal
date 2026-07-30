# markdownscan — shared CommonMark fence primitives (ANTS-3603)

`src/markdownscan.{h,cpp}` hoists the fence-scanning helpers that had been
copied verbatim into `feedbackfile.cpp` and `speclog.cpp` (Rule of Three: the
second call-site was the trigger), and — since ANTS-3649 — the inline-code-span
scanner hoisted out of `DocIntegrity::maskInlineCode`. Qt6::Core-only, in
`ants_core_lib`.

This test locks the `fenceMask` and `codeSpans` contracts directly — the
callers' existing suites (`FeedbackV2Delta`, `McpSpecLog`, `DocIntegrity`)
cover `fenceOpenerChar` and the masking policy through their own behaviour.

## Contract

- **INV-1** — `fenceRe()` matches `^ {0,3}(```+(?!.*`)|~~~+)`: a fence opener is
  three-or-more backticks or tildes with **0–3 leading spaces**, space-only
  indent (a tab must not open a fence — ANTS-3598). *Test:* `fenceOpenerChar`
  returns the fence char for a 0/1/2/3-space-indented opener and null for a
  4-space-indented line or a tab-indented line.
- **INV-2** — `fenceMask` masks the opener, every body line, and the closer
  line **true**; prose outside any fence is **false**. *Test:* a fenced block
  surrounded by prose yields the expected true-run bracketed by false.
- **INV-3** — a fence closes only on a line whose fence char **matches** the
  opener's: a ``` block is not closed by a ~~~ line (and vice-versa). *Test:* a
  ``` opener followed by a ~~~ line keeps the fence open (the ~~~ line masks
  true as body).
- **INV-4** — an **unterminated** fence masks true to end-of-input (CommonMark
  leniency, matching the prior local scanners). *Test:* an opener with no closer
  masks every subsequent line true.
- **INV-5** — the returned mask has exactly one entry per input line. *Test:*
  `mask.size() == lines.size()` on every case.
- **INV-6** (ANTS-3638) — `fenceOpenerChar` hand-scans the indent instead of
  matching `fenceRe()`, because its limit is now a parameter (default 3).
  `fenceRe()` remains the written statement of the top-level rule, so the two
  must **agree on every line** at the default limit. *Test:* a table of
  openers, near-misses and non-fences asserts `fenceRe().hasMatch()` ⟺
  non-null `fenceOpenerChar`, with the same fence char.
- **INV-7** (ANTS-3638) — `fenceMask` tracks **list containers**: CommonMark
  re-bases a list item's content at its marker's content column, so a fence
  inside an item opens at up to 3 spaces past *that* column, not past 0.
  Leaving the item restores the outer allowance. *Test:* a 4-space fence
  under a `- ` bullet masks true; a 4-space ``` line after the list has ended
  does not.

  INV-1's 0–3 rule is the top-level case of INV-7 (content column 0), not a
  separate rule. Only `fenceMask` carries this — the stateless
  `fenceOpenerChar` callers (`feedbackfile`, `speclog`, `docsindex`) keep the
  top-level limit, because container tracking needs state they do not hold.

- **INV-8** (ANTS-3655) — a **backtick** fence's info string may contain no
  backtick (CommonMark § 4.5), so a line that is really a multi-backtick inline
  code span opens no fence. Tilde fences are exempt — their info string may hold
  a backtick. *Test:* ```` ```` ``` ```` ```` and `` ``` `x` ``` `` return null
  while a bare ```` ```` ```` and ```` ```cpp ```` still return the fence char;
  and a document whose second line is that span masks **entirely false**, so the
  heading below it stays visible. Pre-fix the opener never closed and masked to
  end-of-input, which silently truncated every consumer (`feedbackfile`,
  `speclog`, `featurecoverage`, `docsindex`, `docintegrity`) on any spec that
  documents fence syntax by example.

- **INV-9** (ANTS-3649) — `fenceMask(lines, &opener)` reports the **1-based
  line of an unclosed fence opener**, and `-1` when every fence closes; the
  1-argument overload is `fenceMask(lines, nullptr)`. The fact is *not*
  recoverable from the mask — a doc ending in a fence **closer** and a doc
  ending inside an **unclosed** fence both end in a run of `true` — so any rule
  inferred from the mask alone false-alarms on every doc that ends in a properly
  closed code block. *Test:* an unclosed opener at line 2 → `2`; the same
  fixture closed on the document's **final** line → `-1`; a `~~~` line inside an
  open ``` block → the outer opener, not the inner line; `nullptr` and an empty
  document are both accepted.
- **INV-10** (ANTS-3649) — `codeSpans` returns each inline span's **content**
  bounds (`[startCol, endCol)`, delimiters excluded) plus `delimLen`, so the
  opening run starts at `startCol - delimLen` and the closing run ends at
  `endCol + delimLen`. Content is returned **verbatim**: CommonMark's one-space
  strip is the caller's job. *Test:* `` `code` `` → content `[3,7)`, `delimLen`
  1; `` ``a`b`` `` → `delimLen` 2 with the inner backtick **not** closing it
  (the equal-run rule); two spans on one line → two spans; `` ` :45 ` `` → the
  content keeps both spaces.
- **INV-11** (ANTS-3649) — the scan is **whole-document**: a span may cross a
  newline (CommonMark § 6.1), and its closing run is searched forward but never
  past a **blank** line or a **fence** line. A run with no equal-length partner
  is literal text and yields no span; lines inside a fence yield none either.
  *Test:* a span opening on one line and closing on the next → one span with
  `startLine` 0 and `endLine` 1; the same fixture with a blank line, and with a
  fenced block, between → zero; a stray backtick → zero; `` `code` `` inside a
  fence → zero.

  This is `DocIntegrity::maskInlineCode` hoisted (ANTS-3635a), so the boundary
  rule is load-bearing rather than incidental: it decides where a span *ends*,
  which is what `doc_citations`' "a continuation fills a whole span" branches
  on. `maskInlineCode` is now a policy wrapper over `codeSpans` — it blanks
  what this verb locates — and its own INV-5b–5e cases in
  `tests/features/doc_integrity/` are the no-regression half of the hoist.

- **INV-12** (ANTS-3740) — `headings` returns every ATX heading in document
  order as `{line, level, text, slug, endLine}`, all lines **1-based**.
  `endLine` is the line before the next heading of the same or a **higher**
  level, or `lines.size()` for the final section — so a section owns its deeper
  subsections. `headingLevel` accepts a 1–6 `#` run followed by a space or
  end-of-line and nothing else. Fence-aware via `fenceMask`, so INV-8's
  fence-teaching document keeps the headings below it. *Test:* a 4-heading
  fixture asserts each level, line and span; a fenced `# …` and a
  ```` ``` ````-demonstrating inline span both yield no heading;
  `#######` / `#nospace` → level 0.
- **INV-13** (ANTS-3740) — `headingSlug` is the key `read_region section=`
  resolves: lowercase, every run of non-alphanumerics to a single `-`,
  leading/trailing `-` trimmed, and **idempotent** (the slug of a slug is
  itself), so a caller may pass either the heading text or its slug. This is
  deliberately **not** `DocIntegrity::gfmSlug`, which implements GitHub's
  *anchor* rules and disagrees on real corpus headings (`compact_resolved` →
  `compact_resolved` there, `compact-resolved` here). They answer different
  questions; a caller that wants a section it can then **fetch** needs this
  one. *Test:* `4.2 Emission model` → `4-2-emission-model`, that slug maps to
  itself, `2.1 a_b` → `2-1-a-b`, whitespace-only → empty.

  `headings` and `headingSlug` were hoisted out of
  `ReadRegion::resolveSection` when `cold_eyes_brief`'s `section_index` became
  the second consumer. That resolver now calls them, so the slugs the brief
  publishes and the slugs `read_region` accepts cannot drift apart — which is
  the reason for the hoist, not tidiness.

## Test

`tests/features/markdownscan/test_markdownscan.cpp`, compiled into the
`test_core` bundle (Core-only, no Widgets/RemoteControl link). Must fail against
a deliberately-broken `fenceMask` (verified by sabotage before restore).
