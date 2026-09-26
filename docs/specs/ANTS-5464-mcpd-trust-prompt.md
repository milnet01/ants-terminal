# ANTS-5464 — ants-mcpd asks the terminal before trusting a project's verify.json

**Status:** accepted (2026-09-26).
**Kind:** review-fix.
**Source:** ROADMAP.md ANTS-5464 (review-contract ANTS-1337 loop 2,
2026-09-26; design option (a) chosen by the user the same day).
**Composes with:** ANTS-1337 (the trust gate), ANTS-4932 (ants-mcpd and
its terminal socket), ANTS-5411 (the trust file is re-read on change).

**Layman:** When Claude wants to run a project's own check commands for
the first time, the terminal asks you again, as it did before the MCP
moved into its own helper.

## 1. Problem

`verify_changes` is registered in `mcp::registerProjectScopedVerbs`.
So `ants-mcpd` serves it for every Claude Code session.

`ants-mcpd`'s `main` installs a bare `VerifyTrust::FilePersistedTrustClient`.
Its `prompt()` returns `Outcome::Headless`. So an untrusted
`.ants/verify.json` always falls back to auto-detect.

The only prompt that grants trust is `VerifyTrust::ModalClient::prompt()`
in the terminal. A session served by `ants-mcpd` never reaches it; only
one still using `tools/mcp-bridge.py` does. Safe, but there is no way to
say yes.

ANTS-5411 already carries a grant to a running `ants-mcpd`. This item
makes the terminal be asked.

## 2. Surface

### 2.1 The request — a JSON-RPC method, not a tool

`ants-mcpd` sends one line to the terminal's MCP socket:

```json
{"jsonrpc":"2.0","id":1,"method":"ants/verifyTrustPrompt","params":{"root":"<canonical project root>"}}
```

and reads one reply line whose `result` is

```json
{"outcome":"trusted|denied|headless|no_config","sha":"<sha256 hex, empty for no_config>"}
```

It is not a tool: it is absent from `tools/list` in both hosts and from
`mcp::terminalScopedVerbNames()`, so no model can call it.

A host installs the handler with
`ClaudeIntegration::setVerifyTrustPromptHandler(fn)`. The terminal installs
one. `ants-mcpd` installs none, so it answers JSON-RPC error `-32601`.

### 2.2 The terminal side

The handler canonicalises `root`. It reads `<root>/.ants/verify.json`
through `loadGateConfig`'s anchored read: `pathStrictlyBelow`, then
`readFile`. That read is factored into one function both call. It parses
the bytes with `parseVerifyJson`.

No file, a file outside the root, a parse error or no gates → `no_config`.

Otherwise it calls the terminal's trust client:
`outcomeForConfig(rootCanon, bytes)`. That call honours a stored trust,
applies the session-denied cache, and shows the modal on the GUI thread.

The reply maps `Trusted` → `trusted`, `UntrustedFellBack` → `denied`,
`Headless` → `headless`. It carries the SHA the terminal computed.

**Everything that decides trust comes from the terminal's own read.** The
handler reads `params.root` and ignores every other key.

The request loop posts the handler to the Bulk worker with
`postWorkerJob`, and writes the reply when it returns. An open prompt
holds only that worker. The GUI thread and the Shared worker keep
serving. Other Bulk verbs in the terminal wait until the user answers.

### 2.3 The ants-mcpd side

`ants-mcpd` installs `mcpd::ForwardingTrustClient`, a
`FilePersistedTrustClient` whose `prompt()`:

1. Finds the terminal socket and checks its uid as `mcpd::Forwarder`
   does (ANTS-4932 § 2.5, INV-11). With none acceptable, it returns
   `Headless` at once.
2. Otherwise it connects on the calling thread and sends § 2.1's line. It
   waits at most `kTrustPromptTimeoutMs` = 120 000 ms for the reply.
3. It maps the reply:
   - `trusted` with its own `sha` → re-read the trust file (ANTS-5411).
     Return `Trusted` **only if** the file now holds the SHA or a matching
     repo pin. Otherwise `Headless`. **The terminal's word alone never
     trusts anything.**
   - `denied` → `UntrustedFellBack`. The base class caches it, so this
     process does not ask again for that SHA.
   - Anything else → `Headless`, not cached. That covers `headless`,
     `no_config`, a different `sha`, an error, a timeout and a dropped
     connection.

This item moves `verify_changes` to `DispatchLane::Bulk` in
`mcp::registerProjectScopedVerbs`. So in both hosts a pending prompt holds
only the Bulk worker. The main thread and the Shared worker keep answering.

On a timeout the terminal's dialog stays open. A later click still saves
the trust. ANTS-5411 carries it to `ants-mcpd` on its next call.

## 3. Invariants

- **INV-1** — The terminal decides from bytes it read itself. It ignores
  every `params` key but `root`, reads the config through the anchored
  read, and never prompts when that read yields no gates. *Test:*
  `tests/features/mcpd_trust_prompt/` drives the terminal-side handler
  in-process with a recording fake client: a request whose `params` also
  carry a bogus `sha` and `config` gives the fake the bytes on disk and
  replies with their SHA; a root whose `.ants/verify.json` is a symlink
  out of the root replies `no_config` and the fake is never called.
  Broken by: taking the config or the SHA from `params`.
