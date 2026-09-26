# ANTS-5464 — ants-mcpd asks the terminal before trusting a project's verify.json

**Status:** spec draft (2026-09-26).
**Kind:** review-fix.
**Source:** ROADMAP.md ANTS-5464 (review-contract ANTS-1337 loop 2,
2026-09-26; design option (a) chosen by the user the same day).
**Composes with:** ANTS-1337 (the trust gate), ANTS-4932 (ants-mcpd and
its terminal socket), ANTS-5411 (the trust file is re-read on change).

**Layman:** When Claude wants to run a project's own check commands for
the first time, the terminal asks you again, as it did before the MCP
moved into its own helper.

## 1. Problem

`verify_changes` is registered in `mcp::registerProjectScopedVerbs`, so
`ants-mcpd` serves it for every Claude Code session. `ants-mcpd`'s `main`
installs a bare `VerifyTrust::FilePersistedTrustClient`, whose `prompt()`
returns `Outcome::Headless`. So an untrusted `.ants/verify.json` always
falls back to auto-detect with `verify_untrusted:true`, and the only
prompt that can grant trust — `VerifyTrust::ModalClient::prompt()` in the
terminal — is reachable only through the terminal's own socket, which no
Claude session uses any more. Safe, but there is no way to say yes.

ANTS-5411 already carries a grant made in the terminal to a running
`ants-mcpd`. This item makes the terminal be asked.

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
`mcp::terminalScopedVerbNames()`, so no model can call it. `ClaudeIntegration`
answers the method only when its host installed a handler; without one it
answers JSON-RPC error `-32601`. The terminal installs one; `ants-mcpd`
does not.

### 2.2 The terminal side

The handler canonicalises `root`, reads `<root>/.ants/verify.json` through
the same anchored read `loadGateConfig` uses (`pathStrictlyBelow` against
the canonical root, then `readFile`) — factored into one function both
call — and parses it with `parseVerifyJson`. No file, a file outside the
root, a parse error or no gates → `no_config`. Otherwise it calls the
terminal's installed trust client, `outcomeForConfig(rootCanon, bytes)`,
which already honours a stored trust, applies the session-denied cache
and shows the modal on the GUI thread (`ModalClient::prompt`). The reply
maps `Trusted` → `trusted`, `UntrustedFellBack` → `denied`, `Headless` →
`headless`, and carries the SHA the terminal computed.

**Everything that decides trust comes from the terminal's own read.** The
request carries a root and nothing else; any other `params` key is
ignored.

The handler runs on the pipeline's Bulk worker (`DispatchLane::Bulk`,
ANTS-5086), and the modal itself on the GUI thread through
`ModalClient::prompt`. An open prompt therefore holds only the Bulk
worker; the GUI thread and the Shared worker keep serving.

### 2.3 The ants-mcpd side

`ants-mcpd` installs `mcpd::ForwardingTrustClient`, a
`FilePersistedTrustClient` whose `prompt()`:

1. With no acceptable terminal socket — found and uid-checked exactly as
   `mcpd::Forwarder` does (ANTS-4932 § 2.5, INV-11) — returns `Headless`
   at once.
2. Otherwise connects on the calling thread, sends § 2.1's line and waits
   at most `kTrustPromptTimeoutMs` = 120 000 ms for the reply, blocking
   the thread `verify_changes` runs on. This item moves `verify_changes`
   to `DispatchLane::Bulk` in `mcp::registerProjectScopedVerbs`, so in
   both hosts a pending prompt — like a long gate run — holds only the
   Bulk worker, and the main thread and the Shared worker keep answering.
3. Maps the reply:
   - `trusted` with a `sha` equal to its own → re-reads the trust file
     (ANTS-5411) and returns `Trusted` **only if** that file now holds the
     SHA or a matching repo pin; otherwise `Headless`. **The terminal's
     word alone never trusts anything**: only the file it wrote does.
   - `denied` → `UntrustedFellBack`, which the base class records in its
     session-denied cache, so this process does not ask again for that SHA.
   - anything else — `headless`, `no_config`, a different `sha`, an error
     reply, a timeout, a dropped connection → `Headless`, not cached, so
     the next call may ask again.

On a timeout the terminal's dialog stays open. A later click still saves
the trust, and ANTS-5411 carries it to `ants-mcpd` on its next call.

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
`test_claude` beside `standalone_mcp_server` and reusing its
`McpdSession` and `StubTerminal`, the stub gaining a handler for § 2.1's
method. Covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7 and
INV-8. Label `features;fast`. Red first: INV-1 fails
pre-change with `-32601` (no handler), and INV-2, INV-3, INV-5 and INV-8
because the stub receives no request; INV-6 tests the new class. INV-4
and INV-7 pin today's behaviour and pass before the change — their red
run is their *Broken by* applied.

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

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-26 | 2 (review-lane; every question each) | 1 | 0 | 1 | 1 | Verified 3 / fixed 3 / dismissed 0. Q4 (both lanes): § 6 claimed every case fails pre-change, but INV-2, INV-4, INV-7 and INV-8 pass on today's code and INV-2/INV-8 could not tell "asked" from "never asked" — INV-2 now counts the stub's requests, INV-8 waits for the request, § 6 names INV-4/INV-7 as guards. Q1 (lane A, confirmed by the orchestrator): "other requests keep being answered" was false for the Shared worker, a single thread — `verify_changes` moves to `DispatchLane::Bulk`, INV-8 probes a Shared-lane verb. Q3 (lane A): the terminal handler's thread was unpinned — now the Bulk worker. Open questions resolved clean by the orchestrator: the base class caches `UntrustedFellBack`; `pathStrictlyBelow` canonicalises through a symlink; the spawned `ants-mcpd` inherits the test XDG sandbox. |
