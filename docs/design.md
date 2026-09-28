# Ants Terminal — Design

> **Purpose — so the shape is decided once, and anyone can tell where a
> new piece of work belongs and what it is allowed to touch.**

**This document is a gate** (`~/.claude/workflow.md` § 4). It serves
[`discovery.md`](discovery.md): every sign of success there is placed below
in the part that delivers it.

**Status:** draft (2026-09-28). When agreed: `agreed (YYYY-MM-DD)`.

This records a codebase that already exists. Where a fact already has a
home, this points there rather than copying it.

## The parts

Each part is a build target in `CMakeLists.txt`, and **its files are that
target's source list** — the one list the compiler actually uses.
[`subsystems.md`](subsystems.md) maps the same files by subject, for review
and search.

| Part | Target | Responsible for |
|---|---|---|
| Roadmap parser | `ants_roadmapparse_lib` | Reading roadmap markdown into records. Qt Core only. |
| MCP core | `ants_mcpcore_lib` | The project-scoped MCP verbs, the verb registry, and config. No window code. |
| Audit | `ants_audit_lib` | The audit engine, its runner and caches, and the review dispatcher. No window code. |
| Lua plugins | `ants_lua_lib` | The sandboxed Lua 5.4 plugin engine. Optional at build time. No window code. |
| Roadmap store | `ants_roadmapstore_lib` | The SQLite roadmap store, its render to `ROADMAP.md`, writes and migration. No window code. |
| Terminal core | `ants_core_lib` | The PTY, the byte stream, sessions, desktop integration, and the dispatch of terminal-scoped MCP verbs with the `--remote` socket. |
| VT engine | `ants_vt_lib` | The escape-sequence parser, the screen grid, and the terminal widget that paints it. |
| Chrome | `ants_chrome_lib` | The main window, title bar, tabs and command palette. The main window registers the terminal-scoped verb providers. |
| Claude UI | `ants_claude_lib` | Claude session tracking and state, status widgets, transcripts, background tasks and model switching. |
| Dialogs | `ants_dialogs_lib` | Settings, roadmap, review and other dialogs. |
| Audit dialog | `ants_audit_dialog_lib` | The audit results window. |
| Terminal app | `ants-terminal` | The executable: links the parts above into one program. |
| MCP server | `ants-mcpd` | A separate process serving the project-scoped MCP verbs, so a verb change needs no terminal relaunch. |
| Helper | `ants-helper` | An optional command-line tool with JSON in and out, off by default (`ANTS_ENABLE_HELPER_CLI`). Qt Core only. |

## What may depend on what

**The rule: a part depends only on parts with a lower number in this
list.** Depending means a `target_link_libraries` edge, or an `#include` of
another part's header.

1. Roadmap parser
2. Roadmap store
3. MCP core, Audit, Lua plugins
4. Terminal core
5. VT engine
6. Chrome
7. Claude UI
8. Dialogs
9. Audit dialog
10. Terminal app, MCP server, Helper — the programs

Parts 1 to 3 are the **window-free layer**.

**The window-free rule: nothing the MCP server links may use Qt Gui,
Widgets or DBus.** That is what lets a project-scoped verb change reach
running sessions without a relaunch (SIGN-3). Checked by `StandaloneMcpServer`'s
`Inv1LinksNoGuiLibrary` test (`tests/features/standalone_mcp_server/`),
which reads the binary's linked libraries.

**The hot-reload rule** — how a new feature reaches a running terminal —
is owned by `CLAUDE.md` § Hot reload is the design default, and binds
every part.

**Known exceptions, recorded so no one reads them as permission for
another:**

- MCP core and Audit depend on each other, and so do MCP core and Lua
  plugins. `CMakeLists.txt` declares both edges so the single-pass linker
  on the Qt 6.2 floor resolves them.
- Terminal core reaches up: `src/remotecontrol_terminal.cpp` includes the
  main window's and the terminal widget's headers, and
  `src/sessionmanager.cpp` includes the screen grid's.
