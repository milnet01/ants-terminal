# Feature: Per-tab colour groups (ColoredTabBar)

## Contract

The tab context menu's "Red / Green / Blue / …" colour picker MUST
produce a visible, persistent colour tag on the tab that:

1. **Renders independently of the window stylesheet.** The pre-0.6.23
   code stored the user's choice in `m_tabColors[idx]` and called
   `QTabBar::setTabTextColor`, but the window-level stylesheet rule
   `QTabBar::tab { color: %6; }` wins over `setTabTextColor` with
   higher QSS specificity — so the colour never displayed. The bar
   MUST render the colour via a path the stylesheet can't pre-empt
   (overpainting a bottom strip after the base class has drawn).

2. **Survives tab reorder (drag-and-drop).** Storage keyed by tab
   *index* (as the old `m_tabColors[int]` was) goes stale the moment
   the user drags a tab to a new position. Qt's per-tab user data
   (`QTabBar::tabData(int)` + `setTabData`) is maintained by the
   tab bar through moves/inserts/removals; the colour must follow the
   tab it was attached to, not the index.

3. **Auto-drops when a tab closes.** Removing a tab must remove its
   colour metadata with it — no leaked entries that could mis-target
   a future tab that re-uses the vacated slot. Again a consequence
   of using `tabData()`: QTabBar clears per-tab data on `removeTab`.

4. **Round-trips exactly.** `bar.setTabColor(i, c)` followed by
   `bar.tabColor(i)` returns `c` when `c` is valid, and an invalid
   `QColor()` after `bar.setTabColor(i, QColor())` clears it.

5. **Persists across app restart.** The user picks a colour from the
   context menu, quits ants-terminal, relaunches — the tab (restored
   via `SessionManager` by its stable UUID) comes back with the same
   colour strip. Storage lives in the `tab_groups` config key as a
   JSON object mapping tab UUID → `"#rrggbbaa"` hex string. Keying by
   UUID rather than index means drag-reorder survives trivially;
   keying by UUID rather than title means rename survives too. Persisted
   entries are auto-pruned on `closeTab` so the config doesn't
   accumulate orphans for tabs that will never re-exist.

## Rationale

The context-menu feature was implemented in 0.6.x but silently broken
by the time themes gained explicit tab-colour stylesheet rules —
`setTabTextColor` lost the specificity fight. Users reported "right-
click, choose a colour, nothing happens" because the choice was
stored but never rendered. The fix moves both storage (into
`tabData`) and rendering (into a `paintEvent` override that overlays
a strip on top of the base-class paint) out of the stylesheet's reach.

## Scope

### In scope
- Colour storage round-trip through `setTabColor` / `tabColor`.
- Survival across `QTabBar::moveTab` (drag reorder).
- Cleanup on `QTabBar::removeTab`.
- Clearing via invalid `QColor()`.
- Cross-session persistence via `Config::tabGroups()`: write hex under
  a UUID, construct a fresh `Config` (simulates relaunch), read back,
  assert the colour still parses to the original `QColor`. Driven
  against `QStandardPaths::setTestModeEnabled(true)` so the real user
  config is never written.

### Out of scope
- Pixel-level paint verification. `paintEvent` draws a 3-px strip at
  a fixed location; correctness of pixel coordinates is trivial
  (fixed integer arithmetic on `tabRect`) and changes with the style,
  so we don't snapshot it.
- Interaction with the colour-picker context menu itself (requires a
  full MainWindow harness to drive the menu). A MainWindow-level GUI
  test could exercise the menu → ColoredTabBar path once such a
  harness exists.

## Regression history

- **0.6.x → 0.6.22:** the context menu built the colour list and
  stored the user's pick into `MainWindow::m_tabColors[int]`, then
  called `setTabTextColor`. The window stylesheet's
  `QTabBar::tab { color: %6; }` rule outranked the call, producing
  no visible colour. The index-keyed map also went stale on drag
  reorder. User-reported: "Right-click a tab, choose a colour,
  nothing happens."
- **0.6.23:** introduce `ColoredTabBar` (QTabBar subclass) +
  `ColoredTabWidget` (trivial QTabWidget wrapper to install it).
  Storage moves into `QTabBar::tabData`; rendering moves into
  `paintEvent` as a 3-px bottom strip drawn after the base class,
  outside stylesheet influence.
- **Post-0.6.30:** `tabData` storage is process-lifetime only — colours
  vanished on relaunch. User report: "I set a tab colour, close the
  app, re-open, the colour is gone." Fix: persist a UUID → hex map
  under the `tab_groups` config key, re-apply on session restore via
  `MainWindow::applyPersistedTabColor`, and prune entries on
  `closeTab` so orphans don't accumulate.
