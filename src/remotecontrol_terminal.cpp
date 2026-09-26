// ANTS-3833 TU 2/19 — The --remote socket, dispatch(), and the terminal and
// window verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "projectsettings.h"   // ANTS-3771 — the declared id format
#include "findsources.h"
#include "mainwindow.h"
#include "pathvalidation.h"
#include "scrollbackerrors.h"
#include "terminalwidget.h"
#include "claudeintegration.h"
#include "tokenusageengine.h"
#include "guithread.h"
#include "debuglog.h"
#include "localsockethub.h"   // ANTS-4932 — the --remote socket moved here
#include "roadmapparse.h"
#include "secureio.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

using namespace rcdetail;  // ANTS-3833

// ANTS-4932 § 2.2 — the --remote socket and dispatch(), moved here from TU 1:
// dispatch() routes to this TU's terminal verbs, so it cannot join the
// window-free ants_mcpcore_lib that TU 1 now compiles into.

QString RemoteControl::defaultSocketPath() {
    // Override wins unconditionally — lets the user script
    // multi-instance setups without touching the source.
    const QByteArray override = qgetenv("ANTS_REMOTE_SOCKET");
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);

    const QString xdg = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (!xdg.isEmpty()) {
        return xdg + "/ants-terminal.sock";
    }
    // ANTS-1365 — /tmp fallback wraps the socket in a per-user 0700
    // subdir (`/tmp/ants-<uid>/`) so a same-UID rogue can't pre-create
    // the socket path as a regular file or symlink. The subdir is
    // brought up by `ensureSocketDir` in `start()` before listen().
    return QStringLiteral("/tmp/ants-%1/ants-terminal.sock")
        .arg(::getuid());
}

bool RemoteControl::start() {
    if (m_server) return true;

    const QString path = defaultSocketPath();
    // ANTS-1365 — bring up the socket-containing directory at 0700,
    // verified to be owned by us, before listen(). Replaces the
    // previous `QDir::mkpath` (which always creates with 0755 on
    // POSIX and offers no ownership/mode verification). On any
    // failure — wrong owner, wrong mode, inherited symlink, mkdir
    // failure — return false and disable rc/MCP for this process.
    // The XDG primary path is already a systemd-managed 0700 dir,
    // so this is a no-op there; the /tmp fallback is the real
    // beneficiary.
    const QString socketDir = QFileInfo(path).absolutePath();
    if (!ensureSocketDir(socketDir)) {
        ANTS_LOG(DebugLog::Network,
            "remote-control: socket dir %s unavailable; "
            "remote-control disabled for this process",
            qUtf8Printable(socketDir));
        return false;
    }

    // ANTS-5144 § 2.2 — every window shares one server per path. A stale
    // socket file is replaced; a path a live server holds (another Ants
    // process) is never taken over, and remote control stays off here. The
    // hub keeps ANTS-1132's UserAccessOption and safe-unlink guards.
    m_server = ants::LocalSocketHub::instance().acquire(path);
    if (!m_server) {
        ANTS_LOG(DebugLog::Network,
            "remote-control: cannot listen on %s — another instance owns "
            "it, or it is not a socket owned by this user; remote-control "
            "disabled for this process", qUtf8Printable(path));
        return false;
    }
    m_socketPath = path;
    ants::LocalSocketHub::instance().attach(
        path, this,
        [this] { return !m_windowVisibleProbe || m_windowVisibleProbe(); },
        [this] { onNewConnection(); });
    ANTS_LOG(DebugLog::Network,
        "remote-control: listening on %s", qUtf8Printable(path));
    return true;
}

// ANTS-5093 — see the declaration.
void RemoteControl::armReplyDrainGuard(QLocalSocket *socket, int idleMs) {
    if (!socket || socket->state() == QLocalSocket::UnconnectedState) return;
    auto *drain = new QTimer(socket);
    drain->setSingleShot(true);
    drain->setInterval(idleMs);
    connect(drain, &QTimer::timeout, socket, [socket]() { socket->abort(); });
    connect(socket, &QIODevice::bytesWritten, drain,
            [drain](qint64) { drain->start(); });
    drain->start();
}

