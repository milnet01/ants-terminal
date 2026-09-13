# ANTS-5144 — One socket listener per path, shared by every window

**Status:** accepted (2026-09-13), review-contract loops 1 + 2 folded; cap
reached.
**Kind:** fix.
**Source:** ROADMAP.md ANTS-5144 (in-session-2026-09-13, ANTS-5121
investigation; listener ownership decided by the user 2026-09-13: one shared
listener for the whole process).
**Unblocks:** ANTS-5121.
**Composes with:** ANTS-2132 (MCP dispatch worker), ANTS-1901 (MCP master
toggle), ANTS-1897 (`ANTS_MCP_SOCKET` export), ANTS-1365 (socket directory),
ANTS-1151 and ANTS-1797 (accept-time checks).

## 1. Problem

Every `MainWindow` creates its own `ClaudeIntegration` and `RemoteControl`, and
each binds three Unix sockets:

- MCP, at `QDir::tempPath()` + `/ants-terminal-mcp-<pid>`
  (`MainWindow::setupClaudeMcpProviders` → `ClaudeIntegration::startMcpServer`).
- Claude hooks, at `QDir::tempPath()` + `/ants-claude-hooks-<pid>`
  (`ClaudeIntegration::startHookServer`).
- Remote control, at `RemoteControl::defaultSocketPath()`: per user, not per
  process (`$XDG_RUNTIME_DIR/ants-terminal.sock`).

`startMcpServer` and `startHookServer` call `QLocalServer::removeServer` before
`listen`. `RemoteControl::start` does the same once its first `listen` fails.
`safeToUnlinkLocalSocket` checks that the file is a socket owned by this user,
not that nobody is listening on it. So a later bind takes the path from a live
server.

Measured 2026-09-13 with a throwaway `QLocalServer` probe making the same calls
(recorded on ANTS-5144):

- a second server's `listen` over a live path fails with `AddressInUse`;
- after `removeServer` it binds, and clients reach only the second server;
- deleting the second server removes the socket file;
- afterwards no client connects, while the first server still reports
  `isListening`.

The consequence in the app: File > New Window creates a window with
`Qt::WA_DeleteOnClose`, and that window takes all three paths. Closing it
deletes its servers, which removes the socket files, so every Claude session in
the process loses MCP, hooks and remote control until Ants restarts. The first
window, built in `main`, only hides on close, so it never gives a path back.

Remote control has a second route to the same failure, across processes: a
second Ants process takes `ants-terminal.sock` from the first.

## 2. Surface

### 2.1 A process-wide hub, keyed by socket path

New `ants::LocalSocketHub` in `src/localsockethub.{h,cpp}`, in `ants_core_lib`
(below both `ClaudeIntegration` and `RemoteControl`). One instance per process,
parented to `QCoreApplication`, so no window owns it.

```cpp
namespace ants {
class LocalSocketHub : public QObject {
public:
    static LocalSocketHub &instance();

    // The server listening on `path`, binding it on first use (§ 2.2).
    // nullptr when a server outside this process holds the path, or when
    // listen fails.
    QLocalServer *acquire(const QString &path);

    // Register `owner` as a target for connections on `path`. `serve` pulls
    // and serves the pending connections; `visible` says whether the owner's
    // window is on screen. On the hook path, `ownsSession` says whether the
    // owner's window tracks a session and `onHookEvent` processes an event
    // (§ 2.4); elsewhere both stay empty. Detaches automatically when `owner`
    // is destroyed.
    void attach(const QString &path, QObject *owner,
                std::function<bool()> visible, std::function<void()> serve,
                std::function<bool(const QString &)> ownsSession = {},
                std::function<void(const QJsonObject &)> onHookEvent = {});

    // Run `onHookEvent` for exactly one owner on `path` (§ 2.4).
    void deliverHookEvent(const QString &path, const QJsonObject &event);

    // `owner`'s window became active (§ 2.3).
    void noteActivated(QObject *owner);

    // Every owner attached to `path`, in activation order, newest first.
    QList<QObject *> owners(const QString &path) const;
};
}
```

`ClaudeIntegration::startMcpServer(path)`, `ClaudeIntegration::startHookServer`
and `RemoteControl::start` acquire their server from the hub and attach
themselves instead of creating one. `m_mcpServer`, `m_hookServer` and
`m_server` become non-owning pointers to the hub's server. Each class's
`newConnection` connection moves to the hub, which chooses a target and calls
that owner's `serve`.

`startHookServer` gains a `const QString &path` parameter defaulting to the
current path, so a test can bind its own.

