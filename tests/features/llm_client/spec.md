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

## Test notes

Static-helper + scrub only; no live network, no event loop. Label
`features;fast`.
