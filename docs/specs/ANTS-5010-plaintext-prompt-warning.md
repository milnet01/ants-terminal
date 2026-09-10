# ANTS-5010 — Warn when a keyless AI request goes out over plain http

**Status:** accepted (2026-09-10).
**Kind:** security.
**Source:** ROADMAP.md ANTS-5010 (cold-sweep-2026-08-18, triaged in-session-2026-09-10; user decisions 2026-09-10).
**Composes with:** ANTS-1727 (`LlmClient`, `ReviewDialogBase`), ANTS-1352 (`indie_review_dispatch`), ANTS-2121 (`LlmClient::endpointEgressError`), ANTS-5018.

## 1. Problem

`LlmClient::endpointEgressError` refuses a plain-http remote endpoint only
when an API key is set. With no key, the request goes out unencrypted, and
every AI send path puts terminal or project content in it:

1. `AiDialog::sendRequest` sends recent terminal output as context.
2. The review dialogs send lane briefs from each subclass's `composeBrief`
   (`ColdEyesDialog`, `IndieReviewDialog`, `TestAuditDialog`).
3. `AuditDialog::requestAiTriage` and `AuditDialog::requestAiTriageBatch`
   send a finding's rule, file and line, source snippet and git blame.
   `AuditDialog::onDebtTriageClicked` sends `debtTriagePrompt()`.
4. `RemoteControl::cmdIndieReviewDispatch` sends briefs from
   `IndieReviewEngine::assembleBriefForDispatch`.

None of them tells the user. On 2026-09-10 the user decided to warn and
still send, because refusing would break a keyless AI server reached over
plain http. The user also decided the warning appears in every sending
window.

## 2. Surface

### 2.1 One predicate, one text

A new static member in `src/llmclient.h`, in `ants_core_lib`:

```cpp
// ANTS-5010 — non-empty when a request to `endpoint` would carry its prompt
// unencrypted: no API key, plain http, a non-loopback host, and every
// endpointEgressError gate passed. The text names the host.
static QString plaintextPromptWarning(const QString &endpoint,
                                      const QString &apiKey);
```

It returns empty when `apiKey` is non-empty, since `endpointEgressError`
refuses that case. It returns empty when `endpointEgressError(endpoint,
apiKey)` is non-empty, since that request never goes out. It returns empty
when `LlmClient::isPlaintextRemote(endpoint)` is false. Otherwise it
returns this text, with the host taken from `QUrl(endpoint).host()`:

> Not encrypted: this request goes to <host> over plain http, so anyone on
> the network path can read it. Use https:// to protect it.

Every surface shows this text unchanged. A surface may put its own label in
front of it. `llmclient` stays widget-free (ANTS-1727 INV-16).

### 2.2 AI chat

`AiDialog::sendRequest` shows the warning as `appendMessage("System", …)`,
after its keyed-cleartext refusal and before `m_client->send(req)`. A new
member `m_plaintextWarnedEndpoint` holds the endpoint it last warned about.
A send to that endpoint adds no second copy; a send to any other endpoint
warns again and replaces it. The request is sent either way.

### 2.3 Review dialogs

`ReviewDialogBase::beginRound` sets `m_statusLabel` to the warning, after
its `updateDispatchEnabled()` call, when
`plaintextPromptWarning(m_config->aiEndpoint(), m_config->aiApiKey())` is
non-empty. When it is empty, `beginRound` clears the label if the label
still shows the warning an earlier round set: `MainWindow` reloads `Config`
in place while the dialog is open, so the endpoint can change between
rounds. A member holds the text it set, as `m_failureStatus` does for
`onAllFinished`. Each subclass's `composeBrief` puts that same endpoint and
key in its request.

`startDispatch` and `redispatch` both call `beginRound`, so every round
warns. `dispatchOne` does not warn: its callers,
`TestAuditDialog::onAllReportsCollected` and
`IndieReviewDialog::dispatchSynthesis`, run after a round has finished.
`onAllFinished` may replace the text with its failure summary, and leaves
it on success.

