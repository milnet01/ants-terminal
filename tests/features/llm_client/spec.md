# Feature: LlmClient — reusable OpenAI-compatible streaming client

## Problem

`AiDialog`'s network code (request build, SSE drain, 10 MiB caps, scheme
allowlist, secret scrub, transfer timeout) is welded to the widget and
single-flight — unusable by the review-dialog family (ANTS-1721/1722).
ANTS-1727 § 2.1 extracts it into a widget-free `LlmClient`
(`src/llmclient.{h,cpp}`, ants_core_lib, Qt6::Core + Qt6::Network) with
testable static kernels so the parse / scrub / cap logic is verifiable
without a live network.

## Invariants under test (ANTS-1727)

- **INV-2** — `isEndpointAllowed` accepts only http/https; rejects
  `file://`, `gopher://`, a scheme-less host, and empty.
- **INV-3** — `buildRequestBody` with `scrubSecrets` runs both prompts
  through `SecretRedact`; the serialised body contains no scrubbed secret
  and `redactedCount > 0`.
- **INV-4** — accumulated content is capped at `kMaxBytes` (10 MiB) with
  `truncated` set on overflow (and the marker emitted once); the SSE line
  buffer is capped at the same bound. ANTS-1846 — the accumulator cap is
  byte-accurate: `accumulateCapped` tracks the running UTF-8 byte total
  (advancing by each delta's UTF-8 length, not its UTF-16 unit count), so the
  cap bounds decoded content regardless of codepoint width.
- **INV-2b** (ANTS-1846) — `isPlaintextRemote` treats the entire 127.0.0.0/8
  loopback range + `::1` as local (via `QHostAddress::isLoopback`), matching
  `isEndpointHostBlocked`; e.g. `http://127.0.0.2` is not flagged as a
  cleartext-remote endpoint.
- **INV-5** — `sseContentDelta` returns content for a
  `data:{…delta.content…}` line and empty for `[DONE]` / non-content / non-
  data lines.
- **INV-6** (ANTS-2108) — `send()` is the single egress chokepoint and
  backstops the cleartext-Bearer guard: with a non-empty `apiKey` and an
  `isPlaintextRemote` endpoint it refuses via `emitDeferredError` (emitting
  `finished(ok=false)` whose error names "cleartext") and never opens a
  network reply. Covers the auditdialog batch path + the v2 review dialogs
  (coldeyesdialog → ReviewDialogBase) that never pre-checked
  `isPlaintextRemote`. Loopback/localhost stay exempt.
- **INV-7** (ANTS-2109 H1) — `send()` refuses an endpoint whose URL embeds
  userinfo (`user:pass@host`), scheme-agnostic, before posting — those
  credentials would otherwise egress as an `Authorization: Basic` header,
  unscrubbed and invisible to the host-keyed scheme/SSRF gates. The refusal
  error names "credentials".
- **INV-8** (ANTS-2121) — `endpointEgressError` is the single shared egress
  validator: it runs, in order, the http/https scheme allowlist, the
  URL-userinfo refusal (ANTS-2109 H1), the SSRF host-block (ANTS-1746), and the
  cleartext-remote Bearer refusal (ANTS-1826/2108, gated on a non-empty key),
  returning an empty string on pass or a prefix-free reason on the first failure.
  `send()` calls it (preserving its verbatim `AI endpoint rejected — …`
  messages), and the AuditDialog AI-triage POSTs (`requestAiTriage` +
  `requestAiTriageBatch`) — which build a raw `QNetworkAccessManager` instead of
  routing through `send()` — call it too and additionally set
  `QNetworkRequest::ManualRedirectPolicy` (ANTS-1798), so both channels enforce
  the identical policy. A source-grep guard locks the auditdialog wiring against
  regressing to the former scheme+cleartext-only subset.
- **INV-2c** (ANTS-2109 H2) — `isEndpointHostBlocked` is documented as
  IP-literal-only: a DNS hostname is NOT resolved and passes through. The
  guarantee is not overstated (no claim of hostname SSRF protection); the
  residual exposure is bounded by hard-refused redirects + the 0600
  user-owned `ai_endpoint` trust model.
- **INV-16** — `llmclient` / `llmdispatcher` / `briefdispatch` headers +
  sources include no Qt Widgets header (widget-free discipline; the lib
  links Qt6::Widgets PUBLIC so this is not enforced by the lib boundary).
- **INV-17** (regression) — `~LlmClient` with a request in flight neither
  crashes nor emits `finished()`. The destructor calls `abort()` (which
  nulls `m_reply` *before* aborting, so a synchronous `finished()` emitted
  by `QNetworkReply::abort()` reaches `onFinished()` with `m_reply` already
  null and no-ops) rather than aborting a still-connected `m_reply` and
  then dereferencing it via `m_reply->deleteLater()` after `onFinished()`
  has already nulled that member.
- **INV-18** (regression — the drain-on-finish truncation) — a reply that
  finishes while more complete SSE lines are still buffered must still
  deliver every line, in order. `drain()` parses at most
  `kMaxLinesPerTick` (256) lines per tick and re-arms the remainder via
  `QTimer::singleShot(0)`; when the underlying `QNetworkReply`'s
  `finished()` is delivered before that re-armed `drain()` runs,
  `onFinished()` nulls `m_reply` and emits, so `drain()`'s early
  `if (!m_reply) return;` guard fires and the remaining buffered lines
  are silently dropped (`ok` still true). This invariant also guards the
  offset-walk refactor of `drain()`'s per-tick loop (trimming
  `m_sseLineBuffer` via a single offset walk instead of a repeated
  `.mid()` copy per line): that refactor must not change which lines get
  parsed before an early finish, so it must stay green across it too.
- **INV-19** (ANTS-5007, regression) — a reply with no SSE data line is
  read as one plain JSON body, whatever its HTTP status. A
  `choices[0].message.content` answer surfaces as `text`, with `ok` true.
  An `error.message` body surfaces as `error`, with `ok` false. `drain()`
  keeps the raw bytes, capped at `kMaxBytes`, until a data line proves the
  reply is a stream. Pre-fix, `drain()` consumed the body and the fallback
  ran only on an HTTP error: a success came back blank, and an error came
  back as Qt's generic `errorString()`.
- **INV-20** (ANTS-5008, regression) — reaching `kMaxBytes` ends the
  download. `drain()` aborts the reply, and `finished()` reports the capped
  answer with `ok` and `truncated` both true. The client closes the
  connection even while the server holds it open. Pre-fix, the cap only
  stopped the appending; the transfer ran on until the server closed it or
  the transfer timeout fired.

## Test notes

Most cases are static-helper + scrub only — no live network, no event
loop. INV-17, INV-18, INV-19 and INV-20 are the exception: they need a
real `QNetworkReply` in flight, which the static kernels can't produce, so
they drive `LlmClient::send` against a fake HTTP server (`FakeHttpServer`,
in the test file's anonymous namespace) bound to `127.0.0.1` (loopback).
Loopback passes every `LlmClient` egress gate (`isEndpointHostBlocked` and
`isPlaintextRemote` both exempt it — see INV-2b/INV-2c above), so this is
not live network access: no packet leaves the host. Label `features;fast`.

INV-20's server holds the connection open (`FakeHttpServer` mode
`RespondKeepOpen`), so only the client can end the transfer. Its request
sets `timeoutMs` well past the test's wait bound, so the transfer timeout
cannot be what ends it.
