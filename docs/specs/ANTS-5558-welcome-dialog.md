# ANTS-5558 — A welcome dialog with one-click setup

**Status:** accepted (2026-09-29), review-contract loops 1 + 2 folded, cap reached.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5558 (user-request-2026-09-29; decisions recorded in the item's body and its 2026-09-29 note).
**Composes with:** `tests/features/help_about_menu/spec.md` (the Help and Donate menus), `docs/standards/dialogs.md` (D1 to D6), ANTS-4932 (`ants-mcpd`), `packaging/shell-integration/README.md` (the OSC 133 hook).

**Layman:** The first time Ants Terminal opens, a window thanks the user, says briefly what the terminal can do, sets up the Claude Code extras and shell integration in one click each, and invites a donation.

## 1. Problem

Nothing greets a new user. Every setup step is buried: the two Claude Code hook installers sit on Settings → General (`SettingsDialog::installClaudeHooks`, `SettingsDialog::installClaudeGitContextHook`), registering `ants-mcpd` with Claude Code is a command the user types from the README, shell integration is three hand edits described in `packaging/shell-integration/README.md`, and no screen asks for support beyond the Donate menu.

## 2. Surface

### 2.1 When it appears

- **Automatically, once.** After the main window is first shown, `MainWindow` calls `welcome::maybeAutoShow(Config &, show)`: if the config key `ui.welcome_shown` is false or absent, it calls `show` and sets the key to true. An upgrading user whose config lacks the key sees it once too.
- **On demand.** Help gains `Show &Welcome...`, placed after `About &Qt...`. It opens a fresh dialog whether or not the key is set.
- The dialog is non-modal, heap-allocated with `WA_DeleteOnClose`, and shown with `show()`, `raise()` and `activateWindow()`; it never calls `exec()` or `setModal(true)`. That is `help_about_menu` Invariant 7's shape, adopted here for its Wayland reason.
- A second request while one is open raises the open one rather than making another.

### 2.2 Layout

One window, `WelcomeDialog`, built through `DialogChrome::install(this, themeName, true, "WelcomeDialog")` into `contentArea`, with its body in a `QScrollArea` (D1 to D6). Top to bottom:

1. **Thanks.** A heading welcoming the user and thanking them for using Ants Terminal.
2. **What it does.** A short grouped list of features: the terminal itself (tabs and splits, ligatures and images, search, command palette, themes, session restore, Lua plugins, the project audit) and, for Claude Code users, the live status bar, session and permission tools, Review Changes, and the Ants MCP toolkit.
3. **Set up with Claude Code** (§ 2.4). Shown only when Claude Code is detected; otherwise one line reads "Using Claude Code? Install it, then reopen this from Help → Show Welcome."
4. **Shell integration** (§ 2.6).
5. **Support Ants Terminal.** A sentence saying donations keep the project improving, and three buttons: GitHub Sponsors (`https://github.com/sponsors/milnet01`), Patreon (`https://www.patreon.com/c/AntsProjectsHub`) and PayBru (`https://paybru.co.za/tip/ants-projects-hub`), each opened with `QDesktopServices::openUrl`.
6. A Close button (a plain `QPushButton`, as the About dialog uses).

Each setup row shows its current state beside its button: installed, partly installed, or not installed. The button reads "Install" or "Reinstall" to match. States are read when the dialog opens and again after each action.

**Previews.** A row that asks before writing shows a non-modal preview dialog owned by the welcome dialog. The module's writer is called only from the preview's accept handler.

The text is compiled in. The Donate menu gains a third item, `Tip via &PayBru...`, after the Patreon one, opening the same URL.

### 2.3 Where the setup code lives

A new module `src/claudesetup.{h,cpp}`, namespace `ants::claude_setup`, holds every setup action and its state read, as free functions with no widget or dialog in them:

```cpp
enum class State { Installed, Partial, Missing, Unavailable };
struct Status  { State state; QString detail; };
struct Outcome { bool ok; QString message; };
```

`SettingsDialog::installClaudeHooks`, `installClaudeGitContextHook` and their two `refresh...Status` functions become callers of this module: they keep their buttons, labels and message boxes, and the file work moves. Behaviour is unchanged, including the corrupt-file refusal (`rotateCorruptFileAside`), the `ConfigWriteLock` and the owner-only permissions. `WelcomeDialog` calls the same functions, so the two surfaces cannot drift. The source-scrape tests that anchor on those `SettingsDialog` bodies are re-pointed at `src/claudesetup.cpp` (§ 5).

### 2.4 The Claude Code actions

**Detection.** Claude Code is present when `QStandardPaths::findExecutable("claude")` finds it or `~/.claude` exists. Read on each open.

Four rows:

1. **Status-bar hooks.** `installStatusHooks()` / `statusHooksStatus()`: the moved `installClaudeHooks` work.
2. **Connect Ants MCP to Claude Code.** `registerMcp()` / `mcpStatus()`.
   - The command registered is `$APPIMAGE --mcpd` when the environment names an AppImage: the AppImage bundles `usr/bin/ants-mcpd`, and its `AppRun` routes `--mcpd` to it. Otherwise it is the `ants-mcpd` beside the running binary, else on `PATH`, found with `mcpd::locateBinary(QString(), appDir)`, which skips the registered-command step. None found: the row's state is `Unavailable` with the reason, and its button is disabled.
   - It runs `claude mcp add --scope user ants -- <command...>` through `QProcess` with an argv list, never a shell. `--scope user` is required: the default scope is per project, and Ants reads the user scope's `mcpServers.ants.command` in `~/.claude.json`.
   - Registered with the command and arguments this build would register: `Installed`. Registered with anything else, including a command that no longer exists: `Partial`, and the button moves the registration to this build. Re-registering runs `claude mcp remove --scope user ants` first, because `add` refuses an existing name.
   - A per-project registration named `ants` (`projects.<dir>.mcpServers.ants` in `~/.claude.json`, which the README's old command created) wins over the user one in its project. The row's detail says how many exist; the dialog does not remove them.
   - A non-zero exit shows the command's stderr. Success says the toolkit reaches Claude Code sessions started from then on, and running ones after `/mcp` reconnects.
   - `mcpd`'s file-local `registeredCommand` becomes a declared function in `mcpdversion.h` returning the command and its `args`. A new `mcpd::locateLaunch(claudeJsonPath, appDir)` returns a program and its args: the registration's args when the registered command is the program chosen, none when a fallback is. The About dialog's version query runs `<program> <args...> --version`, so an AppImage registration is asked `$APPIMAGE --mcpd --version` rather than starting the terminal.
   - Claude Code detected but no `claude` program on `PATH`: this row is `Unavailable`, "the claude program is not on PATH".
3. **Git-context hook.** `installGitContextHook()` / `gitContextStatus()`: the moved `installClaudeGitContextHook` work.
4. **Ants notes in CLAUDE.md** (optional). `installClaudeMdNote()` / `claudeMdNoteStatus()` / `removeClaudeMdNote()`.
   - The button first shows the exact text to be written and the file path, and writes nothing unless the user confirms.
   - The note sits between the lines `<!-- ants-terminal:begin -->` and `<!-- ants-terminal:end -->` in `~/.claude/CLAUDE.md`, created if absent. A second install replaces the block in place. Remove deletes the block and its markers. Every byte outside the block is kept.
   - The note is a few lines: that Ants Terminal's MCP tools are available as `mcp__ants__*`, that they are usually cheaper than raw file reads and searches, and that `session_orient` is the first call. The automatic SessionStart orientation hook (`ants::mcp_orientation::install`) stays the default route and is untouched.

### 2.5 Reload

The dialog is built on every open, so every state it shows is read fresh. Its text ships with the build. It adds no call into `MainWindow` from the MCP verb layer.

### 2.6 Shell integration

`installShellIntegration()` / `shellIntegrationStatus()`, in the same module.

- **Shell.** The basename of `$SHELL`: `bash` or `zsh`. Anything else is `Unavailable`, "supported for bash and zsh".
- **Script.** The first existing of `<appDir>/../share/ants-terminal/shell-integration/ants-osc133.<shell>`, then `/usr/local/share/...` and `/usr/share/...`. None found is `Unavailable`, "the shell-integration scripts are not in this install". The search list is a parameter of `shellIntegrationStatus` and `installShellIntegration`, defaulting to that order, so a test can point it at a fixture directory. Under an AppImage the found script is inside a mount whose path changes each launch, so it is copied to `~/.local/share/ants-terminal/shell-integration/` and the copy is sourced; a reinstall refreshes the copy.
- **What it writes,** after the user confirms a preview showing both files and their exact lines:
  - To `~/.profile` (bash) or `~/.zshenv` (zsh): `export ANTS_OSC133_KEY="<64 hex digits>"`, the key taken from `QRandomGenerator::system()`.
  - To `~/.bashrc` or `~/.zshrc`: `[ -f <script> ] && source <script>`.
  - Each inside `# >>> ants-terminal shell integration >>>` and `# <<< ants-terminal shell integration <<<` lines. A block already present is replaced, so the key is not regenerated on a reinstall unless the block is missing, and every byte outside the block is kept.
- **Status.** Both blocks present and the sourced script exists: `Installed`. Any block present otherwise: `Partial`. Neither: `Missing`.
- Success says the key takes effect at the next login and the source line in a new shell.

### 2.7 Alternatives

- **Welcome text in a data file the dialog re-reads.** Rejected: the app has no lookup for shipped data files yet, and the text changes only with a release, which replaces the binary anyway.
- **Calling the `SettingsDialog` installers from the welcome dialog.** Rejected: they are private, report through their own message boxes, and write status into that dialog's labels.
- **Writing `~/.claude.json` directly instead of running `claude mcp add`.** Rejected: that file belongs to Claude Code, which rewrites it while running; its own command is the supported writer.

## 3. Invariants

- **INV-1** — The dialog opens automatically once: with `ui.welcome_shown` false or absent, `welcome::maybeAutoShow` calls its `show` callback and the key becomes true; with it true it does not. `MainWindow` calls it from its first show. Broken by showing it on every launch, or never. *Test:* `WelcomeFirstRun.AutoShowOnceThenLatched` calls `maybeAutoShow` twice against a temporary config with a counting callback: one call counted, and the key reads true; a source check asserts `MainWindow`'s first-show path calls it.
- **INV-2** — Help carries `Show &Welcome...` after `About &Qt...`, and it opens the dialog however the key is set. Donate stays the last menu. Broken by a Help block missing the action, or a Donate menu added before Help. *Test:* `WelcomeDialog.HelpMenuActionOpensIt` extends the `help_about_menu` source checks: the action string inside the Help block, after the About Qt one, and `addMenu(tr("&Donate"))` still after `addMenu("&Help")`.
- **INV-3** — The dialog is built by `DialogChrome::install` with `resizable` true and size key `"WelcomeDialog"`, its body is in a `QScrollArea`, and it is opened without `exec()` or `setModal(true)`. Broken by a fixed-size or modal dialog. *Test:* `WelcomeDialog.ChromeAndNonModal` constructs it offscreen and asserts the `QScrollArea` child and `isModal()` false; a source check asserts the `DialogChrome::install` call with that key and no `exec(` in `welcomedialog.cpp`.
- **INV-4** — The Claude Code section is shown only when Claude Code is detected. Broken by detection ignored in either direction. *Test:* `WelcomeDialog.ClaudeSectionFollowsDetection` constructs the dialog with detection injected false, then true, and asserts the section's visibility and the hint line's.
- **INV-5** — The moved hook installers behave as before: under a temporary `HOME`, `installStatusHooks()` writes all seven events and a second run adds no duplicate; a `settings.json` that does not parse is copied aside by `rotateCorruptFileAside` and left unwritten; `installGitContextHook()` adds one `UserPromptSubmit` entry, once. Broken by the move changing what is written. *Test:* `ClaudeSetup.HookInstallersIdempotentAndRefuseCorrupt`.
- **INV-6** — `registerMcp` runs `claude mcp add --scope user ants -- <command...>` as an argv list, with `<command...>` `$APPIMAGE --mcpd` when `APPIMAGE` is set, else the located `ants-mcpd`; it runs `claude mcp remove --scope user ants` first when a registration exists. `mcpd::locateLaunch` carries the registration's args only when the registered command is the program it returns, and the About query runs `<program> <args...> --version`. Broken by a shell string, a missing `--scope user`, the wrong command under an AppImage, or an About query that drops `--mcpd`. *Test:* `ClaudeSetup.McpRegistrationArgv` calls the argv builder with each case, and `locateLaunch` with an executable `$APPIMAGE --mcpd` registration (args kept) and with a registration whose command is missing (the sibling binary, no args).
- **INV-7** — The CLAUDE.md note lives between its markers: installing twice leaves one block, remove leaves no marker, and bytes outside the block are unchanged in both. Broken by appending a second block or rewriting the user's text. *Test:* `ClaudeSetup.ClaudeMdNoteBlockIsReplacedNotAppended` works on a temporary file holding user text above and below.
- **INV-8** — Shell integration writes its two blocks idempotently and keeps an existing key: a reinstall leaves one block per file and the same key; bytes outside the blocks are unchanged; an unsupported shell or a missing script yields `Unavailable` and writes nothing. Broken by appending on every click, regenerating the key, or writing when unavailable. *Test:* `ClaudeSetup.ShellIntegrationBlocksAndKey` under a temporary `HOME`, with the search list pointed at a fixture directory, with `SHELL` set to bash, then to `fish`; with `APPIMAGE` set, the sourced path is the per-user copy.
- **INV-9** — Nothing is written by the CLAUDE.md or shell-integration buttons until the user accepts the preview. Broken by writing on click. *Test:* `WelcomeDialog.ConfirmBeforeWriting` clicks each button under a temporary `HOME`, the shell search list pointed at a fixture directory: the target files are unchanged while the preview is open, and written after it is accepted.
- **INV-10** — The dialog and the Donate menu each offer all three donation links, and the Donate menu keeps `Sponsor on &GitHub...` first. Broken by a missing link. *Test:* `WelcomeDialog.DonationLinks` asserts the three URLs in the dialog's buttons and in `setupDonateMenu()`, with the GitHub action before the other two.

## 4. RAM / build cost

The dialog exists only while open. One new translation unit for the dialog and one for the setup module, both in the main application's sources.

## 5. Tests

`tests/features/welcome_dialog/` holds this spec's tests in an existing GUI bundle.

| INV | Covered by |
|---|---|
| INV-1 | `WelcomeFirstRun.AutoShowOnceThenLatched` |
| INV-2 | `WelcomeDialog.HelpMenuActionOpensIt` |
| INV-3 | `WelcomeDialog.ChromeAndNonModal` |
| INV-4 | `WelcomeDialog.ClaudeSectionFollowsDetection` |
| INV-5 | `ClaudeSetup.HookInstallersIdempotentAndRefuseCorrupt` |
| INV-6 | `ClaudeSetup.McpRegistrationArgv` |
| INV-7 | `ClaudeSetup.ClaudeMdNoteBlockIsReplacedNotAppended` |
| INV-8 | `ClaudeSetup.ShellIntegrationBlocksAndKey` |
| INV-9 | `WelcomeDialog.ConfirmBeforeWriting` |
| INV-10 | `WelcomeDialog.DonationLinks` |

The source-scrape tests anchored on the moved `SettingsDialog` bodies keep what each protects, and each check follows the text it reads: file work to `src/claudesetup.cpp`, button wiring and message boxes staying in `settingsdialog.cpp`. Where the move changes the text's form, the pattern changes with it — `settings_parse_failure_mirror`'s "`return;` after the rotation" becomes the module's early return of a failed `Outcome`. The tests: `hook_events_wired`, `claude_git_context_hook` (`test_installer_grep.cpp`, `test_script.sh`), `git_optional_locks`, `concurrent_writer_lock`, `persistence_post_rename_chmod`, `settings_parse_failure_mirror` and `secureio_configbackup_split`.

## 6. Cross-doc impact

- `README.md` "Getting started" says the MCP helper is not in a released package. The release workflow installs the whole tree into the AppImage (`cmake --install`), so it ships `ants-mcpd` and the shell-integration scripts. The README section gains the welcome dialog as the way to connect, the stale claim is corrected, and its command gains `--scope user`.
- `tests/features/help_about_menu/spec.md` gains the Help item and the third Donate item.
- The About dialog's Ants MCP line reads the registration's args as § 2.4 says, which changes how it queries an AppImage registration.
- `docs/subsystems.md` and `.indie-review/partition.json` list `src/welcomedialog.{h,cpp}` and `src/claudesetup.{h,cpp}`.

## Cold-eyes loop log

The rows are in [`../reviews/ANTS-5558-welcome-dialog-loop-log.md`](../reviews/ANTS-5558-welcome-dialog-loop-log.md).
