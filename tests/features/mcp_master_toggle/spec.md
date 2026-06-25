# Master Ants-MCP on/off toggle — feature spec

Test contract for ANTS-1901. Full design: `docs/specs/ANTS-1901.md`.

`claude.mcp_enabled` (default **true**) is the master switch for the whole
Ants MCP integration. When off: the socket isn't bound at launch, the
orientation hook is removed, the auto model switcher stands down, and
every MCP verb is refused with `mcp_disabled`. The Settings checkbox
lives on the **General** tab and greys out the dependent toggles.

## Invariants

**INV-1 — Config round-trip.** `Config::claudeMcpEnabled()` defaults to
`true` on a fresh config and persists a written value across reload.
*Test:* real `Config` round-trip under a sandboxed `XDG_CONFIG_HOME`.

**INV-2 — Startup gate.** `src/mainwindow.cpp` contains exactly one
`startMcpServer(` call and exactly one `qputenv("ANTS_MCP_SOCKET"`, both
behind a `claudeMcpEnabled()` branch that precedes the bind. *Test:*
source-grep count + ordering.

**INV-3 — Orientation hook gated on the master.** The orientation
install fires only when `mcpOn && …claudeMcpOrientationEnabled()`; the
master-off path reaches `ants::mcp_orientation::uninstall()`. *Test:*
source-grep `mainwindow.cpp`.

**INV-4 — Auto-switcher early return.** A `!cfg.claudeMcpEnabled()`
early return precedes the `QStringLiteral("/model ")` injection in
`src/claudestatuswidgets.cpp`. *Test:* source-grep ordering.

**INV-5 — Dispatcher refusal.** A `!m_mcpEnabled` guard emitting
`mcp_disabled` sits inside the `tools/call` branch of
`src/claudeintegration.cpp`, before the `caller_cwd_required` emit.
*Test:* source-grep ordering.

**INV-6 — Settings master checkbox.** `m_claudeMcpEnabled` is created
inside `setupGeneralTab`, written in `applySettings`, and drives the
`mirrorMcpMaster` cascade. *Test:* source-grep `settingsdialog.cpp`.

**INV-7 — Runtime propagation.** The `settingsChanged` handler calls
`m_claudeIntegration->setMcpEnabled(m_config.claudeMcpEnabled())`.
*Test:* source-grep `mainwindow.cpp`.

**Registration.** `mcp_disabled` is a table row under the
`### 5 — Dispatcher / registry` heading of
`docs/standards/mcp-error-codes.md`. *Test:* source-grep placement.

## Test strategy

INV-1 is a real `Config` round-trip (links the config lib, same
`XDG_CONFIG_HOME` + `QTemporaryDir` harness as
`config_tab_title_format`). INV-2…INV-7 + registration are source-greps
(the `flatpak_host_shell` pattern) — the runtime gating fires in the
MainWindow ctor / forkpty child / live socket, none headlessly
exercisable, so the contract is "this wiring exists with this shape."
Compiled into the `test_core` bundle.
