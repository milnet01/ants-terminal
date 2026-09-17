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

**INV-4 — CreateSession times out.** `createSession` starts
`m_createSessionTimer`. When it fires with the session still pending, the
portal detaches the Response slot, marks itself permanently failed and emits
`sessionFailed`. `onCreateSessionResponse` and the error-reply path stop the
timer.

**INV-5 — each BindShortcuts Response detaches its own request path.**
`onBindShortcutsResponse` takes the `QDBusMessage` and detaches
`message.path()`. No member holds a single BindShortcuts request path, so a
second flush cannot overwrite the first request's path.

## Rationale

`CreateSession`'s reply was watched, but `BindShortcuts` was sent with a bare
`asyncCall` and its reply discarded. An error reply therefore never fired
`sessionFailed`, and the "Global hotkey unavailable" message never appeared.
`isAvailable()` asked only whether the portal was registered, which is false
for a D-Bus activatable portal not yet started, so the portal was never built
and nothing retried while quake mode stayed on. `sessionReady` fired before
the pending binds were sent.

`CreateSession` had no timeout, so a portal that never sent its Response left
the session pending forever and queued every later bind silently.
`BindShortcuts` kept its request path in one member. A second flush while the
first was in flight overwrote it, so the first Response detached the second
request's slot, the second Response was lost, and the first slot stayed
connected.

## Test surface

`test_portal_failure_paths.cpp` reads `src/globalshortcutsportal.cpp`, found
beside `SRC_MAINWINDOW_CPP_PATH`, and checks the text.

## Regression history

- **ANTS-5081:** the defects above. Locked by this spec.
