# markdownscan — shared CommonMark fence primitives (ANTS-3603)

`src/markdownscan.{h,cpp}` hoists the fence-scanning helpers that had been
copied verbatim into `feedbackfile.cpp` and `speclog.cpp` (Rule of Three: the
second call-site was the trigger). Qt6::Core-only, in `ants_core_lib`.

This test locks the `fenceMask` contract directly — the callers'
existing suites (`FeedbackV2Delta`, `McpSpecLog`) cover `fenceOpenerChar`
through their own behaviour.

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

## Test

`tests/features/markdownscan/test_markdownscan.cpp`, compiled into the
`test_core` bundle (Core-only, no Widgets/RemoteControl link). Must fail against
a deliberately-broken `fenceMask` (verified by sabotage before restore).
