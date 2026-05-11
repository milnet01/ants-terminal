# claude_pid_replacement — ANTS-1225

> **Source:** user 2026-05-10 — *"There is also no Claude Code status until I change tabs."* Reproduced live: `/compact` → `/exit` → `claude --resume <id>` from the same shell tab leaves the bottom-bar Claude status indicator (`m_statusLabel`, "Claude: idle/thinking/...") hidden until the user changes tabs and back.
>
> **Reproduction state.** Live observed in session `94218f91-…` (Ants Terminal v0.7.81 + Claude Code v2.1.138). Debug log shows `procStartMs=0` for every `sessionPathForCwd/result:` line — `m_claudePid` is non-zero but its process has died, so `processStartTimeMs(m_claudePid)` returns 0 and the resolver falls back to recency-only mode (which still picks the right JSONL because tasks chip works). Tab-switch fixes the indicator because `setShellPid` zeroes `m_claudePid` (`claudeintegration.cpp:88`), unblocking the next poll's "newly detected" branch.

## 1. Contract

`ClaudeIntegration::pollClaudeProcess` MUST treat any transition where `findClaudeChildPid(m_shellPid)` returns a PID different from `m_claudePid` — whether `m_claudePid == 0` (initial detection) or a stale dead PID from a prior Claude that exited within the 2 s poll window (live PID replacement) — as a **fresh detection**: rebind `m_transcriptPath` via `sessionPathForCwd`, re-arm the transcript watcher, reseed state from the new transcript tail via `parseTranscriptForState`, and emit `stateChanged(Idle)` if the cached `m_state` differs.

## 2. Invariants

INV labels qualified `ANTS-1225-INV-N`. Citations are against current `src/`.

