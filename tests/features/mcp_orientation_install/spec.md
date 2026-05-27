# Feature: ANTS-1897 MCP discoverability — SessionStart hook installer

The installer in `src/mcporientation.cpp` writes a small orientation
script to `~/.config/ants-terminal/hooks/mcp-orientation.sh` and merges
a `SessionStart` hook entry into `~/.claude/settings.json`. The hook
prints a short Ants-MCP cheat-sheet at every Claude Code session start
so the assistant reaches for `changelog_log` / `file_outline` / etc.
instead of always-loaded `Edit` / `Write` / `Bash`.

Full design + 14 invariants in [`docs/specs/ANTS-1897.md`]
(../../../docs/specs/ANTS-1897.md).

This test exercises the installer, the marker contract, the
settings.json merge, the parse-failure refusal, the script byte
equality, the un-install path, and the source-grep wiring that proves
the env-var export + `selection_hint` coverage hold against the live
codebase.

Cases:

- `Inv1_IdempotentRewrite` — fresh install + second install yields no
  change.
- `Inv1_VersionBumpOverwrites` — second install with a different
  `ANTS_VERSION` rewrites the file.
- `Inv2_UserOwnedPreserved` — pre-existing file without marker line
  is not overwritten.
- `Inv2_FirstLaunchWritesFresh` — first install writes the file fresh.
- `Inv3_SettingsMergeIdempotent` — second install does not duplicate
  the SessionStart entry.
- `Inv3_SweepsExistingDuplicates` — pre-existing duplicate entries
  (capital-cased `Ants Terminal` path) are swept down to one
  canonical entry on install (ANTS-1902 self-healing).
- `Inv3_CommandRunnableWithSpacesInPath` — the written `command` is
  bash-runnable when the install path contains a space (ANTS-1902).
- `Inv4_SettingsMergePreservesUnrelated` — other top-level keys + a
  pre-existing `UserPromptSubmit` hook survive the merge.
- `Inv5_DisableRemovesEntry` — uninstall removes the marker-bearing
  entry but preserves the rest.
- `Inv6_SelectionHintCoverage` — source-grep on
  `src/claudeintegration.cpp` proves 67/67 descriptors carry a
  selection_hint.
- `Inv7_SelectionHintFormat` — source-grep proves every hint starts
  with "Use " (case-insensitive) + is ≤ 280 chars.
- `Inv9_ScriptExitZero` — running the installed script with no
  `ANTS_MCP_SOCKET` exits 0 silently.
- `Inv10_ScriptOutputByteCap` — running the script against a fake
  socket prints ≤ 1200 chars.
- `Inv11_SettingsPermsOwnerOnly` — settings.json is 0600 after the
  merge.
- `Inv12_ScriptMatchesTemplate` — after a fresh install, file body
  equals `orientationScriptTemplate().arg(ANTS_VERSION).toUtf8()`.
- `Inv13_BadJsonNoClobber` — when settings.json fails to parse, the
  installer refuses + reports a warning + leaves the file untouched.
- `Inv14_MainWindowExportsSocket` — source-grep on
  `src/mainwindow.cpp` proves `qputenv("ANTS_MCP_SOCKET"…)` is called
  immediately after `startMcpServer(mcpSocket)`.

Label `features;fast`. Each case must FAIL against pre-ANTS-1897
source before the fix is restored.