void RemoteControl::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        // ANTS-5144 § 2.5 — take the socket from the shared server, so
        // destroying this window's RemoteControl closes what it was serving.
        socket->setParent(this);
        // ANTS-1132 — SO_PEERCRED UID match. The trust-model comment
        // at the top of this file claims "UID-scoped + 0700 perms +
        // lstat-checked S_ISSOCK"; UserAccessOption + safeToUnlink
        // already cover the file-side guarantees, but the peer side
        // needs explicit getsockopt(SO_PEERCRED) to enforce that the
        // connecting process is the same UID. Defense in depth — on
        // Linux with 0700 socket perms, the kernel already gates
        // connect(2) on the file ACL, but if the socket path is
        // ever moved (ANTS_REMOTE_SOCKET env override, abstract
        // socket migration), the file ACL stops applying and only
        // the peer-cred check holds the line.
        // ANTS-1797 — fail CLOSED: if the socket fd is unavailable we cannot
        // verify the peer UID, so the connection must be refused rather than
        // served unauthenticated. (A bare `if (fd >= 0)` guard would skip the
        // whole check on fd<0 — exactly the moved-socket scenario the comment
        // above names as the case where only peer-cred holds the line.)
        const qintptr fd = socket->socketDescriptor();
        bool peerVerified = false;
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            const int gscRet = ::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                                            SO_PEERCRED, &cred, &len);
            if (gscRet == 0 && len == sizeof(cred) && cred.uid == ::getuid()) {
                peerVerified = true;
            } else {
                // getsockopt failed OR truncated struct OR UID mismatch.
                // Log strerror on the syscall-failure path so a zero-init
                // cred.uid isn't reported as a fake "root tried to connect".
                if (gscRet != 0 || len != sizeof(cred))
                    ANTS_LOG(DebugLog::Network,
                        "remote-control: SO_PEERCRED failed (%s) — disconnecting",
                        std::strerror(errno));
                else
                    ANTS_LOG(DebugLog::Network,
                        "remote-control: peer UID mismatch "
                        "(peer=%d self=%d) — disconnecting",
                        static_cast<int>(cred.uid),
                        static_cast<int>(::getuid()));
            }
        } else {
            ANTS_LOG(DebugLog::Network,
                "remote-control: no socket fd for peer-cred check — "
                "disconnecting (fail-closed)");
        }
        if (!peerVerified) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        // ANTS-5093 — bound how many connections this window holds open.
        if (!ants::admitLiveConnection(this, socket, m_liveConnections)) continue;
        // ANTS-1132 — slow-loris defence. Cap idle time per
        // connection at 5 seconds. Each message is one-shot; if
        // a peer hasn't sent a complete request within the
        // window, abort.
        QTimer *idleTimer = new QTimer(socket);
        idleTimer->setSingleShot(true);
        idleTimer->setInterval(5000);
        connect(idleTimer, &QTimer::timeout, socket,
                [socket]() { socket->abort(); });
        idleTimer->start();
        // Line-buffer incoming data. Each connection handles exactly
        // one request/response round-trip today — simpler than a
        // persistent-session protocol and good enough for the full
        // Kitty command set (which is also one-shot).
        socket->setProperty("_buf", QByteArray());
        // ANTS-2202 — re-entrancy latch, mirroring the MCP twin
        // (claudeintegration.cpp). Once a complete line is dispatched, _handled
        // blocks a second readyRead (e.g. fired from inside a nested event loop)
        // from re-dispatching buffered bytes.
        socket->setProperty("_handled", false);
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket, idleTimer]() {
            if (socket->property("_handled").toBool()) return;
            QByteArray buf = socket->property("_buf").toByteArray();
            buf += socket->readAll();
            // Bound the in-memory buffer for defence-in-depth against
            // a malicious client on the same machine. 1 MB is far
            // more than any realistic Kitty rc_protocol envelope.
            if (buf.size() > 1 * 1024 * 1024) {
                socket->disconnectFromServer();
                return;
            }
            socket->setProperty("_buf", buf);

            int nlIdx = buf.indexOf('\n');
            if (nlIdx < 0) return;  // partial line, wait for more

            // ANTS-2202 — a complete line is in hand. Latch _handled and drop the
            // consumed line from _buf so a re-entrant readyRead (should a future
            // RC verb pump a nested event loop) can't re-dispatch it.
            socket->setProperty("_handled", true);
            socket->setProperty("_buf", buf.mid(nlIdx + 1));

            // ANTS-2026 — stop the slow-loris idle timer BEFORE dispatching. No
            // current RC verb runs a nested event loop, but if one is added a
            // still-armed timer could fire timeout -> socket->abort() ->
            // disconnected -> deleteLater(), and that deleteLater would be
            // processed by the nested loop, freeing this socket before the write
            // below. Defensive, and parity with the MCP path (ANTS-2101).
            idleTimer->stop();

            const QByteArray line = buf.left(nlIdx);
            QJsonParseError err;
            QJsonDocument req = QJsonDocument::fromJson(line, &err);
            QJsonDocument resp;
            // ANTS-2026 — defence in depth: the peer can still disconnect during
            // a nested-loop dispatch, freeing the socket via the disconnected ->
            // deleteLater chain. A QPointer lets the post-dispatch write bail
            // instead of touching a dangling pointer.
            QPointer<QLocalSocket> guard(socket);
            // ANTS-2132 § 2.7 — the reply write, shared by the inline path and
            // a worker route's deferred one. Both run it on the GUI thread.
            const auto writeReply = [](const QPointer<QLocalSocket> &sock,
                                       const QJsonDocument &doc) {
                if (!sock || sock->state() != QLocalSocket::ConnectedState)
                    return;
                sock->write(doc.toJson(QJsonDocument::Compact) + '\n');
                sock->flush();
                sock->disconnectFromServer();
                // ANTS-5093 — a peer that never reads the reply must not pin
                // the socket and its write buffer.
                RemoteControl::armReplyDrainGuard(sock.data(), kReplyDrainIdleMs);
            };
            if (err.error != QJsonParseError::NoError || !req.isObject()) {
                QJsonObject e;
                e["ok"] = false;
                e["error"] = QStringLiteral("invalid JSON: %1")
                    .arg(err.errorString());
                resp = QJsonDocument(e);
            } else {
                const QJsonObject reqObj = req.object();
                const QString cmd = reqObj.value(QStringLiteral("cmd")).toString();
                if (m_dispatchWorkerPoster && routeRunsOnDispatchWorker(cmd)) {
                    // dispatch() runs on the worker and the write is queued
                    // back here. The job carries `guard` but never tests it: a
                    // QPointer may only be dereferenced on the socket's thread.
                    const bool posted = m_dispatchWorkerPoster(
                        [this, guard, reqObj, writeReply]() {
                            const QJsonDocument out = dispatch(reqObj);
                            // ANTS-5087 — the worker's parse memo is dead the
                            // moment this call returns, and it retains the last
                            // document parsed here: text and records both. Held
                            // ACROSS the dispatch, so a call that parses the
                            // same roadmap twice still hits it, and dropped
                            // between calls, which is the retention the finding
                            // is about. The GUI thread never reaches this and
                            // keeps its memo, which is what it is for.
                            RoadmapParse::releaseParseMemo();
                            QMetaObject::invokeMethod(this,
                                [guard, out, writeReply]() {
                                    writeReply(guard, out);
                                }, Qt::QueuedConnection);
                        });
                    if (posted) return;  // the reply follows from the worker
                    QJsonObject e;
                    e["ok"]             = false;
                    e["code"]           = QStringLiteral("dispatch_queue_full");
                    e["error"]          = QStringLiteral(
                        "%1: too many MCP calls are already in flight; retry "
                        "shortly").arg(cmd);
                    e["retry_after_ms"] = 250;
                    resp = QJsonDocument(e);
                } else {
                    resp = dispatch(reqObj);
                }
            }
            writeReply(guard, resp);
        });
        connect(socket, &QLocalSocket::disconnected,
                socket, &QLocalSocket::deleteLater);
    }
}

bool RemoteControl::routeRunsOnDispatchWorker(const QString &cmd) {
    static const QSet<QString> kWorkerRoutes = {
        QStringLiteral("roadmap-query"),
        QStringLiteral("workspace-search"),
        QStringLiteral("file-outline"),
        QStringLiteral("find-definition"),
        QStringLiteral("find-caller"),
        QStringLiteral("similar-code"),
        QStringLiteral("git-state"),
        QStringLiteral("subsystem"),
    };
    return kWorkerRoutes.contains(cmd);
}