### 2.4 Audit dialog

- `AuditDialog::onBatchTriageClicked` and `AuditDialog::onDebtTriageClicked`
  append the warning to their `QMessageBox::question` text, after a blank
  line, when it applies. They compute it from `Config::aiEndpoint()` and
  `Config::aiApiKey()`, the values the request uses. Choosing Yes still
  sends.
- `AuditDialog::requestAiTriage` has no confirmation. It puts the warning
  first in its status text, ahead of "AI triage: querying <host>…":
  `m_statusLabel` is an `ElidedLabel` set to `Qt::ElideRight`, so text
  appended at the end is the first cut. The reply's own status text later
  replaces the line.
- `AuditDialog::requestAiTriageBatch` needs nothing of its own. Its only
  caller is `onBatchTriageClicked`.

### 2.5 MCP `indie_review_dispatch`

`RemoteControl::cmdIndieReviewDispatch` adds a top-level `warning` string
to its success envelope when `plaintextPromptWarning(endpoint, dr.apiKey)`
is non-empty, and no `warning` key otherwise. A refusal envelope carries
none, because nothing was sent.

The key is `warning` because `projectFields` in `src/mcpprojection.cpp`
re-inserts `warning` into every narrowed reply (ANTS-4698). A caller
passing `fields=` still receives it. The tool description needs no change:
the field explains itself.

### 2.6 Choices, and who made them

- Warn and still send, rather than refuse — the user, 2026-09-10.
- Warn in every sending window — the user, 2026-09-10.
- In a chat dialog, no repeat while the endpoint stays the one last warned
  about — Claude, 2026-09-10. A chat sends a request per message, and one
  line repeated on each buries the conversation. The user may overrule it.
- The warning's wording — Claude, 2026-09-10.

### 2.7 Alternatives rejected

- **A `warnings` array, as `roadmap_log` returns.** `projectFields` does not
  re-insert it, so a caller narrowing with `fields=` would lose a security
  warning.
- **Warning from inside `LlmClient::send`.** `LlmClient` has no
  warn-and-continue channel, and the audit triage POSTs do not go through
  `send`. Each caller already owns a surface the user reads.
- **Warning on every chat send.** The same line on every message; see
  § 2.6.
- **Warning only when the endpoint is saved in Settings.** The user chose
  send-time warnings, and a Settings warning says nothing when a review
  sends later.

## 3. Invariants

- **INV-1** — `LlmClient::plaintextPromptWarning` returns non-empty text naming the host exactly when the key is empty, the endpoint is plain http to a non-loopback host, and `endpointEgressError` passes. It returns empty for a non-empty key, for https, for `localhost` and loopback addresses, for a private or link-local IP literal, and for an endpoint with URL userinfo. Broken by text for any of those, or by empty for a keyless remote plain-http endpoint. *Test:* `tests/features/llm_client`.
- **INV-2** — `AiDialog::sendRequest` with a keyless remote plain-http endpoint adds one "System" message carrying the INV-1 text and still sends the request. A second send to the same endpoint adds no second copy. An https or loopback endpoint adds none. Broken by no message, a message on every send, or a refusal. *Test:* `tests/features/plaintext_prompt_warning`.
- **INV-3** — after `ReviewDialogBase::startDispatch` or `ReviewDialogBase::redispatch` with a keyless remote plain-http endpoint, the status label shows the INV-1 text and the jobs are enqueued. With https, a loopback endpoint or a key, the label does not show it, including after an earlier plain-http round in the same dialog. Broken by a warning from only one of the two entry points, a warning that survives a switch to https, or a round that does not run. *Test:* `tests/features/review_dialog_base`.
- **INV-4** — the bodies of `AuditDialog::onBatchTriageClicked`, `AuditDialog::onDebtTriageClicked` and `AuditDialog::requestAiTriage` each call `LlmClient::plaintextPromptWarning` and put its result in the text shown there. Broken by any of the three omitting it. *Test:* `tests/features/plaintext_prompt_warning`, a source-grep of the three bodies.
- **INV-5** — `RemoteControl::cmdIndieReviewDispatch` sets the success envelope's `warning` from `LlmClient::plaintextPromptWarning`, after its refusal return, and sets no other warning key. Broken by a missing key, a different key name, or an assignment a refusal can reach. *Test:* `tests/features/indie_review_dispatch`, a source-grep of the handler body.

