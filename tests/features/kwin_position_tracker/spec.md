# XcbPositionTracker → KWinPositionTracker rename + non-KWin bail + temp-file leak fix (ANTS-1045)

Indie-review-2026-04-27 finding (Tier 3 structural). The class
named `XcbPositionTracker` doesn't use XCB at all — it positions
windows by writing a small JavaScript file to /tmp and asking
KWin's scripting D-Bus interface to load it. The misleading name
is the headline; two other defects share the file:

1. **Misleading name** — readers (and grep) infer XCB. The class
   is KWin-specific (D-Bus + KWin scripting). On non-KWin
   compositors (GNOME/Mutter, Sway, Hyprland, anything Wayland-
   without-KWin), the dbus-send chain races silently — the
   service name `org.kde.KWin` is unreachable, the temp file
   gets written and not removed, the call-site spends 5 ms on a
   no-op.

2. **Temp-file leak on every failure branch.** The script path
   created by `QTemporaryFile` (with `setAutoRemove(false)`) is
   only removed inside the success-chain lambda
   (`unloadScript` → `QFile::remove`). If `dbus-send` exits
   non-zero, the file leaks. If the second `dbus-send` errors,
   the file leaks. If the QProcess never starts, the file
   leaks. /tmp accumulates `kwin_pos_ants_XXXXXX.js` files at a
   rate of ~10 per Quake-mode toggle on a non-KWin desktop.

3. **No KWin-presence guard.** The setPosition path runs every
   time, regardless of compositor. On non-KWin systems the work
   is wasted; with the temp-file leak it's harmful.

## Fix

(a) Rename class `XcbPositionTracker` → `KWinPositionTracker`,
    file `xcbpositiontracker.{h,cpp}` →
    `kwinpositiontracker.{h,cpp}`. Update all 6 use sites in
    `src/mainwindow.{h,cpp}` plus the CMakeLists.txt source line.

(b) Add an early `setPosition()` bail when KWin isn't the
    compositor. Detection: check for `KDE_FULL_SESSION=true` OR
    `XDG_CURRENT_DESKTOP` containing `KDE`. (XDG_SESSION_TYPE
    alone is insufficient — KWin runs on both X11 and Wayland.)
    The bail returns before any temp-file is written, so the
    leak class can't apply.

(c) Wrap the temp-file write + lambda chain in a
    `QScopeGuard`-style `auto cleanup = qScopeGuard([&]{ ... })`
    that calls `QFile::remove(scriptPath)`. Dismiss the guard
    inside the unloadScript lambda's success branch so the
    cleanup only fires on the no-success-path exit.

## Out of scope

- Switching off KWin scripting entirely (e.g. for a Wayland-
  native KDE alternative). The KWin-script path remains the
  only reliable way to position frameless windows on KDE
  Plasma — see ANTS-1058 / 0.7.5 quake-mode work.
- Reproducing the position-tracking logic on GNOME/Mutter or
  Sway. Compositor-specific implementations live in their own
  follow-ups; non-KWin systems get default Qt placement
  (which is "good enough" — frameless centring works, just
  not pixel-perfect restore).
- Detecting flatpak-host KWin (the dbus-send call needs
  `--bus=unix:path=…` in flatpak; out of scope for this
  rename + leak fix). **Note:** flatpak gets the temp-file
  leak fix for free via INV-5 — the dbus path still fails,
  but the QScopeGuard cleans up the file. The wasted ~5 ms
  of dbus-send work remains.

## Invariants

Source-grep + behavioural where the harness can reach.

- **INV-1** Class renamed: `KWinPositionTracker` declared in
  `src/kwinpositiontracker.h` (line 1 file-existence + class
  decl with the new name). `XcbPositionTracker` is **not**
  defined anywhere in `src/`. Asserted by source-grep across
  `src/*.{h,cpp}` for the absence of `XcbPositionTracker` (one
  exception: a brief block comment in
  `kwinpositiontracker.{h,cpp}` may reference the old name in
  the rename-history note, formatted as
  `// renamed from XcbPositionTracker (ANTS-1045)`; the
  source-grep exception is on that exact phrase only).

- **INV-2** File renamed: `src/kwinpositiontracker.h` and
  `src/kwinpositiontracker.cpp` exist; the old paths
  `src/xcbpositiontracker.{h,cpp}` do **not** exist.