Both classes gain `setWindowVisibleProbe(std::function<bool()>)`. `MainWindow`
calls it with its own `isVisible`. The start functions pass `attach` a `visible`
that reads the stored probe each time it runs, so a probe set after the sockets
start still applies; with none set, `visible` returns true. A test sets a stub.

`stopMcpServer` and `stopHookServer` detach their owner from the path and clear
the pointer. They never close or delete the hub's server, which other owners
may still be serving. Destroying an owner detaches it the same way.

### 2.2 Binding without taking over

`acquire(path)`:

1. A server this hub already holds for `path` is returned as is. Nothing is
   removed or rebound.
2. Otherwise, when a file exists at `path`: return nullptr unless
   `safeToUnlinkLocalSocket(path)` holds. Then connect to `path` with a
   `QLocalSocket`, waiting at most 200 ms (the author's setting, not
   measured: the probe runs once per path at start-up, and a local socket
   that is listening accepts at once). A connection that succeeds means a
   live server holds the path: disconnect and return nullptr. A connection
   that fails means a stale file: `removeServer`.
3. `listen(path)`, and return the server or nullptr.

The check precedes `listen` because `listen` cannot report a held path here.
With `UserAccessOption` set, `QLocalServer::listen` binds in a private
directory and renames the socket over `path`, replacing a live server's
socket (traced with `strace` during this build, 2026-09-13).

`acquire` sets `UserAccessOption` before each `listen` and runs
`setOwnerOnlyPerms(path)` after each successful one, as the three start
functions do today. `ensureSocketDir` (ANTS-1365) stays in
`RemoteControl::start`, which runs it on its socket's directory before calling
`acquire`. `acquire` never runs it: it refuses any directory that is not mode
0700 and owned by the user, and the MCP and hook sockets sit directly in the
system temp directory.

### 2.3 Choosing the target of a connection

MCP and remote-control connections each carry exactly one request (the
`_handled` latch in `ClaudeIntegration::onMcpConnection` and
`RemoteControl::onNewConnection`). On `newConnection` the hub picks one owner
for the path: the most recently activated owner whose `visible` returns true,
else the most recently activated owner. It calls that owner's `serve`, which
runs the existing per-connection code with the reparenting § 2.5 adds.

`MainWindow` calls `noteActivated` for its `ClaudeIntegration` and its
`RemoteControl` on `QEvent::WindowActivate`, in the branch of
`MainWindow::event` that already restarts the status timer. `attach` counts
as an activation, so a newly attached owner ranks first until another owner is
activated.

Choosing the owner chooses the window. So tab verbs over the socket, and MCP
tool providers registered in `MainWindow::setupClaudeMcpProviders` (whose
lambdas capture their own window), serve the most recently active window.
That is the per-call resolution ANTS-5121 decided.

### 2.4 A hook event reaches one window

A hook connection carries one event, parsed on disconnect. The owner § 2.3
chose accepts the connection, and `onHookConnection` hands the parsed event to
`LocalSocketHub::deliverHookEvent(path, event)`. That calls `processHookEvent`
on exactly one `ClaudeIntegration`: the one whose window tracks the event's
`session_id`, else § 2.3's target. A tracker learns a session only when its
poll notices the new Claude child, so every `SessionStart`, and any event before
that poll, goes to § 2.3's target. Today's single hook server sends every event
to one window, so those events fare no worse.

A window answers through `ClaudeIntegration::setSessionOwnerProbe(std::function<bool(const QString &)>)`.
`MainWindow` sets it to ask its status-bar tracker's `shellForSessionId` for a
shell, the routing the `permissionRequested` slot already uses within a window.
`startHookServer` passes `attach` an `ownsSession` that reads the stored probe
each time it runs, as § 2.1 does for `visible`.

`ClaudeIntegration::hookEventsProcessedForTest()` counts calls into
`processHookEvent`, on entry, before any of its gates.

Delivering to every window would be wrong. `processHookEvent` does not gate a
`PermissionRequest` on `isFocusedTabSession`, and `isFocusedTabSession` accepts
any session while a window has no transcript path, so every window would show
the prompt and change its Claude state.

### 2.5 Lifetime of a connection

`nextPendingConnection` creates a socket as a child of the server, so each of
the three handlers reparents it to its owner (`socket->setParent(this)`) straight
after taking it. Destroying the owner therefore closes and deletes every socket
it was serving; a client sees a disconnect and no reply. ANTS-2132 § 2.6
already writes no reply after a `ClaudeIntegration` begins shutdown, so this
adds no second rule.

