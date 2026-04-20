# NoAnimStyle — zero style-driven animations

## Contract

Ants Terminal wraps its QStyle in a `QProxyStyle` subclass (`NoAnimStyle`) that
reports `QStyle::SH_Widget_Animation_Duration` and `QStyle::SH_Widget_Animate`
as `0`. This neutralises Fusion's built-in `QWidgetAnimator` that otherwise
creates a persistent 60 Hz `QPropertyAnimation(target=QWidget, prop=geometry)`
cycle on an idle window.

## Why it matters

Diagnostic paint-logging on 2026-04-20 captured **1439
`QPropertyAnimation(geometry)` `DeferredDelete` events in a 23 s idle window**
(≈62 Hz). Each animation completion drove a full
`LayoutRequest → UpdateRequest → Paint` cascade across every widget in the
main window, which the user saw as dropdown-menu flicker.

`QApplication::setEffectEnabled(UI_AnimateMenu, …)` covers menu show/hide,
not the style-hint-driven animations Fusion uses for widget state
transitions. `QMainWindow::setAnimated(false)` only disables the dock-widget
animator — a subset of the paths that consult the style hint. The only
robust switch is the style hint itself, via `QProxyStyle`.

## Invariants

- **INV-1** `main.cpp` defines a class named `NoAnimStyle` derived from
  `QProxyStyle`.
- **INV-2** `NoAnimStyle` overrides `styleHint(…)` and returns `0` for
  `SH_Widget_Animation_Duration`.
- **INV-3** `NoAnimStyle` also returns `0` (falsey) for `SH_Widget_Animate`.
- **INV-4** `QApplication::setStyle(…)` in `main()` wraps `Fusion` in
  `NoAnimStyle` — `setStyle(new NoAnimStyle(QStyleFactory::create("Fusion")))`
  or equivalent. A plain `app.setStyle("Fusion")` call is a regression.

## Regressions this would catch

- "Reverted to plain Fusion because NoAnimStyle broke nothing visually" —
  the 60 Hz layout storm is invisible without instrumentation; a future
  cleanup commit could drop the wrapper without noticing the regression.
  The `setStyle(new NoAnimStyle(...))` call is load-bearing.
- A refactor that keeps `NoAnimStyle` but forgets to override
  `SH_Widget_Animation_Duration` (e.g. overrides only
  `SH_Widget_Animate`) — Fusion consults both; missing either re-enables
  the cycle.
