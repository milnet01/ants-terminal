<!-- ants-menu-standards: 1 -->
# Ants Terminal menu standard

Project-local convention for the menu bar and its menus / submenus
(File, Edit, View, Split, Tools, Settings, Help, and every submenu
under them). Not part of the shareable `/start-app` standards set — it
depends on this codebase's `MainWindow::setupMenus` wiring and the
themed-stylesheet cascade.

Three invariants. A menu that breaks any of them is a bug, not a style
nit.

---

## M1 — A checkbox toggle keeps the menu open; a radio pick closes it

A **non-exclusive** checkable action (an independent on/off toggle —
Session Logging, Visual Bell, Background Blur, Auto-copy on Select, …)
MUST leave the menu open when clicked, so the user can flip several in
one visit. An **exclusive** checkable action (a "choose one" radio
member inside a `QActionGroup` with `setExclusive(true)` — Themes,
Opacity, Scrollback) keeps Qt's default "toggle and close" — picking
one option is a terminal choice.

**Why.** Qt's default closes the menu on *any* action trigger, so a
column of independent toggles forces a reopen per flip. That is a
usability bug for toggles but correct for radios.

**Mechanism — one shared event filter, installed in `setupMenus()`.**
`StayOpenOnToggleFilter` (anonymous namespace in `mainwindow.cpp`)
consumes the mouse-release over a checkable action, triggers it (toggle
+ `triggered()`), and returns `true` so the menu does not close. The
exclusive-group guard is what distinguishes a checkbox from a radio:

```cpp
if (a && a->isEnabled() && a->isCheckable()
    && !(a->actionGroup() && a->actionGroup()->isExclusive())) {
    a->trigger();
    return true;        // swallow the release — menu stays open
}
```

`setupMenus()` installs ONE filter instance across every menu and
submenu under the bar:

```cpp
auto *stayOpen = new StayOpenOnToggleFilter(this);
for (QMenu *m : m_menuBar->findChildren<QMenu *>())
    m->installEventFilter(stayOpen);
```

A new menu added under `m_menuBar` inherits the behavior with no extra
wiring, **provided it is built before** the install loop at the end of
`setupMenus()`. A menu built later must install the filter itself.
(ANTS-1982.)

## M2 — Menus are themed by the app stylesheet cascade, never per-menu

Menu colors come from the application stylesheet (`QMenu` / `QMenuBar`
/ `QMenu::item` / `QMenu::separator` rules in
`themedstylesheet::buildAppStylesheet`), which cascades to every menu
because the menus are descendants of the themed top-level window.

- **Never** call `setStyleSheet()` on an individual `QMenu` or
  `QAction` with literal colors — it breaks live theme switching and
  desyncs that menu from the palette. Structural-only local overrides
  (padding, icon size) are the sole exception.
- A new menu needs no theming code; it inherits the cascade.

## M3 — Menus and actions should carry a mnemonic

Top-level menus and their actions SHOULD declare an `&` accelerator
(`"&File"`, `"S&ettings"`, `"&Visual Bell"`) so the bar is
keyboard-navigable. Pick a letter unique within the parent menu to
avoid collisions. Known gaps include the Settings menu's `Next Bookmark`
and `Previous Bookmark` actions — add one when next touching that menu.

**Data-driven submenu *entries* are exempt.** The individual entries in
generated lists — the model picks (Opus / Sonnet / Haiku), thinking
levels, scrollback line-counts, and theme names — carry no `&`, by
design: `&`-collisions across a generated list are likely and arrow-key
navigation already covers them. Their parent submenu titles (`Switch
&Model`, `Thinking &Level`, `Scrollback &Lines`) DO keep mnemonics — M3
targets those static, hand-authored actions, not the generated children.

**Scope.** M1 and M3 cover the **menu bar and its submenus** (the
filter is installed across `m_menuBar->findChildren<QMenu *>()`, and
mnemonics are a menu-bar navigation aid). Per-tab right-click **context
menus** (e.g. the tab `Rename Tab…` menu) are out of scope for both —
they are not children of `m_menuBar`, so the M1 stay-open filter is not
installed on them, and they need no mnemonics.

---

## Checklist for a new menu / action

1. Build the menu inside (or before the end of) `setupMenus()` so it
   picks up the M1 stay-open filter; otherwise install
   `StayOpenOnToggleFilter` on it explicitly. (M1)
2. Put "choose one" options in an exclusive `QActionGroup`
   (`setExclusive(true)`) so they read as radios and close on pick;
   leave independent toggles group-free so they stay open. (M1)
3. No per-menu `setStyleSheet` with literal colors — rely on the
   cascade. (M2)
4. Give the menu and each action an `&` mnemonic, unique within its
   parent. (M3)
