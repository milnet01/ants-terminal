# ANTS-5506 — Run an Ants MCP verb from a shell: `ants-mcpd --call`

**Status:** spec draft (2026-09-28).
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

- `<verb>` is a project-scoped verb: one `mcp::registerProjectScopedVerbs` registers. A name in `mcp::terminalScopedVerbNames()`, or a name nothing registers, is a usage error.
- `<json>` is one JSON object holding the verb's MCP arguments exactly, as the roadmap item's decided shape requires. `-`, or no argument, reads stdin to EOF; empty stdin is `{}`, as `ants-helper` does (ANTS-1116 INV-10). Anything that is not a JSON object is a usage error.
- With no `caller_cwd` in the object, the root resolves as it does for any `ants-mcpd` call without one: the process's own working directory (ANTS-4932 INV-6). A gate runs from the repository root, so that is the project.

The call runs through the same `ClaudeIntegration` pipeline, registry and gates as stdio mode, built by the same code. Two things differ:

1. **Offload is off** (`mcp::setOffloadConfig` with offloading disabled), so a large result is printed whole rather than replaced by a spill handle a script cannot read.
2. **The reply is unwrapped.** stdout carries the verb's JSON envelope, the text inside the `ants_mcp_data` wrap (`docs/standards/mcp-tools.md`, response-wrap contract), as one line with a trailing newline. Nothing else goes to stdout.

The MCP master gate (`claude.mcp_enabled`) is honoured: a user who turned Ants' MCP off has turned these checks off, and a gate sees a refusal rather than a silent pass. Author's call, open to review.

A `--call` run writes no usage snapshot (ANTS-5311): it is not a Claude session, and counting it would inflate the saved-token figures.

### 2.2 Exit codes

The same four meanings `ants-helper` fixed (ANTS-1116 INV-8), so Ants has one convention:

| Code | Meaning |
|---|---|
| `0` | The verb ran and returned `ok:true`, and, with `--exit-code`, found nothing. |
| `1` | The verb returned `ok:false` (a refusal), the call failed at run time, or, with `--exit-code`, the envelope's `check_errors` is non-empty (a check did not complete). stdout still carries the envelope where there is one. |
| `2` | Usage: an unknown flag, no verb, JSON that is not an object, a terminal-scoped or unknown verb, or `--exit-code` on a verb whose envelope has no top-level `findings` key. |
| `3` | With `--exit-code` only: `ok:true` and the envelope's top-level `findings` array is non-empty. |

When `--exit-code` finds both a non-empty `check_errors` and a non-empty `findings`, the exit is `1`: could-not-check outranks found-a-problem.

Without `--exit-code`, findings never change the code: a caller that wants the envelope gets `0` on any `ok:true`.

**`--exit-code` fails closed.** A verb with no `findings` key cannot say whether it found anything, so asking it to gate is a usage error, never a `0`.

### 2.3 Where it lives

- `src/mcpdcall.h` / `src/mcpdcall.cpp`, in the `ants-mcpd` target: argument parsing, the one-shot `McpReplyChannel`, unwrapping, and the envelope-to-exit-code mapping, as free functions a test can call.
- `src/mcpdmain.cpp`: `main` checks for `--call` before reading stdin, beside the `--version` check. The pipeline and registry setup moves into one function both modes call, so they cannot drift.

### 2.4 Reload and cost

Each call is a fresh process, so a rebuilt `ants-mcpd` is used by the next call; nothing reaches the terminal (`CLAUDE.md` § Hot reload is the design default). Memory is one `ants-mcpd` process for the life of one verb call, with no daemon.

### 2.5 Alternatives

- **Host it in `ants-helper`.** Rejected: it is off by default (`ANTS_ENABLE_HELPER_CLI`) and has its own subcommand set, so every verb would need registering twice.
- **One executable per verb** (a bare `doc_lint` command, as the roadmap's example reads). Rejected: one binary per verb multiplies install and packaging for no gain. The decided shape, a verb name plus one JSON argument plus `--exit-code`, is kept as the arguments to `--call`.

## 3. Invariants

- **INV-1** — A call's stdout is the JSON envelope the same verb returns over stdio for the same arguments, unwrapped. Broken by a second dispatch path that shapes the result differently. *Test:* `tests/features/mcpd_call/` runs `ants-mcpd --call spec_lint '<args>'` and the same `tools/call` over stdio, strips the wrap from the second, and compares them.
- **INV-2** — The exit code follows § 2.2. Broken by any mapping that passes a finding or a refusal as `0`. *Test:* `tests/features/mcpd_call/`, one case per row: a clean fixture gives `0`; a fixture with one finding gives `3` with `--exit-code` and `0` without; a missing path gives `1`; `'[1]'` gives `2`; `tab_list` gives `2`; an unregistered name gives `2`; `--exit-code` on `tool_info` gives `2`.
- **INV-3** — A result over the offload threshold is printed whole. Broken by leaving offload on. *Test:* `tests/features/mcpd_call/` sets a tiny offload threshold in a temporary `HOME`'s config and asserts stdout has no spill `handle` and holds the full body.
- **INV-4** — With `--exit-code`, a non-empty top-level `check_errors` gives `1`. Broken by reading `findings` alone. *Test:* `tests/features/mcpd_call/` calls the mapping function in `src/mcpdcall.cpp` with a synthesised envelope, and with a second where both `check_errors` and `findings` are non-empty, expecting `1`; this tests the mapping, not a verb producing `check_errors`.
- **INV-5** — With a JSON argument, `--call` reads no stdin and exits after one reply. Broken by falling through to the stdio loop. *Test:* `tests/features/mcpd_call/` runs it with stdin held open and asserts exit within the test's timeout.
- **INV-6** — A `--call` run leaves no file in `TokenUsageEngine::peerSnapshotDir()`. Broken by sharing stdio mode's snapshot setup. *Test:* `tests/features/mcpd_call/` lists the directory under a temporary `HOME` before and after a call.
- **INV-7** — With `claude.mcp_enabled` false, `--call` exits `1` with the master gate's refusal. Broken by bypassing the gate. *Test:* `tests/features/mcpd_call/` with that key set false in a temporary `HOME`'s config.

## 4. Out of scope

- The new `doc_lint` checks. This item adds them by amending ANTS-3663 in place.
- Terminal-scoped verbs. They need a running terminal, and a gate must not depend on one.
- A resident server for repeated calls.

## 5. Tests

- `tests/features/mcpd_call/` — INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7. It drives the built `ants-mcpd` binary as a subprocess; `build_target_for` names its bundle once the test exists.

## Cold-eyes loop log

The rows are in [`../reviews/ANTS-5506-mcpd-call-loop-log.md`](../reviews/ANTS-5506-mcpd-call-loop-log.md).
