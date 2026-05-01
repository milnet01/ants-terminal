#include "remotecontrol.h"
#include "mainwindow.h"
#include "roadmapdialog.h"
#include "terminalwidget.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTabWidget>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QTimer>

// safeToUnlinkLocalSocket lives in secureio.h as of ANTS-1132 (0.7.66)
// so the Claude hook + MCP server start paths can share the same
// helper. The file-scope static here was unified with that lift.


RemoteControl::RemoteControl(MainWindow *main, QObject *parent)
    : QObject(parent), m_main(main) {}

RemoteControl::~RemoteControl() {
    if (m_server) {
        m_server->close();
    }
}

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
    return QStringLiteral("/tmp/ants-terminal-%1.sock")
        .arg(::getuid());
}

bool RemoteControl::start() {
    if (m_server) return true;

    const QString path = defaultSocketPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_server = new QLocalServer(this);
    // Restrict access to the owning user — matches the hook/MCP
    // sockets' posture. Must be set before listen() on Unix; Qt
    // enforces this on the socket itself.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    // If a stale socket file exists (previous crash didn't clean up),
    // remove it. `removeServer` is a no-op if no socket exists and
    // succeeds when the path exists but is not actively bound.
    // If another live instance holds the lock, listen() fails and
    // we skip the takeover (see outer `if` below).
    if (!m_server->listen(path)) {
        if (safeToUnlinkLocalSocket(path)) {
            QLocalServer::removeServer(path);
        } else {
            ANTS_LOG(DebugLog::Network,
                "remote-control: refusing to unlink %s — not a socket "
                "owned by this user (possible symlink or foreign file); "
                "remote-control disabled for this process",
                qUtf8Printable(path));
            delete m_server;
            m_server = nullptr;
            return false;
        }
        if (!m_server->listen(path)) {
            ANTS_LOG(DebugLog::Network,
                "remote-control: listen(%s) failed — another instance "
                "may own the socket; remote-control disabled for this "
                "process", qUtf8Printable(path));
            delete m_server;
            m_server = nullptr;
            return false;
        }
    }
    setOwnerOnlyPerms(path);

    connect(m_server, &QLocalServer::newConnection,
            this, &RemoteControl::onNewConnection);
    ANTS_LOG(DebugLog::Network,
        "remote-control: listening on %s", qUtf8Printable(path));
    return true;
}

void RemoteControl::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
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
        const qintptr fd = socket->socketDescriptor();
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            if (::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                             SO_PEERCRED, &cred, &len) != 0 ||
                cred.uid != ::getuid()) {
                ANTS_LOG(DebugLog::Network,
                    "remote-control: peer UID mismatch "
                    "(peer=%d self=%d) — disconnecting",
                    static_cast<int>(cred.uid),
                    static_cast<int>(::getuid()));
                socket->disconnectFromServer();
                socket->deleteLater();
                continue;
            }
        }
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
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
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

            const QByteArray line = buf.left(nlIdx);
            QJsonParseError err;
            QJsonDocument req = QJsonDocument::fromJson(line, &err);
            QJsonDocument resp;
            if (err.error != QJsonParseError::NoError || !req.isObject()) {
                QJsonObject e;
                e["ok"] = false;
                e["error"] = QStringLiteral("invalid JSON: %1")
                    .arg(err.errorString());
                resp = QJsonDocument(e);
            } else {
                resp = dispatch(req.object());
            }
            socket->write(resp.toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::disconnected,
                socket, &QLocalSocket::deleteLater);
    }
}

