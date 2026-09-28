# mcpd_call — ANTS-5506

The contract is `docs/specs/ANTS-5506-mcpd-call.md` § 3. This directory holds
the cases § 5 assigns to it: INV-1 through INV-9, each named for its
invariant, plus two pairs of unit-level cases against the free functions in
`src/mcpdcall.h` ahead of any process.

| Case | Invariant | How |
|---|---|---|
| `Inv1StdoutMatchesStdioUnwrapped` | INV-1 | `spec_lint` on one fixture, called both ways: over stdio (terse responses and offload OFF in that child's config) and through `ants-mcpd --call` (terse and offload left ON in *its* child's config, on purpose — to prove `--call` forces them off itself rather than inheriting the setting). Same explicit `caller_cwd` + `path`, no reply-shaping key, on either side. The stdio side's wrap is stripped by `McpdSession::payload`; the `--call` side's stdout is parsed directly, with no wrap to strip. The two JSON objects must be equal. |
| `Inv2ExitCodesFollowSection2Point2` | INV-2 | One `ants-mcpd --call` per § 2.2 row, table-driven in one case: a clean `spec_lint` fixture (`0`); a one-finding fixture with `--exit-code` (`3`) and the same fixture without `--exit-code` (`0`); `spec_lint` with `path:"../outside.md"` escaping the root, `bad_path` (`1`); a non-object JSON argument `[1]` (`2`); the terminal-scoped `tab_list` (`2`); an unregistered verb name (`2`); and `tool_info {"name":"doc_lint"}`, whose descriptor reply carries no top-level `findings` key (`2`). |
| `Inv3OffloadIsForcedOff` | INV-3 | `read_region` on a fixture file (`start_line`/`end_line` covering the whole file, `offload:true` in the arguments) — over the 4096-byte offload floor on its own — against a child config with offload enabled and the threshold clamped to that floor. Asserts stdout carries no `handle` key, `truncated:false`, and `returned` equals the fixture's known line count. `read_region` is deliberately the verb here, not `tool_info`: `mcp::isOffloadEligible` (`src/mcpprojection.cpp`) does not list `tool_info`, so a `tool_info`-based case cannot fail this invariant however offload is configured — ANTS-5506 Q3 mutation finding, a mutant that left offload on for `--call` survived against the original case. |
| `Inv4CheckErrorsWithExitCodeIsFailed` | INV-4 | Calls `mcpd::exitCodeFor` directly with a synthesised `{ok:true, findings:[], check_errors:[...]}` envelope and `exitCodeFlag:true`; asserts `CallFailed`. Tests the mapping function in isolation, not a verb that actually produces `check_errors`. |
| `Inv5DoesNotReadStdinWithAJsonArgument` | INV-5 | Runs `--call spec_lint <json>` with stdin left open (never closed by the test) and asserts the process still exits inside the test's bound — it must not fall through to the stdio read loop, which would wait on that open stdin forever. |
| `Inv6WritesNoUsageSnapshot` | INV-6 | Lists `TokenUsageEngine::peerSnapshotDir()` (`<XDG_DATA_HOME>/ants-terminal/mcpd-usage`) under the child's own `XDG_DATA_HOME` before and after a `--call` run; asserts no file appears either side. |
| `Inv7MasterGateRefusesWithExitOne` | INV-7 | `claude.mcp_enabled:false` in the child's config; asserts exit `1` and `mcp_disabled` present in stdout. |
| `Inv8NoCallerCwdUsesProcessCwd` | INV-8 | `--call spec_lint {}` run with the process's working directory set to a fixture project's root and no `caller_cwd` in the arguments; asserts `ok:true` and that fixture's spec path in `checked_docs`. |
| `Inv9ReplyShapingArgsAndTerseAreOverridden` | INV-9 | `claude.mcp_terse_responses:true` in the child's config (the opposite of what `--call` must force); `--call doc_lint` on a clean fixture with `{"compact":true,"fields":["ok"]}` and `--exit-code`; asserts `findings` is present in stdout (not dropped by terse compaction, and not narrowed away by `fields`) and the exit code is `0`. |
| `Inv9TerseIsForcedOffForADefaultCompactVerb` | INV-9 | `read_region` on a small fixture file, terse ON in both the stdio reference run's config and `--call`'s own config. `mcp::isDefaultCompactTool` (`src/mcpprojection.cpp`) lists `read_region` but neither `spec_lint` nor `doc_lint`, so this is the one case in the suite where terse compaction actually changes the reply shape — a clean read's `truncated:false` is dropped by default compaction and kept only when terse is genuinely off. The stdio call is a control proving the field really is dropped there, before checking `--call` keeps it. ANTS-5506 Q3 mutation finding: `Inv1`/`Inv9` above both call verbs terse compaction never touches, so a mutant that left terse on for `--call` passed every existing case untouched. |

Two pairs of cases exercise the free functions directly, ahead of any
process — the seam `src/mcpdcall.h` exists for:

| Case | What |
|---|---|
| `ParseCallRequestNoJsonArgumentDefaultsToEmptyObject` | § 2.1 — `--call <verb>` with no JSON argument parses to `args == {}`, `isCall == true`, `readStdin == false`. |
| `ParseCallRequestDashReadsStdin` | § 2.1 — an explicit `-` JSON argument sets `readStdin == true`. |
| `UnwrapToolTextStripsTheWrapWhenPresent` | § 2.1 point 4 — a wrapped `<ants_mcp_data>` tool result comes back as the bare JSON. |
| `UnwrapToolTextLeavesControlPlaneTextUnchanged` | § 2.1 point 4 — text that was never wrapped (a control-plane verb's reply) comes back unchanged. |

**Isolation.** Every case that starts `ants-mcpd` runs the built binary as a
child process, named by `ANTS_MCPD_BIN`
(`tests/features/standalone_mcp_server/mcpd_session.h`). The bundle's shared
sandbox (`tests/bundle_main_gui.cpp`) points `XDG_CONFIG_HOME` /
`XDG_DATA_HOME` at a `QTemporaryDir` with no `config.json` in it, which is
correct for a case that only needs isolation from the user's real files. A
case that needs a *particular* `claude.mcp_*` key, or needs to inspect
`TokenUsageEngine::peerSnapshotDir()`, instead sets `XDG_CONFIG_HOME` and/or
`XDG_DATA_HOME` on that one child to its own fresh `QTemporaryDir`, and writes
`config.json` under `<XDG_CONFIG_HOME>/ants-terminal/config.json`
(`src/config.cpp` `Config::configPath()` — the on-disk keys are the flat
dotted strings `Config`'s accessors read, e.g. `"claude.mcp_enabled"`, not a
nested object).

## Cold-eyes loop log

Reviewed alongside `docs/specs/ANTS-5506-mcpd-call.md`; see
`docs/reviews/ANTS-5506-mcpd-call-loop-log.md`.