QJsonDocument RemoteControl::dispatch(const QJsonObject &req) {
    const QString cmd = req.value("cmd").toString();
    // ANTS-1176: per-verb structured log so a same-UID-attack
    // post-mortem has a record. Deliberately does NOT include the
    // payload itself (text/cwd/command bodies can carry secrets);
    // size + tab + stripped-bytes count are the diagnostic axes.
    const int tabId = req.value("tab").toInt(-1);
    const int textBytes = req.value("text").toString().size();
    // ANTS-2119 M2 — record whether the control-char filter bypass was
    // requested (send-text / launch / new-tab honour raw:true). Without it a
    // post-mortem of a same-UID attack can't distinguish a benign filtered send
    // from a raw control-byte injection — the exact threat this log exists for.
    const int rawBypass = req.value("raw").toBool(false) ? 1 : 0;
    // ANTS-5093 — `cmd` is the peer's; DebugLog::write escapes every
    // message, so a newline in it cannot start a forged line (CWE-117).
    ANTS_LOG(DebugLog::Network,
             "rc dispatch cmd=%s tab=%d text_bytes=%d raw=%d",
             qUtf8Printable(cmd), tabId, textBytes,
             rawBypass);
    if (cmd == QLatin1String("ls")) {
        return cmdLs();
    }
    if (cmd == QLatin1String("send-text")) {
        return cmdSendText(req);
    }
    if (cmd == QLatin1String("new-tab")) {
        return cmdNewTab(req);
    }
    if (cmd == QLatin1String("select-window")) {
        return cmdSelectWindow(req);
    }
    if (cmd == QLatin1String("set-title")) {
        return cmdSetTitle(req);
    }
    if (cmd == QLatin1String("get-text")) {
        return cmdGetText(req);
    }
    if (cmd == QLatin1String("launch")) {
        return cmdLaunch(req);
    }
    if (cmd == QLatin1String("tab-list")) {
        return cmdTabList();
    }
    // ANTS-2049 — e2e inject verbs (socket-only). Gated behind m_e2eMode: on a
    // normal binary (no --e2e) the gate is false and every inject verb refuses
    // with code:"e2e_disabled" and posts no event / does no resize/grab
    // (INV-1). The verbs carry new argument shapes reached via --remote-json.
    if (cmd == QLatin1String("inject-key")
            || cmd == QLatin1String("inject-click")
            || cmd == QLatin1String("resize-window")
            || cmd == QLatin1String("grab-image")) {
        if (!m_e2eMode) {
            QJsonObject o;
            o["ok"]    = false;
            o["code"]  = QStringLiteral("e2e_disabled");
            o["error"] = cmd + QStringLiteral(
                ": refused — instance not launched with --e2e");
            return QJsonDocument(o);
        }
        if (cmd == QLatin1String("inject-key"))     return cmdInjectKey(req);
        if (cmd == QLatin1String("inject-click"))   return cmdInjectClick(req);
        if (cmd == QLatin1String("resize-window"))  return cmdResizeWindow(req);
        return cmdGrabImage(req);
    }
    if (cmd == QLatin1String("roadmap-query")) {
        // ANTS-1247: thread `req` through so `--remote roadmap-query
        // status=active` (if a future --remote-status flag lands)
        // reaches the filter.
        return cmdRoadmapQuery(req);
    }
    if (cmd == QLatin1String("workspace-search")) {
        // ANTS-1248-INV-4: IPC dispatch entry for the ripgrep wrapper.
        return cmdWorkspaceSearch(req);
    }
    if (cmd == QLatin1String("file-outline")) {
        // ANTS-1249: IPC dispatch entry for the file outline scanner.
        return cmdFileOutline(req);
    }
    if (cmd == QLatin1String("find-definition")) {
        // ANTS-1303: IPC dispatch entry for the symbol-definition scanner.
        return cmdFindDefinition(req);
    }
    if (cmd == QLatin1String("find-caller")) {
        // ANTS-1303: IPC dispatch entry for the symbol-caller scanner.
        return cmdFindCaller(req);
    }
    if (cmd == QLatin1String("similar-code")) {
        // ANTS-1305: IPC dispatch entry for the shape matcher.
        return cmdSimilarCode(req);
    }
    if (cmd == QLatin1String("git-state")) {
        // ANTS-1250: IPC dispatch entry for the consolidated git tool.
        // Inner op-switch lives in cmdGitState.
        return cmdGitState(req);
    }
    if (cmd == QLatin1String("subsystem")) {
        // ANTS-1251: IPC dispatch entry for the consolidated subsystem
        // tool. Inner op-switch lives in cmdSubsystem.
        return cmdSubsystem(req);
    }
    QJsonObject e;
    e["ok"] = false;
    e["error"] = QStringLiteral("unknown command: %1").arg(cmd);
    return QJsonDocument(e);
}

QJsonDocument RemoteControl::cmdLs() {
    QJsonObject out;
    out["ok"] = true;
    out["tabs"] = m_main->tabListForRemote();
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdSendText(const QJsonObject &req) {
    // Request shape: {"cmd":"send-text","tab":<int>,"text":"<string>",
    //                 "raw":<bool optional>}
    //   - `tab` optional (default: the active tab)
    //   - `text` required; UTF-8 written to the tab's PTY. By default
    //     dangerous C0 control bytes (0x00-0x08, 0x0B-0x1F, 0x7F) are
    //     stripped to prevent local-UID processes from injecting ESC
    //     sequences / bracketed-paste toggles / OSC 52 clipboard
    //     overwrites through the rc socket. See
    //     tests/features/remote_control_opt_in/spec.md.
    //   - `raw`  optional; when `true`, the filter is skipped and
    //     bytes pass through verbatim. Preserves Kitty-compat for
    //     callers that genuinely need raw byte access (terminal test
    //     harnesses, escape-sequence driven plugins).
    QJsonObject out;
    const QJsonValue textVal = req.value("text");
    if (!textVal.isString()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "send-text: missing or non-string \"text\" field");
        return QJsonDocument(out);
    }
    const QString text = textVal.toString();
    // `tab` arrives as a JSON number. toInt() returns 0 for a missing
    // or non-number value, which would silently target tab 0 — use
    // the `isDouble()` check to distinguish "not specified" from
    // "specified as 0" so `--remote-tab 0` stays meaningful.
    const QJsonValue tabVal = req.value("tab");
    TerminalWidget *target = nullptr;
    if (tabVal.isDouble()) {
        const int idx = tabVal.toInt();
        target = m_main->terminalAtTab(idx);
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "send-text: no tab at index %1").arg(idx);
            return QJsonDocument(out);
        }
    } else {
        target = m_main->currentTerminal();
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "send-text: no active terminal");
            return QJsonDocument(out);
        }
    }
    const bool rawBypass = req.value("raw").toBool(false);
    const QByteArray rawBytes = text.toUtf8();
    int stripped = 0;
    const QByteArray payload = rawBypass
        ? rawBytes
        : RemoteControl::filterControlChars(rawBytes, &stripped);
    target->sendToPty(payload);
    out["ok"] = true;
    out["bytes"] = payload.size();
    if (!rawBypass && stripped > 0) {
        out["stripped"] = stripped;
    }
    return QJsonDocument(out);
}

