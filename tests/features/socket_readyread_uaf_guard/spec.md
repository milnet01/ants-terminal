# ANTS-2102 — Nested-loop socket use-after-free regression lock

## Background

The Ants MCP server, the Claude hook server, and the Kitty
remote-control server each accept connections on a `QLocalServer` and
dispatch one request per connection from a `QLocalSocket::readyRead`
handler. Several MCP/remote verbs (`audit_run`, `indie_review_dispatch`,
any verb that pumps `QProcess` or `QNetworkAccessManager`) spin a
**local `QEventLoop`** to multiplex their child work.

Because the MCP/remote sockets live on the **main thread**, running such
a verb synchronously inside the `readyRead` handler pumps that nested
`QEventLoop` on the main thread. The nested loop then reentrantly
delivers the *same* socket's disconnect notification: the peer drops →
`disconnected` → `socket->deleteLater()`, and `deleteLater()` issued
inside a nested loop **is processed by that nested loop** (Qt loop-level
semantics). The socket is destroyed before `dispatch()` returns, and the
tail-of-handler `write()/flush()` then touches freed memory →
use-after-free SIGSEGV in Qt's socket engine.

This bit twice in production:

- **ANTS-2101 / ANTS-2026** — fixed the MCP and remote-control handlers
  with two guards each: stop the slow-loris idle timer *before*
  dispatch (so it can't fire `abort()` mid-loop) **and** wrap the socket
  in a `QPointer` so the post-dispatch write bails on a freed socket.
- **ANTS-2103 / ANTS-2104** — the deeper fix: run `audit_run` and
  `indie_review_dispatch` on a `QThread::create` worker so their
  `QEventLoop` never pumps on the main thread at all. `QThread::wait()`
  is a join — it does not pump events — so no foreign socket
  notification fires during the sweep.

Neither pair shipped a regression test. A true runtime test would need a
live `QLocalServer`, an external audit toolchain, and a *deterministic*
mid-dispatch peer disconnect — flaky and impractical. Following the
`qpointer_destroyed_safe` (ANTS-1320/1324) precedent for this crash
class, this is a **source-grep regression lock** instead: cheap,
non-flaky, and survives implementation rewrites of everything except the
guards themselves.

## Handler sweep (ANTS-2102 part 2)

Every server-side `QLocalSocket::readyRead` handler was swept; the three
that exist are all safe:

| Handler | File | Dispatches a nested loop? | Guard |
|---|---|---|---|
| MCP | `claudeintegration.cpp` (`[this, socket, idleTimer]`) | yes (`audit_run` et al.) | `idleTimer->stop()` + `QPointer<QLocalSocket>` (ANTS-2101) |
| remote-control | `remotecontrol.cpp` | yes (`audit_run` et al.) | `idleTimer->stop()` + `QPointer<QLocalSocket>` (ANTS-2026) |
| hook | `claudeintegration.cpp` (`[socket]`) | **no** — buffers only; dispatch (`processHookEvent`) runs in the `disconnected` handler, no nested loop | n/a (short-op) |

`QNetworkReply::readyRead` in `llmclient.cpp` connects to a short
`drain()` slot — it is a client reply, not a server accept loop, and
pumps nothing. Not in scope.

## Contract

- **INV-1** — the MCP `readyRead` handler in `claudeintegration.cpp`
  (the one capturing `idleTimer`) MUST call `idleTimer->stop()` and then
  guard the socket with `QPointer<QLocalSocket>` before the dispatch.
- **INV-2** — the remote-control `readyRead` handler in
  `remotecontrol.cpp` MUST do the same `idleTimer->stop()` +
  `QPointer<QLocalSocket>` pair before `dispatch()`.
- **INV-3** — `mainwindow.cpp` MUST run `AuditRunner::runAudit`
  (`audit_run`) inside a `QThread::create` worker, never synchronously
  on the dispatching thread.
- **INV-4** — `mainwindow.cpp` MUST run
  `cmdIndieReviewDispatch` (`indie_review_dispatch`) inside a
  `QThread::create` worker for the identical reason.

Removing any one guard re-opens the crash class and fails this test.

## Out of scope

- The structural end-state (every `QEventLoop`-pumping verb off the main
  thread) is **ANTS-2131**, tracked separately.
- A live-socket runtime reproduction — impractical (see Background).

## Test

`test_socket_readyread_uaf_guard.cpp` — pure source-grep against
`claudeintegration.cpp`, `remotecontrol.cpp`, and `mainwindow.cpp`
(paths supplied as compile definitions by the `test_claude` bundle). No
build-time runtime dependency.
