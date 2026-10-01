# ANTS-5485 — Remove the twenty deprecated Ants MCP verbs

**Status:** accepted (2026-10-01).
**Kind:** chore.
**Source:** ROADMAP.md ANTS-5485 (user-request-2026-09-27: mark, measure,
remove). Step 1, the deprecation notice, shipped in 0.7.112.
**Composes with:** ANTS-4932 (ants-mcpd serves the project-scoped verbs),
ANTS-1402 (`recordDispatch`'s failure branch).

**Layman:** Twenty old Ants tools that no Claude session uses are taken
out, so every session stops paying to read their descriptions. A session
that still asks for one is told what replaced it.

## 1. Problem

Each verb in `tools/list` is read by every Claude Code session at start.
Twenty of them are no longer used:

1. `mcp::table()` (`src/mcpdeprecation.cpp`) lists them with their
   replacements, and since 0.7.112 each description leads with
   `DEPRECATED (ANTS-5485)`.
2. `recordDeprecatedCall` appends every call to
   `~/.local/share/ants-terminal/deprecated-calls.jsonl`. That file holds
   one line, the 2026-09-30 positive-control call to `get_git_status`
   (`wc -l` → 1). No session called any of the twenty after the notice
   shipped.
3. claude-config removed every use under `~/.claude` (claude-config
   e10268a) and asked for none to be kept (session message, 2026-10-01).
   `hooks/ants-bash-veto.sh` no longer names `get_git_status` (Ants
   df24e84d).

The twenty: `get_git_status`, `current_state`, `session_brief`,
`cross_doc_diff`, `cold_eyes_cross_doc_diff`, `cold_eyes_single_doc`,
`cold_eyes_fold_in`, `indie_review_orchestrate`, `indie_review_brief`,
`indie_review_synthesis_prompt`, `indie_review_fold_in`,
`test_audit_fold_in`, `test_audit_recheck`, `test_audit_synthesis_prompt`,
`debt_sweep_scan`, `debt_sweep_apply_fix`, `debt_sweep_triage_prompt`,
`debt_sweep_defer`, `plan_template`, `roadmap_branch_drift`.

Today a removed name would get `finishToolDispatch`'s generic
`Unknown tool: <name>` (JSON-RPC -32602), which says nothing about what
replaced it.

## 2. Surface

### 2.1 Where a verb exists

No single list drives registration. Each verb is removed from every one of
these that names it (inventory, 2026-10-01):

| Place | Symbol |
|---|---|
| Handler registration (both servers) | `mcp::registerProjectScopedVerbs` (`src/mcptoolregistry.cpp`); `get_git_status` alone is registered in `MainWindow::setupClaudeMcpProviders` |
| Terminal-forwarded set | `mcp::terminalScopedVerbNames()` (`get_git_status`) |
| `tools/list` descriptor | the `["name"] = "<verb>"` descriptor blocks in `ClaudeIntegration::handleMcpRequest` |
| Caller-cwd contract | `ClaudeIntegration::callerCwdContractFor` |
| Token cost | `kCosts` in `handleMcpRequest`'s `tokenCostFor` |
| Kind bucket | `kindForName` in `handleMcpRequest` (explicit rows; prefix rows stay for the kept verbs) |
| Rate-limit class | `ClaudeIntegration::rateLimitClassFor` |
| ETag allowlist | `ClaudeIntegration::isEtagSupportedTool` |
| Offload allowlist | `isOffloadEligible` (`src/mcpprojection.cpp`) |
| Token budget | the table in `src/tokenusageengine.cpp` |
| Handler bodies | `RemoteControl::cmd*` in `remotecontrol_state.cpp`, `remotecontrol_coldeyes.cpp`, `remotecontrol_review.cpp`; the three `test_audit_*` lambdas in `mcptoolregistry.cpp`; `get_git_status`'s lambda in `MainWindow::setupClaudeMcpProviders` |
| A name inside kept code | `TestAuditEngine::synthesize` uses `test_audit_synthesis_prompt` in its `reports_dir required` error text and its `PathValidation::validatePath` label; both become `test audit synthesis` |

The removal lands in both servers at once, because `registerProjectScopedVerbs`
serves the terminal and ants-mcpd alike.

### 2.2 What a removed name gets

`src/mcpdeprecation.{h,cpp}` keeps `table()` and becomes the removed-verb
record. Everything else in it goes: `deprecationPrefix`,
`withDeprecationAdvisory`, `recordDeprecatedCall`, `deprecatedCallsPath`.
The table is never listed, so it costs no session anything.

```cpp
namespace mcp {
// ANTS-5485 — what replaced a removed verb; empty for any other name.
QString removedReplacement(const QString &verb);
// The JSON-RPC error a call to a removed verb gets; empty object otherwise.
QJsonObject removedVerbError(const QString &verb);
}
```

`removedVerbError("get_git_status")` returns:

```json
{"code": -32602,
 "message": "Tool get_git_status was removed (ANTS-5485); use git_state instead.",
 "data": {"code": "verb_removed", "replacement": "git_state"}}
```

`finishToolDispatch`'s unknown-tool branch uses it when it is non-empty
and keeps today's `Unknown tool: <name>` otherwise. `recordDispatch` still
records `tool_not_found`. `tool_info`'s `unknown_tool` refusal adds
`replacement` for a removed name.

The existing `deprecated-calls.jsonl` is user data and is left on disk.

### 2.3 What stays

Kept because a GUI dialog or a kept verb uses it:

- `DebtSweepEngine::scanAll`, `applyMechanicalFix`, `triagePrompt`,
  `evaluateTriageGate`, `templateDebtSweepFoldInBlock` (AuditDialog's
  debt-sweep tab).
- `IndieReviewEngine::synthesisPrompt`, `assembleThreatModelExtras`,
  `templateIndieReviewFoldInBlock` (IndieReviewDialog);
  `derivePartition`, `corroboratedFindings*` (`indie_review_partition`,
  `indie_review_dispatch`, `indie_review_corroborate`).
- `ColdEyesEngine::crossDocDiffFromReports`, `templateColdEyesFoldInBlock`,
  `assembleBriefManifest` (ColdEyesDialog, `cold_eyes_brief`).
- `TestAuditEngine::synthesize`, `foldIn` (TestAuditDialog).
- `RoadmapFoldIn::*`, `collectGitSnapshot` (`verify_changes`).
- `RemoteControl::cmdCurrentState`: unregistered, but
  `cmdSessionOrient` still builds its `current_state` block from it.

Deleted because only a removed verb calls it: `PlanTemplateEngine`
(`src/plantemplateengine.{h,cpp}`), `ColdEyesEngine::assembleSingleDocBrief`,
`ColdEyesEngine::crossDocDiffFromDir`,
`ColdEyesEngine::templateColdEyesFoldInBlockFreeform`,
`IndieReviewEngine::assembleBriefManifest`, `TestAuditEngine::recheck`,
`DebtSweepEngine::detectorsByCategory`, and the handler bodies of the twenty other than `cmdCurrentState`.

### 2.4 Reaching a running terminal

19 of the 20 leave with an ants-mcpd rebuild and `/mcp`, with no terminal
relaunch. `get_git_status` is registered by the terminal itself and leaves
at the next relaunch; until then it still answers, which harms nothing.

## 3. Invariants

`V` below is the twenty names joined with `|`, as listed in § 1.

- **INV-1** — Neither server registers or lists a removed verb.
  *Test:* `grep -rcE 'registerToolProvider\("(V)"' src/mcptoolregistry.cpp src/mainwindow.cpp`
  → 0 in each file (19 and 1 before the change), and
  `grep -cE '\["name"\] = "(V)"' src/claudeintegration.cpp` → 0
  (20 before).
- **INV-2** — A call to a removed verb returns the § 2.2 error: code
  -32602, a message naming the replacement, and
  `data.code == "verb_removed"`. *Test:*
  `tests/features/mcp_removed_verbs` calls `mcp::removedVerbError` for all
  twenty, and scrapes `finishToolDispatch`'s unknown-tool branch for the
  call. It breaks if the branch keeps the generic message.
- **INV-3** — A name that was never a verb keeps today's reply,
  `Unknown tool: <name>` with no `data`. *Test:*
  `tests/features/mcp_removed_verbs`: `removedVerbError("no_such_verb")`
  is empty.
- **INV-4** — `tool_info` on a removed name refuses `unknown_tool` and
  carries `replacement`. *Test:* `tests/features/mcp_removed_verbs`
  scrapes the `unknown_tool` branch for `removedReplacement`.
- **INV-5** — The deprecation advisory is gone: no description prefix, no
  `deprecated` reply key, no write to `deprecated-calls.jsonl`. *Test:*
  `grep -rnE 'withDeprecationAdvisory|deprecationPrefix|recordDeprecatedCall|deprecated-calls' src`
  → no output.
- **INV-6** — Outside the removed-verb table, no source file names a
  removed verb as a quoted string, except the two `current_state` lines in
  `cmdSessionOrient` (its `noteOrFail` label and its reply key). *Test:*
  `grep -rnE '"(V)"' src` lists only `src/mcpdeprecation.cpp` and those
  two lines of `src/remotecontrol_state.cpp`. Before the change it lists
  ten files.
- **INV-7** — Every kept surface in § 2.3 survives: the kept verbs
  `indie_review_partition`, `indie_review_corroborate`,
  `indie_review_dispatch`, `cold_eyes_partition`, `cold_eyes_brief`,
  `test_audit_partition`, `test_audit_brief`, `session_orient` and
  `verify_changes` are still registered, and `session_orient` still
  returns `current_state`. *Test:* `tests/features/mcp_removed_verbs`
  scrapes the registry for the nine; `tests/features/session_orient_bundle`;
  the dialog suites (`ctest --preset=default`) stay green.
- **INV-8** — The verb-only code in § 2.3 is deleted, and no comment
  in the files that held it still names it. *Test:* each of these prints
  nothing:
  `grep -rnE 'assembleSingleDocBrief|crossDocDiffFromDir|templateColdEyesFoldInBlockFreeform|cmdSessionBrief|cmdRoadmapBranchDrift' src`;
  `grep -n 'assembleBriefManifest' src/indiereviewengine.h src/indiereviewengine.cpp src/verifyengine.cpp`;
  `grep -nE '\brecheck\(' src/testauditengine.h src/testauditengine.cpp`;
  `grep -n 'detectorsByCategory' src/debtsweepengine.h src/debtsweepengine.cpp`;
  `grep -ni 'plantemplate' CMakeLists.txt`; `ls src/plantemplateengine.*`
  (each prints lines before the change). The qualified spelling is not
  used because the definitions sit inside their namespace unqualified.
- **INV-9** — Each spec whose title names a removed verb carries
  `**Superseded by:** ANTS-5485`: ANTS-1279, ANTS-1281, ANTS-1290,
  ANTS-1569, ANTS-1583, ANTS-1635, ANTS-1644. *Test:*
  `grep -L 'Superseded by:\*\* ANTS-5485' docs/specs/ANTS-1279.md docs/specs/ANTS-1281.md docs/specs/ANTS-1290.md docs/specs/ANTS-1569.md docs/specs/ANTS-1583.md docs/specs/ANTS-1635.md docs/specs/ANTS-1644.md`
  → no output.
- **INV-10** — `docs/standards/` describes no removed verb as live.
  `mcp-error-codes.md` lists `verb_removed`. *Test:*
  `grep -rlE '(V)' docs/standards` lists only `mcp-error-codes.md`, where
  each hit is the `verb_removed` row or history (5 files before).

## 4. RAM / build cost

Removes code only: `src/plantemplateengine.cpp` leaves the build, and no
state is added. The removed-verb table is a static hash of twenty short
strings.

## 5. Out of scope

- Re-wording historical specs and CHANGELOG entries that mention a removed
  verb — not done, because shipped records describe what was true then
  (`specs.md` § 5.6). INV-9 marks only the specs whose whole subject went.
- The `kindForName` prefix rows (`cold_eyes_`, `indie_review_`,
  `test_audit_`, `debt_sweep_`) — kept, because kept verbs carry those
  prefixes.
- `tools/mcp-bridge.py` — tracked by ANTS-5308.

## 6. Tests

New feature test `tests/features/mcp_removed_verbs/` covers INV-2, INV-3,
INV-4 and the INV-7 registry scrape; label `features;fast`. Verify it
fails against pre-change source first.

Deleted with their verbs (they test nothing that stays):
`mcp_roadmap_branch_drift`, `mcp_session_brief`, `mcp_current_state`,
`mcp_plan_template_tool`, `mcp_debt_sweep_tools`,
`indie_review_fold_in_narrative`, `cold_eyes_fold_in_narrative`,
`fold_in_caller_anchor`, `indie_review_brief_manifest`,
`test_audit_recheck`, `plan_template_engine`, `cold_eyes_fold_in_freeform`,
`mcp_verb_deprecation`.

Edited, not deleted, where they also cover a kept verb: `mcp_cold_eyes`,
`mcp_indie_review_tools`, `mcp_test_audit_trio`, and every other test that
names a removed verb in a list (the build and suite find them).

INV-1, INV-5, INV-6, INV-8, INV-9 and INV-10 are commands, run at
review.

## 7. Cross-doc impact

- `docs/standards/`: `mcp-error-codes.md` (add `verb_removed`, drop the
  removed verbs' rows), `audit-false-positives.md`, `mcp-tools.md`,
  `test-audit-resume.md`, `mcp-behavioural-notes.md`.
- The seven specs in INV-9.
- CHANGELOG (`### Removed`).
- `tests/features/*/spec.md` of the edited tests where they list a
  removed verb.

## Cold-eyes loop log

The rows are in [`docs/reviews/ANTS-5485-retire-deprecated-verbs-loop-log.md`](../reviews/ANTS-5485-retire-deprecated-verbs-loop-log.md).