// filterControlChars is defined inline in remotecontrol.h so feature
// tests can exercise it without pulling the full MainWindow dep chain.

QJsonDocument RemoteControl::cmdSelectWindow(const QJsonObject &req) {
    // Request shape: {"cmd":"select-window","tab":<int>}
    //   - `tab` required. Kitty's rc_protocol uses `--match id:N`;
    //     we use 0-based tab index to stay consistent with the
    //     other ants rc commands and with the `ls` response shape.
    //   - No match → error envelope with out-of-range message; the
    //     tab strip is unchanged.
    QJsonObject out;
    const QJsonValue tabVal = req.value("tab");
    if (!tabVal.isDouble()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "select-window: missing or non-integer \"tab\" field");
        return QJsonDocument(out);
    }
    const int idx = tabVal.toInt();
    if (!m_main->selectTabForRemote(idx)) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "select-window: no tab at index %1").arg(idx);
        return QJsonDocument(out);
    }
    out["ok"] = true;
    out["index"] = idx;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdGetText(const QJsonObject &req) {
    // Request shape: {"cmd":"get-text","tab":<int optional>,"lines":<int optional>}
    //   - `tab`   optional; default = active tab. isDouble() guard
    //     (consistent with send-text / set-title).
    //   - `lines` optional; default 100. Number of trailing lines from
    //     scrollback + screen, joined with `\n`. Negative or zero
    //     falls back to the default (matches the existing
    //     TerminalWidget::recentOutput contract). Capped at 10 000
    //     here so a script that writes `--remote-lines 1000000`
    //     against a million-line scrollback doesn't return a 100 MB
    //     JSON envelope. Beyond 10 000 lines the caller probably
    //     wants the file directly (Ctrl+Shift+P → Export Scrollback)
    //     rather than over the wire.
    QJsonObject out;
    TerminalWidget *target = nullptr;
    const QJsonValue tabVal = req.value("tab");
    if (tabVal.isDouble()) {
        const int idx = tabVal.toInt();
        target = m_main->terminalAtTab(idx);
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "get-text: no tab at index %1").arg(idx);
            return QJsonDocument(out);
        }
    } else {
        // ANTS-1392 — when `tab` is omitted, prefer the caller_cwd
        // anchor over the focused tab. terminalForCaller falls back
        // to focusedTerminal() when caller_cwd is empty or no tab
        // matches, preserving the pre-1392 contract.
        const QString callerCwd =
            req.value(QStringLiteral("caller_cwd")).toString();
        target = m_main->terminalForCaller(callerCwd);
        if (!target) {
            out["ok"] = false;
            out["error"] = QStringLiteral("get-text: no active terminal");
            return QJsonDocument(out);
        }
    }

    int lines = 100;
    const QJsonValue linesVal = req.value("lines");
    if (linesVal.isDouble()) {
        const int requested = linesVal.toInt();
        if (requested > 0) lines = std::min(requested, kGetTextMaxLines);
    }

    // ANTS-1348 — server-side byte cap. Default 1 MiB matches the MCP
    // bridge's receive budget so the happy path never trips the
    // transport limit. Caller can lower (test harness) or raise (up
    // to the 16 MiB ceiling for non-MCP rc consumers).
    int maxBytes = RemoteControl::kGetTextDefaultBytesCap;
    const QJsonValue maxBytesVal = req.value("max_bytes");
    if (maxBytesVal.isDouble()) {
        const int requested = maxBytesVal.toInt();
        if (requested > 0) maxBytes = requested;
    }

    const QString raw = target->recentOutput(lines);
    const auto trim =
        RemoteControl::trimScrollbackForGetText(raw, maxBytes);

    out["ok"] = true;
    out["text"] = trim.text;
    out["lines"] =
        trim.text.count('\n') + (trim.text.isEmpty() ? 0 : 1);
    out["bytes"] = trim.text.toUtf8().size();
    out["truncated"] = trim.truncated;
    if (trim.truncated) {
        out["bytes_dropped"] = trim.bytesDropped;
        out["lines_dropped"] = trim.linesDropped;
    }
    if (trim.capClamped) out["bytes_cap_clamped"] = true;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdRecentErrors(const QJsonObject &req) {
    // ANTS-1301 — scan the focused terminal's recent scrollback for
    // structured errors. Terminal resolution mirrors cmdGetText
    // (ANTS-1392): explicit `tab` int → terminalAtTab; else caller_cwd
    // → terminalForCaller → focused fallback. MCP-only.
    QJsonObject out;
    TerminalWidget *target = nullptr;
    const QJsonValue tabVal = req.value(QStringLiteral("tab"));
    if (tabVal.isDouble()) {
        target = m_main->terminalAtTab(tabVal.toInt());
    } else {
        target = m_main->terminalForCaller(
            req.value(QStringLiteral("caller_cwd")).toString());
    }
    if (!target) {
        out["ok"]    = false;
        out["error"] = QStringLiteral("recent_errors: no terminal to read");
        out["code"]  = QStringLiteral("no_window");
        return QJsonDocument(out);
    }

    int lines = 500;
    const QJsonValue linesVal = req.value(QStringLiteral("lines"));
    if (linesVal.isDouble()) {
        const int requested = linesVal.toInt();
        if (requested > 0) lines = std::min(requested, 10000);
    }

    ScrollbackErrors::Options opts;
    const QJsonValue mr = req.value(QStringLiteral("max_results"));
    if (mr.isDouble()) opts.maxResults = mr.toInt();  // lib clamps ≤0 / >1000

    const ScrollbackErrors::Result res =
        ScrollbackErrors::parse(target->recentLogicalOutput(lines), opts);

    QJsonArray errors;
    for (const ScrollbackErrors::ErrorEntry &e : res.errors) {
        QJsonObject o;
        o["category"] = e.category;
        if (!e.file.isEmpty()) o["file"]   = e.file;
        if (e.line   > 0)      o["line"]   = e.line;
        if (e.column > 0)      o["column"] = e.column;
        o["message"] = e.message;
        o["text"]    = e.text;
        errors.append(o);
    }

    // ANTS-3374 — best-effort add_include hint on undeclared-symbol errors.
    enrichLikelyFixes(errors, resolveRootCanonical(m_roots, req));

    out["ok"]            = true;
    out["errors"]        = errors;
    out["errors_count"]  = res.errorsTotal;
    out["lines_scanned"] = res.linesScanned;
    out["truncated"]     = res.truncated;
    return QJsonDocument(out);
}

