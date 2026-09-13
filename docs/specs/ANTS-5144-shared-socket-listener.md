# ANTS-5144 — One socket listener per path, shared by every window

**Status:** spec draft (2026-09-13).
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
    // window is on screen. Detaches automatically when `owner` is destroyed.
    void attach(const QString &path, QObject *owner,
                std::function<bool()> visible, std::function<void()> serve);

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

### 2.2 Binding without taking over

`acquire(path)`:

1. A server this hub already holds for `path` is returned as is. Nothing is
   removed or rebound.
2. Otherwise `listen(path)`. On success the server is kept and returned.
3. On failure, if `safeToUnlinkLocalSocket(path)` holds, the hub connects to
   `path` with a `QLocalSocket`, waiting at most 200 ms (the author's setting,
   not measured: the probe runs once per path at start-up, and a local socket
   that is listening accepts at once). A connection that
   succeeds means a live server holds the path: disconnect and return nullptr.
   A connection that fails means a stale file: `removeServer`, `listen` again,
   and return the server or nullptr.

The socket-directory and permission steps each class runs today (ANTS-1365
`ensureSocketDir`, `UserAccessOption`, `setOwnerOnlyPerms`) run once, inside
`acquire`, before step 2.

### 2.3 Choosing the target of a connection

MCP and remote-control connections each carry exactly one request (the
`_handled` latch in `ClaudeIntegration::onMcpConnection` and
`RemoteControl::onNewConnection`). On `newConnection` the hub picks one owner
for the path: the most recently activated owner whose `visible` returns true,
else the most recently activated owner. It calls that owner's `serve`, which
runs the existing per-connection code.

`MainWindow` calls `noteActivated` for its `ClaudeIntegration` and its
`RemoteControl` on `QEvent::WindowActivate`, in the branch of
`MainWindow::event` that already restarts the status timer. An owner attached
and never activated ranks by attach order, newest first.

Choosing the owner chooses the window. So tab verbs over the socket, and MCP
tool providers registered in `MainWindow::setupClaudeMcpProviders` (whose
lambdas capture their own window), serve the most recently active window.
That is the per-call resolution ANTS-5121 decided.

### 2.4 Hook events reach every window

A hook connection carries one event, parsed on disconnect. The hub serves it
through one owner, as § 2.3 does, but the parsed event goes to
`processHookEvent` on every `ClaudeIntegration` attached to the hook path.
Each already drops an event that is not for its own focused tab
(`isFocusedTabSession`), so delivering to all keeps today's per-window result.

### 2.5 Lifetime of a connection

An owner that takes a connection from the server becomes the socket's parent.
Destroying the owner therefore closes and deletes every socket it was serving;
a client sees a disconnect and no reply. ANTS-2132 § 2.6 already writes no reply
after a `ClaudeIntegration` begins shutdown, so this adds no second rule.

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
  name that returns its own label, with `visible` stubbed. Activate B, then A:
  a call returns A's label. Make A invisible: B's label. Breaks if the target is
  fixed when the server is bound.
- **INV-5** — A hook event reaches `processHookEvent` on every
  `ClaudeIntegration` attached to the hook path, once each. *Test:* two
  instances on one temporary hook path; send one event; each instance's
  `hookEventsProcessedForTest()` rises by one. Breaks if only the serving owner
  processes it: the other count stays put.
- **INV-6** — Destroying an owner closes the connections it was serving.
  *Test:* A serves a deferred MCP verb that never replies; destroy A while a
  client waits. The client sees a disconnect well inside the 5 s idle bound, and
  the `debug` (ASan) preset reports no leak. Breaks if the sockets stay parented
  to the hub's server: they stay open until the idle timer aborts them and are
  never deleted.
- **INV-7** — The accept-time checks stay ahead of reading any request, in each
  connection handler. *Test:* source scrape of `ClaudeIntegration::onMcpConnection`,
  `ClaudeIntegration::onHookConnection` and `RemoteControl::onNewConnection`:
  each contains `SO_PEERCRED` and `setInterval(5000)`, before its `readyRead`
  connection. Breaks if serving is moved into a path that skips them.
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