When no owner is attached for a path, the hub accepts and closes the
connection without a reply.

### 2.6 What stays as it is

- Every accept-time check, before any request is read: `SO_PEERCRED` same-UID,
  failing closed (ANTS-1797); the 5 s single-shot idle timer (ANTS-1151); the
  buffer caps (ANTS-1659). They stay in `onMcpConnection`, `onHookConnection` and
  `RemoteControl::onNewConnection`.
- The one `startMcpServer(` call in `mainwindow.cpp`, its `claudeMcpEnabled()`
  gate and the `qputenv("ANTS_MCP_SOCKET", …)` after it (ANTS-1901 INV-2,
  ANTS-1897 INV-14).
- The signature `void ClaudeIntegration::onMcpConnection()` (ANTS-2132 INV-3
  scrapes it).
- One dispatch worker per `ClaudeIntegration` (ANTS-2132). A request runs on
  the worker of the owner § 2.3 chose.
- The first window hiding rather than deleting on close (ANTS-5118).

### 2.7 Alternatives rejected

- **The first window keeps the listeners and hands them on when it closes.**
  Rejected by the user. Every close has to hand off correctly, and a missed
  case brings the dead socket back.
- **One `ClaudeIntegration` for the whole process.** It would also share the
  hook state, the token session and the dispatch worker, which ANTS-2132 and the
  per-window status bar keep per window. That is a larger change than the
  defect needs.

## 3. Invariants

- **INV-1** — A process binds each socket path at most once: a second
  `ClaudeIntegration` or `RemoteControl` starting on a path the process already
  serves binds nothing and removes no file. *Test:* two `ClaudeIntegration`s
  call `startMcpServer` on one temporary path. The socket file's inode is the
  same before and after the second call, and a client's `initialize` gets a
  reply. Breaks if the second call removes and rebinds: the inode changes.
- **INV-2** — The server on a path outlives any one owner: while one owner
  remains attached, destroying any other leaves the path accepting connections.
  *Test:* A then B start on one path. Destroy A, send `initialize`, get a
  reply; repeat with B destroyed and A kept. Breaks if the first or the newest
  owner holds the server: destroying it removes the socket file and the connect
  fails.
- **INV-3** — A path a live server accepts on is never unlinked. When the
  process does not serve the path and another server does, starting returns
  false and that server keeps accepting. A socket file nobody accepts on is
  replaced. *Test:* a plain `QLocalServer` listens on a temporary path;
  `RemoteControl::start` with `ANTS_REMOTE_SOCKET` set to it returns false, and
  a client still reaches the plain server. Then a socket file bound without a
  listener: `start` returns true and a client reaches `RemoteControl`. Breaks if
  the liveness probe is removed: `start` unlinks and orphans the plain server.
- **INV-4** — An MCP or remote-control connection is served by the most
  recently activated visible owner, else the most recently activated owner.
  *Test:* A and B attached to one MCP path, each registering a verb under one
  name that returns its own label, each with `setWindowVisibleProbe` stubbed.
  Call `noteActivated` for B, then A: a call returns A's label. Make A
  invisible: B's label. Attach a third instance C and never activate it: C's
  label. Breaks if the target is fixed when the server is bound, or if a newly
  attached owner ranks behind activated ones.
- **INV-5** — A hook event reaches `processHookEvent` on exactly one
  `ClaudeIntegration` attached to the hook path: the one whose session probe
  claims its `session_id`, else § 2.3's target. *Test:* two instances on one
  temporary hook path. B's `setSessionOwnerProbe` claims `S1`, A's claims
  nothing, and A was activated last. An event for `S1` raises B's
  `hookEventsProcessedForTest()` by one and leaves A's alone; an event for an
  unclaimed `S2` raises A's. Breaks if every instance processes the event, or
  if the probe is ignored: A's count rises for `S1` either way.
- **INV-6** — Destroying an owner closes and deletes the connections it was
  serving. *Test:* A serves a deferred MCP verb that never replies. Hold a
  `QPointer` to the `QLocalSocket` A took (`findChildren<QLocalSocket *>`), then
  destroy A while the client waits. The client sees a disconnect, and the
  `QPointer` reads null once events are processed. Breaks if the sockets stay
  parented to the hub's server: `onMcpConnection` stopped the idle timer before
  dispatch, so the client waits and the `QPointer` stays set.
