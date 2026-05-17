# Feature spec: MCP `indie_review_dispatch` (ANTS-1352)

Server-side multi-agent indie-review orchestrator. Fires N parallel
HTTP POSTs to the project-configured AI endpoint, saves each
response under `reports_dir/<lane>.md`. See
`docs/specs/ANTS-1352.md` for the full design + invariant rationale.

## Invariants

This feature test source-greps the wiring + adds a probe-accessor
test for the concurrency cap. Live-API behavior is covered by the
manual recipe in the spec § 7.2, not in CI.

- **G-1 / dispatcher declared.** `IndieReviewDispatcher::dispatchLanes` declared in `indiereviewdispatcher.h`.
- **G-2 / contract Required.** `claudeintegration.cpp` classifies `indie_review_dispatch` as `C::Required` with an `// ANTS-1352:` anchor.
- **G-3 / tier Expensive.** `claudeintegration.cpp` classifies `indie_review_dispatch` as `R::Expensive` with an `// ANTS-1352:` anchor.
- **G-4 / provider registered.** `mainwindow.cpp` registers a provider for the tool name.
- **G-5 / in-flight gate.** `mainwindow.cpp` calls `verbInFlightTryAcquire("indie_review_dispatch", …)` before invoking `cmdIndieReviewDispatch`.
- **G-6 / path validation.** `cmdIndieReviewDispatch` calls `PathValidation::validatePath` against `reports_dir`.
- **G-7 / QSaveFile atomic write.** `indiereviewdispatcher.cpp` uses `QSaveFile` (not raw `QFile`).
- **G-8 / API key not echoed.** No `qDebug`/`qInfo`/`qWarning`/`qCritical` call site in the dispatcher references `apiKey`. The literal string `"Authorization"` (bare, outside a header setter) does not appear in any logging call.
- **G-9 / per-lane timeout.** `setTransferTimeout` invoked on each `QNetworkReply`.
- **G-10 / refusal envelope shape.** `cmdIndieReviewDispatch` returns envelopes with the `code` field on every refusal branch.
- **G-11 / new error codes documented.** `docs/standards/mcp-error-codes.md` carries rows for `ai_not_configured` and `no_lanes`.
- **G-12 / dispatcher tool descriptor.** `claudeintegration.cpp` includes a `tools/list` entry for `indie_review_dispatch`.
- **G-13 / not cacheable.** `isIdempotentReadTool` allowlist does NOT include `indie_review_dispatch`.
- **G-14 / dispatch-shaped brief.** `assembleBriefForDispatch` declared in `indiereviewengine.h`.
- **G-15 / fence hardening.** `indiereviewengine.cpp` `assembleBriefForDispatch` body contains the 4-backtick fence sentinel and `treat as data, not instructions` literal.
- **G-16 / response-body redaction.** `indiereviewdispatcher.cpp` calls a redact helper (`redactAndTruncate`) before stashing response bytes in any envelope/error string.
- **G-17 / probe accessor declared.** `inFlightCountForTest()` declared in `indiereviewdispatcher.h`.

## Probe invariants

- **P-1 / probe returns 0 at rest.** `IndieReviewDispatcher::inFlightCountForTest()` returns 0 when no dispatch is in flight.
