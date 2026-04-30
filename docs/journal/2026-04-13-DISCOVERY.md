# DISCOVERY — Ants Terminal (historical snapshot)

> **Historical snapshot, not current.** This file was authored as the
> Phase-A discovery doc against commit `be261d9` (2026-04-13). The
> project has shipped many releases since (current 0.7.58+). Do **not**
> read this as the live state of the codebase — read the README / CLAUDE
> for that. This file is preserved as a journal entry for the historical
> record of the early-2026 audit cycle.
>
> Relocated from `/DISCOVERY.md` to `docs/journal/` per ANTS-1121
> theme T3 (2026-04-30 cold-eyes review).

## Original snapshot (commit `be261d9`, 2026-04-13)

## 1. Project map

## 1. Project map

| Attribute | Value |
|---|---|
| Name / version | `ants-terminal` 0.4.0 |
| Language | C++20 (single binary, no libraries split out) |
| GUI toolkit | Qt 6.11.0 (Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets, DBus) |
| Optional runtime | Lua 5.4 (plugin system, gated on `ANTS_LUA_PLUGINS`) |
| System deps | `libutil` (forkpty), XCB (position tracker) |
| Build | CMake ≥ 3.20, `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` |
| Compiler (dev host) | gcc 15.2.1 (openSUSE) |
| Source layout | flat `src/` — 26 `.cpp`, 25 `.h`, ~17.7 kloc |
| Entry point | `src/main.cpp` → `MainWindow` |
| Runtime entry | `launch.sh` wrapper → `build/ants-terminal` |

### Largest translation units (lines)

```
 3755 src/terminalwidget.cpp
 2671 src/mainwindow.cpp
 1648 src/terminalgrid.cpp
  862 src/claudeintegration.cpp
  857 src/auditdialog.cpp
  638 src/settingsdialog.cpp
  601 src/glrenderer.cpp
  528 src/claudeallowlist.cpp
  521 src/config.cpp
```

Both `terminalwidget.cpp` and `mainwindow.cpp` are over 2 kloc and will warrant
complexity analysis in Phase 2.

## 2. External services / runtime surfaces

- **PTY** (`src/ptyhandler.cpp`): `forkpty()` spawns a login shell; master fd
  is non-blocking with `FD_CLOEXEC`; child closes fds 3–1023.
- **AI endpoint** (`src/aidialog.cpp`): OpenAI-compatible chat completions;
  user-supplied URL + API key; plaintext HTTP warns once when an API key is
  attached and the host isn't loopback.
- **SSH** (`src/sshdialog.cpp`): invokes system `ssh` via PTY; inherits user
  config/keys; never stores passwords.
- **Claude Code hooks / MCP** (`src/claudeintegration.cpp`): local-socket
  server for Claude Code hook events; separate MCP socket exposing
  scrollback / cwd / last command / git status / env to Claude.
- **Lua plugins** (`src/luaengine.cpp`, `src/pluginmanager.cpp`): sandboxed
  Lua 5.4 under `~/.config/ants-terminal/plugins/`; instruction-count
  timeout; dangerous globals stripped.
- **DBus**: linked in, used for portal/system colour scheme detection.

## 3. Data / secrets handling

- Config at `~/.config/ants-terminal/config.json`, 0600, atomic write
  (`.tmp` + rename).
- Sessions under `~/.config/ants-terminal/sessions/`, zlib-compressed binary.
- AI API key stored in config (0600). No logging of key material spotted in
  Phase 1 skim — verify in Phase 2.
- No secrets in git HEAD (verified earlier session: `.claude/` is gitignored;
  personal paths scrubbed through `be261d9`).

## 4. Existing standards

| Artifact | Present | Notes |
|---|---|---|
| `CLAUDE.md` | yes | Project instructions, conventions |
| `STANDARDS.md` | yes | Code style, arch, perf, security rules |
| `RULES.md` | yes | Release checklist, security rules, plugin rules |
| `README.md` | yes | 33 KB, user-facing |
| `.gitignore` | yes | Hardened through `b47528f` + `be261d9` |
| `LICENSE` | yes | |
| `CONTRIBUTING.md` | **no** | single-dev project |
| `.editorconfig` | **no** | |
| `.clang-format` / `.clang-tidy` | **no** | but clang-tidy + cppcheck available on host |
| pre-commit hooks | **no** | |
| CI/CD (GitHub Actions, GitLab, etc.) | **no** | |
| Unit / integration tests | **none** | no `tests/` dir, no `ctest` targets |
| `audit_config.yaml` | yes | feeds a Python audit tool (`audit.py`, external to repo) |

