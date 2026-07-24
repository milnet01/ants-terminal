# markdownscan — shared CommonMark fence primitives (ANTS-3603)

`src/markdownscan.{h,cpp}` hoists the fence-scanning helpers that had been
copied verbatim into `feedbackfile.cpp` and `speclog.cpp` (Rule of Three: the
second call-site was the trigger). Qt6::Core-only, in `ants_core_lib`.

This test locks the `fenceMask` contract directly — the callers'
existing suites (`FeedbackV2Delta`, `McpSpecLog`) cover `fenceOpenerChar`
through their own behaviour.

## Contract

- **INV-1** — `fenceRe()` matches `^ {0,3}(```|~~~)`: a fence opener is three
  backticks or three tildes with **0–3 leading spaces**, space-only indent (a
  tab must not open a fence — ANTS-3598). *Test:* `fenceOpenerChar` returns the
  fence char for a 0/1/2/3-space-indented opener and null for a
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

## Test

`tests/features/markdownscan/test_markdownscan.cpp`, compiled into the
`test_core` bundle (Core-only, no Widgets/RemoteControl link). Must fail against
a deliberately-broken `fenceMask` (verified by sabotage before restore).
