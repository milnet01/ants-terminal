# ANTS-5558 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-5558-welcome-dialog.md`](../specs/ANTS-5558-welcome-dialog.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-09-29 | 2 lanes, each holding every question, headless via neutral-lane | 1 | 3 | 2 | 2 | 8 verified / 0 dismissed, all 8 fixed. Both lanes found the central pair: `mcpd::locateBinary` returns the registered command first, so it cannot say which binary to register; and registering `$APPIMAGE --mcpd` would make the About dialog ask `$APPIMAGE --version`, which AppRun sends to the terminal. Now the lookup skips the registered step and `registeredCommand` returns args the About query passes. Also fixed: shell integration under an AppImage sourced a transient mount path (now a per-user copy, and status checks the path exists); INV-1 named a `MainWindow` call no feature test can make (now `welcome::maybeAutoShow`); a blocking confirmation contradicted the non-modal rule (now a non-modal preview, writer called from its accept); the README command lacked `--scope user`. From settling open questions: `rotateCorruptFileAside` copies rather than moves (INV-5 reworded), and a per-project `ants` registration, which the README's old command made, wins over the user one (measured in a throwaway HOME; the row now reports them). Resolved clean: the AppImage bundles `ants-mcpd` and the scripts (`cmake --install` into AppDir); `claude mcp add` refuses an existing name (exit 1, already handled). Loop 2 dispatched. |
