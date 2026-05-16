# mcp_caller_cwd_suffix_helper — ANTS-1409

See `docs/specs/ANTS-1409.md`.

## Test scope

Source-scrape regression locks the `callerCwdSuffix` helper and
the three call-site refactors that consume it. Two negative
checks confirm the long-form (`get_scrollback`) and inline-arg-
list (`get_text`) sites keep their non-canonical phrasing.

## Invariants checked

- **INV-1.** `callerCwdSuffix` lambda present in
  `claudeintegration.cpp` within ~5000 bytes after
  `makeCallerCwdReadProp` (same `tools/list` block).
- **INV-2.** Lambda body returns the exact canonical literal
  `"Pass \`caller_cwd\` to anchor to your tab (ANTS-1392)."`.
- **INV-3a.** `get_last_command` descriptor calls
  `callerCwdSuffix()`.
- **INV-3b.** `get_git_status` descriptor calls
  `callerCwdSuffix()`.
- **INV-3c.** `get_environment` descriptor calls
  `callerCwdSuffix()`.
- **INV-4a.** `get_scrollback` descriptor does NOT call
  `callerCwdSuffix()` (long-form preserved).
- **INV-4b.** `get_text` descriptor does NOT call
  `callerCwdSuffix()` (inline-arg-list form preserved).