- **INV-2** — `ants-mcpd` returns `Trusted` only when its own re-read of
  the trust file finds the SHA or the repo pin after the reply. *Test:*
  a stub terminal replies `trusted` with the right SHA without writing
  the trust file; the stub receives exactly one `ants/verifyTrustPrompt`
  request, and `verify_changes` through `ants-mcpd` still reports
  `verify_untrusted:true`. Broken by: returning `Trusted` from the reply.
- **INV-3** — Where the terminal grants trust, the same `verify_changes`
  call runs the project's own gates. *Test:* a stub terminal writes the
  SHA to the sandboxed trust file and replies `trusted`;
  `verify_changes` reports `config_source` `.ants/verify.json` and
  `verify_untrusted:false`. Broken by: consulting the terminal only on the
  next call.
- **INV-4** — With no terminal socket, `verify_changes` falls back as
  today, without waiting. *Test:* `ANTS_MCP_SOCKET` names a path with no
  listener; `verify_changes` answers `verify_untrusted:true` in under
  5 s. Broken by: a forwarding client that waits for a connection.
- **INV-5** — A `denied` reply is asked once per SHA per `ants-mcpd`
  process. *Test:* a stub terminal that replies `denied` and counts
  requests; two `verify_changes` calls on one project → one request.
  Broken by: returning `Headless` for `denied`.
- **INV-6** — A reply later than `kTrustPromptTimeoutMs`, or none,
  yields `Headless` and is not cached. *Test:* `ForwardingTrustClient`
  constructed with a 200 ms timeout against a stub that never answers
  returns `Headless` within 2 s, and asks again on the next lookup.
  Broken by: an unbounded wait, or caching the timeout as a denial.
- **INV-7** — `ants/verifyTrustPrompt` is not a tool, and a host without
  a handler refuses it. *Test:* the method is absent from `tools/list` in
  `ants-mcpd` and in the in-process GUI pipeline; sent to `ants-mcpd` it
  answers error `-32601`. Broken by: registering it through
  `registerToolProvider`.
- **INV-8** — `ants-mcpd` keeps answering main-thread and Shared-lane
  requests while a prompt is pending. *Test:* against a stub terminal
  that holds the trust request open, once the stub has received it, a
  `tools/list` and a `spec_lint` are both answered before the stub
  replies. Broken by: waiting on the main thread, or leaving
  `verify_changes` on the Shared lane.
- **INV-9** — The terminal keeps answering while its handler is open.
  *Test:* an in-process pipeline whose installed handler blocks on a
  latch answers a `tools/list` and a `spec_lint` before the latch is
  released. Broken by: running the handler inline in the request loop.

## 4. RAM / build cost

One short-lived socket connection per untrusted config asked about;
nothing resident. `ForwardingTrustClient` lives in `ants-mcpd`'s sources
with `QLocalSocket`, which the forwarder already links — no GUI library
(ANTS-4932 INV-1). The terminal gains one handler.

**Reaching a running terminal:** the terminal-side handler is compiled
into the terminal, so this needs one terminal relaunch; `ants-mcpd` then
needs a rebuild and an MCP reconnect. Accepted by the user 2026-09-26.
Until the terminal is relaunched, `ants-mcpd` gets `-32601` and falls
back, which is today's behaviour.

## 5. Out of scope

- A menu or Settings entry for trusting a project by hand — option (b),
  not chosen.
- Prompting when no terminal is running: there is no window to ask from,
  and the fallback stands.
- Correcting ANTS-1337's older drift — tracked by ANTS-5465.

## 6. Tests

Feature test: `tests/features/mcpd_trust_prompt/`, built into
`test_claude` beside `standalone_mcp_server`. It reuses that directory's
`McpdSession` and `StubTerminal`. The stub gains a handler for § 2.1's
method. Label `features;fast`.

It covers every invariant in § 3.

Red first:
- INV-1 fails pre-change with `-32601`: no handler exists.
- INV-2, INV-3, INV-5 and INV-8 fail because the stub receives no request.
- INV-6 and INV-9 test API this item adds.
- INV-4 and INV-7 pin today's behaviour and pass before the change. Their
  red run is their *Broken by* applied.

## 7. Cross-doc impact

- ANTS-1337 § 4.3 and § 8: the prompt is reached through `ants-mcpd` by
  this request.
- ANTS-4932 § 2.5: the terminal socket carries one non-tool method.
- CHANGELOG `[Unreleased]` at ship.

## What checks this

| Invariant | What catches a breach |
|---|---|
| INV-1 | `tests/features/mcpd_trust_prompt/` |
| INV-2 | `tests/features/mcpd_trust_prompt/` |
| INV-3 | `tests/features/mcpd_trust_prompt/` |
| INV-4 | `tests/features/mcpd_trust_prompt/` |
| INV-5 | `tests/features/mcpd_trust_prompt/` |
| INV-6 | `tests/features/mcpd_trust_prompt/` |
| INV-7 | `tests/features/mcpd_trust_prompt/` |
| INV-8 | `tests/features/mcpd_trust_prompt/` |
| INV-9 | `tests/features/mcpd_trust_prompt/` |

## Cold-eyes loop log

The rows are in [`docs/reviews/ANTS-5464-mcpd-trust-prompt-loop-log.md`](../reviews/ANTS-5464-mcpd-trust-prompt-loop-log.md).
