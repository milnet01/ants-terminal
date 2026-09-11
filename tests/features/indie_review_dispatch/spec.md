# Feature spec: MCP `indie_review_dispatch` (ANTS-1352)

Server-side multi-agent indie-review orchestrator. Fires N parallel
HTTP POSTs to the project-configured AI endpoint, saves each
response under `reports_dir/<lane>.md`. See
`docs/specs/ANTS-1352.md` for the full design + invariant rationale.

## Invariants

This feature test source-greps the wiring + adds a probe-accessor
test for the concurrency cap, plus behavioral tests (INV-1..INV-4,
ANTS-5018) that drive `dispatchLanes` directly — against
refusal-triggering endpoints, and against a loopback redirect
fixture. No live remote API access. Other live-API behavior is
covered by the manual recipe in the spec § 7.2, not in CI.

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
- **G-18 / plaintext-prompt warning key.** `cmdIndieReviewDispatch` sets the success envelope's `warning` from `LlmClient::plaintextPromptWarning(endpoint, dr.apiKey)`, after the refusal-branch return, and sets no other key named `warning`. See `docs/specs/ANTS-5010-plaintext-prompt-warning.md` § 2.5 and § 3 INV-5 — this handler needs a `MainWindow`, so the check is a source-grep rather than a behavioural test.
- **G-19 / worker never marshals (ANTS-5024).** The provider joins its worker with `QThread::wait()` on the GUI thread, so `cmdIndieReviewDispatch` calls none of `resolveRootCanonical`, `resolveCallerCwdRoot`, `ants::onGuiThread` or `m_main->`. The provider passes the root it resolved on the GUI thread as `cmdIndieReviewDispatch(args, canon)`. A marshal from that worker is a blocking queued call the parked GUI thread never serves. Source-grep with comments stripped: reproducing the hang needs a live `MainWindow`.

### ANTS-5018 — shared AI egress checks

`dispatchLanes` runs `LlmClient::endpointEgressError`, the validator
every other AI send path runs (ANTS-2121), in place of its old
scheme-only check. A refusal keeps that check's `bad_args` code. Each
request also sets `ManualRedirectPolicy` (ANTS-1798). The test endpoints
are unreachable or loopback, so no test run sends a request to a real
host. The paired test file is each invariant's test surface.

- **INV-1** — a non-empty `apiKey` with a remote plain-`http` endpoint is refused: `ok` is false, the error names the cleartext refusal, and `reports_dir` is never created. An empty key against remote plain-http is allowed, per ANTS-5010.
- **INV-2** — a private or link-local IP-literal endpoint is refused, with or without a key: the error names the SSRF refusal, and `reports_dir` is never created.
- **INV-3** — an endpoint embedding URL userinfo is refused, with or without a key: the error names the credential refusal, and `reports_dir` is never created.
- **INV-4** — a redirect is not followed: against a loopback server answering `307` with a `Location` at a second loopback listener, the second listener receives no request.

## Probe invariants

- **P-1 / probe returns 0 at rest.** `IndieReviewDispatcher::inFlightCountForTest()` returns 0 when no dispatch is in flight.
