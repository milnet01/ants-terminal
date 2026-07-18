# tokens_saved_chip — feature-conformance contract (ANTS-3572)

Source-grep wiring contract for the MCP tokens-saved status-bar chip +
persisted month / YTD / all-time aggregate. Full design:
`docs/specs/ANTS-3572.md`. The pure fold/sum/humanize math is covered by
`tests/features/token_usage_engine/` (FOLD-*/HUMAN-1); the config-key
defaults by `tests/features/auto_switch_surfacing/`. This bundle locks the
cross-file wiring that a pure test can't reach.

- **TSC-1 (INV-6)** — `ClaudeIntegration` declares `tokensSavedUpdated(qint64)`
  and `tokenSessionEnding()`; the report-valued (non-zero) `emit
  tokensSavedUpdated` lives in `recordDispatch` (the single dispatch hook).
- **TSC-2 (INV-2/INV-3)** — `endTokenSession()` is the *only* body containing
  `m_tokenUsage.reset()` and emits `tokenSessionEnding` before it; the
  initialize handler and `resetTokenUsage()` both delegate to it; `MainWindow`
  defines `foldTokenSavingsIntoConfig`, calls `endTokenSession()` in
  `closeEvent`, and connects `tokenSessionEnding` to the fold slot with no
  `Qt::QueuedConnection` argument.
- **TSC-3 (INV-5/INV-11)** — the chip update slot gates `show()` on
  `claudeTokensSavedChipEnabled()` AND a `> 0` session, with a `hide()` path;
  the chip is added after the context bar.
- **TSC-4 (INV-7)** — no persistence (`setClaudeTokensSaved…`) in the dispatch
  layer.
- **TSC-5 (INV-10)** — the four aggregate fields are set on the verb response;
  they are absent from the `token_usage` input schema.
- **TSC-6 (INV-4)** — the fold derives its bucket key with
  `toString("yyyy-MM")`.
- **TSC-7 (INV-12)** — the fold never references `totalFailedBytes`.
- **TSC-8 (INV-1)** — the summary combines a stored getter with the live
  session total.
