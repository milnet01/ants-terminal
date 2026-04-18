# Command-Mark Gutter + Portal-Binding Status Hint

Feature specification for the 0.6.41 UX polish pair:

1. A narrow tick-mark gutter just left of the scrollbar, one tick per
   OSC 133 prompt region, color-coded by last exit status.
2. A portal-binding status label in Settings → Quake groupbox that
   reports whether out-of-focus hotkeys are available on this session.

## Contract

### (1) Config exposes show_command_marks with default true

`Config::showCommandMarks()` / `setShowCommandMarks(bool)` persist to the
`show_command_marks` JSON key. Default-on so users with shell integration
see the gutter automatically; default-off would hide the feature behind a
configuration step most users won't discover.

### (2) TerminalWidget exposes setShowCommandMarks

`setShowCommandMarks(bool)` + `showCommandMarks()` on TerminalWidget,
backed by `m_showCommandMarks` (default true).

### (3) paintEvent gates the gutter on m_showCommandMarks AND non-empty regions

The painting block must be gated on `m_showCommandMarks` and must early-out
when `promptRegions().empty()`. The first condition is the user toggle;
the second is the "no shell integration" silent-no-op invariant — users
without OSC 133 markers see no visual change.

### (4) Gutter positioned just left of the scrollbar

The gutter strip subtracts `m_scrollBar->width()` from the right edge
when the scrollbar is visible, so ticks don't overlap the scrollbar
thumb. A constant gutter width (4 px by spec) is reserved regardless of
prompt-region count.

### (5) Color logic covers three states

Per tick:
- `commandEndMs > 0 && exitCode == 0` → green
- `commandEndMs > 0 && exitCode != 0` → red
- `commandEndMs == 0` (in-progress) → gray

Hex values match the Catppuccin-ish palette used elsewhere; what we
enforce is the three-state branch exists in code.

### (6) MainWindow propagates config to new + existing terminals

`applyConfigToTerminal(t)` calls `t->setShowCommandMarks(m_config.showCommandMarks())`.
This function runs on terminal creation (new tab) and on settings-apply
(loops over `findChildren<TerminalWidget*>`), so the toggle takes effect
for all live terminals without restart.

### (7) SettingsDialog surfaces the toggle + portal status

Settings → Terminal tab has a `QCheckBox *m_showCommandMarks` that
load/save round-trips through `Config::showCommandMarks()`.

The Quake groupbox includes a status QLabel that branches on
`GlobalShortcutsPortal::isAvailable()` — "Portal binding active" (green)
or "Portal unavailable" with a one-line hint about installing
xdg-desktop-portal backends. Static-query based, because portal
availability is a per-session property that doesn't change at runtime.

### (8) CMakeLists.txt wires the test target with features;fast label