## 5. Baseline build

- `cmake --build build --clean-first`: **green, zero warnings**
  under `-Wall -Wextra -Wpedantic -Wformat -Wformat-security`.
- Security hardening flags active: `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-fcf-protection`, `-D_FORTIFY_SOURCE=3`,
  `-D_GLIBCXX_ASSERTIONS`, `-Wl,-z,relro,-z,now,-z,noexecstack`, PIE.
- Sanitizer option `ANTS_SANITIZERS` available (ASan+UBSan), off by default.

## 6. Baseline static analysis

### cppcheck 2.20.0 (`--enable=all --std=c++20`)

**Actionable (non-style)**:

- `src/toggleswitch.h:30` — `ToggleSwitch::m_anim` not initialized in class
  definition (uninitMemberVarPrivate). Should default to `nullptr`.

**Style (audit will decide which to roll up)**:

- `src/config.cpp:14` — `Config::configPath()` can be `static`.
- `src/luaengine.cpp:154,163,195,200,215` — local emit shadows `logMessage`
  signal name (shadowFunction). Cosmetic.
- `src/pluginmanager.cpp:36,87,89` — same pattern.
- `src/terminalgrid.cpp:135,146` — `cell` variable shadows `cell()` method.
- `src/terminalgrid.cpp:1159,1208` — `auto &logical` can be const ref.
- `src/titlebar.cpp:122` — `windowMoved` emit shadows signal.
- `src/auditdialog.cpp:757`, `src/themes.cpp:274`, `src/terminalgrid.cpp:554`
  — STL-algorithm replacement suggestions.

Cppcheck's `unknownMacro` errors around `private slots:` are Qt-macro noise,
not real findings.

### clang-tidy

Available on host; not yet run. Will defer targeted checks
(`bugprone-*,performance-*,modernize-*`) to Phase 2 for hot files.

### Dependency audit

- Qt 6.11.0 (system package, distro-managed).
- Lua 5.4 (system package, distro-managed).
- No bundled third-party code, no vendored deps, no submodules.
- No package manager lockfiles to audit.

## 7. Risk surfaces worth prioritising in Phase 2

Based on size + external trust boundary + recent churn:

1. **`src/terminalwidget.cpp`** (3755 lines) — keyboard, mouse, selection,
   painting; consumes PTY output, the largest untrusted input surface.
2. **`src/terminalgrid.cpp`** (1648 lines) — VT action dispatch, OSC/DCS/APC
   payload handling; past audits found DoS gaps here.
3. **`src/mainwindow.cpp`** (2671 lines) — ties everything; dialog lifecycle,
   split-pane bugs have recurred here.
4. **`src/claudeintegration.cpp`** (862 lines) — IPC (QLocalServer) for hooks
   and MCP; receives JSON from Claude Code processes.
5. **`src/aidialog.cpp`** — network I/O, secrets.
6. **`src/sessionmanager.cpp`** — binary deserialization (QDataStream) of
   scrollback; known dangerous surface if sizes aren't bounds-checked.
7. **`src/luaengine.cpp`** — sandbox completeness.
8. **`src/glrenderer.cpp`** — GL resource lifecycle, texture uploads.

## 8. Known carry-overs from prior audits

(from memory, to cross-check against HEAD rather than re-ligitate)

- Seven prior audits have landed (through commit `be261d9`). Known-fixed
  surfaces: PTY fd leak (child-side fd close), Lua bytecode reject, OSC 52
  1 MB cap, UTF-8 atomic appends, split-pane `findChild` helper, git-branch
  cache, bracket-paste injection sanitization, SIGPIPE ignore, CSI cap
  (32 params, 16384 per value), CVE-2024-56803 title-reply disablement.
- Coverage gaps flagged but not addressed: tests/CI, a11y, i18n, structured
  logging / observability. Expect Phase 2 to flag these again.

---

**Status:** Phase 1 complete. Proceeding to Phase 2 (AUDIT.md) per user
direction (Option A, full audit).
