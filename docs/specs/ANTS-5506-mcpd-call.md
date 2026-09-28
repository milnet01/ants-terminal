# ANTS-5506 — Run an Ants MCP verb from a shell: `ants-mcpd --call`

**Status:** accepted (2026-09-28), review-contract loops 1 + 2 folded, cap reached.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5506 (claude-config joint review 2026-09-27, A5; CLI shape decided by claude-config 2026-09-27).
**Composes with:** ANTS-4932 (`ants-mcpd`), ANTS-1116 (`ants-helper`'s exit codes), ANTS-3663 (`doc_lint`, whose new checks this item adds by amending that spec), ANTS-5543 and ANTS-5537 (checks a push gate will call through this).

**Layman:** A project's push check is a small script, and it cannot talk to Ants' checking tools today. This lets a script run any of them and get a simple pass or fail.

## 1. Problem

A push gate is a shell script (`local-gate.md` § 2). Ants' document and spec checks exist only as MCP verbs, reached through `ants-mcpd`, which reads JSON-RPC on stdin and whose only flag is `--version` (`src/mcpdmain.cpp`, `main`). So no gate can run `doc_lint` or `spec_lint`, and projects re-implement the checks by hand. ANTS-5543 (a changelog check for push gates) and ANTS-5537 need the same entry point.

## 2. Surface

### 2.1 The command

```
ants-mcpd --call <verb> [<json> | -] [--exit-code]
```

- `<verb>` is a name `ants-mcpd`'s pipeline answers itself: in `ClaudeIntegration::registeredToolNames()` and not in `mcp::terminalScopedVerbNames()`. That includes the inline `tool_info`. Any other name is a usage error.
- `<json>` is one JSON object holding the verb's MCP arguments, as the roadmap item's decided shape requires. No argument is `{}`. Only an explicit `-` reads stdin to EOF, where empty is `{}`. This departs from `ants-helper` (ANTS-1116 INV-10) on purpose: a `pre-push` hook receives git's ref lines on stdin, and a gate script that inherits them must not have them parsed as arguments. Anything that is not a JSON object is a usage error.
- **With no `caller_cwd` in the object, `--call` adds one: the process's working directory.** Verbs such as `doc_lint` and `spec_lint` are `CallerCwdContract::Required` and refuse without it, and a gate runs from the repository root. ANTS-4932 INV-8 forbids synthesising `caller_cwd` on a *forwarded* request; `--call` forwards nothing.
- **`--call` removes the reply-shaping arguments** `compact`, `offload`, `fields` and `raw` before dispatch. They change what the reply looks like, never what is checked, and each can hide `findings` or replace the envelope.

The call runs through the same `ClaudeIntegration` pipeline, registry and gates as stdio mode, built by one shared setup function. Four things differ, and all four stay outside that function:

1. **Offload is off** (`mcp::setOffloadConfig` with offloading disabled), so a large result is printed whole.
2. **Terse responses are off** (`mcp::setTerseDefault(false)`), so default compaction cannot drop an empty `findings`.
3. **No usage snapshot and no forwarder** are set up (ANTS-5311 § 2.4, ANTS-4932 § 2.5).
4. **The reply is unwrapped.** stdout carries the tool result's text with the `ants_mcp_data` wrap removed where it is present (`docs/standards/mcp-tools.md`, response-wrap contract; control-plane verbs such as `tool_info` arrive unwrapped). That text, then a newline, and nothing else on stdout.

The MCP master gate (`claude.mcp_enabled`) is honoured: a user who turned Ants' MCP off has turned these checks off, and a gate sees a refusal rather than a silent pass. Author's call, open to review.

A `--call` run is not a Claude session, which is why it writes no usage snapshot: counting it would inflate the saved-token figures.

### 2.2 Exit codes

The same four meanings `ants-helper` fixed (ANTS-1116 INV-8), so Ants has one convention. **Rules apply in this order, and the first that matches decides:** usage (`2`), then `ok:false` (`1`), then `check_errors` (`1`), then a missing `findings` key (`2`), then findings (`3`), else `0`.

| Code | Meaning |
|---|---|
| `0` | The verb ran and returned `ok:true`, and, with `--exit-code`, found nothing. |
| `1` | The verb returned `ok:false` (a refusal), the call failed at run time, or, with `--exit-code`, the envelope's `check_errors` is non-empty (a check did not complete). stdout still carries the envelope where there is one. |
| `2` | Usage: an unknown flag, no verb, JSON that is not an object, a terminal-scoped or unknown verb, or, with `--exit-code`, an `ok:true` reply with no top-level `findings` key. |
| `3` | With `--exit-code` only: `ok:true` and the envelope's top-level `findings` array is non-empty. |

Without `--exit-code`, findings never change the code: a caller that wants the envelope gets `0` on any `ok:true`.

**`--exit-code` fails closed.** A reply with no `findings` key cannot say whether it found anything, so asking it to gate is a usage error, never a `0`. The rule is read per reply; § 2.1's removal of the reply-shaping arguments is what makes the key's absence mean the verb has none.

### 2.3 Where it lives

- `src/mcpdcall.h` / `src/mcpdcall.cpp`, in `ants_mcpcore_lib` beside `mcpdsocket.cpp` and `mcpdforwarder.cpp`, so a test can call them: argument parsing, the one-shot `McpReplyChannel`, unwrapping, and the envelope-to-exit-code mapping.
- `src/mcpdmain.cpp`: `main` parses `--call` before reading stdin, beside the `--version` check. Both modes run the same pipeline and registry setup lines, so they cannot drift; `--call` returns straight after `mcp::registerProjectScopedVerbs`, before the stdio-only forwarder and usage snapshot.
- Before the call, `--call` sends `tools/list` through the pipeline, as a client does: `tool_info` refuses `tools_not_ready` until something has.

### 2.4 Reload and cost

Each call is a fresh process, so a rebuilt `ants-mcpd` is used by the next call; nothing reaches the terminal (`CLAUDE.md` § Hot reload is the design default). Memory is one `ants-mcpd` process for the life of one verb call, with no daemon.

### 2.5 Alternatives

- **Host it in `ants-helper`.** Rejected: it is off by default (`ANTS_ENABLE_HELPER_CLI`) and has its own subcommand set, so every verb would need registering twice.
- **One executable per verb** (a bare `doc_lint` command, as the roadmap's example reads). Rejected: one binary per verb multiplies install and packaging for no gain. The decided shape, a verb name plus one JSON argument plus `--exit-code`, is kept as the arguments to `--call`.

## 3. Invariants

- **INV-1** — A call's stdout is the JSON envelope the same verb returns over stdio, unwrapped, when the stdio run has terse responses and offload off and both runs get the same arguments with an explicit `caller_cwd` and no reply-shaping key. Broken by a second dispatch path that shapes the result differently. *Test:* `tests/features/mcpd_call/` runs `ants-mcpd --call spec_lint '<args>'` and the same `tools/call` over stdio under a config with both settings off, strips the wrap from the second, and compares them.
- **INV-2** — The exit code follows § 2.2. Broken by any mapping that passes a finding or a refusal as `0`. *Test:* `tests/features/mcpd_call/`, one case per row, each with `--exit-code` unless it says otherwise: a clean fixture gives `0`; a fixture with one finding gives `3`, and `0` without `--exit-code`; `spec_lint` with a `path` escaping the root (`bad_path`) gives `1`; `'[1]'` gives `2`; `tab_list` gives `2`; an unregistered name gives `2`; `tool_info '{"name":"doc_lint"}'` gives `2`.
- **INV-3** — A result over the offload threshold is printed whole. Broken by leaving offload on. *Test:* `tests/features/mcpd_call/` calls a verb whose reply is larger than the lowest threshold `setOffloadConfig` allows (4096 bytes), with offload enabled in a temporary `HOME`'s config and `"offload":true` in the arguments, and asserts stdout has no spill `handle` and holds the full body.
- **INV-4** — With `--exit-code`, a non-empty top-level `check_errors` gives `1`. Broken by reading `findings` alone. *Test:* `tests/features/mcpd_call/` calls the mapping function in `src/mcpdcall.cpp` with a synthesised envelope; this tests the mapping, not a verb producing `check_errors`.
- **INV-5** — With a JSON argument, `--call` reads no stdin and exits after one reply. Broken by falling through to the stdio loop. *Test:* `tests/features/mcpd_call/` runs it with stdin held open and asserts exit within the test's timeout.
- **INV-6** — A `--call` run leaves no file in `TokenUsageEngine::peerSnapshotDir()`. Broken by sharing stdio mode's snapshot setup. *Test:* `tests/features/mcpd_call/` lists the directory under a temporary `HOME` before and after a call.
- **INV-7** — With `claude.mcp_enabled` false, `--call` exits `1` with the master gate's refusal. Broken by bypassing the gate. *Test:* `tests/features/mcpd_call/` with that key set false in a temporary `HOME`'s config.

- **INV-8** — With no `caller_cwd` in the arguments, a `Required` verb runs against the process's working directory. Broken by passing the arguments through unchanged. *Test:* `tests/features/mcpd_call/` runs `spec_lint` with `{}` from a fixture project's root and asserts `ok:true` and that fixture's spec in `checked_docs`.
- **INV-9** — `compact`, `offload`, `fields` and `raw` never reach the verb, and terse responses are off. Broken by passing them through, which lets a clean reply lose its empty `findings` and exit `2`. *Test:* `tests/features/mcpd_call/` enables terse responses in a temporary `HOME`'s config, calls `doc_lint` on a clean fixture with `"compact":true,"fields":["ok"]`, and asserts `findings` is present and the exit is `0`.

## 4. Out of scope

- The new `doc_lint` checks. This item adds them by amending ANTS-3663 in place.
- Terminal-scoped verbs. They need a running terminal, and a gate must not depend on one.
- A resident server for repeated calls.

**A test's "temporary `HOME`" sets `XDG_CONFIG_HOME` and `XDG_DATA_HOME` inside it as well.** Where either is set, as it is on the development machine (measured 2026-09-28), `HOME` alone redirects neither the config nor the snapshot directory, and INV-3, INV-6, INV-7 and INV-9 would pass against the real ones.

## 5. Tests

- `tests/features/mcpd_call/` — INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9. It drives the built `ants-mcpd` binary as a subprocess; `build_target_for` names its bundle once the test exists.

## Cold-eyes loop log

The rows are in [`../reviews/ANTS-5506-mcpd-call-loop-log.md`](../reviews/ANTS-5506-mcpd-call-loop-log.md).
