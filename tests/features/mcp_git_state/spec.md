# mcp_git_state — ANTS-1250 conformance

Source spec: [docs/specs/ANTS-1250.md](../../../docs/specs/ANTS-1250.md).

This is a source-grep conformance harness — it does NOT spin up a
RemoteControl + QProcess + real git repo. It locks the wiring
between the MCP tools/list registration, the tools/call dispatch, the
RemoteControl public method declaration, the gitwrap helper, the IPC
dispatcher entry, and the MainWindow provider lambda. Behavioural
testing of the parsers happens via manual relaunch + Claude-side
tool-call exercising once the bytes land — same shape as the
ANTS-1248 / ANTS-1249 harnesses.

## What this test asserts

1. `cmdGitState(const QJsonObject &req)` is declared public on
   `RemoteControl` alongside the ANTS-1244/1248/1249 block.
2. `remotecontrol.cpp` carries the 13 `// ANTS-1250-INV-N` anchors,
   one per spec invariant.
3. Shell-less argv enforced: no `bash -c`, `/bin/sh`, `system(`,
   `popen(` in `gitwrap.cpp` (the only place git is forked).
4. `gitwrap.cpp` invokes `QProcess::start("git", ...)` with the
   StringList overload (literal `"git"` appears).
5. IPC dispatcher in `remotecontrol.cpp` routes `"git-state"` →
   `cmdGitState`.
6. MCP `tools/list` registers a single `git_state` entry with
   `op` enum {status, log, diff}. (ANTS-3365 — `op` is no longer in
   `required[]`; it defaults to `"status"`.)
7. MCP `tools/call` dispatcher carries
   `toolName == "git_state" && m_gitStateProvider`.
8. `claudeintegration.h` declares `setGitStateProvider` +
   `m_gitStateProvider` member with the
   `std::function<QString(const QJsonObject&)>` signature.
9. `mainwindow.cpp::setupClaudeMcpProviders` calls
   `setGitStateProvider` and delegates to `cmdGitState`.
10. `cmdGitState` body contains the op-dispatch chain (string
    literals `"status"`, `"log"`, `"diff"` appear).
11. Stricter range regex appears in source — the regex literal
    must NOT include `-` as the first allowable character of a
    rev-component (closes the leading-`-` flag-injection vector
    from cold-eyes pass 2).
12. Two-tier kill timer: gitwrap uses `kHardKillMs` (5 s) +
    `kKillGraceMs` (200 ms), calls `.terminate(` and `.kill(`.
13. CMake wires `src/gitwrap.cpp` into `ants_core_lib`.
14. ANTS-3365 — `cmdGitState` defaults an omitted/empty `op` to
    `"status"` (`if (op.isEmpty()) op = ... "status"`) so a bare
    `git_state{caller_cwd}` returns the one-call status read; a
    non-empty unknown op still refuses `bad_op`. The schema marks
    `op` optional and advertises the default.

## Why source-grep and not integration

`cmdGitState` forks `git` and depends on the user's working
directory carrying a real repo. Spinning up an isolated temp repo
inside a `QCoreApplication`-only test would work, but the value of
this test is locking the *wiring contract* — that all four sites
(remotecontrol.h, remotecontrol.cpp, claudeintegration.h/.cpp,
mainwindow.cpp, gitwrap.cpp) stay in sync as refactors happen
(notably ANTS-1253's consolidation sweep). Same call as the
ANTS-1248 / ANTS-1249 sibling tests.

Behavioural verification: once the binary relaunches under a Claude
session, a single tool-call `git_state {op:"status"}` returning the
parsed `{branch, ahead, behind, files[], untracked[]}` envelope
confirms the parser. The user has the spec § 6 token-saving math as
the acceptance signal for the implementation.

## Pre-fix verification

`git stash`-revert the four source changes; the harness emits 13+
INV failures. Restore and the harness passes. (Same pattern as
ANTS-1248/1249 — proves the test catches absence of the wiring.)
