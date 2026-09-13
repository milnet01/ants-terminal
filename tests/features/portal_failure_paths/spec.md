# Feature: the global-shortcuts portal reports every failure

## Invariants

**INV-1 — a BindShortcuts error reply fails the session.** The
`BindShortcuts` call is watched with a `QDBusPendingCallWatcher`; an error
reply drains the pending binds, clears the session handle, marks the portal
permanently failed and emits `sessionFailed`.

**INV-2 — an on-demand portal counts as available.**
`GlobalShortcutsPortal::isAvailable()` returns true when the portal service is
registered or listed in `activatableServiceNames()`.

**INV-3 — BindShortcuts is sent before sessionReady.** In
`onCreateSessionResponse`, `flushPending()` runs before `emit sessionReady()`.

## Rationale

`CreateSession`'s reply was watched, but `BindShortcuts` was sent with a bare
`asyncCall` and its reply discarded. An error reply therefore never fired
`sessionFailed`, and the "Global hotkey unavailable" message never appeared.
`isAvailable()` asked only whether the portal was registered, which is false
for a D-Bus activatable portal not yet started, so the portal was never built
and nothing retried while quake mode stayed on. `sessionReady` fired before
the pending binds were sent.

## Test surface

`test_portal_failure_paths.cpp` reads `src/globalshortcutsportal.cpp`, found
beside `SRC_MAINWINDOW_CPP_PATH`, and checks the text.

## Regression history

- **ANTS-5081:** the two defects above. Locked by this spec.