QJsonDocument RemoteControl::dispatch(const QJsonObject &req) {
    const QString cmd = req.value("cmd").toString();
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
    if (cmd == QLatin1String("roadmap-query")) {
        return cmdRoadmapQuery();
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
        target = m_main->currentTerminal();
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
        if (requested > 0) lines = std::min(requested, 10000);
    }

    const QString text = target->recentOutput(lines);
    out["ok"] = true;
    out["text"] = text;
    out["lines"] = text.count('\n') + (text.isEmpty() ? 0 : 1);
    out["bytes"] = text.toUtf8().size();
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
    const int idx = m_main->newTabForRemote(cwd, filteredCommand);
    out["ok"] = true;
    out["index"] = idx;
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

    const int idx = m_main->newTabForRemote(cwd, filteredCommand);
    out["ok"] = true;
    out["index"] = idx;
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

// ANTS-1117 v1: roadmap-query — parse the active tab's ROADMAP.md
// (cached on mtime; INV-10 rate-limit) into a structured bullet
// stream for Claude. Returns the unified `{ok, error, code}` shape
// when no roadmap is loaded for the active tab.
QJsonDocument RemoteControl::cmdRoadmapQuery() {
    QJsonObject out;
    const QString path = m_main->roadmapPathForRemote();
    if (path.isEmpty()) {
        out["ok"] = false;
        out["error"] = QStringLiteral(
            "no ROADMAP.md detected for the active tab");
        out["code"] = QStringLiteral("no_roadmap_loaded");
        return QJsonDocument(out);
    }

    const QFileInfo fi(path);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // INV-10 wall-clock cap: even if mtime hasn't advanced (1-second
    // mtime resolution on some filesystems), force a refresh after
    // kRoadmapCacheTtlMs so an in-place edit within the same tick is
    // still picked up within the spec's "≤ 100 ms" budget.
    const bool fresh = (m_roadmapCachePath == path) &&
                       (m_roadmapCacheMtimeMs == mtime) &&
                       (mtime != 0) &&
                       (nowMs - m_roadmapCacheStampMs <= kRoadmapCacheTtlMs);
    if (!fresh) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out["ok"] = false;
            out["error"] = QStringLiteral(
                "could not open %1 for reading").arg(path);
            out["code"] = QStringLiteral("read_failed");
            return QJsonDocument(out);
        }
        const QString markdown = QString::fromUtf8(f.readAll());
        const auto bullets = RoadmapDialog::parseBullets(markdown);
        QJsonArray arr;
        for (const auto &b : bullets) {
            QJsonObject o;
            o["id"] = b.id;
            o["status"] = b.status;
            o["headline"] = b.headline;
            o["kind"] = b.kind;
            QJsonArray lanes;
            for (const QString &l : b.lanes) lanes.append(l);
            o["lanes"] = lanes;
            arr.append(o);
        }
        m_roadmapCachePath = path;
        m_roadmapCacheMtimeMs = mtime;
        m_roadmapCacheStampMs = nowMs;
        m_roadmapCacheBullets = arr;
    }

    out["ok"] = true;
    out["bullets"] = m_roadmapCacheBullets;
    out["path"] = path;
    out["count"] = m_roadmapCacheBullets.size();
    return QJsonDocument(out);
}

// Client — runs in the --remote invocation of the binary. No Qt
// event loop; synchronous connect → write → readLine → exit.
int RemoteControl::runClient(const QString &command,
                             const QJsonObject &args,
                             const QString &socketPath) {
    QJsonObject env = args;
    env["cmd"] = command;
    const QByteArray payload = QJsonDocument(env).toJson(
        QJsonDocument::Compact) + '\n';

    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(2000)) {
        fprintf(stderr,
            "ants-terminal --remote: cannot connect to %s (%s)\n"
            "  Is Ants Terminal running with remote-control enabled?\n"
            "  Override the path via ANTS_REMOTE_SOCKET=...\n",
            qUtf8Printable(socketPath),
            qUtf8Printable(socket.errorString()));
        return 1;
    }
    socket.write(payload);
    if (!socket.waitForBytesWritten(2000)) {
        fprintf(stderr, "ants-terminal --remote: write timeout\n");
        return 1;
    }
    // Read until newline or disconnect.
    QByteArray resp;
    while (socket.waitForReadyRead(2000)) {
        resp += socket.readAll();
        if (resp.contains('\n')) break;
    }
    if (resp.isEmpty()) {
        fprintf(stderr, "ants-terminal --remote: no response\n");
        return 1;
    }
    // Strip trailing newline for tidier stdout.
    while (!resp.isEmpty() && (resp.endsWith('\n') || resp.endsWith('\r'))) {
        resp.chop(1);
    }
    fwrite(resp.constData(), 1, resp.size(), stdout);
    fputc('\n', stdout);

    // Exit-code shaping: parse the "ok" field so callers can
    // `if ants-terminal --remote ls; then ...` without piping
    // through jq.
    QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (doc.isObject() && doc.object().value("ok").toBool()) return 0;
    return 2;
}