- The main window (Chrome) calls into Claude UI, Dialogs and the Audit
  dialog. `CMakeLists.txt` resolves these symbol cycles with
  `--start-group` on the `ants-terminal` link line (ANTS-1444).

## What every part does the same way

Each has one owner. This table points to it.

| Concern | Owner |
|---|---|
| MCP errors and refusal codes | [`standards/mcp-error-codes.md`](standards/mcp-error-codes.md) |
| MCP verb contracts | [`standards/mcp-tools.md`](standards/mcp-tools.md) |
| Configuration | `~/.config/ants-terminal/config.json`, mode 0600, re-read without a relaunch ([`standards/config-hot-reload.md`](standards/config-hot-reload.md)) |
| Persistence | The roadmap store (`~/.local/share/ants-terminal/roadmap.sqlite`), sessions via `QDataStream` + `qCompress` (`CLAUDE.md` § Key design decisions) |
| Logging | The debug log (`src/debuglog.cpp`), read through the `read_log` verb |
| Cross-part messages | Qt signals and slots (`CLAUDE.md` § Conventions) |
| Coding, testing, commits | The standards in [`standards/`](standards/README.md) |

## Where each sign of success lives

| Sign | Delivered by | How it is measured |
|---|---|---|
| **SIGN-1** Cheaper | MCP core and MCP server | A practice project with one planted bug, kept at `tools/token-bench/`, with its fixed prompt in that folder's README. Neither changes after the first measurement. Claude Code gets that prompt three times in Ants and three in Konsole; Ants' average tokens must be at least 20% lower. |
| **SIGN-2** Fast | VT engine and Terminal core | On the same machine: the time to print one fixed large log file, and the time for a keypress to appear while Claude-style output streams. Ants must be no slower than Konsole on both. |
| **SIGN-3** No relaunch | MCP server, for project-scoped verbs. Terminal-scoped verbs are served by Terminal core and Chrome, so changing one still needs a relaunch: that half of SIGN-3 is not met yet. | Change one verb of each kind, rebuild, reconnect: each change is live and the Claude sessions in open tabs are still running. |
| **SIGN-4** Programs just work | VT engine | Claude Code, vim, htop, tmux and less display and respond correctly. vttest's cursor-movement, screen-features, terminal-reports and VT102 insert/delete tests all pass. vttest is not installed on the development machine yet. |
| **SIGN-5** See what Claude is doing | Claude UI and Dialogs | Without leaving the terminal: each Claude session's status, its token cost so far, and the project roadmap are visible. |

## The stack, and what it rules out

| Choice | Why | Runner-up | Rules out |
|---|---|---|---|
| C++20 and Qt 6, floor Qt 6.2 | One toolkit for the window, network and database | none recorded | Any API newer than Qt 6.2 (checked by `tools/qt62-guard.sh`) |
| Own VT parser | Full control of the parse path and its speed | libvterm | Borrowing another emulator's behaviour for free |
| Linux PTY (`libutil`), KDE/Wayland integration | The one platform it ships for | none | Windows and macOS without new work |
| SQLite through Qt Sql | The roadmap store | none recorded | Editing a migrated project's `ROADMAP.md` by hand: it is a render of the store |
| Lua 5.4, optional | Sandboxed user plugins | none recorded | Plugins with host access the sandbox strips |
| CMake with Ninja | The build's memory caps (`JOB_POOLS`) apply only under Ninja | Make | The memory caps, under any other generator |

## Close calls

The existing decisions are in [`decisions/`](decisions/README.md):
ADR-0001 to ADR-0005. This design raises no new one.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-09-28 | 2 (neutral-lane; each held every question) | 6 | 0 | 3 | — | 9 verified, 9 fixed, 0 dismissed. Lanes: part ownership of MCP verbs, incomplete dependency exceptions, direction wording, SIGN-1 task unnamed, SIGN-3 half-delivered, helper unplaced. Orchestrator, refuting its own fix: upward #includes from Terminal core and Dialogs; two stack "rules out" overclaims. Three open questions resolved clean (C++20, Lua 5.4, no window includes in the window-free layer). Packet defect: its Terminal-core source list omitted remotecontrol_terminal.cpp. |
