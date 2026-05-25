# audit_run_since_last_run — feature spec

Conformance test for `scope:"since-last-run"` (and the shared narrowing
resolver) on the `audit_run` MCP tool. Full design + invariant contract:
[`docs/specs/ANTS-1504.md`](../../../docs/specs/ANTS-1504.md). This file
records what the C++ test pins; INV numbers mirror the spec.

The resolver lives in `src/auditscope.{h,cpp}`, split pure/impure so the
parsing/filtering is testable without spawning git; only
`AuditScope::resolveChangedFiles` shells out.

## Invariants asserted here

**INV-1 (resolve + parse)** — `resolveChangedFiles("since-last-run", …)`
anchors to the prior commit and returns the files changed since it
(`git diff <anchor>..HEAD` ∪ working tree). The pure `parseChangedFiles`
unions `git diff --name-only` output with `git status --porcelain`,
deduplicates, drops deleted entries, keeps untracked (`??`), and resolves
a rename (`R old -> new`) to the new path.

**INV-3 (per-tool language filter)** — `filterForTool` keeps only files
matching a tool's language set: cppcheck/clazy/clang-tidy → C/C++,
ruff/bandit/mypy → `.py`, shellcheck → `.sh`/`.bash`; semgrep (and any
tool with no specific set) receives every file.

**INV-4 (repo-global skip)** — `isFileScopedTool` is false for `gitleaks`
and `trivy`, true for every other tool.

**INV-5 (path-safety drop)** — the runAudit wiring validates each
git-derived path through `isAuditArgSafe` before argv (source-scrape).

**INV-6 (stale-cache demotion)** — `since-last-run` demotes to a full
scan with a reason: `no_prior_run` (empty prior commit),
`no_prior_commit` (`"nogit"`), `prior_commit_unreachable`
(`git cat-file -e` fails), `not_git` (not a work tree); `files` also
demotes `no_merge_base`.

**INV-7 (empty-changeset short-circuit)** — a narrowing scope resolving to
zero files reports `noChanges` and (in runAudit) spawns nothing and skips
`recordRun` so the prior anchor survives (resolver-level + source-scrape).

**INV-8 (envelope)** — the runAudit envelope surfaces `scope_resolved`,
`changed_files_count`, `scope_anchor_commit`, and the
`scope_demoted`/`no_changes` fields (source-scrape of `mainwindow.cpp`).

**INV-9 (pure/impure split)** — only `resolveChangedFiles` references
`QProcess` in `auditscope.cpp`; `parseChangedFiles` / `filterForTool` /
`isFileScopedTool` are pure (source-scrape).

## Out of scope

- The precise `delta:{added,removed,carried_forward}` — deferred to the v2
  per-finding SARIF parser (ANTS-1504 § 5).
- End-to-end tool execution under narrowing (the runner spawn path is
  covered by the existing `mcp_audit_run` / `audit_run_cache` tests).
