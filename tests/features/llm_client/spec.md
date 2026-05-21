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
  buffer is capped at the same bound.
- **INV-5** — `sseContentDelta` returns content for a
  `data:{…delta.content…}` line and empty for `[DONE]` / non-content / non-
  data lines.
- **INV-16** — `llmclient` / `llmdispatcher` / `briefdispatch` headers +
  sources include no Qt Widgets header (widget-free discipline; the lib
  links Qt6::Widgets PUBLIC so this is not enforced by the lib boundary).

## Test notes

Static-helper + scrub only; no live network, no event loop. Label
`features;fast`.
