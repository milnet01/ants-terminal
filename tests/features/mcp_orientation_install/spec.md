# Feature: ANTS-1897 MCP discoverability — SessionStart hook installer

The installer in `src/mcporientation.cpp` writes a small orientation
script to `~/.config/ants-terminal/hooks/mcp-orientation.sh` and merges
a `SessionStart` hook entry into `~/.claude/settings.json`. The hook
prints a short Ants-MCP cheat-sheet at every Claude Code session start
so the assistant reaches for `changelog_log` / `file_outline` / etc.
instead of always-loaded `Edit` / `Write` / `Bash`.

Full design + 14 invariants in [`docs/specs/ANTS-1897-ants-mcp-discoverability-sessionstart.md`]
(../../../docs/specs/ANTS-1897-ants-mcp-discoverability-sessionstart.md).

This test exercises the installer, the marker contract, the
settings.json merge, the parse-failure refusal, the script byte
equality, the un-install path, and the source-grep wiring that proves
the env-var export + `selection_hint` coverage hold against the live
codebase.

Cases:

- `IdempotentRewrite` — fresh install + second install yields no
  change.
- `InVersionBodyRefresh` — a file that carries the marker but a stale
  body is rewritten to the template, even at the same version
  (ANTS-2038).
- `UserOwnedPreserved` — pre-existing file without marker line
  is not overwritten.
- `FirstLaunchWritesFresh` — first install writes the file fresh; its
  body equals `orientationScriptTemplate().arg(ANTS_VERSION).toUtf8()`
  (INV-12).
- `SettingsMergeIdempotent` — second install does not duplicate
  the SessionStart entry.
- `NoOpInstallLeavesSettingsUntouched` — an install with nothing to
  change leaves settings.json byte-identical (ANTS-5104).
- `SweepsExistingDuplicates` — pre-existing duplicate entries
  (capital-cased `Ants Terminal` path) are swept down to one
  canonical entry on install (ANTS-1902 self-healing).
- `EntryFiresOnStartupAndClearOnly` — the Ants entry carries
  `"matcher": "startup|clear"`, so it is not re-run on resume or after
  compaction; an older entry without it is upgraded to one entry
  (ANTS-5693).
- `CommandRunnableWithSpacesInPath` — the written `command` is
  bash-runnable when the install path contains a space (ANTS-1902).
- `SettingsMergePreservesUnrelated` — other top-level keys + a
  pre-existing `UserPromptSubmit` hook survive the merge.
- `DisableRemovesEntry` — uninstall removes the marker-bearing
  entry but preserves the rest.
- `SelectionHintCoverage` — source-grep on
  `src/claudeintegration.cpp` proves 67/67 descriptors carry a
  selection_hint.
- `SelectionHintFormat` — source-grep proves every hint starts
  with "Use " (case-insensitive) + is ≤ 280 chars.
- `ScriptExitZero` — running the installed script with no
  `ANTS_MCP_SOCKET` exits 0 silently.
- `ScriptOutputByteCap` — running the script against a fake
  socket prints ≤ 500 bytes (ANTS-5715).
- `SettingsPermsOwnerOnly` — settings.json is 0600 after the
  merge.
- `BadJsonNoClobber` — when settings.json fails to parse, the
  installer refuses + reports a warning + leaves the file untouched.
- `WrongTypedHooksNoClobber` — valid JSON whose `hooks` is not an
  object, or whose `SessionStart` is not an array, is refused the same way
  (ANTS-5104).
- `MainWindowExportsSocket` — source-grep on
  `src/mainwindow.cpp` proves `qputenv("ANTS_MCP_SOCKET"…)` is called
  immediately after `startMcpServer(mcpSocket)`.

Label `features;fast`. Each case must FAIL against pre-ANTS-1897
source before the fix is restored.
