# Feature: the global-shortcuts portal reports every failure

## Invariants

**INV-1 — a BindShortcuts error reply fails the session.** The
`BindShortcuts` call is watched with a `QDBusPendingCallWatcher`; an error
reply calls `failPermanently` (INV-6).

**INV-2 — an on-demand portal counts as available.**
`GlobalShortcutsPortal::isAvailable()` returns true when the portal service is
registered or listed in `activatableServiceNames()`.

**INV-3 — BindShortcuts is sent before sessionReady.** In
`onCreateSessionResponse`, `flushPending()` runs before `emit sessionReady()`.

**INV-4 — CreateSession times out.** `createSession` starts
`m_createSessionTimer`. When it fires with the session still pending, the
portal detaches the Response slot and calls `failPermanently` (INV-6). `onCreateSessionResponse` and the error-reply path stop the
timer.

**INV-5 — each BindShortcuts Response detaches its own request path.**
`onBindShortcutsResponse` takes the `QDBusMessage` and detaches
`message.path()`. No member holds a single BindShortcuts request path, so a
second flush cannot overwrite the first request's path.

**INV-6 — every failure cleans up one way.** Only
`GlobalShortcutsPortal::failPermanently` sets `m_permanentlyFailed`. It
drops the pending binds, sets the latch and emits `sessionFailed(reason)`.
No failure clears `m_sessionHandle`, so the destructor still closes a
session that was created (`portal_session_close`).

**INV-7 — BindShortcuts times out.** `flushPending` adds each request path
to `m_bindsInFlight` and starts a `kBindShortcutsTimeoutMs` single-shot
timer. The timer, the error reply and `onBindShortcutsResponse` each remove
the path; the timer and the error reply fail the bind (INV-6) only when the
path was still there, so a bind fails at most once.

**INV-8 — a late Response fails nothing.** `onBindShortcutsResponse` for a
path no longer in `m_bindsInFlight` returns without calling
`failPermanently`, so a rejected Response after the timer or the error reply
does not emit `sessionFailed` a second time.

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

The failure paths each cleaned up differently. Two cleared the session
handle, so the destructor skipped the `Session.Close` that
`portal_session_close` requires. The latch, not the cleared handle, is what
stops a later bind retrying.

## Test surface

`test_portal_failure_paths.cpp` reads `src/globalshortcutsportal.cpp`, found
beside `SRC_MAINWINDOW_CPP_PATH`, and checks the text. INV-8 builds a
`GlobalShortcutsPortal` and calls its Response slot directly.

## Regression history

- **ANTS-5081:** the defects above. Locked by this spec.
- **ANTS-6190:** the failure paths' differing clean-up. Locked by INV-6.
- **ANTS-6625:** `BindShortcuts` had no timeout, so a portal that never
  sent its Response left the shortcut unbound with no message. Locked by
  INV-7.
- **ANTS-6985:** a rejected `BindShortcuts` Response arriving after the
  timer or the error reply had failed the bind emitted `sessionFailed`
  twice. Locked by INV-8.