| # | Lane | Statement |
|---|------|-----------|
| 1 | claudeintegration | `pollClaudeProcess` enters the rebind branch when `m_claudePid != foundPid && foundPid > 0`. The pre-existing `m_claudePid == 0` gate is generalized — initial-detection is the `m_claudePid == 0 && foundPid > 0` sub-case of the new gate. |
| 2 | claudeintegration | When the rebind branch fires, in this order: `m_claudePid = foundPid` → `sessionPathForCwd(projectCwd, processStartTimeMs(m_claudePid), nowMs)` resolves a path. **If non-empty:** `m_transcriptWatcher.removePaths(oldFiles)` + `m_transcriptWatcher.addPath(m_transcriptPath)` + `parseTranscriptForState(m_transcriptPath)` execute in this order. **If empty:** the watcher is NOT swapped, `m_transcriptPath` retains its prior value, `parseTranscriptForState` is NOT called for the rebind path (the 10-tick backstop continues to handle the prior path). Either way, `m_state` is set to `ClaudeState::Idle` and `emit stateChanged(m_state, "idle")` fires, gated by `if (m_state != ClaudeState::Idle)` (mandatory — removing the guard breaks the "no flap on steady-state Idle" property). |
| 3 | claudeintegration | When `findClaudeChildPid` returns `0` (no Claude under the focused tab's shell), the existing `!found` branch still clears `m_claudePid`, `m_transcriptPath`, and emits `stateChanged(NotRunning)`. INV-1 does not subsume this — both branches coexist on the two sides of `if (!found)`. |
| 4 | claudeintegration | The rebind branch is reachable only when `m_shellPid > 0` — the early-return at `claudeintegration.cpp:214` (`if (m_shellPid <= 0) return;`) gates the whole function, including INV-1's path. |

## 3. Negative invariants (what MUST NOT happen)

| # | Statement | Decomposes to |
|---|-----------|---------------|
| N1 | The bottom-bar Claude status indicator MUST NOT remain hidden after a `/compact` + `/exit` + `claude --resume` (or "Continue previous coding session") cycle that completes within the 2 s poll window. | INV-1 fires on the next poll; INV-2 emits `stateChanged(m_state, "idle")`; the existing controller connection (`claudestatuswidgets.cpp:240-245`) calls `apply()`; the `m_lastState != NotRunning` branch in `apply` shows the label. |
| N2 | Linux PID reuse MUST NOT silently retain a stale `m_claudePid` whose PID is now bound to a non-Claude process. | `findClaudeChildPid` matches argv[0] basename against `claude` / `claude-code` (`claudeintegration.cpp:174-208`); a non-Claude PID returns 0; INV-3's not-found branch fires and clears state. |
| N3 | The fix MUST NOT regress same-PID rebind into a transcript flap. | INV-1's gate is `!=`, so `m_claudePid == foundPid` — the steady-state — never enters the rebind branch. The 10-tick backstop re-parse at `claudeintegration.cpp:275` continues to handle inotify-watch-loss recovery without touching `m_transcriptPath`. |

## 4. Out of scope

- **Resolver correctness post-rebind.** `sessionPathForCwd` selection logic (process-start anchor, freshness floor, stale-session liveness filter) is owned by `claude_session_freshness` (ANTS-1163). INV-2 asserts the rebind invokes the resolver — not that the resolver picks the right file given valid inputs.
- **Per-tab Claude PID under multi-tab Claude.** `m_claudePid` is singleton-scoped on `ClaudeIntegration`. If two tabs each run Claude, the focused-tab gate (ANTS-1161) decides which PID `m_claudePid` tracks. PID-replacement on the *unfocused* tab is not in scope here.
- **`findClaudeChildPid` correctness.** The /proc walk and exec-name match are owned by ANTS-1048. This spec assumes `findClaudeChildPid` returns the right PID when one exists.
- **Tasks chip (ANTS-1158/1219).** Driven by the resolver + JSONL-on-disk recency, not by `m_claudePid`. The same `/compact`+relaunch sequence that breaks the status indicator does NOT break the tasks chip — they are independent surfaces. The pre-fix divergence (chip works, indicator broken) is in fact the diagnostic signature of this bug.

## 5. Acceptance

`ctest -L features -R claude_pid_replacement` exits zero.

Test shape (source-grep, no GUI, no QTemporaryDir, no `ClaudeIntegration` instantiation; same pattern as ANTS-1219):

1. **INV-1** — grep `src/claudeintegration.cpp` for `if (m_claudePid != foundPid)` inside the body of `pollClaudeProcess`. Anchored by `// ANTS-1225-INV-1`.
2. **INV-2** — grep for the load-bearing rebind sequence inside the same branch: `m_transcriptWatcher.removePaths`, `m_transcriptWatcher.addPath(m_transcriptPath)`, `parseTranscriptForState(m_transcriptPath)`, `emit stateChanged(m_state, "idle")`. Anchor: `// ANTS-1225-INV-2`.
3. **INV-3** — grep for the not-found branch's clear sequence (`m_claudePid = 0`, `m_transcriptPath.clear()`, `emit stateChanged(m_state, m_currentTool)`) so a future refactor that collapses INV-1 into the not-found path is caught. Anchor: `// ANTS-1225-INV-3`.
4. **INV-4** — grep for `if (m_shellPid <= 0) return;` at the top of `pollClaudeProcess`. Anchor: `// ANTS-1225-INV-4`.

INV-N anchor comments embedded in `claudeintegration.cpp` next to each load-bearing string so the grep is intentional, not coincidental — same convention as ANTS-1163-INV-9/10 and ANTS-1219-INV-1..6.

**Anchors are mandatory, not optional**, because the load-bearing strings in this spec are NOT unique within `claudeintegration.cpp`:

- `emit stateChanged(m_state, "idle")` appears at **two** sites (line 286 in `pollClaudeProcess`'s rebind branch — the INV-2 site — and line 1038 inside another emission path).
- `parseTranscriptForState(m_transcriptPath)` appears at **three** sites (line 37 in the constructor's initial seed, line 277 in the rebind branch — the INV-2 site — and line 297 in the 10-tick backstop re-parse).

Without `// ANTS-1225-INV-N` anchors next to the INV-2 site, the source-grep test could match the wrong location and silently green-light a refactor that broke the rebind branch. The anchor comments now sit on the same line as (or the line immediately preceding) each load-bearing string at lines 236, 262/281, 223, and 214 of the post-fix file.

Test slot: `tests/features/claude_pid_replacement/test_claude_pid_replacement.cpp`. Wired into the existing `test_claude` bundle per ANTS-1217 — no new standalone executable, no new CMake target.

## 6. Memory budget

Zero new fields, zero new caches, zero new buffers.

- Generalizing the gate from `m_claudePid == 0` to `m_claudePid != foundPid` adds no state — `foundPid` is already a stack-local in the same scope.
- The existing `parseTranscriptForState` and `m_transcriptWatcher` calls are unchanged.
- Test is source-grep only — peak working set ≈ size of `claudeintegration.cpp` slurp (~70 KiB at the time of writing). No fixtures.
- Build cost: one new test source in the `test_claude` bundle (per `feedback_be_mindful_of_system_resources`); zero new `add_executable`, zero new Qt6 link step.

## 7. How to verify pre-fix code fails

To confirm each grep would catch a real regression:

- **INV-1:** revert the gate to `if (m_claudePid == 0)` at `claudeintegration.cpp:229`. Acceptance step 1 turns red. (Manual repro: `/compact` + `/exit` + `claude --resume` in the same tab; status indicator stays hidden until tab-switch.)
- **INV-2:** delete `m_transcriptWatcher.addPath(m_transcriptPath)` (or `parseTranscriptForState(m_transcriptPath)`). Acceptance step 2 turns red.
- **INV-3:** delete `m_claudePid = 0;` from the not-found branch. Acceptance step 3 turns red. (This would mean the indicator never hides when Claude exits — separate regression.)
- **INV-4:** delete the `if (m_shellPid <= 0) return;` early-out. Acceptance step 4 turns red. (Without it, `findClaudeChildPid(0)` runs, walking `/proc/0/task/0/children` which is meaningless.)

Restore each before committing.

## 8. Re-open conditions

If the symptom recurs after this spec lands, the failure mode is one of:

1. **`findClaudeChildPid` not detecting the new Claude** (e.g., it's a grandchild via a wrapper that isn't `node`/`deno`/`bun`) → ANTS-1048 boundary breach. Add INV there.
2. **`processStartTimeMs(foundPid)` returning 0 for a live PID** (kernel `/proc` gap, container quirks) → the resolver falls back to recency-only mode (graceful degradation, not a failure path). Tasks chip continues working in this mode. If the resolver picks the wrong file *because* the process-start anchor is missing, that's an ANTS-1163 boundary issue and not in scope here.
3. **The resolver's freshness floor rejecting the new transcript** (clock skew, post-relaunch JSONL not yet written) → ANTS-1163-INV-7 boundary; would also break the tasks chip in the same way.
4. **A new launch flow that bypasses `pollClaudeProcess` entirely** (e.g., a hypothetical IPC-driven session-id push) → extend §4 "Out of scope" and add the corresponding INV.

The existing `sessionPathForCwd/result:` debug log line already prints `procStartMs`, `winner-effMs`, and `nowMs` per call — sufficient to localise (1)-(3). Adding a `pollClaudeProcess` line would make (4) easier to spot if it ever ships, but is not required by this spec.
