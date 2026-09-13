# Feature: the tab tooltip has one writer

## Invariants

**INV-1 — the tab bar does not write tab tooltips.** `ColoredTabBar::paintEvent`
calls no `setTabToolTip`.

**INV-2 — the status-bar controller writes them.**
`ClaudeStatusBarController` sets the tab tooltip from the tracker's state,
naming the tool in use.

## Rationale

The controller set a per-tool tooltip ("Claude: <tool>") when the tracker's
state changed, then asked the tab bar to repaint. The tab bar's `paintEvent`
wrote a coarser "Claude: <state>" tooltip on every paint, overwriting it. The
tooltip is also the screen-reader surface (ANTS-1185), so one writer keeps what
is read aloud and what is shown the same.

## Test surface

`test_tab_tooltip_single_writer.cpp` reads `src/coloredtabbar.cpp` and
`src/claudestatuswidgets.cpp`, found beside `SRC_MAINWINDOW_CPP_PATH`.

## Regression history

- **ANTS-5081:** the tab bar's paint overwrote the controller's tooltip.
  Locked by this spec.
