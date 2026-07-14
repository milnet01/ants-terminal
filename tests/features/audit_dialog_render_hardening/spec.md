# AuditDialog render + AI-triage hardening (ANTS-1826 / ANTS-1830)

Two indie-review #6 findings in the AuditDialog AI-triage / results-render path.

## Surface

- `AuditDialog::requestAiTriage` (single-finding) and
  `AuditDialog::requestAiTriageBatch` (batch) each compose an OpenAI-style
  `chat/completions` request and attach the configured API key as an
  `Authorization: Bearer …` header. Both use their own
  `QNetworkAccessManager` — not `LlmClient::send` — so each must carry the
  egress guards independently.
- `AuditDialog::renderResults` builds the in-app `QTextBrowser` HTML, including a
  per-finding "verdict badge" whose `title` attribute carries the
  AI-supplied `aiReasoning` text.

## Invariants

- **INV-1** (ANTS-1826, superseded by ANTS-2121) The AI-triage POST never sends
  the Bearer API key over cleartext to a remote host. ANTS-2121 folded the
  cleartext refusal — together with the scheme, SSRF host-block, and URL-userinfo
  gates — into the shared `LlmClient::endpointEgressError` validator, so the
  auditdialog path now enforces it by routing through that helper (empty return ==
  pass) rather than an inline `isPlaintextRemote` check. The predicate + message
  live in `llmclient.cpp` (contract covered by the `llm_client` bundle's INV-8);
  localhost / loopback stays exempt so a local dev LLM server still works.
- **INV-3** (ANTS-2108, superseded by ANTS-2121) The egress guard is present on
  **both** AI-triage POST paths — single-finding and batch. Because neither
  routes through `LlmClient::send`, the chokepoint backstop added there does not
  cover them; each calls `LlmClient::endpointEgressError(...)` independently (and
  sets `ManualRedirectPolicy`). ANTS-2121 replaced the former per-path inline
  cleartext check with this shared validator. (Pre-2108 only the single-finding
  path was guarded.)
- **INV-2** (ANTS-1830) The verdict-badge `title` attribute that interpolates
  the untrusted `aiReasoning` is **double-quoted**, not single-quoted.
  `QString::toHtmlEscaped()` escapes `"` but not `'`, so a single-quoted
  attribute holding escaped-but-apostrophe-bearing reasoning (reachable from a
  MitM'd or prompt-injected AI endpoint) could break out of the attribute. The
  escaping (`toHtmlEscaped`) is still applied to the reasoning text.

## Test

Source-grep conformance against `src/auditdialog.cpp` (mirrors the
`audit_run_scoped_check` pattern — the render/POST methods are large private
GUI methods with no isolated behavioral seam).
