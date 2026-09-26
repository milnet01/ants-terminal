# mcpd_verify_trust_wiring — ants-mcpd never wires a VerifyTrust client

Canonical trust design: `docs/specs/ANTS-1337.md`. Standalone-server design:
`docs/specs/ANTS-4932-standalone-mcp-server.md`. This directory's case
continues the MC-numbering `tests/features/verify_trust_gate/spec.md`
reserves for "Phase 2 (MCP envelope)" tests — MC-1..MC-4 there are
source-grep checks that `RemoteControl::cmdVerifyChangesImpl` *reads* a
trust client and threads the autotrust env var; **MC-5** here is the
first behavioural check that a served `RemoteControl` instance actually
*has* one.

## Invariants

- **MC-5** — Every `RemoteControl` instance that serves a real
  `verify_changes` call carries a real `VerifyTrust::Client`, so an
  untrusted `.ants/verify.json` falls back to auto-detect rather than
  running its bespoke commands. This must hold for `ants-mcpd`
  (`src/mcpdmain.cpp`), not only for the GUI host
  (`src/mainwindow.cpp`). Concretely: a project whose
  `.ants/verify.json` names a bespoke `build` command, with an empty
  (never-trusted) trust store and no `ANTS_VERIFY_TRUST_AUTOTRUST`
  bypass, must not have that command executed by a `verify_changes` call
  routed through `ants-mcpd`; the response must report
  `verify_untrusted:true` and `config_source` must not be
  `".ants/verify.json"`.

## Rationale

`VerifyEngine::loadGateConfig` (`src/verifyengine.cpp:401-430`) has three
outcomes for a found `.ants/verify.json`: a non-null trust client that
trusts it (honour it), a non-null trust client that doesn't (fall back to
auto-detect, `verifyUntrusted=true`), and a **null** trust client — which
it treats as "Phase-1 back-compat: honour it unconditionally". That third
branch exists so callers that hadn't yet wired ANTS-1337's trust client
kept their old behaviour during the rollout. It also means: whichever
`RemoteControl` never gets a trust client wired stays permanently on
pre-ANTS-1337 behaviour, silently.

`src/mainwindow.cpp:1149` wires the GUI host:
`m_remoteControl->setVerifyTrustClient(std::make_unique<VerifyTrust::ModalClient>(this));`
`src/mcpdmain.cpp:91` constructs a second, independent `RemoteControl` for
`ants-mcpd` — `RemoteControl rc(nullptr, nullptr, &roots);` — and never
calls `setVerifyTrustClient` on it. `RemoteControl::cmdVerifyChangesImpl`
(`src/remotecontrol_review.cpp:2016-2032`) does thread `opts.trustClient =
m_verifyTrustClient.get()` through to `loadGateConfig` correctly — but for
the `ants-mcpd` instance that pointer is always null, so every
`verify_changes` call served through `ants-mcpd` takes the "honour it
unconditionally" branch, for every project, forever. A hostile cloned repo
opened through Claude Code (which talks to `ants-mcpd`, not the GUI
terminal's own MCP path) runs arbitrary shell via its `.ants/verify.json`
the first time `verify_changes` is called, with no trust prompt and no
auto-detect fallback — the exact attack ANTS-1337 was written to close,
reopened on the one host ANTS-1337's own tests (source-grep only; see
`verify_trust_gate`'s MC-1..MC-4) could not see, because they check that
the code *reads* `m_verifyTrustClient`, not that any given instance
*has* one.

## Scope

In scope: whether a `verify_changes` call served by the real `ants-mcpd`
binary honours or refuses an untrusted bespoke `.ants/verify.json`. Out of
scope: the trust modal UI (`verify_trust_modal_gui_thread` owns that), the
trust file's on-disk format and persistence (`verify_trust_gate`'s TF-*
invariants own that), and the GUI host's own wiring (already exercised by
`src/mainwindow.cpp`'s construction, which this test does not touch —
`ants-mcpd` is a separate binary with a separate `RemoteControl`
instance).

## Regression history

Not yet fixed as of this writing (verified against the tree at the time
this test was written). `src/mcpdmain.cpp:91` never calls
`RemoteControl::setVerifyTrustClient`. The expected fix constructs a
`VerifyTrust::FilePersistedTrustClient` there and wires it the same way
`src/mainwindow.cpp:1149` does for the GUI host — `ants-mcpd` has no
window to show a modal from, but `FilePersistedTrustClient`'s base-class
`prompt()` already returns `Outcome::Headless` with no GUI available,
which `loadGateConfig` already treats as "fall back to auto-detect, report
untrusted" — so no chrome-layer dependency is needed to close this gap.

## Cases

| Case | Invariant | How |
|---|---|---|
| `UntrustedBespokeConfigDoesNotRun` | MC-5 | A fixture project (no `CMakeLists.txt` / `package.json` / etc, so auto-detect finds nothing) whose `.ants/verify.json` names a `build` gate that touches a marker file. `HOME` / `XDG_CONFIG_HOME` point at a fresh temp directory (empty trust store; the real `~/.config/ants-terminal/verify-trust.json` is never read or written) and `ANTS_VERIFY_TRUST_AUTOTRUST` is unset. Drives `verify_changes` through a real `ants-mcpd` child process (`McpdSession`, reused from `tests/features/standalone_mcp_server/`). Asserts the marker file does not exist afterward, and that the response reports `verify_untrusted:true` with `config_source` not `".ants/verify.json"`. |