## 4. RAM / build cost

None. One static function joins `ants_core_lib`, and one string member
each joins `AiDialog` and `ReviewDialogBase`. No new target, library or
dependency.

## 5. Out of scope

- Refusing a keyless plain-http request — not done, by the user's decision
  of 2026-09-10.
- A warning when the endpoint is saved in Settings — not done; the user
  chose send-time warnings.
- A hostname that resolves to a loopback or private address — not resolved
  here, as `LlmClient::isEndpointHostBlocked` documents (ANTS-2109 H2). Any
  DNS name other than `localhost` counts as remote.

## 6. Tests

Label `features;fast`. Verify each test fails against pre-fix source first.
Remote test endpoints use TEST-NET-1 (RFC 5737) addresses, and loopback ones
a port nothing listens on, so no request reaches a server.

- `tests/features/llm_client` — INV-1, a table over the endpoint and key
  cases.
- `tests/features/plaintext_prompt_warning` — new. INV-2 drives a real
  `AiDialog`; INV-4 is a source-grep.
- `tests/features/review_dialog_base` — INV-3, through the synchronous fake
  runner that `setJobRunner` installs.
- `tests/features/indie_review_dispatch` — INV-5, a source-grep, because the
  handler needs a `MainWindow`.

## 7. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `tests/features/llm_client`, a gtest table over the endpoint and key cases |
| INV-2 | `tests/features/plaintext_prompt_warning`, a gtest that drives a real `AiDialog` |
| INV-3 | `tests/features/review_dialog_base`, a gtest through the synchronous fake runner |
| INV-4 | **Partial:** `tests/features/plaintext_prompt_warning`, a source-grep of the three bodies. It does not show the text reaching either confirmation box or the status line. |
| INV-5 | **Partial:** `tests/features/indie_review_dispatch`, a source-grep of the handler body. No test produces the envelope, and whether `projectFields` keeps `warning` under `fields=` rests on ANTS-4698's own test. |

## 8. Cross-doc impact

- `docs/specs/ANTS-1352.md` — its success envelope gains `warning`; add one
  sentence when this ships.
- `CHANGELOG.md` — one `### Security` entry.
- ROADMAP — ANTS-5010 flips to shipped when built.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-10 | 3 | 0 | 2 | 0 | 0 | 2 findings, 2 verified / 0 dismissed, both fixed. Q2: § 2.2 said once per endpoint while naming one member for the last endpoint; now states the last-endpoint rule. Q2: § 2.3 set the review warning and nothing cleared it after an endpoint change; beginRound now clears it, INV-3 covers plain-http then https. Five open questions resolved clean, none a finding. Packet build found no defect. Loop 2 dispatched. |
| 2 | 2026-09-10 | 3 | 1 | 0 | 0 | 1 | 2 findings, 2 verified / 0 dismissed, both fixed. Q4 (lane 3): § 6 put every test endpoint on TEST-NET-1, which INV-2's loopback case cannot be; loopback now uses a port nothing listens on. Q1 (orchestrator, from a lane's open question): § 2.4 appended the triage warning to a label set to Qt::ElideRight, so it was cut first; it now comes first. Two lanes had no findings; five open questions resolved clean. Final-loop share on text this run wrote earlier: 0 of 2, a calm cap. Gated-span share: 4 of 4 run findings, the whole document being new. Cap reached; spec accepted, no deferred tail. |
