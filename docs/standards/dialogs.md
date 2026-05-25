<!-- ants-dialogs-standards: 1 -->
# Ants Terminal dialog standard

Project-local convention for **every** `QDialog` in Ants Terminal —
Settings, Project Audit, the review-dialog family (Cold-eyes /
Test-audit / Independent-review), the AI Assistant, Roadmap viewer,
diff viewer, the Claude task/bg-task lists, SSH, and any new dialog.

Not part of the shareable `/start-app` standards set — it depends on
this codebase's `DialogChrome` + `TitleBar` + `Config` + `Themes`
architecture.

Four invariants. A dialog that breaks any of them is a bug, not a
style nit.

---

## D1 — Conform to the active terminal theme

Dialogs MUST take their colors from the user's chosen terminal theme,
never from the OS default palette or a hard-coded color set.

**Mechanism — use the shared chrome.** In the dialog constructor:

```cpp
auto chrome = DialogChrome::install(this);   // frameless + TitleBar + theme
m_titleBar = chrome.titleBar;                // keep for live re-theming (below)
// Lay out into chrome.contentArea, NOT into `this`.
auto *root = new QVBoxLayout(chrome.contentArea);
```

`DialogChrome::install` (`src/dialogchrome.{h,cpp}`):

- sets `Qt::FramelessWindowHint` + `WA_StyledBackground`,
- builds the custom `TitleBar` (close / minimize / maximize),
- applies the active theme via `DialogChrome::applyTheme`, which reads
  these `Theme` fields (`src/themes.h`): the dialog palette uses
  `bgPrimary` / `bgSecondary` / `textPrimary`, and the title bar uses
  `setThemeColors(bgSecondary, textPrimary, accent, border)` (signature
  has an optional 5th `danger` parameter, default `QColor::fromRgb(0xe74856)`,
  for a custom close-button danger color — pass it explicitly only when
  the dialog needs a non-default danger color),
- returns an `InstallResult{ titleBar, contentArea }` — lay the dialog's
  own widgets into `contentArea`.

Rules:

- **Never** hand-roll a title bar, call `setWindowFlags` to re-add the
  native frame, or `setPalette` with literal colors.
- The active theme name is process-global (a file-static in
  `dialogchrome.cpp`): `MainWindow` calls
  `DialogChrome::setActiveTheme()` on every theme change, so a dialog
  constructed *after* a theme switch picks up the new theme with no
  per-dialog wiring.
- Any in-dialog stylesheet (e.g. an ID-scoped `QPushButton` override)
  MUST read its colors from the `Theme` struct, not literals, so it
  tracks theme changes. The only literals permitted are structural
  (padding, radius, font-size).
- A dialog open while the theme changes SHOULD re-apply
  `DialogChrome::applyTheme(this, titleBar, newName)` live; at minimum
  it must not be left with stale OS-default colors.

## D2 — User-resizable

Dialogs MUST be resizable by the user. A dialog that the user cannot
make bigger to read a long path, a wide diff, or a tall finding list
is a bug.

- **Never** call `setFixedSize()` / `setFixedWidth()` /
  `setFixedHeight()` on the dialog itself, and do not set equal
  minimum and maximum sizes.
- Set a sensible **minimum** size (`setMinimumSize`) so the layout
  can't collapse below usable, and a sensible **default** size — not a
  fixed one.
- Because the chrome is frameless, the OS window border that normally
  drives edge-resize is absent. The dialog MUST therefore provide its
  own resize affordance. Preferred: a bottom-corner `QSizeGrip` parented
  on the content area, e.g.

  ```cpp
  auto *grip = new QSizeGrip(chrome.contentArea);
  // bottom-right of whatever bottom row the dialog already has:
  bottomRow->addWidget(grip, 0, Qt::AlignBottom | Qt::AlignRight);
  ```

  (or an edge-resize handler folded into `DialogChrome` / `TitleBar`).
  As of ANTS-1842 `DialogChrome::install(…, resizable=true)` adds this
  grip for you — see Project overrides. The maximize button alone does
  NOT satisfy D2: the user must be able to pick an arbitrary size.
- Inner scrollable regions (a `QTextBrowser`, a long form) belong in a
  `QScrollArea` / scroll host so growing the dialog reveals more
  content rather than just stretching whitespace.

## D3 — Geometry persists across sessions

The **size** the user picks MUST survive closing and reopening the
dialog, and survive an app restart.

- Persist a bare **`QSize`** (`size()`) to a per-dialog key in `Config`
  (`~/.config/ants-terminal/config.json`, mode 0600) — e.g. a new
  `auditDialogSize` key (no such key exists yet; add one per dialog).
  The one geometry key that exists today, `roadmapDialogGeometry`
  (`src/config.h`), is the D3/D4 counter-example — see Project
  overrides — because it stores a base64 `saveGeometry()` blob
  (position included), not a `QSize`. Do **NOT** use
  `QWidget::saveGeometry()` here: it serialises the window *position* as
  well as the size, which D4 explicitly forbids persisting.