- **INV-7** — The accept-time checks stay ahead of reading any request, in each
  connection handler. *Test:* source scrape of `ClaudeIntegration::onMcpConnection`,
  `ClaudeIntegration::onHookConnection` and `RemoteControl::onNewConnection`:
  each contains `SO_PEERCRED` and `setInterval(5000)`, before its `readyRead`
  connection. It also scrapes the `attach` calls in `claudeintegration.cpp` and
  `remotecontrol.cpp`, which pass those three handlers as `serve`, and
  `src/localsockethub.cpp`, where `nextPendingConnection` appears only in the
  no-owner close path. Breaks if connections are pulled and read in the hub, or
  through a `serve` other than those handlers: the checks are then skipped while
  the handler bodies still scrape clean.
- **INV-8** — The MCP start-up call site and export in `mainwindow.cpp` are
  unchanged. *Test:* `McpMasterToggle.INV2_StartupGate` and
  `McpOrientation_Inv14.MainWindowExportsSocket` pass unmodified.

## 4. RAM / build cost

One hub per process: a map from path to a server and a short owner list. Three
paths in the app. No new build target; `localsockethub.cpp` joins
`ants_core_lib`. The feature test joins an existing bundle.

## 5. Out of scope

- Which window a *new tab* opens in over the socket — § 2.3 decides the window,
  and each verb's own contract decides the rest.
- A second Ants process serving remote control. It now finds the path held and
  runs without remote control, where today it takes the path away from the
  first process. `ANTS_REMOTE_SOCKET` still gives a second process a path of its
  own. No id: this is the intended behaviour the `RemoteControl::start` comment
  already described.
- Moving the accept-time checks into the hub — kept per class so the scrapes
  that lock them keep their subject.

## 6. Tests

Feature test: `tests/features/shared_socket_listener/`, in the `test_claude`
bundle beside `tests/features/mcp_async_dispatch/`. Covers INV-1, INV-2,
INV-3, INV-4, INV-5, INV-6 and INV-7
with bare `ClaudeIntegration` and `RemoteControl` instances on temporary paths,
as `tests/features/mcp_async_dispatch/` does. INV-8 is the two existing tests.
Label `features;fast`. Verify each test fails against pre-fix source first.

## 7. Cross-doc impact

- `MainWindow::closeEvent`'s ANTS-5118 comment says the first window "owns the
  remote-control listener"; reword it.
- `RemoteControl::start`'s comment that a live instance makes the takeover fail
  becomes true; keep it and point it at § 2.2.
- ANTS-5121: its defect is closed by § 2.3. Annotate it when this ships.
- CHANGELOG: a `Fixed` entry.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-13 | 3 cold `review-lane` | 2 | 0 | 2 | 2 | Verified 6, fixed 6, dismissed 1. Fixed: `acquire` no longer runs `ensureSocketDir` and runs `setOwnerOnlyPerms` after `listen` (§ 2.2); a hook event reaches the one window that owns its session, with `ownsSession`, `onHookEvent` and `deliverHookEvent` in the hub API (§ 2.1, § 2.4, INV-5); `setWindowVisibleProbe` named as the `visible` seam (§ 2.1, INV-4); stop functions detach and never delete the shared server (§ 2.1); INV-7 also scrapes the `attach` calls and the hub; INV-6 asserts the socket object is gone rather than a leak report. Dismissed: token accounting follows the serving window (true, changes nothing built). Four open questions resolved with no finding. |
| 2 | 2026-09-13 | 3 cold `review-lane` | 2 | 0 | 3 | 0 | Verified 5, fixed 5, dismissed 2. Fixed: each handler reparents its socket to its owner after `nextPendingConnection` (§ 2.5, § 2.3); an unpolled session, including every `SessionStart`, goes to § 2.3's target, and the § 2.4 heading says one window; the start functions pass probes that read the stored value when called (§ 2.1, § 2.4); `hookEventsProcessedForTest()` counts on entry, before any gate (§ 2.4); `attach` counts as an activation, with a never-activated owner added to INV-4 (§ 2.3). Dismissed: a never-replying deferred verb hanging the destructor join (unverified: the deferred branch runs on the GUI thread, not the worker); § 2.4's rationale predating ANTS-2190 (the conclusion and the build are unchanged). Two open questions resolved with no finding. Cap reached (2 for a spec): calm, 3 of this loop's 5 findings landed on text loop 1 wrote (the § 2.1 probe paragraph, § 2.4, INV-5) and 2 on the original draft (§ 2.3, § 2.5); 5 of 5 findings in the gated span, the whole new file. Tail filed: none. |