// ANTS-1312 — last_selection. Return the focused (or routed) terminal's
// current selection text so Claude can pull the highlighted error /
// trace without walking the scrollback. Sole source of truth is
// TerminalWidget::selectedText() — no reimplementation.
QJsonDocument RemoteControl::cmdLastSelection(const QJsonObject &req) {
    QJsonObject out;
    TerminalWidget *target = nullptr;
    const QJsonValue tabVal = req.value(QStringLiteral("tab"));
    if (tabVal.isDouble()) {
        target = m_main->terminalAtTab(tabVal.toInt());
    } else {
        // ANTS-1392 — caller_cwd anchors to caller's tab; falls back
        // to focused tab when absent or no match.
        target = m_main->terminalForCaller(
            req.value(QStringLiteral("caller_cwd")).toString());
    }
    if (!target) {
        out[QStringLiteral("ok")]    = false;
        out[QStringLiteral("error")] = QStringLiteral(
            "last_selection: no terminal to read");
        out[QStringLiteral("code")]  = QStringLiteral("no_window");
        return QJsonDocument(out);
    }

    const QString text = target->selectedText();
    const bool hasSelection = !text.isEmpty();

    out[QStringLiteral("ok")]            = true;
    out[QStringLiteral("has_selection")] = hasSelection;
    out[QStringLiteral("text")]          = text;
    out[QStringLiteral("length")]        = text.size();
    out[QStringLiteral("bytes")]         = text.toUtf8().size();
    return QJsonDocument(out);
}


QJsonDocument RemoteControl::cmdSetTitle(const QJsonObject &req) {
    // Request shape: {"cmd":"set-title","tab":<int optional>,"title":"<string>"}
    //   - `tab` optional; default = active tab. `isDouble()` guard to
    //     keep `--remote-tab 0` distinct from "tab omitted" — same
    //     pattern as send-text.
    //   - `title` required (must be a string). Empty string clears the
    //     pin and lets the auto-title path resume — useful for
    //     scripts that want to "reset to default" without restarting
    //     the tab.
    QJsonObject out;
    const QJsonValue titleVal = req.value("title");
    if (!titleVal.isString()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "set-title: missing or non-string \"title\" field");
        return QJsonDocument(out);
    }
    const QString title = titleVal.toString();

    int idx;
    const QJsonValue tabVal = req.value("tab");
    if (tabVal.isDouble()) {
        idx = tabVal.toInt();
    } else {
        // No explicit tab → resolve the active one. We need an index
        // (not just a TerminalWidget*) because `setTabTitleForRemote`
        // operates by index. Look up via currentIndex() rather than
        // walking all tabs.
        idx = m_main->currentTabIndexForRemote();
    }

    if (!m_main->setTabTitleForRemote(idx, title)) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "set-title: no tab at index %1").arg(idx);
        return QJsonDocument(out);
    }
    out["ok"] = true;
    out["index"] = idx;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdLaunch(const QJsonObject &req) {
    // Request shape: {"cmd":"launch","cwd":"<path optional>","command":"<string required>",
    //                 "raw":<bool optional>}
    //
    // `launch` differs from `new-tab` in two ways:
    //   1. `command` is REQUIRED — the whole point of launch is to
    //      spawn something, so we reject the no-command call up front
    //      rather than silently behaving like new-tab.
    //   2. We auto-append `\n` if the command doesn't already end in
    //      one — matches user intent ("launch this command" implies
    //      "and run it"). new-tab leaves command untouched because
    //      it's the lower-level building block; launch is the sugar
    //      that "just works" for the common case.
    //
    // 0.7.52 (2026-04-27 indie-review HIGH) — `command` is routed
    // through filterControlChars by default, identical to send-text.
    // Without this, a same-UID attacker reaching the rc socket gets
    // ESC-sequence / bracketed-paste / OSC 52 injection via launch
    // even though send-text was hardened against it. The `raw: true`
    // opt-out matches send-text's escape hatch for callers (test
    // harnesses, plugins) who need raw byte access.
    QJsonObject out;
    const QJsonValue commandVal = req.value("command");
    if (!commandVal.isString() || commandVal.toString().isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "launch: missing or empty \"command\" field "
            "(use new-tab if you want a bare shell)");
        return QJsonDocument(out);
    }
    QString command = commandVal.toString();
    if (!command.endsWith('\n')) command += '\n';

    const bool rawBypass = req.value("raw").toBool(false);
    int stripped = 0;
    const QByteArray rawBytes = command.toUtf8();
    const QByteArray payload = rawBypass
        ? rawBytes
        : RemoteControl::filterControlChars(rawBytes, &stripped);
    const QString filteredCommand = QString::fromUtf8(payload);

    const QString cwd = req.value("cwd").toString();
    // ANTS-1347 — `cwd` hygiene + anchor.
    //
    // Byte hygiene (always): the shared cwdHasBadByte helper rejects
    // C0 (U+0000..U+001F), backslash, and C1 (U+0080..U+009F). The
    // C1 leg is the path-side counterpart to ANTS-1335's byte-strip
    // on text payloads — same threat (rc/MCP seam delivering
    // untrusted bytes), different semantics (reject-not-strip for
    // paths, where silent mutation would mislead the caller).
    //
    // Anchor (default-on): non-empty cwd routes through
    // PathValidation::validatePath against the focused project root,
    // matching every other path-typed rc/MCP verb post-ANTS-1295.
    // The optional `allow_outside_root: true` opt-out skips the
    // anchor while keeping byte hygiene — for callers (Lua plugins,
    // ants @ launch CLI) that legitimately need to chdir outside any
    // project root.
    if (!cwd.isEmpty()) {
        if (RemoteControl::cwdHasBadByte(cwd)) {
            QJsonObject errOut;
            errOut["ok"] = false;
            errOut["error"] = QStringLiteral(
                "launch: cwd contains control or backslash characters");
            errOut["code"] = QStringLiteral("bad_cwd");
            return QJsonDocument(errOut);
        }
        const bool allowOutside =
            req.value("allow_outside_root").toBool(false);
        if (!allowOutside) {
            const QString root = resolveRootCanonical(m_roots);
            if (root.isEmpty()) {
                QJsonObject errOut;
                errOut["ok"] = false;
                errOut["error"] = QStringLiteral(
                    "launch: no focused project root (set "
                    "allow_outside_root:true to chdir outside any project)");
                errOut["code"] = QStringLiteral("no_project");
                return QJsonDocument(errOut);
            }
            const auto check = PathValidation::validatePath(
                cwd, root, QStringLiteral("launch"),
                QStringLiteral("cwd"));
            if (check.bad) return QJsonDocument(check.err);
        }
    }
    bool started = false;
    const int idx = m_main->newTabForRemote(cwd, filteredCommand, &started);
    out["index"] = idx;
    if (!started) {
        // The tab exists but holds no shell; a follow-up send-text to it
        // would go nowhere, so this is not a success.
        out["ok"] = false;
        out["code"] = QStringLiteral("shell_failed");
        out["error"] = QStringLiteral("the new tab's shell did not start");
        return QJsonDocument(out);
    }
    out["ok"] = true;
    if (!rawBypass && stripped > 0) out["stripped"] = stripped;
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdNewTab(const QJsonObject &req) {
    // Request shape: {"cmd":"new-tab","cwd":"<path>","command":"<string>",
    //                 "raw":<bool optional>}
    //   - `cwd` optional; empty/absent → inherit cwd from the focused
    //     terminal (same default as the menu-driven newTab() slot)
    //   - `command` optional; when present, written to the new tab's
    //     shell after a 200 ms settle (matches onSshConnect's timing).
    //     Caller is responsible for the trailing newline — matches
    //     `send-text` semantics so the two commands behave
    //     consistently with shell pipes.
    //   - `raw` optional; default false. When true, skips C0 filter
    //     (matches send-text). Otherwise `command` is filtered
    //     identically to send-text — see cmdLaunch for rationale.
    //
    // 0.7.52 (2026-04-27 indie-review HIGH) — `command` is routed
    // through filterControlChars by default, identical to send-text.
    QJsonObject out;
    const QString cwd     = req.value("cwd").toString();
    const QString command = req.value("command").toString();
    const bool rawBypass  = req.value("raw").toBool(false);

    QString filteredCommand = command;
    int stripped = 0;
    if (!command.isEmpty() && !rawBypass) {
        const QByteArray payload =
            RemoteControl::filterControlChars(command.toUtf8(), &stripped);
        filteredCommand = QString::fromUtf8(payload);
    }

    // ANTS-1347 — `cwd` hygiene + anchor. See cmdLaunch for the
    // full rationale; this verb mirrors the same flow.
    if (!cwd.isEmpty()) {
        if (RemoteControl::cwdHasBadByte(cwd)) {
            QJsonObject errOut;
            errOut["ok"] = false;
            errOut["error"] = QStringLiteral(
                "new-tab: cwd contains control or backslash characters");
            errOut["code"] = QStringLiteral("bad_cwd");
            return QJsonDocument(errOut);
        }
        const bool allowOutside =
            req.value("allow_outside_root").toBool(false);
        if (!allowOutside) {
            const QString root = resolveRootCanonical(m_roots);
            if (root.isEmpty()) {
                QJsonObject errOut;
                errOut["ok"] = false;
                errOut["error"] = QStringLiteral(
                    "new-tab: no focused project root (set "
                    "allow_outside_root:true to chdir outside any project)");
                errOut["code"] = QStringLiteral("no_project");
                return QJsonDocument(errOut);
            }
            const auto check = PathValidation::validatePath(
                cwd, root, QStringLiteral("new-tab"),
                QStringLiteral("cwd"));
            if (check.bad) return QJsonDocument(check.err);
        }
    }
    bool started = false;
    const int idx = m_main->newTabForRemote(cwd, filteredCommand, &started);
    out["index"] = idx;
    if (!started) {
        // The tab exists but holds no shell; a follow-up send-text to it
        // would go nowhere, so this is not a success.
        out["ok"] = false;
        out["code"] = QStringLiteral("shell_failed");
        out["error"] = QStringLiteral("the new tab's shell did not start");
        return QJsonDocument(out);
    }
    out["ok"] = true;
    if (!rawBypass && stripped > 0) out["stripped"] = stripped;
    return QJsonDocument(out);
}

