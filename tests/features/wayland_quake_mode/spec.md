# Wayland-native Quake mode — spec

Shipped in 0.6.38. This test pins the source-level contract that keeps
Quake mode working on both X11 and Wayland:

1. The `ANTS_WAYLAND_LAYER_SHELL` compile-time flag is the **only** gate on
   the `<LayerShellQt/Window>` include and on the layer-shell setup block.
   A bare `#include <LayerShellQt/Window>` or an un-guarded
   `LayerShellQt::Window::get(...)` would make the build fail on
   distros without `layer-shell-qt6-devel`.

2. `setupQuakeMode()` performs runtime platform dispatch:
   - Wayland branch with layer-shell guarded by `#ifdef ANTS_WAYLAND_LAYER_SHELL`.
   - X11 branch keeps the pre-0.6.38 behaviour — `WindowStaysOnTopHint`,
     `Qt::Tool`, and a client-side `move()`. This branch must live **outside**
     the Wayland conditional (silent conversion to always-on-top-via-compositor
     would strand X11 users).

3. `toggleQuakeVisibility()` degrades the slide animation on Wayland.
   `QPropertyAnimation` against `"pos"` is a no-op on Wayland (the
   compositor owns position) and would visibly snap at animation end; the
   Wayland branch is a plain show/hide toggle.

4. The `quake_hotkey` config key (added pre-0.6.38, dead until now) is
   wired to a `QShortcut` when Quake mode is active. The connect target
   is `MainWindow::toggleQuakeVisibility`.

5. `CMakeLists.txt` uses `find_package(LayerShellQt CONFIG QUIET)` — the
   *QUIET* flavour matters. Without it, building on a host without the
   devel package emits a warning that looks like a broken CMake every
   distro package-ship.

A future test (0.6.39) will pin the Freedesktop Portal GlobalShortcuts
path. For now, regression coverage is source-level.
