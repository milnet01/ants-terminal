# OSC 133 "Last Completed Command" Actions

Feature specification for `copyLastCommandOutput` / `rerunLastCommand` — top-level
keyboard-driven equivalents of the right-click block context menu's Copy Output /
Re-run Command entries.

Ships in 0.6.40 as the OSC 133 shell-integration polish follow-up to the
0.6.38 + 0.6.39 Wayland Quake-mode pair.

## Contract

### (1) TerminalWidget declares both slots in its public API

`src/terminalwidget.h` declares:

```cpp
int copyLastCommandOutput();
int rerunLastCommand();
```

Both return `>= 0` on success, `-1` on "no completed command". MainWindow
uses the return to decide between the "Copied N chars" success toast and
the "No completed command to copy (enable shell integration)" hint.

### (2) "Last completed" means walk backwards skipping in-progress tail

Both methods iterate `promptRegions()` from back to front and accept the
first region with `commandEndMs > 0`. If the tail region is still running
(the user typed a command but it hasn't emitted OSC 133 D yet), both
methods must skip it and use the previous completed region.

Rationale: Ctrl+Alt+O during `sleep 30` should copy the *previous* command's
output, never the partial in-progress command. Ctrl+Alt+R during the same
should re-run the previously-completed command, never splice text into the
running command's stdin.

The test verifies this by grepping for the backwards loop + `commandEndMs > 0`
check in both methods.

### (3) copyLastCommandOutput uses outputTextAt, not lastCommandOutput

`lastCommandOutput()` caps output at 100 lines — it's sized for the
`commandFailed` trigger where huge stderr noise is harmful. A user-
initiated copy expects the full output. The implementation must delegate
to `outputTextAt(idx)` (unbounded) not `lastCommandOutput()`.

### (4) MainWindow wires both actions with configurable keybindings

`mainwindow.cpp` must call `m_config.keybinding(...)` with keys
`copy_last_output` (default `Ctrl+Alt+O`) and `rerun_last_command`
(default `Ctrl+Alt+R`). Hard-coded shortcuts would lock users out of
rebinding via config.json, which is the established pattern for every
other user-facing keybinding.

### (5) Defaults do not collide with existing keybindings

`Ctrl+Alt+O` and `Ctrl+Alt+R` must not already be bound elsewhere in
`mainwindow.cpp`. `Ctrl+Shift+O` is taken by split_vertical and
`Ctrl+Shift+R` by record_session — picking those defaults would shadow
existing features. The test greps `mainwindow.cpp` for any other
`Ctrl+Alt+O` / `Ctrl+Alt+R` binding and fails if one appears.

### (6) MainWindow surfaces status-bar feedback

Both action handlers call `showStatusMessage(...)` on success and
failure. Silent behavior on Ctrl+Alt+O is disconcerting when the user
doesn't know the integration isn't installed — the hint
"(enable shell integration)" in the failure toast is load-bearing
UX guidance.

### (7) CMakeLists.txt wires the test target

The test is built as `test_osc133_last_command` with `features;fast`
labels and TIMEOUT 10, matching the existing pattern.