// ANTS-1117 v1: tab-list — richer per-tab snapshot than `ls`.
QJsonDocument RemoteControl::cmdTabList() {
    QJsonObject out;
    out["ok"] = true;
    out["tabs"] = m_main->tabsAsJson();
    return QJsonDocument(out);
}

// ============================================================================
// ANTS-2049 — e2e harness inject verbs (socket-only, gated on m_e2eMode).
// All run on the GUI thread (dispatch() is called from the QLocalServer
// readyRead handler, which the GUI thread owns), so posting synthetic events
// to widgets is main-thread-safe using raw QKeyEvent / QMouseEvent —
// the shipped binary gains no Qt Test-module link (INV-8).
// ============================================================================

// Shared refusal envelope for the e2e verbs. Mirrors the {ok:false, code,
// error} shape the harness dispatches on (spec §2.3).
static QJsonDocument e2eRefusal(const QString &verb, const QString &code,
                                const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = verb + QStringLiteral(": ") + msg;
    return QJsonDocument(o);
}

// Offscreen-safe target resolver (spec §2.3 / INV-4). Order: (1) an active
// modal dialog; else (2) the first visible non-main top-level; else (3) the
// main window. Never QApplication::activeWindow() (unreliable offscreen / on
// some WMs — the codebase avoids it deliberately). With a non-empty
// objectName, descend into the resolved top-level via findChild (nullptr →
// the caller emits widget_not_found).
QWidget *RemoteControl::e2eResolveTarget(const QString &objectName) const {
    QWidget *top = QApplication::activeModalWidget();
    if (!top) {
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (w == static_cast<QWidget *>(m_main)) continue;
            if (!w->isVisible()) continue;
            top = w;
            break;
        }
    }
    if (!top) top = m_main;
    if (objectName.isEmpty()) return top;
    return top->findChild<QWidget *>(objectName);
}