- **INV-3** All use sites in `src/mainwindow.{h,cpp}` plus the
  `CMakeLists.txt` source line, plus any `*.cmake.in` /
  `*.qrc` / packaging / `.desktop` reference, use the new
  name. Asserted by source-grep across `src/`,
  `CMakeLists.txt`, and `packaging/` for the literal
  `XcbPositionTracker` matching in **zero** lines (modulo
  INV-1's rename-history-comment carve-out — only lines
  starting with `//` or ` * ` may mention the old name).

- **INV-4** KWin-presence guard. `setPosition()` returns early
  when neither `KDE_FULL_SESSION=true` nor
  `XDG_CURRENT_DESKTOP` contains `KDE`. Asserted by source-grep
  for the literal `KDE_FULL_SESSION` and `XDG_CURRENT_DESKTOP`
  references inside `setPosition()`'s body. Guard placement is
  **before** any temp-file write — verified by source-grep
  that the first `qgetenv` call appears before the first
  `QTemporaryFile` reference in the function.

- **INV-4b** Behavioural drive of the bail. With both
  `KDE_FULL_SESSION` and `XDG_CURRENT_DESKTOP` unset
  (`unsetenv` before constructing the tracker), invoking
  `setPosition(0, 0)` produces **no** new file matching the
  `kwin_pos_ants_*` glob in `QDir::tempPath()`, and spawns no
  `dbus-send` QProcess. Asserted by entry-list snapshot of
  the temp dir filtered by the glob, taken **after** tracker
  construction and **before** `setPosition()` is called, then
  re-snapshotted after — both must be empty (or
  byte-identical for any unrelated files). This is the
  substantive negation of INV-4 — guards INV-4 against being
  technically present in source but not actually bailing at
  runtime.

- **INV-5** `QScopeGuard` cleanup. The function references
  `qScopeGuard` (or `QScopeGuard`) from `<QScopeGuard>`. The
  guard's body calls `QFile::remove` on the script path. The
  guard is **dismissed** (`dismiss()`) inside the
  unloadScript-success branch so the cleanup only fires on the
  early-exit / failure path. Asserted by source-grep for
  `qScopeGuard` and `dismiss()` in the same function.

- **INV-5b** Scope-guard ordering. The `qScopeGuard` line
  appears **before** the `QTemporaryFile` open in source order
  inside `setPosition()`, so a future restructure that buries
  the guard inside an `if` branch trips this check. Asserted
  by source-grep ordering: line number of the first
  `qScopeGuard` < line number of the first `QTemporaryFile`
  in the function body. (Same trick INV-4 uses for the env-
  guard placement.)

- **INV-5c** Behavioural drive of the cleanup. On the
  forced-failure path (KWin guard passes by setting
  `KDE_FULL_SESSION=true`, but `dbus-send` is unavailable —
  simulate by globally overriding `PATH` to an empty dir via
  `setenv("PATH", "", 1)` for the duration of the test, so
  the QProcess the tracker spawns inherits the same broken
  PATH and fails at `start()`-time via `errorOccurred`), the
  temp file written by the tracker does NOT remain in the
  system temp dir after the failure-path lambda settles.
  Because `dbus-send` failure is delivered asynchronously
  via a QProcess signal, the test must spin a bounded event
  loop (e.g. `QTRY_VERIFY` with a 500 ms cap, or
  `QEventLoop::processEvents` until a `kwin_pos_ants_*` glob
  on the temp dir is empty) before asserting. Asserted by
  the post-settle `kwin_pos_ants_*` glob being empty.

- **INV-6** No regression of the 0.7.12 indie-review TOCTOU
  fix: `QTemporaryFile` is still used inside
  `kwinpositiontracker.cpp` specifically (grep narrowed to
  that file, not project-wide), AND no literal
  `"/tmp/kwin_pos_ants"` path string appears anywhere in
  `src/`. The `setAutoRemove(false)` call is preserved (the
  dbus chain still reads the file after `setPosition`
  returns).

## How to verify pre-fix code fails

```bash
# Pre-rename source has the old class name and the leaky path.
git checkout HEAD~1 -- src/xcbpositiontracker.cpp src/xcbpositiontracker.h \
                      src/mainwindow.cpp src/mainwindow.h CMakeLists.txt
cmake --build build --target test_kwin_position_tracker
ctest --test-dir build -R kwin_position_tracker
# Expect: INV-1, 2, 3 fail (class still named XcbPositionTracker);
# INV-4 fails (no KDE_FULL_SESSION guard); INV-5 fails (no
# qScopeGuard reference).
```
