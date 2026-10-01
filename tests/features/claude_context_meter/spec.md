# Claude context meter (status bar)

The status bar shows how full the focused Claude Code session's context is.
It went blank because the percentage was computed from `usage.input_tokens`
alone: with prompt caching that field is a handful of tokens, so the meter
read 0% and hid itself. It also assumed a 200,000-token window. The user is
partially sighted, so the meter must be large and readable.

## Invariants

- **INV-1** — a transcript's context size is `input_tokens +
  cache_creation_input_tokens + cache_read_input_tokens` of the newest event
  carrying `message.usage`. *Test:* `Inv1TokensSumAllThreeFields`.
- **INV-2** — the percentage is `min(100, tokens * 100 / window)`, where the
  window defaults to 1,000,000. Changing the window recomputes it and emits
  `contextUpdated`. *Test:* `Inv2PercentAgainstWindow`.
- **INV-3** — `claude.context_window_tokens` defaults to 1,000,000 and is
  clamped to [10,000, 10,000,000]. *Test:* `Inv3ConfigDefaultAndClamp`.
- **INV-4** — the hover text names the tokens used, the window and the
  percentage, with thousands separators, and advises `/compact` from 80%.
  *Test:* `Inv4TooltipNamesTheNumbers`.
- **INV-5** — the meter is shown whenever the session has a non-zero token
  count, even when the percentage rounds to 0. *Test:*
  `Inv5ShownOnTokensNotPercent` (source scrape of the `contextUpdated`
  handler).
- **INV-6** — the meter is readable: at least 150×22 px, bold text of at
  least 13 px reading `Context N%`, drawn with a dark outline so it contrasts
  with every fill colour. *Test:* `Inv6ReadableMeter` (source scrape).

## Reload

The window is a Settings field (Claude tab). Apply and an external
`config.json` edit both call `ClaudeIntegration::setContextWindowTokens`, so
the meter updates with no relaunch. The meter code itself ships in the
terminal binary, so it first appears after the next relaunch.

## Build

Compiled into `test_claude`.