QJsonDocument RemoteControl::cmdInjectKey(const QJsonObject &req) {
    const QString widget = req.value(QStringLiteral("widget")).toString();
    QWidget *target = e2eResolveTarget(widget);
    if (!widget.isEmpty() && !target)
        return e2eRefusal(QStringLiteral("inject-key"),
                          QStringLiteral("widget_not_found"),
                          QStringLiteral("no widget named \"%1\"").arg(widget));
    if (!target)
        return e2eRefusal(QStringLiteral("inject-key"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));

    // Post to the focus widget (falling back to the target itself) — the
    // robust, accelerator-independent path the smoke suite relies on
    // (a printable string reaching the focused grid's keyPressEvent → PTY).
    QWidget *dest = target->focusWidget();
    if (!dest) dest = target;

    // key: an integer Qt::Key value, or a small set of named keys.
    int key = 0;
    const QJsonValue kv = req.value(QStringLiteral("key"));
    if (kv.isDouble()) {
        key = kv.toInt();
    } else {
        static const QHash<QString, int> named = {
            {QStringLiteral("Return"),    Qt::Key_Return},
            {QStringLiteral("Enter"),     Qt::Key_Enter},
            {QStringLiteral("Escape"),    Qt::Key_Escape},
            {QStringLiteral("Tab"),       Qt::Key_Tab},
            {QStringLiteral("Backspace"), Qt::Key_Backspace},
            {QStringLiteral("Space"),     Qt::Key_Space},
            {QStringLiteral("Up"),        Qt::Key_Up},
            {QStringLiteral("Down"),      Qt::Key_Down},
            {QStringLiteral("Left"),      Qt::Key_Left},
            {QStringLiteral("Right"),     Qt::Key_Right},
        };
        key = named.value(kv.toString(), 0);
    }
    const QString text = req.value(QStringLiteral("text")).toString();
    if (key == 0 && text.isEmpty())
        return e2eRefusal(QStringLiteral("inject-key"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("need a \"key\" or \"text\""));

    Qt::KeyboardModifiers mods = Qt::NoModifier;
    const QJsonArray modArr = req.value(QStringLiteral("modifiers")).toArray();
    for (const QJsonValue &m : modArr) {
        const QString ms = m.toString().toLower();
        if (ms == QLatin1String("ctrl") || ms == QLatin1String("control"))
            mods |= Qt::ControlModifier;
        else if (ms == QLatin1String("shift"))
            mods |= Qt::ShiftModifier;
        else if (ms == QLatin1String("alt"))
            mods |= Qt::AltModifier;
        else if (ms == QLatin1String("meta"))
            mods |= Qt::MetaModifier;
    }

    QCoreApplication::postEvent(
        dest, new QKeyEvent(QEvent::KeyPress, key, mods, text));
    QCoreApplication::postEvent(
        dest, new QKeyEvent(QEvent::KeyRelease, key, mods, text));
    QJsonObject o;
    o["ok"] = true;
    return QJsonDocument(o);
}

QJsonDocument RemoteControl::cmdInjectClick(const QJsonObject &req) {
    const QString widget = req.value(QStringLiteral("widget")).toString();
    QWidget *target = e2eResolveTarget(widget);
    if (!widget.isEmpty() && !target)
        return e2eRefusal(QStringLiteral("inject-click"),
                          QStringLiteral("widget_not_found"),
                          QStringLiteral("no widget named \"%1\"").arg(widget));
    if (!target)
        return e2eRefusal(QStringLiteral("inject-click"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));

    QPoint pt;
    if (req.contains(QStringLiteral("x")) && req.contains(QStringLiteral("y")))
        pt = QPoint(req.value(QStringLiteral("x")).toInt(),
                    req.value(QStringLiteral("y")).toInt());
    else
        pt = target->rect().center();

    Qt::MouseButton btn = Qt::LeftButton;
    const QString bs = req.value(QStringLiteral("button")).toString().toLower();
    if (bs == QLatin1String("right"))       btn = Qt::RightButton;
    else if (bs == QLatin1String("middle")) btn = Qt::MiddleButton;

    const QPointF local(pt);
    const QPointF global = target->mapToGlobal(pt);
    QCoreApplication::postEvent(
        target, new QMouseEvent(QEvent::MouseButtonPress, local, global,
                                btn, btn, Qt::NoModifier));
    QCoreApplication::postEvent(
        target, new QMouseEvent(QEvent::MouseButtonRelease, local, global,
                                btn, Qt::NoButton, Qt::NoModifier));
    QJsonObject o;
    o["ok"] = true;
    return QJsonDocument(o);
}

QJsonDocument RemoteControl::cmdResizeWindow(const QJsonObject &req) {
    // resize-window ignores `widget` — it acts on the resolved top-level
    // window. Reply echoes the post-clamp size (INV-1: socket-observable).
    QWidget *target = e2eResolveTarget(QString());
    if (!target)
        return e2eRefusal(QStringLiteral("resize-window"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));
    QWidget *win = target->window();

    int w = req.value(QStringLiteral("w")).toInt(0);
    int h = req.value(QStringLiteral("h")).toInt(0);
    if (w <= 0 || h <= 0)
        return e2eRefusal(QStringLiteral("resize-window"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("\"w\" and \"h\" must be positive"));

    // Clamp to [minimum, screen-available].
    const QSize minSz =
        win->minimumSizeHint().expandedTo(win->minimumSize());
    if (minSz.width()  > 0) w = std::max(w, minSz.width());
    if (minSz.height() > 0) h = std::max(h, minSz.height());
    if (QScreen *scr = win->screen()) {
        const QRect avail = scr->availableGeometry();
        if (avail.width()  > 0) w = std::min(w, avail.width());
        if (avail.height() > 0) h = std::min(h, avail.height());
    }
    win->resize(w, h);

    QJsonObject o;
    o["ok"] = true;
    o["w"]  = win->width();
    o["h"]  = win->height();
    return QJsonDocument(o);
}

QJsonDocument RemoteControl::cmdGrabImage(const QJsonObject &req) {
    // Guard order (spec §2.3 / INV-5): (1) empty path → bad_args;
    // (2) unset artifact dir → bad_args (clearer than validatePath's
    // fail-closed empty-root message); (3) path escaping the artifact root
    // → bad_path via PathValidation.
    const QString path = req.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("\"path\" required"));
    const QByteArray dirEnv = qgetenv("ANTS_E2E_ARTIFACT_DIR");
    if (dirEnv.isEmpty())
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("ANTS_E2E_ARTIFACT_DIR unset"));
    const QString root =
        QFileInfo(QString::fromLocal8Bit(dirEnv)).canonicalFilePath();
    const auto chk = PathValidation::validatePath(
        path, root, QStringLiteral("grab-image"), QStringLiteral("path"));
    if (chk.bad) return QJsonDocument(chk.err);

    const QString widget = req.value(QStringLiteral("widget")).toString();
    QWidget *target = e2eResolveTarget(widget);
    if (!widget.isEmpty() && !target)
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("widget_not_found"),
                          QStringLiteral("no widget named \"%1\"").arg(widget));
    if (!target)
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("no live top-level window"));

    // ANTS-5132 — anchor the write under the artifact root. `argvForm` is the
    // NFC-normalised INPUT, not the validated location: validatePath computes
    // `QDir(root).filePath(nfc)` for its containment check and discards it, and
    // `resolved` is empty for a file that does not exist yet, which a PNG about
    // to be written never does. Saving argvForm therefore resolved a relative
    // path against the Ants process's CWD, so the grab landed outside the
    // artifact dir it had just been proved to be inside — measured writing into
    // the launching shell's project root. An absolute path is already anchored
    // by the check above and must not be re-joined.
    const QString out = QFileInfo(chk.argvForm).isAbsolute()
                            ? chk.argvForm
                            : QDir(root).filePath(chk.argvForm);
    if (!target->grab().save(out, "PNG"))
        return e2eRefusal(QStringLiteral("grab-image"),
                          QStringLiteral("bad_args"),
                          QStringLiteral("PNG save failed"));
    QJsonObject o;
    o["ok"]   = true;
    o["path"] = out;
    return QJsonDocument(o);
}

