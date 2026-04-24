# Feature: AI context + user-message secret redaction before send

## Problem

When the user opens the AI dialog (`Ctrl+Alt+A`), `MainWindow` populates
it with recent terminal scrollback via
`m_aiDialog->setTerminalContext(t->recentOutput(m_config.aiContextLines()))`
(`mainwindow.cpp:1160`). `AiDialog::sendRequest` then interpolates
`m_terminalContext` verbatim into the system prompt
(`aidialog.cpp:182`) and POSTs the resulting JSON body to whatever
endpoint the user has configured — OpenAI, Anthropic, an in-house
gateway, a self-hosted Ollama, or an arbitrary third party.

Anything on the screen at the moment the user opens the dialog is
exfiltrated. A single `cat .env`, `env | grep`, `aws configure show`,
`git clone https://ghp_…@github.com/…`, or a `ssh-keygen -y` paste in
the recent scrollback ships the credential off-machine.

The user's own input (`userMessage` at `aidialog.cpp:186`) has the
same problem — pasted secrets in the question itself are equally
exposed.

The 0.7.12 indie-review sweep addressed LLM01 (prompt injection from
AI output back to the PTY) and LLM02 (insecure output handling via
the `Insert Cmd` button) via the `ai_insert_command_sanitize` feature
test. This slice addresses **LLM06 — sensitive information disclosure
from context to provider**, explicitly called out in the ROADMAP
under "Follow-ups from the re-review checkpoint (2026-04-23)".

## External anchors

