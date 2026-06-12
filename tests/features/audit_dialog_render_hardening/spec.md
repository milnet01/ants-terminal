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

- **INV-1** (ANTS-1826) The AI-triage POST never sends the Bearer API key over
  cleartext to a remote host: when an API key is set and the endpoint is
  plaintext `http` to a non-loopback host, the request is refused before any
  network send. The check reuses `LlmClient::isPlaintextRemote` (localhost /
  loopback is exempt so a local dev LLM server still works).
- **INV-3** (ANTS-2108) The guard from INV-1 is present on **both** AI-triage
  POST paths — single-finding and batch. Because neither path routes through
  `LlmClient::send`, the chokepoint backstop added there does not cover them;
  each gates `!apiKey.isEmpty() && LlmClient::isPlaintextRemote(...)`
  independently. (Pre-2108 only the single-finding path was guarded.)
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