// ANTS-4932 § 2.3 — token_usage is terminal-scoped: it reads the terminal's
// own counters, so its body lives on the GUI side.
QJsonDocument RemoteControl::cmdTokenUsage(const QJsonObject &req,
                                           ClaudeIntegration *ci) {
    // ANTS-1427 — middle checkpoint in the multi-stage MCP audit
    // trail. Pairs with the lambda-entry log (registerToolProvider
    // wrapper) and the dispatch-end log (recordDispatch). The
    // pointer value lets future debug sessions confirm the ci
    // captured at lambda-registration time is still the same here.
    ANTS_LOG(DebugLog::Claude,
             "mcp cmd-enter cmdTokenUsage ci=%p",
             static_cast<const void *>(ci));

    const bool wantsReset  = req.value(QStringLiteral("reset")).toBool(false);
    const bool includeZero = req.value(QStringLiteral("include_zero")).toBool(false);

    // Snapshot first; reset (if requested) only AFTER the snapshot
    // exists in the response — INV-9 (read-and-clear atomicity).
    const TokenUsageEngine::Snapshot snap = ci->tokenUsageReport(includeZero);
    // ANTS-3572 — read the persisted aggregate (stored + live session) BEFORE
    // any reset folds the session into storage, so the fields already include
    // the session about to be folded (a follow-up call then returns the same
    // lifetime). m_main is non-owning/non-null by contract; guard defensively.
    // ANTS-4684 — a REFUSED marshal is not a zero summary. § 2.5 forbids
    // answering from a default here: the caller would read "no savings" where
    // the truth is "nobody looked", and this verb exists to report a number.
    // A null m_main is a DIFFERENT case and still defaults — there is no
    // window to ask, which is not a refusal.
    // ANTS-5311 — the ants-mcpd snapshots, read ONCE here and handed to the
    // summary, so the `mcpd` block and the period figures see one read.
    const TokenUsageEngine::PeerUsage peers = TokenUsageEngine::readPeerSnapshots(
        TokenUsageEngine::peerSnapshotDir(), /*claimDead=*/false, nullptr, nullptr);
    TokenSavingsSummary savings;
    if (m_main) {
        const auto got = ants::onGuiThread(
            [this, &peers]() { return m_main->tokenSavingsSummary(peers); });
        if (!got) {
            QJsonObject env;
            env[QStringLiteral("ok")]    = false;
            env[QStringLiteral("code")]  = QStringLiteral("gui_read_refused");
            env[QStringLiteral("error")] = QStringLiteral(
                "token_usage: the savings read was refused because the "
                "dispatcher is shutting down — reporting zero would "
                "understate the total rather than say it is unavailable");
            return QJsonDocument(env);
        }
        savings = *got;
    }
    if (wantsReset) {
        ci->resetTokenUsage();
    }

    QJsonObject env;
    env["ok"] = true;
    env["since"] = QDateTime::fromMSecsSinceEpoch(snap.sinceUnixMs, QTimeZone::utc())
                       .toString(Qt::ISODate);
    env["since_unix_ms"] = static_cast<qint64>(snap.sinceUnixMs);
    env["tools_called"]  = snap.toolsCalled;
    env["total_saved"]   = static_cast<qint64>(snap.totalSaved);
    // ANTS-1355 — envelope sum across ALL tools (includes those
    // filtered out of `calls[]` by include_zero:false).
    env["total_wrap_bytes"] = static_cast<qint64>(snap.totalWrapBytes);
    // ANTS-1432 — Σ(failed_bytes_in + failed_bytes_out) across ALL
    // tools. Net-token-impact for the session is
    //     total_saved - total_failed_bytes / 4.
    env["total_failed_bytes"] = static_cast<qint64>(snap.totalFailedBytes);
    env["reset_performed"] = wantsReset;

    QJsonArray calls;
    for (const auto &r : snap.calls) {
        QJsonObject c;
        c["tool"]              = r.tool;
        c["n_calls"]           = r.nCalls;
        c["bytes_in"]          = static_cast<qint64>(r.bytesIn);
        c["bytes_out"]         = static_cast<qint64>(r.bytesOut);
        // ANTS-1355 — wrap-overhead + latency breakdown.
        c["wrap_bytes"]        = static_cast<qint64>(r.wrapBytes);
        c["duration_us_min"]   = static_cast<qint64>(r.durationUsMin);
        c["duration_us_max"]   = static_cast<qint64>(r.durationUsMax);
        c["duration_us_mean"]  = static_cast<qint64>(r.durationUsMean);
        c["est_tokens_saved"]  = static_cast<qint64>(r.estTokensSaved);
        // ANTS-1432 — per-tool failure cost. Zero for tools that
        // have only ever succeeded.
        c["failed_calls"]      = static_cast<qint64>(r.failedCalls);
        c["failed_bytes_in"]   = static_cast<qint64>(r.failedBytesIn);
        c["failed_bytes_out"]  = static_cast<qint64>(r.failedBytesOut);
        calls.append(c);
    }
    env["calls"] = calls;
    // ANTS-3572 — persisted month / YTD / all-time saved (each = stored + this
    // session). monthly[] is the folded buckets only, recent-first. Placed
    // after calls[] (JSON order is immaterial; summary totals follow the detail).
    env["month_saved"]    = savings.month;
    env["ytd_saved"]      = savings.ytd;
    env["lifetime_saved"] = savings.lifetime;
    env["monthly"]        = savings.monthly;
    // ANTS-5311 — calls served by ants-mcpd, in their own block: calls[] and
    // total_saved above stay this process's session, so ANTS-1284's
    // total_saved = Σ calls[].est_tokens_saved still holds.
    QJsonObject mcpd;
    mcpd["sessions"]     = peers.sessions;
    mcpd["calls"]        = static_cast<qint64>(peers.calls);
    mcpd["failed_calls"] = static_cast<qint64>(peers.failedCalls);
    mcpd["total_saved"]  = static_cast<qint64>(peers.savedTokens);
    env["mcpd"] = mcpd;
    if (!peers.skipped.isEmpty())
        env["snapshots_skipped"] = QJsonArray::fromStringList(peers.skipped);
    return QJsonDocument(env);
}
