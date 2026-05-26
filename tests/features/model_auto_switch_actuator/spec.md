# Feature: ANTS-1735 actuator wiring INV-14

The autonomous-switcher actuator (`ClaudeStatusBarController::refreshAutoModelSwitch`)
must:

1. **Bail when disabled** — read `Config::claudeAutoModel()` at the top
   of the method and `return` immediately when
   `switch_enabled == false`. With the default-OFF config, the timer
   tick costs one config read.
2. **Suppress the Shape A chip when enabled** —
   `refreshModelChip()` must hide `m_modelBtn` and return when
   `switch_enabled == true`. The user opted in to autonomy; a
   clickable `→ Opus` chip would reintroduce the manual-decision
   surface that pushed them to autonomy in the first place.
3. **Run on the 2 s status timer** — `mainwindow.cpp` wires the
   actuator to `m_statusTimer::timeout` alongside `refreshModelChip`.

Source-grep test — the actuator's `decide()` payload is exercised by
`tests/features/model_auto_switch/`; this test locks the controller-
level wiring + the chip-suppression gate against
`src/claudestatuswidgets.cpp` and `src/mainwindow.cpp`.
