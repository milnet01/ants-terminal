# Feature: plaintext-prompt warning in the AI chat dialog + audit triage

## Problem

`docs/specs/ANTS-5010-plaintext-prompt-warning.md` is the owning contract:
`LlmClient::plaintextPromptWarning` (§ 2.1) is the one predicate + text every
AI-sending surface must show when a request would carry its prompt
unencrypted (no API key, plain http, a non-loopback host). This directory
covers the two surfaces that have no other paired feature-test home:

- `AiDialog::sendRequest` (§ 2.2) — the interactive chat dialog. No existing
  test constructs a real `AiDialog` (`tests/features/ai_insert_command_sanitize`
  and `tests/features/ai_context_redaction` test only static helpers /
  source-grep), so this is the first behavioural drive of the dialog.
- `AuditDialog::onBatchTriageClicked` / `onDebtTriageClicked` /
  `requestAiTriage` (§ 2.4) — three call sites the owning spec requires to
  each surface the warning; grouped here because none of the three has an
  existing lane of its own to extend, and `AuditDialog` is a `QDialog` the
  MCP-only `indie_review_dispatch` directory cannot reach.

The other two surfaces the owning spec names — `LlmClient::plaintextPromptWarning`
itself and `ReviewDialogBase::beginRound` — extend existing paired test
files instead (`tests/features/llm_client` INV-21, `tests/features/review_dialog_base`
INV-23), per the "one contract per feature" convention: a feature test's own
home is wherever the corpus already tests that class, not a new directory
per invariant. `RemoteControl::cmdIndieReviewDispatch` (§ 2.5) is a
source-grep addition to `tests/features/indie_review_dispatch` (G-18) for
the same reason.

## Scope

In scope: `AiDialog::sendRequest`'s warning message + de-dup-by-endpoint
behaviour (behavioural, driving a real `AiDialog`); the three `AuditDialog`
call sites (source-grep — none of the three is reachable without a live AI
server or a `QMessageBox::question` interaction the offscreen test platform
cannot drive, matching the owning spec's own "Partial" note in its § 7
table).

Out of scope: constructing the OTHER `AiDialog` warning paths (the
keyed-cleartext refusal, the endpoint-rejected-scheme path) — already
covered where they were introduced; not this warning's concern. The exact
wording of the warning text is `LlmClient::plaintextPromptWarning`'s
contract, locked in `tests/features/llm_client` INV-21 — this directory
treats the returned string as a black box and compares against it verbatim
only where the owning spec § 2.1 states the text is stable enough to do so.

## Invariants

- **INV-1** (ANTS-5010 § 3 INV-2) — `AiDialog::sendRequest` with a keyless
  remote plain-http endpoint appends one `"System"` chat message carrying
  `LlmClient::plaintextPromptWarning`'s text, and the request is still sent
  (observable as the Send button going disabled — `sendRequest` disables it
  unconditionally before ever refusing or sending). A second send to the
  *same* endpoint adds no second copy of the message. An https or loopback
  endpoint adds none.
- **INV-2** (ANTS-5010 § 3 INV-4) — the bodies of
  `AuditDialog::onBatchTriageClicked`, `AuditDialog::onDebtTriageClicked`,
  and `AuditDialog::requestAiTriage` each call
  `LlmClient::plaintextPromptWarning`. Source-grep: none of the three is
  reachable offscreen without a live AI server or a modal
  `QMessageBox::question` click, matching `docs/specs/ANTS-5010-plaintext-prompt-warning.md`
  § 7's own "Partial" note for this invariant.

## Test notes

GUI bundle (`test_claude` — already links `ants_dialogs_lib`, which builds
`aidialog.cpp`, and `ants_audit_dialog_lib`, which builds `auditdialog.cpp`
for the INV-2 source-grep; `SRC_AUDITDIALOG_CPP_PATH` and
`ants_test::slurpFunctionBody` are already available bundle-wide). INV-1
drives a real `AiDialog`: `findChild<QLineEdit*>()` / `findChild<QTextEdit*>()`
locate the input field and chat history (neither has a dedicated accessor —
same technique `review_dialog_base` uses to find the "Dispatch to AI"
button by its label); the Send button is located by its `"Send"` text.
`QTest::keyClick(input, Qt::Key_Return)` drives a second send once the Send
button has gone disabled by the first — the same path a real Enter keypress
takes, since `onSend()` is wired to `QLineEdit::returnPressed()`
unconditionally (the button is never the only route to it). Endpoints follow the owning spec's § 6 convention: TEST-NET-1
(`192.0.2.1`, RFC 5737) for "remote", and a loopback port nothing listens on
(`127.0.0.1:9`) for "loopback" — no request in this file ever reaches a
real server. Label `features;fast`.
