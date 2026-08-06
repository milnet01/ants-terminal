# mcp_global_config_sentinel — ANTS-1390

Locks the wiring contract for the `~global` / `~claude-config`
`caller_cwd` sentinel that routes `workspace_search` and
`file_outline` to `~/.claude/` regardless of the focused tab's
project root.

## Motivation

Cross-session report 2026-05-15: a Claude session was asked to
streamline `~/.claude/skills/` while cwd was `/mnt/Games`.
`workspace_search` rejected the search because `~/.claude/`
doesn't canonicalise under any project root; the session burned
~250-4500 extra tokens per query falling back to `Bash grep -r`.

Adding a sentinel value the caller can pass as `caller_cwd`
closes that gap without growing per-tool surface area:

- `caller_cwd: "~global"` → `~/.claude/` root for that one call.
- `caller_cwd: "~claude-config"` → same.
- Anything else → existing focused-tab / explicit-cwd path
  unchanged.

`verify_changes` is deliberately NOT in scope (no build/test to
run on markdown skill files); follow-up entry will add a
frontmatter-parses + cross-references-resolve check.

## Invariants

| # | Statement |
|---|-----------|
| 1 | `ants::expandGlobalConfigSentinel(const QString&)` is declared in `src/resolvedroot.h` inside `namespace ants`. |
| 2 | `ants::expandGlobalConfigSentinel` is defined in the remotecontrol TUs inside `namespace ants` (alongside `resolveCallerCwdRoot`). |
| 3 | The definition checks for both literal sentinel values `~global` and `~claude-config` — neither value is left out. |
| 4 | `cmdWorkspaceSearch` (in the remotecontrol TUs) calls `ants::expandGlobalConfigSentinel` BEFORE the existing caller_cwd / focused-tab resolution. |
| 5 | `cmdFileOutline` (in the remotecontrol TUs) calls `ants::expandGlobalConfigSentinel` BEFORE the existing `resolveRootCanonical(m_main, req)` call. |
| 6 | The `workspace_search` tool description in `src/claudeintegration.cpp` documents the sentinel (mentions `~global` and `~/.claude/`). |
| 7 | The `file_outline` tool description in `src/claudeintegration.cpp` documents the sentinel (mentions `~global` and `~/.claude/`). |
| 8 | Runtime: `expandGlobalConfigSentinel("~global")` returns a non-empty path that ends in `/.claude`. |
| 9 | Runtime: `expandGlobalConfigSentinel("~claude-config")` returns the same path as INV-8. |
| 10 | Runtime: `expandGlobalConfigSentinel("")` returns an empty string (not a sentinel). |
| 11 | Runtime: `expandGlobalConfigSentinel("/some/project/root")` returns an empty string (not a sentinel). |
| 12 | Runtime: `expandGlobalConfigSentinel("~global/etc")` returns an empty string (only the exact sentinel matches — no path-prefix interpretation). |

## Acceptance

Exit 0 = all 12 invariants hold.

Wired as a source file in the `test_claude` bundle (shares
`SRC_CLAUDE_INTEGRATION_CPP_PATH` + `ANTS_RC_SOURCES`
compile definitions with the existing MCP path-tool tests).

## Out of scope

- `verify_changes` sentinel support — markdown frontmatter checks
  are a separate, larger ticket flagged in the ANTS-1390 roadmap
  bullet.
- Tenant-isolation concerns around `~/.claude/` containing
  secrets — `~/.claude/` is by design user-readable global config;
  the sentinel does not grant access beyond what a Bash invocation
  with the same UID already has.