- [OWASP LLM Top 10](https://genai.owasp.org/llmrisk/) — **LLM06**
  Sensitive Information Disclosure: "LLMs process and generate content
  based on their training data and inputs, which may include
  sensitive information that should not be exposed."
- [OWASP LLM06 detail page](https://genai.owasp.org/llmrisk/llm062025-sensitive-information-disclosure/)
  — recommends "input sanitization to prevent user data from entering
  the training data or LLM model."
- [trufflehog detectors](https://github.com/trufflesecurity/trufflehog/tree/main/pkg/detectors)
  — reference corpus of secret shapes observed in real-world leaks;
  our regex set is a defensive subset anchored here, not invented.
- [GitHub secret-scanning token formats](https://docs.github.com/en/code-security/secret-scanning/secret-scanning-patterns)
  — canonical list of `gh[pours]_` PAT prefixes.
- [AWS IAM access key format](https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_access-keys.html)
  — `AKIA` / `ASIA` prefix + 16 uppercase-alphanum.

## Scope

**In scope:** regex redaction of well-known secret shapes in the two
strings that `sendRequest` ships to the network: `m_terminalContext`
and `userMessage`.

**Out of scope:**

- Review-before-send UX (nice-to-have; bigger change — a separate
  ROADMAP slice).
- Custom in-house secret formats not listed below. The redaction rule
  set is extensible; users with bespoke token shapes can propose
  additions, but regex-based redaction is reasonable-effort, not a
  cryptographic guarantee.
- AI response handling (LLM01/LLM02 — already covered by
  `ai_insert_command_sanitize`).
- The config-stored endpoint / API key (already sanitized on display
  per 0.6.22 — see `setConfig` at `aidialog.cpp:114-136`).
- Claude Code integration (`claudeintegration.cpp`) — that path talks
  to the local `claude` CLI, not a remote endpoint, and has its own
  allowlist/permission model.
- Remote-control `get-text` (`remotecontrol.cpp`) — the client is
  on-box and authenticated via socket perms / future X25519; it's a
  different trust boundary.

## Contract

### Invariant 1 — `SecretRedact::scrub` exists and is a free function

Location: `src/secretredact.h` (inline header, no `.cpp`). Signature:

```cpp
namespace SecretRedact {
    struct Result {
        QString text;
        int redactedCount;
    };
    Result scrub(const QString &input);
}
```

Pure function, no global state, safe to call from any thread.

### Invariant 2 — matched secrets are replaced with `[REDACTED:<kind>]`

The replacement MUST preserve the surrounding text and line breaks.
Only the matched secret substring is replaced; its line context is
left intact so the LLM still sees the command/variable shape (useful
for the AI's reasoning — it can still answer "your git clone failed
because of the URL" without seeing the token).

Examples:

| Input | Output |
|---|---|
| `export GITHUB_TOKEN=ghp_abcdefghijklmnopqrstuvwxyz0123456789` | `export GITHUB_TOKEN=[REDACTED:github_pat]` |
| `curl -H "Authorization: Bearer eyJhbGc..."` | `curl -H "Authorization: [REDACTED:bearer]"` |
| `AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE` | `AWS_ACCESS_KEY_ID=[REDACTED:aws_access_key]` |

### Invariant 3 — redacted shapes

The regex set MUST cover all of the following. Each shape's `<kind>`
label is fixed (tests match on the label string, so a rename is a
contract break):

| Shape | Pattern (informal) | `<kind>` |
|---|---|---|
| AWS access-key ID | `(AKIA\|ASIA)[0-9A-Z]{16}` | `aws_access_key` |
| GitHub PAT (classic) | `ghp_[A-Za-z0-9]{36}` | `github_pat` |
| GitHub OAuth | `gho_[A-Za-z0-9]{36}` | `github_oauth` |
| GitHub app install/user/refresh | `gh[sur]_[A-Za-z0-9]{36}` | `github_app` |
| GitHub fine-grained PAT | `github_pat_[A-Za-z0-9_]{82}` | `github_fine_grained_pat` |
| Anthropic API key | `sk-ant-[A-Za-z0-9_\-]{90,}` | `anthropic` |
| OpenAI project key | `sk-proj-[A-Za-z0-9_\-]{80,}` | `openai_project` |
| OpenAI legacy key | `sk-[A-Za-z0-9]{48}` (when not already matched as `sk-ant-` or `sk-proj-`) | `openai` |
| Slack token | `xox[abpros]-[A-Za-z0-9-]{10,}` | `slack` |
| Stripe live key | `(sk\|rk)_live_[A-Za-z0-9]{24,}` | `stripe` |
| JWT | `eyJ[A-Za-z0-9_\-]{10,}\.eyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}` | `jwt` |
| Bearer header | `(?i)bearer\s+\S{20,}` — replace `bearer <token>` with `[REDACTED:bearer]` | `bearer` |
| Generic secret assignment | `(?i)(api[_-]?key\|token\|password\|passwd\|secret)\s*[:=]\s*['"]?\S{8,}['"]?` — replace value only | `generic_secret` |
| PEM private key block | `-----BEGIN [^-]*PRIVATE KEY-----[\s\S]*?-----END [^-]*PRIVATE KEY-----` (multi-line) | `private_key` |

Precedence: higher-specificity patterns match first
(`sk-ant-…` before `sk-…`, `github_pat_…` before the classic `ghp_`
shape) so a key is labelled with the most accurate `<kind>`.

### Invariant 4 — `redactedCount` reports total replacements

Zero when the input contained no matched shapes. One per matched
secret. A buffer with three `ghp_…` tokens and one JWT reports
`redactedCount == 4`.

### Invariant 5 — negative controls pass through verbatim

The following MUST return `input.text == input` and
`redactedCount == 0`:

- An empty string.
- A plain command line with no secret-shaped text: `ls -la /etc`.
- A URL without credentials: `https://github.com/user/repo.git`.
- A UUID: `550e8400-e29b-41d4-a716-446655440000`.
- A commit SHA: `a1b2c3d4e5f6789012345678901234567890abcd`.
- Text mentioning the word "token" or "secret" without a value:
  `please set your api token in the config`.

The commit-SHA and UUID cases exist specifically to defend against
overly greedy generic regex — a 40-char hex SHA must NOT be caught
by an AWS-secret-style pattern. If it is, the pattern is too loose
and must be tightened (e.g., require the `aws_secret_access_key`
context prefix).

### Invariant 6 — PEM block redaction is line-aware

A multi-line private-key block is replaced in full with a single
`[REDACTED:private_key]` marker; the markers themselves
(`-----BEGIN`, `-----END`) are consumed as part of the match. The
surrounding lines (the `cat` command that printed it, the prompt
that follows) are preserved unchanged.

### Invariant 7 — `AiDialog::sendRequest` calls `SecretRedact::scrub` on both strings

Source-level invariant (grep-enforced): the function body of
`AiDialog::sendRequest` in `src/aidialog.cpp` MUST contain a call
to `SecretRedact::scrub` applied to both the terminal-context
string and the user-message string **before** either is used in
`.arg(…)` or assigned to a `QJsonObject` `content` field. A future
refactor that replaces the JSON construction with a helper must
route through a scrubbed value or the test fails.

### Invariant 8 — user is informed when redaction occurred

When the combined `redactedCount` across context + user message is
> 0, `AiDialog::sendRequest` MUST surface the count to the user via
a mechanism visible without extra clicks (status label append,
chat-history system message, or equivalent). The exact wording is
not pinned (UX copy can evolve); the test asserts a non-empty
signal — e.g., that `m_statusLabel->text()` changes OR
`appendMessage("System", …)` is called in the same request flow
when `redactedCount > 0`.

Rationale: silent redaction is worse than visible redaction — the
user needs to know the LLM is seeing a different payload than what
they pasted, or they'll wonder why the AI's advice doesn't match
their terminal state.

### Invariant 9 — redaction does not leak into the chat history

The `You:` message rendered into `m_chatHistory` (via
`appendMessage("You", text)` at `aidialog.cpp:143`) shows the
**pre-redaction** user input. Redaction happens on the network
boundary, not on the user-facing display. Rationale: the user
already has the plaintext on their own screen; scrubbing the chat
UI would just be confusing. The chat log is local; the wire is
what leaves the machine.

## Regression history

- **Introduced:** when the AI dialog feature shipped. The design
  assumed the user knew their scrollback was being sent and was
  comfortable with it. That's true for `ls` output or a stack
  trace; it's not true for `.env` contents, freshly-pasted SSH keys,
  or `aws configure show`.
- **Discovered:** 2026-04-23 via the multi-agent indie-review sweep.
  Reviewer scored it HIGH under OWASP LLM06 but deferred it from the
  0.7.12 Tier-1 fix batch because it's a new defense layer, not a
  1-line guard-rail like the LLM01/LLM02 fix. Captured in
  `ROADMAP.md:1282-1287`.
- **Fixed:** (pending — this slice).

## What fails without the fix

Without `SecretRedact::scrub` wired into `sendRequest`:

1. A user runs `cat ~/.aws/credentials` in their terminal.
2. Later, they hit `Ctrl+Alt+A` to ask the AI "why is my build
   failing?".
3. The system prompt shipped to the AI endpoint contains
   `aws_access_key_id = AKIA…` and
   `aws_secret_access_key = …` verbatim.
4. If the endpoint is a third-party provider (OpenAI, Anthropic, or
   an in-house gateway with request logging), the credential now
   lives in their logs / training-data pipeline.

With the fix: step 3 replaces both values with `[REDACTED:*]`, the
user sees "(2 secrets redacted before send)" in the dialog's status
area, and the LLM still has enough structure to answer the build
question.

## Test strategy

`test_ai_context_redaction.cpp` — standalone GUI-free C++ test that
links only `src/secretredact.h` (header-only) and exercises a corpus
of ~20 crafted lines. For each line: asserts no raw secret substring
survives, the `[REDACTED:<kind>]` label matches the expected `<kind>`,
and the surrounding text (the `export ` prefix, the URL path, the
`\n`) is preserved byte-for-byte.

Source-grep invariants (Invariant 7) are checked via plain `QFile`
reads of `src/aidialog.cpp` — the test asserts that every call to
`.arg(m_terminalContext)` and every assignment of the user message
into a `QJsonObject` is preceded (within N lines) by a
`SecretRedact::scrub` call whose result feeds the downstream use.
