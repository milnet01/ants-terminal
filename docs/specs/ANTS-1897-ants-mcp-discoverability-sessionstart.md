# ANTS-1897 — Ants MCP discoverability via SessionStart hook + selection_hint sweep

**Status:** **implemented**.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-1897 (in-session-2026-05-26; the
`changelog_log` foot-gun observed during ANTS-1891 fold-in).
**History:** [`docs/history/ANTS-1897-ants-mcp-discoverability-sessionstart.md`](../history/ANTS-1897-ants-mcp-discoverability-sessionstart.md).
**Pairs with:** ANTS-1453 (the original `selection_hint` field on tool
descriptors), ANTS-1883 (`session_orient` bundle).

**Layman:** Right now Ants ships dozens of quick-access tools for
Claude Code (cheaper ways to read a file, append to the changelog,
look up where a function lives, etc.), but a fresh Claude session
has no idea they exist — it falls back to the slower built-in tools.
This change plants a tiny cheat-sheet at the start of every Claude
session listing the high-value Ants tools, and sharpens the
one-line "use this when X" hint on every tool so Claude picks the
right one. A Settings toggle mutes the cheat-sheet for users who
don't want it.

## 1. Problem

The Ants MCP server exposes **67 tools** to Claude Code via the
runtime `tools/list` response (synthesised by the descriptor
builder in `src/claudeintegration.cpp`). The INV-6 test consumes
that same `tools/list` response — it is the authoritative count.
By *handler-dispatch path*, those 67 break down as:

- **65 tools** route through `m_claudeIntegration->
  registerToolProvider(...)` (the registry surface; definition
  `ClaudeIntegration::registerToolProvider`, 65 call sites in
  `src/mainwindow.cpp`).
- **2 control-plane tools** (`get_session_info` and `tool_info`)
  bypass `registerToolProvider` and are dispatched inline by
  `ClaudeIntegration::handleMcpRequest`.

All 67 receive descriptors in the `tools/list` builder — the
"registered vs inline-dispatched" split is about *call-routing*,
not about which tools have descriptors. A grep approximation —
`grep -cE '\["name"\][[:space:]]*='` returns 70 raw hits in
`claudeintegration.cpp`; subtract three non-tool sites
(`serverInfo["name"]`, `props["name"]`,
`env["name"]` inside an inner env-row loop) → 67. Many
of these 67 are specifically built to cut Claude's token spend —
`changelog_log`,
`roadmap_log`, `git_state`, `file_outline`, `find_definition`,
`find_sources`, `read_log`, `session_orient`, `model_switch_stats`,
etc. — and the project's user instructions in `CLAUDE.md` repeatedly
remind sessions to prefer the MCP variant over `Edit` / `Write` /
`Bash`.

But Claude Code shows MCP tools as **deferred** at session start (only
names appear in the harness preamble, schemas load lazily via
`ToolSearch`). A fresh session that does not already-know the
`{operation → tool name}` mapping defaults to built-in `Edit` /
`Write` / `Bash` because they are always-loaded. The discoverability
gap was observed in-session 2026-05-26: the assistant reached for
`Edit` on `CHANGELOG.md` (~50-line diff) instead of
`mcp__ants__changelog_log` (~6-line MCP call). The user reaction was:

> "It is pointless building all these token saving features if the CC
> session isn't aware of them and isn't using them."

The roadmap entry picked architecture **A + C**:

- **(A)** Inject a short orientation prelude when a Claude Code
  session starts inside an Ants-hosted PTY.
- **(C)** Tighten the `selection_hint` field on every MCP tool
  descriptor so when the assistant *does* enumerate available tools
  (via `ToolSearch` or the deferred-tool list), each name carries a
  one-line cue.

### 1.0 Architecture deviation from ROADMAP body — acknowledged

