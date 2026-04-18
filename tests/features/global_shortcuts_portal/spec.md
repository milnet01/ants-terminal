# Freedesktop Portal GlobalShortcuts — spec

Shipped in 0.6.39. This test pins the source-level contract for the
portal-based global hotkey path (the 0.6.38 Wayland-Quake follow-up).

## Invariants

1. `GlobalShortcutsPortal::isAvailable()` is the only gate on portal
   construction in MainWindow. Instantiating `new GlobalShortcutsPortal(this)`
   unconditionally would fail silently on systems where xdg-desktop-portal
   is not running (Flatpak-less minimal desktops, headless CI, some
   sandbox configurations) — the binding call would time out on the bus
   and leak a `QDBusPendingCallWatcher`.

2. The portal object uses the canonical service / path / interface
   triple exactly as defined by the upstream spec:
   - service:   `org.freedesktop.portal.Desktop`
   - path:      `/org/freedesktop/portal/desktop`
   - interface: `org.freedesktop.portal.GlobalShortcuts`
   - request:   `org.freedesktop.portal.Request`
   A typo in any of these is a silent-failure class that's painful to
   diagnose at runtime (the method call succeeds on the bus — it's the
   compositor that never implements the wrong name).

3. `GlobalShortcutsPortal::bindShortcut` is the only public registration
   entry point. MainWindow calls it with the id `"toggle-quake"`; the
   Activated signal handler matches on that exact id. An id mismatch is
   a silent failure (portal fires, MainWindow ignores).

4. The in-app `QShortcut` from 0.6.38 stays wired unconditionally —
   even when the portal is available. This is the defence-in-depth
   fallback for GNOME Shell (portal service present but no
   GlobalShortcuts backend yet), for revoked bindings, and for the
   first-run window before the user confirms the portal prompt.

5. Both activation paths (QShortcut lambda + portal lambda) debounce
   against the same `m_lastQuakeToggleMs` timestamp with a 500 ms
   window. Without debounce, focused-app double-fire would hide-then-
   show the window (visible flicker) on every key press.

6. `CMakeLists.txt` lists `src/globalshortcutsportal.cpp` in the
   ants-terminal sources and keeps the `Qt6::DBus` link. Losing either
   is an immediate runtime failure (portal class referenced by
   mainwindow.cpp → unresolved symbol or missing D-Bus headers).

7. The feature test target must be wired into ctest under the
   `features;fast` label, matching the pattern of every prior
   source-grep regression test (wayland_quake_mode,
   threaded_ptywrite_gating, shift_enter_bracketed_paste, …).

## Not covered (out of scope for this test)

- Live D-Bus round-trip verification. Would require a mock portal
  service on the test bus (QDBusServer spins one up, but it's 200+ lines
  of boilerplate for a feature we don't need at this confidence level).
  The portal backend itself (xdg-desktop-portal-kde) has its own tests;
  our contract is at the D-Bus message shape, which source inspection
  verifies.

- Trigger-format translation. `qtKeySequenceToPortalTrigger` is
  intentionally minimal (modifiers + a few punctuation → xkb keysyms);
  full keysym coverage is xkbcommon's problem, not ours. Regressions
  here are user-visible (portal prompt shows wrong default) but not
  silent.

- The 0.6.38 wayland_quake_mode invariants continue to apply — this
  test does not duplicate them.
