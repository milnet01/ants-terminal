# Feature: GlobalShortcutsPortal closes its session on destruction

## Problem

`GlobalShortcutsPortal` requested a session via the
`org.freedesktop.portal.GlobalShortcuts.CreateSession` D-Bus
method, received a `session_handle` (an object path under
`/org/freedesktop/portal/desktop/session/...`), and used it
for the lifetime of the Ants process. Pre-fix, the class had
NO destructor, so the session handle was never released.

The `org.freedesktop.portal.Session` interface defines a
`Close()` method specifically for this purpose. Without it,
xdg-desktop-portal accumulates one orphan session per Ants
invocation that crashed or was SIGKILLed before destruction
could implicitly happen via D-Bus connection drop. Visible by
introspecting the running portal service:

```
busctl --user introspect org.freedesktop.portal.Desktop \
    /org/freedesktop/portal/desktop/session/<senderId>/<sessionId>
```

The leak is bounded (sessions don't hold compositor state
beyond active binds, which we tear down when the bus
connection drops anyway), but it's a documented contract to
release them and the right thing to do.

## Fix

Define `~GlobalShortcutsPortal()` that issues an asynchronous
`Close` D-Bus call against the session handle iff the
handshake completed (i.e. `m_sessionHandle` is non-empty).

The call is fire-and-forget via `QDBusConnection::asyncCall`:
we're in a destructor, can't wait for the reply, and the
portal processes Close asynchronously regardless. If we exit
before it completes, the kernel-level connection drop has the
same effect.

## Contract

### Invariant 1 — class declares a virtual destructor

`src/globalshortcutsportal.h` declares
`~GlobalShortcutsPortal() override;` so the destructor runs
even when the object is deleted via a base-class pointer (Qt
parent-tree ownership goes through `QObject *`).

### Invariant 2 — destructor calls Session.Close iff handshake completed

The destructor body in `src/globalshortcutsportal.cpp` checks
`m_sessionHandle.isEmpty()` first; on a non-empty handle, it
constructs a `QDBusMessage::createMethodCall` targeting:

- service:   `org.freedesktop.portal.Desktop`
- path:      `m_sessionHandle`
- interface: `org.freedesktop.portal.Session`
- member:    `Close`

and dispatches it via `m_bus.asyncCall(msg)`.

### Invariant 3 — kSessionIface constant exists

`src/globalshortcutsportal.cpp` defines the
`org.freedesktop.portal.Session` interface name as a constant
in the anonymous namespace — same shape as the existing
`kService`, `kPath`, `kIface`, `kReqIface` constants. Source
that uses string literals scattered across the file is harder
to keep in sync.

### Invariant 4 — early return on empty session handle

If `m_sessionHandle` is empty (handshake never completed —
e.g. the GNOME path where CreateSession succeeds but
BindShortcuts fails, or X11 / no portal service), the
destructor must return without dispatching Close. Calling
Session.Close on an empty path crashes Qt's D-Bus marshaller.

## How this test anchors to reality

Source-grep:

1. Header declares `~GlobalShortcutsPortal() override;`.
2. CPP defines `kSessionIface` constant equal to
   `"org.freedesktop.portal.Session"`.
3. CPP defines `GlobalShortcutsPortal::~GlobalShortcutsPortal()`
   with the empty-handle early return AND the asyncCall
   dispatch using kSessionIface + the `"Close"` member.

## Regression history

- **Latent:** since `GlobalShortcutsPortal` shipped (0.6.39).
- **Flagged:** ROADMAP "Portal session close."
- **Fixed:** 0.7.33.