The ROADMAP entry's body proposed mechanism (A) be implemented as
"spawn-path system-prompt assembly" in `src/claudeintegration.cpp`
that "travels with every Ants install, doesn't touch user CLAUDE.md".
That mechanism is **infeasible** as written: Ants does **not** spawn
`claude` (verified § 1.1 below — Ants watches for a `claude` child
appearing inside the user's shell PTY). Without a spawn intercept,
no point in the code can pass `--append-system-prompt` to Claude
Code's argv.

After verifying the constraint, the user picked the SessionStart
hook approach via AskUserQuestion (2026-05-27) — Ants writes a
small entry under `hooks.SessionStart` in `~/.claude/settings.json`
that points at an Ants-bundled shell script. The script prints the
orientation prelude. This:

- Does **not** require Ants to intercept the `claude` command.
- Works wherever Ants Terminal is installed, regardless of how the
  user invokes `claude`.
- *Does* touch a user-owned file (`~/.claude/settings.json`), which
  the roadmap body promised to avoid. The marker-substring contract
  (INV-3, INV-4) bounds that touch so Ants only owns *its* entry.

The roadmap entry body is updated in the same commit as this spec
ships (§ 7 Cross-doc impact) to reflect the chosen mechanism.

### 1.1 Verified facts

- Ants does **not** spawn `claude` directly. It watches the user's
  PTY for `claude` becoming a child of the shell
  (`ClaudeIntegration::findClaudeChildPid`), then attaches
  the MCP socket and the hook server.
- Claude Code accepts `--append-system-prompt <prompt>` and
  `--system-prompt-file <path>` at the CLI, but Ants has no
  intercept point on the user's `claude` command line.
- Claude Code supports SessionStart hooks via
  `~/.claude/settings.json` with the schema:
  ```json
  {
    "hooks": {
      "SessionStart": [
        {"hooks": [{"type": "command",
                    "command": "<shell command>",
                    "timeout": <seconds>}]}
      ]
    }
  }
  ```
  Hook stdout is injected into the session context. Verified against
  the current `~/.claude/settings.json` (a `UserPromptSubmit` hook is
  already installed using this exact schema; the `SessionStart` slot
  is currently empty).
- Ants does **not** currently write to `~/.claude/settings.json`
  (grep `settings.json` in `src/claudeintegration.{cpp,h}` returns
  empty).
- `selection_hint` is used by **67 descriptors** out of 67 tools in
  `claudeintegration.cpp` (the raw `selection_hint` grep returns
  68 hits; one of them — `env["selection_hint"]` — sits
  inside the same inner env-row loop excluded from the tool count
  above, so descriptor-level coverage is 67/67, 1:1). One existing
  hint starts with `"Call once at session start instead of
  …"` rather than the canonical `"Use when …"` form. The sweep
  rewrites that one and tightens cost-cue language across the
  rest. The canonical shape is `"Use when <X>. <token-cost or
  trade-off cue>."` (see `scrollbackTool["selection_hint"]` in
  `appendToolSchemasTerminal`).
- Ants's MCP socket is `privateSocketPath("mcp-<applicationPid>")`,
  built in `MainWindow::setupClaudeMcpProviders`. It is **not** currently exported into the
  child shell environment (a `grep -nE 'ANTS_MCP|env\.insert.*MCP'`
  on `src/claudeintegration.{cpp,h}` returns zero matches). The
  spec adds this export (§ 2.1.5, INV-14).

## 2. Surface

### 2.1 SessionStart hook installer

#### 2.1.1 Hook script (Ants-owned, regenerated on each launch)

Ants writes a small shell script to a per-user path on every launch
of Ants Terminal:

- **Path:** `~/.config/ants-terminal/hooks/mcp-orientation.sh`
  (mode `0755`).
- **Marker definition.** The marker is the **prefix**
  `# ants-orientation-version:` at the start of the second line
  of the file. The version token that follows (e.g. `0.7.92`) is
  parsed only to confirm the marker is **present** (so the file is
  classified Ants-managed vs user-owned). Under the ANTS-2038
  content-parity gate the rewrite decision is a full-body byte
  compare, **not** a version-value match — the token's value is
  informational (it travels in the rendered template, so a version
  bump is naturally one of the byte differences that triggers a
  rewrite).
- **Owner contract — three cases:**
  1. **File does not exist** → Ants writes the script fresh (this
     is the first-launch path).
  2. **File exists with the marker prefix** → Ants treats the file
     as fully managed and keeps its body in lockstep with the
     rendered template: it compares the on-disk bytes to
     `orientationScriptTemplate().arg(ANTS_VERSION)` and rewrites on
     **any** difference — byte-identical is the idempotent skip.
     This covers both an Ants version bump (the marker line itself
     changes) **and** an in-version template edit — a prelude reword
     shipped without a version bump (the **ANTS-2038** fix; the
     prior version-only gate left in-version edits stale on disk
     until the next bump, e.g. ANTS-1985's catalog repoint).
     **Trade-off:** a user hand-edit to the managed body is
     overwritten on the **next launch**, not preserved until a
     version bump. To take ownership of the file, the user removes
     the `# ants-orientation-version:` marker line (→ case 3,
     user-owned, never overwritten). The script's header comment
     states this verbatim: *"This file is managed by Ants Terminal —
     edits to this managed body are overwritten on the next launch.
     To take ownership, delete the '# ants-orientation-version:'
     marker line above."*
  3. **File exists without the marker prefix** → treated as
     user-owned. Ants logs a warning and stops trying to install
     the hook for this session. Never overwrites.
- **Body:** the script prints the orientation prelude to stdout,
  with an early-exit guard. Reference shape — the script lives at
  column 0 (do not indent when implementing; the literal `cat
  <<'EOF'` heredoc preserves leading whitespace, and the closing
  `EOF` token must also be at column 0 to terminate the heredoc).
  The `<ANTS_VERSION>` placeholder below is substituted at install
  time from CMakeLists.txt `project(... VERSION X.Y.Z)`:

  ```sh
  #!/bin/bash
  # ants-orientation-version: %1
  # ants-terminal MCP orientation prelude (ANTS-1897).
  # This file is managed by Ants Terminal — edits to this managed
  # body are overwritten on the next launch. To take ownership,
  # delete the '# ants-orientation-version:' marker line above.
  # Disable: Settings → General → uncheck "Show MCP
  # cheat-sheet at Claude session start", OR delete this file and
  # restart Ants Terminal.
  if [ -z "${ANTS_MCP_SOCKET:-}" ] || [ ! -S "$ANTS_MCP_SOCKET" ]; then
    exit 0   # Ants MCP not reachable; stay silent
  fi
  cat <<'EOF'
  Ants MCP is connected. Before reaching for Edit/Write/Bash, check
  if a dedicated MCP tool exists — they are typically 5–10× cheaper:

    • changelog_log    → CHANGELOG.md append (Keep-a-Changelog aware)
    • roadmap_log      → ROADMAP.md append / status flip / edit
    • file_outline     → outline a file; read_regions = N slices, 1 call
    • find_definition  → "where is foo defined?"
    • find_sources     → "who calls bar?"
    • git_state        → status + branch + ahead/behind in one call
    • read_log         → Ants debug-log tail (NOT git log)
    • session_orient   → first-call session bootstrap (state + layout)
    • model_switch_stats → switcher trust signal + envelope

  Full catalog: load with ToolSearch query "mcp__ants" (or per-name
  with "select:mcp__ants__<name>"). Tool descriptors carry
  selection_hint lines describing when to reach for each.
  EOF
  ```

  The body is held in code as the `kOrientationScriptBody`
  byte-string literal in `src/mcporientation.cpp` (new file, ~120
  LOC including the .h and .cpp combined — ~515 LOC as built),
  exposed as a QString via
  `orientationScriptTemplate()`, with a single `%1` placeholder for
  `ANTS_VERSION`. At install time the template is
  substituted via `QString::arg(ANTS_VERSION)` and the result is
  written to disk. The spec, the shipped script, and the test
  snapshot stay in lockstep via this constant — see INV-12.

  **The fenced body above is illustrative** (the cheat-sheet shape
  only); the canonical, byte-exact source is `kOrientationScriptBody`
  in `src/mcporientation.cpp`, which additionally carries the
  ANTS-1971 conditional `*_Ants_MCP_Feedback.md` tail printed *after*
  the heredoc. INV-12's content-parity check (ANTS-2038) compares the
  installed file to that constant, **not** to this fenced excerpt — so
  do not treat the fence as the byte target.

#### 2.1.2 Settings.json merge

On every launch of Ants Terminal, the integration layer reads
`~/.claude/settings.json` (or creates it if absent), merges in the
Ants SessionStart hook entry, and writes back atomically:

- **Read:** parse via `QJsonDocument`; refuse to merge if the parse
  fails (log + emit a `claudeIntegrationWarning` chip; the user can
  fix their settings.json manually).
- **Merge rule:** the Ants hook entry is identified by a stable
  marker — the `command` string contains the literal substring
  `ants-terminal/hooks/mcp-orientation.sh` (substring match, not
  exact). If a matching entry exists, the merge is a no-op (the
  existing entry's `timeout` and surrounding structure are
  preserved). If absent, the entry is appended to
  `hooks.SessionStart`. No other entries (existing or
  Ants-owned for other slots) are touched. **Collision posture:**
  the substring is specific enough (project name + dir + filename)
  that a legitimate non-Ants tool invoking a script at that
  *exact* path is implausible. If a future Ants component needs a
  *different* SessionStart hook, it must use a different filename
  to avoid the marker collapse.
- **Schema written:** the `command` string is the absolute path
  resolved at install time from `QStandardPaths::
  GenericConfigLocation` plus `ants-terminal` (typically
  `/home/<user>/.config/ants-terminal/hooks/mcp-orientation.sh`; under
  XDG overrides or flatpak this resolves elsewhere — the install path
  and the `command` string are derived from the *same* call in the
  same launch, so they always agree even under non-default
  prefixes). The hook runner does not reliably expand `~`. The
  command **explicitly prefixes `bash`** so the shape matches the
  user's pre-existing `UserPromptSubmit` hook style (`bash
  /home/.../git-context-on-keyword.sh`, verified live) — visual
  consistency in `settings.json` plus a deterministic interpreter
  (no shebang ambiguity if the file's executable bit is later
  stripped). Schema:
  ```json
  {
    "hooks": {
      "SessionStart": [
        { "hooks": [
          { "type": "command",
            "command": "bash /home/<user>/.config/ants-terminal/hooks/mcp-orientation.sh",
            "timeout": 3 }
        ] }
      ]
    }
  }
  ```
- **Write:** `QSaveFile` with `setOwnerOnlyPerms()` (the existing
  `coding.md` § 5.2 helper) so we don't widen permissions of a file
  the user may have hand-trimmed.
- **Atomicity:** read → merge → tmp-write → rename. Concurrent Ants
  launches resolving to the same hook entry are idempotent (the
  marker substring match dedupes).
- **Other writers (ANTS-6471):** the merge holds `ConfigWriteLock` on
  `settings.json` from before its read to after its rename, as the other
  Ants writers of that file do, and refuses when another holder keeps it.
  Claude Code takes no such lock, so just before the rename the merge
  re-reads the file and backs off if it changed. That narrows the window;
  it cannot close it. A refused merge is retried on the next launch.

#### 2.1.3 First-run nudge + Settings toggle

- New config key: `claude.mcp_orientation_enabled`
  (`bool`, default `true`).
- New config key: `claude.mcp_orientation_nudge_shown`
  (`bool`, default `false`) — one-shot latch.
- On Ants launch:
  1. If `claude.mcp_orientation_enabled == false`: ensure the hook
     entry is **removed** from `~/.claude/settings.json` (the same
     marker substring identifies it for removal). Skip the script
     install.
  2. Else if `claude.mcp_orientation_nudge_shown == false`: show a
     non-blocking toast in the main window — *"Ants now installs a
     SessionStart hook in `~/.claude/settings.json` so every Claude
     Code session opens with a short Ants-MCP cheat-sheet. You can
     mute this in Settings → General."* Set
     `claude.mcp_orientation_nudge_shown = true` regardless of user
     interaction with the toast (the toast itself is the nudge).
     Then install the script + merge the hook entry. (The toast
     names the file explicitly — user trust before convenience;
     no silent edits to a user-owned settings file.)
  3. Else: install the script (idempotent per § 2.1.1) and merge the
     hook entry (idempotent per § 2.1.2).
- Settings dialog → **General** tab (verified
  `SettingsDialog::setupGeneralTab()` hosts the
  existing Claude toggles: `m_claudeTabStatusIndicator` and the
  ANTS-1735 `m_claudeAutoModelSwitch` "Let Ants pick the Claude
  model for me" toggle. The "AI Assistant" tab is for the legacy
  Ollama-style chat completion; it is *not* about Claude Code
  integration. Cold-eyes loops 3–5 misread this; the corrected
  placement is General — the implementation lands there next to
  the other Claude toggles.) Add a checkbox **"Show MCP
  cheat-sheet at Claude session start"** bound to
  `claude.mcp_orientation_enabled`. Toggling re-runs the install /
  uninstall flow.

#### 2.1.4 PTY env-var export — the script's reachability hinge

The script's `[ ! -S "$ANTS_MCP_SOCKET" ]` guard requires
`ANTS_MCP_SOCKET` to be exported into the user's shell environment.
Verified gap: today no code in `src/claudeintegration.{cpp,h}` sets
this env var, so without this spec the prelude would never fire.

- **Mechanism — parent-side `qputenv`, child-side inherit.**
  Immediately after the MCP server starts in
  `MainWindow::setupClaudeMcpProviders`
  (`m_claudeIntegration->startMcpServer(mcpSocket)`; the
  `mcpSocket` value is defined just before it), the same `mcpSocket` path is
  exported into the *parent* process environment via
  `qputenv("ANTS_MCP_SOCKET", mcpSocket.toLocal8Bit())`.
  `src/ptyhandler.cpp`'s non-flatpak shell-launch path
  iterates `extern char **environ` to populate the pre-forked
  `childEnvp[]` array, so every PTY spawned after this
  `qputenv` inherits the var without any change to
  `Pty`'s API (`src/ptyhandler.cpp`).
- **Ordering verified:** the MainWindow ctor calls `setupStatusBarChrome()`
  (which in turn calls `setupClaudeMcpProviders()` → `startMcp
  Server`) **before** the first `newTab()`.
  No PTY is spawned in the gap. The qputenv right after
  `startMcpServer` therefore lands before any child shell forks.
- **Scope (INV-14):** the **non-flatpak shell-launch path** in
  `ptyhandler.cpp` (the `environ`-copy branch). The
  flatpak branch in the same file uses `flatpak-spawn` semantics
  with its own env-pass rules — covered by **ANTS-1900** (new
  follow-up). INV-14 explicitly does not enforce on the flatpak
  branch. This is acceptable: the dominant case is openSUSE /
  Linux distros running Ants directly, where the non-flatpak
  branch is the only path.
- **Cleanup:** the env var is set per-Ants-instance (carries the
  per-process MCP socket path). When Ants exits, the socket file
  is removed (`mcpd::reapStaleTerminalSockets` sweeps sockets
  left by dead processes); the env var in still-running
  shells points at a now-defunct socket — the hook script's
  `[ ! -S ... ]` guard catches this and exits 0.

#### 2.1.5 Uninstall on Ants removal

Out of scope for this spec — there is no Ants `--uninstall`
command-line action today (`packaging/` builds AppImage / RPM; users
remove via their package manager or by deleting the AppImage). The
hook script is harmless when Ants is gone: the socket-existence
guard at the top of `mcp-orientation.sh` early-exits, so the hook
emits nothing and Claude Code sessions continue normally.

### 2.2 selection_hint sweep across every MCP tool descriptor

- **Coverage:** every tool descriptor *registered with
  `registerToolProvider`* gets a sibling
  `*Tool["selection_hint"] = QStringLiteral("Use when …")` line.
  Tools that already have one get the hint reviewed against the
  canonical format (§ 2.2.1) and tightened if weak. **Excluded
  by definition** (these are `["name"]` sites that are *not*
  tool registrations): `serverInfo["name"]` (MCP
  server identity), `props["name"]` (an
  inputSchema property name), `env["name"]` (an
  inner row of the `get_environment` response JSON, *not* a
  tool registration).
- **Audit step:** during implementation, the developer runs
  ```sh
  grep -cE '\["name"\][[:space:]]*=' src/claudeintegration.cpp
  grep -cE '\["selection_hint"\][[:space:]]*=' src/claudeintegration.cpp
  ```
  Today's expected counts: **70 `["name"]` raw, 68
  `["selection_hint"]` raw** — subtract the 3 + 1 non-tool
  exclusions named in § 1.1 → 67/67 net (1:1). The runtime test
  (INV-6) is authoritative — it iterates the live `tools/list`
  response and asserts coverage. The implementation commit message records the
  number of tightened hints.
- **Build-time enforcement:** see INV-6 + § 6 test surface.

#### 2.2.1 Canonical selection_hint format

```
Use <when X>. <Token-cost or trade-off cue>. [Avoid for Y.]
```

- **First sentence** names the *operation* — what the caller is
  trying to do, in concrete terms ("append a CHANGELOG entry", "scan
  a file's structure"), not the tool's full feature set.
- **Second sentence** carries a *cost or trade-off cue* — what the
  caller saves by reaching for this tool ("~10× cheaper than `Edit`
  on a 50-line diff", "≤500 B vs full mode's ~3 KB"). If the cost
  signal is "cheap" with no specific comparison, say so but keep it
  one short word.
- **Optional third clause** names an anti-case if the tool is
  routinely mis-reached for ("Avoid for non-Ants-tab terminals.").
- **Length:** ≤ 280 chars (a one-line tweet). Hard cap enforced by
  the test surface in INV-7.
- **Tone:** imperative, no greetings, no marketing.

**INV-7 scope:** the test enforces *shape* (length + `"Use "`
prefix), not *quality* (does the second-sentence cost-cue accurately
capture the trade-off?). Quality is deferred to **ANTS-1898**
(periodic hint audit). This asymmetry is deliberate — quality
review is a judgment call that doesn't compile to a unit test.

### 2.3 RAM / build cost

- Bundled orientation script: ~1.4 KB on disk per user (`~/.config/
  ants-terminal/hooks/mcp-orientation.sh`).
- `~/.claude/settings.json` grows by ~180 B (the new SessionStart
  hook entry).
- `kOrientationScriptBody` constant in `mcporientation.cpp`:
  ~1.4 KB static string baked into the binary.
- New source files: `src/mcporientation.{h,cpp}` (~515 LOC total
  as built; the ~120 estimate predated the ANTS-1901/1971 fold-ins)
  added to the `ants_claude_lib` STATIC library (per CMakeLists.txt
  — the lib that hosts `claudeintegration.cpp`, the only
  caller). Adding to `ants_core_lib` (the foundation lib) would
  invert the dependency direction.
- Existing `selection_hint` string literals are static; coverage
  is already 67/67 (verified § 1.1) so the sweep adds zero new
  hints — it rewrites one outright and tightens cost-cue
  language on an estimated ~20 others. Net binary growth: ~1 KB
  of literal string data churn (the tightening expands some hints,
  trims others), no new symbols.
- No new external dependencies.

## 3. Invariants

- **INV-1** — The hook script `~/.config/ants-terminal/hooks/
  mcp-orientation.sh` carries the header marker
  `# ants-orientation-version: <ANTS_VERSION>`. When the file
  already exists with the marker line (Ants-managed), Ants rewrites
  it whenever the on-disk body differs byte-for-byte from the
  rendered template `orientationScriptTemplate().arg(ANTS_VERSION)`.
  The byte-difference is the sole gate: a version bump is merely one
  such difference (it changes the marker line); an in-version body
  edit (ANTS-2038) is another. A byte-identical body is the
  idempotent skip. *Tests:*
  `tests/features/mcp_orientation_install/` cases
  `idempotent_rewrite` (byte-identical → no rewrite) and
  `in_version_body_refresh` (same version, stale body → rewritten).
- **INV-2** — When the file exists **without** the marker line
  (user-owned), Ants logs a warning and does not overwrite. When
  the file does **not exist at all** (first launch), Ants writes
  it fresh — INV-2 does not block the bootstrap path. *Test:*
  same dir, cases `user_owned_preserved` and
  `first_launch_writes_fresh`.
- **INV-3** — Merging the SessionStart hook into
  `~/.claude/settings.json` is idempotent **and self-healing**.
  Every install sweeps every existing entry whose `command`
  contains the marker substring `/hooks/mcp-orientation.sh`
  (suffix-only — robust against `applicationName`-driven
  directory-name variants: lowercase `ants-terminal` vs Qt's
  verbatim-cased `Ants Terminal`) and replaces them with a
  single canonical entry. The `command` value bash-single-quotes
  the script path so a space in it does not tokenise at hook-run
  time.
  *Tests:* same dir, cases `settings_merge_idempotent`,
  `sweeps_existing_duplicates`, `command_runnable_with_spaces_in_path`
  (ANTS-1902 fold-in).
- **INV-4** — Merging preserves all unrelated entries in
  `~/.claude/settings.json` (existing `UserPromptSubmit` hooks,
  other `SessionStart` entries from other tools, top-level keys).
  *Test:* same dir, case `settings_merge_preserves_unrelated`.
- **INV-5** — Setting `claude.mcp_orientation_enabled = false`
  removes the Ants hook entry from `~/.claude/settings.json` on
  next launch and skips the script install. *Test:* same dir,
  case `disable_removes_entry`.
- **INV-6** — Every MCP tool descriptor returned by the live
  `tools/list` response has a non-empty `selection_hint` field
  (covers the 65 `registerToolProvider`-routed tools *and* the 2
  inline-dispatched control-plane tools). *Test:* `tests/features/
  mcp_selection_hint_coverage/` case `every_descriptor_has_hint`
  (parses the live `tools/list` MCP response and asserts).
  Coverage is already 67/67 today (§ 1.1) — INV-6 is a
  *regression guard* against future descriptor additions
  forgetting the field, not a gap-closing claim.
- **INV-7** — Every `selection_hint` is ≤ 280 chars and starts
  with the case-insensitive prefix `"Use "`. (The one existing
  non-conforming hint —
  `"Call once at session start instead of …"` — is rewritten by
  the sweep to satisfy the prefix.) *Test:* same dir, case
  `selection_hint_format`.
- **INV-8** — The first-run nudge fires at most once per user:
  `claude.mcp_orientation_nudge_shown` is set to `true` after the
  toast is queued. *Test:* `tests/features/mcp_orientation_install/`
  case `nudge_latch`.
- **INV-9** — The hook script exits 0 on every code path,
  including the "Ants MCP socket not reachable" branch — Claude
  Code session-start must never fail because of an Ants hook.
  *Test:* same dir, case `script_exit_zero` (runs the script
  in three scenarios: socket present, socket missing, malformed
  env).
- **INV-10** — The hook script's stdout output is ≤ 1200 chars
  (~300 tokens at ~4 chars/token) — token-cost cap, the prelude
  must stay cheap. The current draft body's heredoc content in
  § 2.1.1 measures **876 bytes** when un-indented — reproducible
  via `ANTS_MCP_SOCKET=/tmp/x bash /home/<user>/.config/
  ants-terminal/hooks/mcp-orientation.sh | wc -c` against an
  actual socket-bearing file at `/tmp/x` (or just measure the
  heredoc content directly with `wc -c` on the column-0
  un-indented body). That leaves ~27 % headroom for future tool
  additions (1200 − 876 = 324). *Test:* `tests/features/
  mcp_orientation_install/` case `script_output_byte_cap` —
  runs the installed script in a controlled `ANTS_MCP_SOCKET`
  env (touch a fake socket file, override `[ -S "$ANTS_MCP_SOCKET
  " ]` by symlinking to a `/dev/null`-like target — or stub the
  guard via env-var override in a wrapper), captures stdout,
  asserts `len <= 1200`.
- **INV-11** — `settings.json` is written with owner-only
  permissions (`0600`); a wider mode pre-merge is preserved by
  the write back (we never widen, but we don't narrow either if
  the user explicitly chose a broader mode). *Test:* same dir,
  case `settings_perms_preserved`.
- **INV-12** — **Whenever the script exists and carries the marker
  prefix (Ants-managed)**, after install its body is byte-identical
  to `orientationScriptTemplate().arg(ANTS_VERSION).toUtf8()` — the
  in-binary template after `%1`→`ANTS_VERSION` substitution,
  converted to UTF-8 bytes. Under the ANTS-2038 content-parity
  rewrite this holds for every marker-present file post-install:
  the case-2 path either found the bytes already identical
  (idempotent skip) or rewrote them to the template. INV-12 does
  **not** apply to a marker-ABSENT file (case 3, user-owned — never
  written; INV-2 governs).
  **Placeholder contract:** the template MUST contain **exactly
  one `%1`** placeholder (the marker version line) and **no other
  `%N` sequences**. Shell-side literal `%` characters (e.g. a
  future `printf "%s"` addition) MUST be escaped as `%%`
  so `QString::arg` does not consume them. Today's template literal
  (`kOrientationScriptBody`) is `%`-clean except for the single
  `%1` on the marker line. (A whole-file
  `grep -oE '%[0-9]|%%' src/mcporientation.cpp` is **not** a valid
  check — the `.arg()`-bearing `QStringLiteral` warning strings
  elsewhere in the file legitimately contain `%1`; scope any check
  to the `kOrientationScriptBody` constant body.) *Test:* same
  dir, case `script_matches_constant` — start from an empty
  `~/.config/ants-terminal/hooks/` dir, trigger the installer
  (forcing the fresh-write path), open the result with `QFile`
  in `ReadOnly` mode (binary, no `QTextStream` line-ending
  normalisation), compute `template.arg(ANTS_VERSION).toUtf8()`,
  compare with `==`.
- **INV-13** — When `~/.claude/settings.json` fails to parse,
  Ants does **not** overwrite it and emits a
  `claudeIntegrationWarning` chip. *Test:* same dir, case
  `bad_json_no_clobber`. The same refusal covers valid JSON whose `hooks`
  is not an object or whose `SessionStart` is not an array, and a merge
  that changes nothing does not rewrite the file.
  *Tests:* `WrongTypedHooksNoClobber`,
  `NoOpInstallLeavesSettingsUntouched`.
- **INV-14** — Every PTY spawned by Ants Terminal via the
  non-flatpak shell-launch branch carries the env var
  `ANTS_MCP_SOCKET=<absolute path to the MCP socket>` (the same
  path passed to `startMcpServer` in
  `MainWindow::setupClaudeMcpProviders`). This is the reachability hinge the
  hook script tests at `[ -S "$ANTS_MCP_SOCKET" ]`. The flatpak
  branch is scoped out (§ 4 ANTS-1900). *Test:*
  `McpOrientation_Inv14.MainWindowExportsSocket` in
  `tests/features/mcp_orientation_install/`.

- **INV-15** (ANTS-6471) — The `settings.json` merge writes nothing
  when another holder keeps its `ConfigWriteLock`, or when the file
  changed between the merge's read and its rename; the install then
  reports a warning and `ok:false`. *Test:*
  `McpOrientation_Inv15.HeldLockRefusesTheMerge` and
  `…ConcurrentEditIsNotOverwritten` in
  `tests/features/mcp_orientation_install/`.

## 4. Out of scope

- **Per-tool selection_hint quality regression tests.** This spec
  enforces the *shape* (INV-6, INV-7) but not the per-tool quality
  ("does the hint actually capture when to reach for this tool?").
  Tracked by **ANTS-1898** (new) — periodic hint audit via a
  cold-eyes-style review focused on hint utility.
- **Per-tool catalog MCP arm** (e.g. `mcp_catalog(category: "files")`
  returning hint + cost cue groups). Defers to **ANTS-1899** (new) —
  a structured catalog tool callable in-session for cases the
  prelude does not cover.
- **Flatpak PTY env-var export** for `ANTS_MCP_SOCKET`. The
  `ptyhandler.cpp` flatpak branch uses `flatpak-spawn` with its
  own env-pass rules; INV-14 covers only the non-flatpak branch.
  Tracked by **ANTS-1900** (new) — flatpak-path support so MCP
  discoverability works inside flatpak-sandboxed Ants installs.
- **System-wide install of the hook script** (e.g. under
  `/usr/share/ants-terminal/hooks/`). The per-user path is
  sufficient and avoids packaging plumbing for AppImage / RPM /
  source builds; promoted only if real demand surfaces.
- **Wrapping the `claude` command** via shell function or alias
  injection — explicitly rejected per the 2026-05-26
  AskUserQuestion (the user picked SessionStart hook).
- **Per-project orientation prelude** (e.g. project-specific
  CLAUDE.md fragment listing MCP tools). Rejected by the user
  2026-05-26 (CLAUDE.md bloat concern); the SessionStart hook
  approach handles this once globally instead.

## 5. Migration / compatibility

- Existing Ants Terminal installs upgrading from a pre-ANTS-1897
  build: on first launch after upgrade, the first-run nudge fires
  once, the hook script is installed, and `~/.claude/settings.json`
  is merged. Users
  who don't want the prelude flip the Settings checkbox and the
  entry is removed on next launch.
- Existing `claude.auto_model_*` config keys from ANTS-1735 are
  unaffected; the new keys (`claude.mcp_orientation_enabled`,
  `claude.mcp_orientation_nudge_shown`) live alongside.
- Users with a hand-maintained `~/.claude/settings.json` keep
  ownership of every entry they wrote — the marker-substring
  approach (INV-2, INV-3) only touches Ants-owned content.
- Users with no `~/.claude/settings.json` get the file created
  with owner-only perms containing only the Ants SessionStart
  entry.
- **Config keys introduced by this spec** (live in
  `~/.config/ants-terminal/config.json` per ANTS-1735's
  precedent):
  - `claude.mcp_orientation_enabled` (bool, default `true`) —
    master toggle.
  - `claude.mcp_orientation_nudge_shown` (bool, default `false`)
    — one-shot latch for the first-run toast.

- **The script's old folder** (ANTS-5730): `<config>/Ants Terminal/hooks/`,
  the folder `AppConfigLocation` names.
  - The script path is `GenericConfigLocation` + `/ants-terminal/hooks/mcp-orientation.sh`.
  - On install, a script found at the old path moves to the new one when
    nothing is there. When the new path is taken, an old copy that carries
    the Ants marker is deleted and one that does not is left alone.
  - The `SessionStart` entry needs no migration of its own: an entry that
    names the old path is not the canonical one, so the merge replaces it.

  Tests: the four `McpOrientation_Ants5730` cases in
  `tests/features/mcp_orientation_install`.

## 6. Tests

- **`tests/features/mcp_orientation_install/`** — covers
  INV-1..INV-5, INV-8..INV-13, INV-15 (filesystem + settings.json
  harness against `QStandardPaths::setTestModeEnabled(true)`
  + `QTemporaryDir`).
- **`tests/features/mcp_pty_env_carries_socket/`** — covers
  INV-14 (separate dir — PTY harness against a real
  `/dev/ptmx`, distinct from the temp-HOME filesystem harness).
  **Not built (ANTS-4135, 2026-08-15).** INV-14 is locked by
  `McpOrientation_Inv14.MainWindowExportsSocket` and
  `…FullCatalogNamesToolInfoCatalog` in the orientation bundle
  above; the `/dev/ptmx` harness was never written.
  - `spec.md` references this spec.
  - `test_mcp_orientation_install.cpp` exercises the installer
    against a temp HOME (Qt `QStandardPaths::setTestModeEnabled`
    + `QTemporaryDir`), each case as a TEST() in the bundled
    `test_core` shared bundle per ANTS-1217.
  - Label `features;fast`.
  - Each case must FAIL against pre-fix source — verified before
    restoring the implementation.

- **`tests/features/mcp_selection_hint_coverage/`** — covers
  INV-6, INV-7. **Shipped under two other names (ANTS-4135,
  2026-08-15):** `McpOrientation_Inv6.SelectionHintCoverage` and
  `McpOrientation_Inv7.SelectionHintFormat` in
  `tests/features/mcp_orientation_install/`, plus
  `tests/features/mcp_selection_hint/` for the tool-info and
  size-budget angles. Both invariants are locked; only this
  directory name is fiction.
  - `test_mcp_selection_hint_coverage.cpp` boots a minimal
    `ClaudeIntegration` instance, calls the internal
    tool-descriptor build path, and asserts the live JSON
    response from `tools/list` has every descriptor carrying a
    non-empty `selection_hint` ≤ 280 chars starting with `"Use "`.
  - Same bundle, same label.

- **Build-time grep test (cheap regression):**
  `tests/audit_fixtures/mcp_selection_hint/bad.cpp` triggers
  on a descriptor pattern missing `selection_hint`; the
  `tests/audit_self_test.sh` harness asserts the rule fires.
  Catches the gap at edit time before the runtime test.

## 7. Cross-doc impact

- **`docs/standards/mcp-tools.md`** — append a `selection_hint`
  bullet to the umbrella authoring checklist. This is the single
  home for the rule; project `CLAUDE.md` already delegates the
  full checklist to this file (`## MCP tool authoring` says
  "follow `docs/standards/mcp-tools.md`"), so no CLAUDE.md edit
  is needed — duplicating the rule there would invite drift.
- **`CHANGELOG.md`** — Added entry: *"MCP discoverability — Ants
  now merges a SessionStart hook entry into
  `~/.claude/settings.json` (under a marker substring it owns;
  other entries in that file are preserved). The hook prints a
  short Ants-MCP cheat-sheet at every Claude Code session start
  so Claude can pick the cheaper MCP variants over generic
  Edit/Write/Bash. First-run toast announces the file edit.
  Toggle off in Settings → General to remove the entry on
  the next launch. ANTS-1897."* Logged via
  `mcp__ants__changelog_log` (NOT Edit) per the project memory
  `use-changelog-log-mcp`.
- **`README.md`** — one-line bullet under the existing AI-Assistant
  / Claude-integration section: *"MCP cheat-sheet auto-injected
  at every Claude Code session start."*
- **`ROADMAP.md`** — split across two commits:
  - **Spec-acceptance commit** (this spec lands): (i) update the
    ANTS-1897 entry body to reflect the SessionStart-hook
    mechanism (§ 1.0 above flagged the original body proposed a
    "spawn-path system-prompt assembly" which the user replaced
    with the SessionStart-hook approach via AskUserQuestion
    2026-05-27); (ii) add ANTS-1898 (per-tool hint quality
    audit), ANTS-1899 (mcp_catalog structured catalog), and
    ANTS-1900 (flatpak PTY env-var path) as 📋 follow-ups.
  - **Implementation-shipped commit** (after impl + tests
    green): flip ANTS-1897 ✅ shipped.
- **`.roadmap-counter`** — current value is `1897` at spec time;
  bumped to `1900` in the spec-acceptance commit (allocates the
  ANTS-1898 / ANTS-1899 / ANTS-1900 IDs above). A future spec
  bumping from a stale value would collide with those
  allocations — bump from the live counter.

## 8. Open questions

- **OQ-1** — Should the orientation script be re-emitted on every
  Claude session start, or rate-limited (e.g. once per UTC day)?
  *Decision*: emit every session. The prelude is ~300 tokens; the
  cost of repeating it is dwarfed by the cost of one unnecessary
  `Edit` on `CHANGELOG.md` it prevents. (Note: an earlier draft
  claimed Claude Code "de-dupes cached context across sessions";
  that's an unverified vendor-behaviour claim — dropped from the
  rationale.)
- **OQ-2** — Should `mcp-orientation.sh` consult `caller_cwd` or
  socket env to skip when the session is *not* Ants-spawned?
  *Decision*: yes, via the `ANTS_MCP_SOCKET` env-var check at the
  top of the script. Specified in § 2.1.1 + § 2.1.4.
- **OQ-3** — Where does `ANTS_MCP_SOCKET` actually get exported,
  and is the plumbing reliable across PTY-spawn paths? *Decision
  (escalated to CRITICAL during cold-eyes loop 1; fix baked into
  § 2.1.4 + INV-14)*: exported via parent-side `qputenv`
  immediately after `startMcpServer` returns; inherited into
  child PTYs via the existing `environ`-copy loop in
  `src/ptyhandler.cpp`. Scope is the non-flatpak branch — see
  ANTS-1900 for the flatpak follow-up.
- **OQ-4** — The orientation list of "high-value MCP tools" will
  grow as new tools ship. How is it kept current? *Decision*: the
  `kOrientationScriptBody` constant lives next to the
  descriptor registry in source — any developer touching a new
  MCP tool sees the constant nearby and updates it. § 4 ANTS-1898
  covers the audit cadence.

## 9. Cold-eyes loop log

Rows live in [`docs/reviews/ANTS-1897-sessionstart-loop-log.md`](../reviews/ANTS-1897-sessionstart-loop-log.md).