- The write goes through a normal `Config` setter, which already
  acquires `ConfigWriteLock` (`src/configbackup.h`) and enforces mode
  0600 internally — do not hand-roll the file write or double-lock.
- Restore on construction / first show; save on close (`closeEvent` /
  `done` — the chrome's close button triggers `QDialog::reject()`,
  which routes through both). Saving in `resizeEvent` is optional and
  not required —
  there is no Qt "resize-finished" signal, and a close-time save
  captures the final size:

  ```cpp
  // restore (ctor / first show) — illustrative pseudocode;
  // auditDialogSize() does not exist, add a real accessor per dialog:
  const QSize sz = m_config->auditDialogSize();   // new per-dialog key
  if (sz.isValid()) resize(sz);
  // save (closeEvent / done):
  m_config->setAuditDialogSize(size());
  ```
- A dialog with no saved size yet falls back to its default size (D2).
- **Position is NOT persisted** — see D4. Persist size only; the
  dialog is re-centered on every open.

**Mechanism (ANTS-1842):** the bare-`QSize` persistence above is now
provided by `DialogChrome::install(…, resizable=true, sizeKey)` via the
generic `Config::dialogSize` / `setDialogSize` map — opt in with a key
rather than hand-rolling save/restore. **Known non-conformer:**
`roadmapDialogGeometry` still stores a base64 `saveGeometry()` blob
(position included) instead of a bare `QSize`; its migration onto the
`install` path is open follow-up.

## D4 — Always open centered on the terminal window

Every time a dialog opens it MUST appear centered over the **current**
`MainWindow` frame — even after the terminal has been moved to another
monitor or resized since the last open.

- On `showEvent` (or just before `exec()` / `show()`), compute the
  centered top-left from the parent's *current* frame, falling back to
  the screen under the cursor when there is no parent (rare — a dialog
  opened standalone). One chain, not two independent `move()` calls:

  ```cpp
  if (auto *p = parentWidget() ? parentWidget()->window() : nullptr) {
      move(p->frameGeometry().center() - rect().center());     // parent
  } else if (auto *s = QGuiApplication::screenAt(QCursor::pos())) {
      move(s->geometry().center() - rect().center());          // fallback
  }
  ```

  Use the parent's live `frameGeometry()` each open — do not cache it,
  or "even if it has been resized" breaks. (`screenAt` returns a
  `QScreen*`, hence the null-guarded `else if`.)
- This is why D3 persists size but NOT position: a remembered absolute
  position would drift off-screen when the terminal moves; re-centering
  each open keeps the dialog where the eye expects it.

---

## Checklist for a new dialog

1. `DialogChrome::install(this, themeName, /*resizable=*/true, "MyDialog")`;
   lay out into `contentArea`. The `resizable` flag gives you D2 (grip),
   D4 (re-center on open), and — with the `sizeKey` + the startup
   `setConfig` registration — D3 (size persistence) in one call. (D1–D4)
2. No `setFixedSize`; set `setMinimumSize` + a default size; wrap long
   content in a scroll host so growing the dialog reveals more. (D2)
3. Nothing to wire by hand for D3/D4 once you pass `resizable=true` +
   a `sizeKey` — `install` owns save/restore (bare `QSize`, never
   `saveGeometry()`) and re-centering.

## Project overrides

**ANTS-1842 — D2–D4 are now folded into `DialogChrome::install`.** Pass
`resizable=true` (and a `sizeKey`) and a single call satisfies D1–D4:

```cpp
auto chrome = DialogChrome::install(this, themeName,
                                    /*resizable=*/true,
                                    QStringLiteral("MyDialog"));
```

- **D2** — `install` adds a bottom-right `QSizeGrip`, kept pinned to the
  corner by an internal `ChromeGuard` event filter.
- **D4** — `ChromeGuard` re-centers over the parent window's *current*
  frame on every show (falls back to the cursor's screen when parentless).
- **D3** — when a `sizeKey` is given AND `DialogChrome::setConfig(Config*)`
  has been called (MainWindow does this once at startup, mirroring
  `setActiveTheme`), the bare `QSize` is restored on first show and saved
  on close under `Config::dialogSize(key)` / `setDialogSize(key, …)` — a
  single `dialog_sizes` map, so new dialogs need no per-key schema growth.

`resizable` defaults to `false`, so a plain `install(this)` /
`install(this, theme)` stays **D1-only** (unchanged). Opted-in today:
SettingsDialog, the review-dialog family (cold-eyes / test-audit /
independent-review, one shared `"ReviewDialog"` key), SshDialog, the About
box, and the diff viewer.

Remaining non-conformer (migration target, not an exemption):
`RoadmapDialog` still persists geometry with `QWidget::saveGeometry()` /
`restoreGeometry()` — which D3 forbids because it restores absolute
*position*. It is NOT opted into the `install` path; migrating it to
`resizable=true` + a `"RoadmapDialog"` key (dropping the base64 blob) is
open follow-up.
