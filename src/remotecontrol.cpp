#include "remotecontrol.h"
#include "fileoutline.h"
#include "gitwrap.h"
#include "mainwindow.h"
#include "roadmapdialog.h"
#include "subsystemmap.h"
#include "terminalwidget.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
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
    // ANTS-1176: per-verb structured log so a same-UID-attack
    // post-mortem has a record. Deliberately does NOT include the
    // payload itself (text/cwd/command bodies can carry secrets);
    // size + tab + stripped-bytes count are the diagnostic axes.
    const int tabId = req.value("tab").toInt(-1);
    const int textBytes = req.value("text").toString().size();
    ANTS_LOG(DebugLog::Network,
             "rc dispatch cmd=%s tab=%d text_bytes=%d",
             qUtf8Printable(cmd), tabId, textBytes);
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
//
// ANTS-1247: accepts optional `req.status` filter
// ("all"/"active"/"shipped", case-insensitive). The cache continues
// to hold the FULL unfiltered array; filtering happens at response
// build time over the cached entries (sub-ms walk).
QJsonDocument RemoteControl::cmdRoadmapQuery(const QJsonObject &req) {  // ANTS-1247-INV-1
    QJsonObject out;

    // ANTS-1247-INV-4: case-insensitive status parse; canonicalise
    // to lowercase. Anchor: filter parse.
    QString filter = req.value(QStringLiteral("status")).toString().toLower();
    if (filter.isEmpty()) filter = QStringLiteral("all");

    // ANTS-1247-INV-5: unknown status → bad_status, cache untouched.
    // ANTS-1247-INV-11: <verbatim> echo capped at 64 bytes; bytes
    // < 0x20 replaced with '?' to prevent ANSI/control passthrough.
    if (filter != QLatin1String("all") &&
        filter != QLatin1String("active") &&
        filter != QLatin1String("shipped")) {
        QString verbatim = req.value(QStringLiteral("status")).toString();
        if (verbatim.size() > 64) verbatim.truncate(64);
        for (int i = 0; i < verbatim.size(); ++i) {
            if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
        }
        out["ok"] = false;
        out["error"] = QStringLiteral("unknown status filter: %1").arg(verbatim);
        out["code"] = QStringLiteral("bad_status");
        return QJsonDocument(out);
    }

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
    // ANTS-1247-INV-6: filter never invalidates the cache; the
    // TTL/mtime check is preserved exactly.
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

    // ANTS-1247-INV-2/3: filter the cached array post-cache.
    // "active" → 📋+🚧 (planned + in-progress);
    // "shipped" → ✅ only; "all" → pass-through. Anchor: filter switch.
    QJsonArray filtered;
    if (filter == QLatin1String("all")) {
        filtered = m_roadmapCacheBullets;
    } else {
        const QString plannedEmoji  = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
        const QString progressEmoji = QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
        const QString doneEmoji     = QString::fromUtf8("\xE2\x9C\x85");     // ✅
        for (const auto &v : std::as_const(m_roadmapCacheBullets)) {
            const QString s = v.toObject().value(QStringLiteral("status")).toString();
            const bool keep =
                (filter == QLatin1String("active")  && (s == plannedEmoji || s == progressEmoji)) ||
                (filter == QLatin1String("shipped") && (s == doneEmoji));
            if (keep) filtered.append(v);
        }
    }

    out["ok"] = true;
    out["bullets"] = filtered;
    out["path"] = path;
    // ANTS-1247-INV-10: count is post-filter bullets.size().
    out["count"] = filtered.size();
    // ANTS-1247-INV-7: filter echo (canonicalised lowercase).
    out["filter"] = filter;
    return QJsonDocument(out);
}

// ANTS-1248: workspace_search — structured ripgrep wrapper for MCP +
// IPC. Replaces `Bash grep -rn 'pattern' src/` with a server-clamped
// {matches[], truncated, elapsed_ms} envelope. ~6-15 K tokens saved
// per typical "investigate a bug" session.
//
// Process model: QProcess::start("rg", argv) — argv-only, no shell
// interpolation (INV-3). Hard wall-clock budget enforced via
// waitForFinished(kWorkspaceSearchHardKillMs) then SIGTERM, then
// waitForFinished(kWorkspaceSearchKillGraceMs) then SIGKILL (INV-5).
// Stderr capped at 4 KiB and surfaced only in the ok:false branch
// to avoid path enumeration on the ok:true path (INV-8).
namespace {
constexpr int kWorkspaceSearchHardKillMs   = 2000;  // ANTS-1248-INV-5
constexpr int kWorkspaceSearchKillGraceMs  =  200;  // ANTS-1248-INV-5
constexpr int kWorkspaceSearchMaxResultsCap = 500;  // ANTS-1248-INV-4
constexpr int kWorkspaceSearchMaxColumns    = 500;
constexpr int kWorkspaceSearchStderrCapBytes = 4096; // ANTS-1248-INV-8
constexpr int kWorkspaceSearchGlobBytesCap   =  256; // ANTS-1248-INV-9

QJsonObject wsErr(const char *code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = QString::fromLatin1(code);
    return o;
}

bool hasControlOrBackslash(const QString &s) {
    for (QChar c : s) {
        if (c.unicode() < 0x20 || c == QLatin1Char('\\')) return true;
    }
    return false;
}
}  // namespace

QJsonDocument RemoteControl::cmdWorkspaceSearch(const QJsonObject &req) {
    QElapsedTimer wall;
    wall.start();

    // ANTS-1248-INV-1: empty/missing pattern → bad_pattern, no fork.
    const QString pattern = req.value("pattern").toString();
    if (pattern.isEmpty()) {
        return QJsonDocument(wsErr("bad_pattern",
            QStringLiteral("workspace-search: missing or empty \"pattern\"")));
    }

    // Server resolves the project root from QCoreApplication::applicationDirPath()
    // is wrong — that's the build dir. The remote-control + MCP path
    // semantically targets the *focused tab's CWD*, but ripgrep's
    // working dir for the search is determined by `lane`. Default
    // (`lane=""`) is the focused tab's shellCwd; explicit `lane` is
    // resolved relative to that root and must canonicalise back inside.
    QString rootCwd;
    if (auto *t = m_main->currentTerminal()) {
        rootCwd = t->shellCwd();
    }
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    const QFileInfo rootInfo(rootCwd);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(wsErr("bad_lane",
            QStringLiteral("workspace-search: project root \"%1\" does not exist").arg(rootCwd)));
    }

    // ANTS-1248-INV-2: lane validation — NFC normalise, reject control
    // chars + backslash, then canonical-path-startswith the root.
    // Symlink-resolving canonicalFilePath() (not lexical cleanPath())
    // closes the parent-traversal vector; ripgrep is invoked without
    // -L to bound the post-resolve symlink TOCTOU window.
    QString laneArg = req.value("lane").toString().normalized(QString::NormalizationForm_C);
    QString laneAbs = rootCanonical;
    if (!laneArg.isEmpty()) {
        if (hasControlOrBackslash(laneArg)) {
            return QJsonDocument(wsErr("bad_lane",
                QStringLiteral("workspace-search: \"lane\" contains control or backslash characters")));
        }
        const QString joined = QDir(rootCanonical).filePath(laneArg);
        const QString resolved = QFileInfo(joined).canonicalFilePath();
        if (resolved.isEmpty() ||
            !(resolved == rootCanonical ||
              resolved.startsWith(rootCanonical + QLatin1Char('/')))) {
            return QJsonDocument(wsErr("bad_lane",
                QStringLiteral("workspace-search: \"lane\" escapes project root or does not exist")));
        }
        laneAbs = resolved;
    }

    // ANTS-1248-INV-9: glob validation — NFC normalise, 256-byte cap,
    // reject `..` substrings.
    QString glob = req.value("glob").toString().normalized(QString::NormalizationForm_C);
    if (!glob.isEmpty()) {
        if (glob.toUtf8().size() > kWorkspaceSearchGlobBytesCap) {
            return QJsonDocument(wsErr("bad_glob",
                QStringLiteral("workspace-search: \"glob\" exceeds 256 bytes")));
        }
        if (glob.contains(QStringLiteral(".."))) {
            return QJsonDocument(wsErr("bad_glob",
                QStringLiteral("workspace-search: \"glob\" contains \"..\" segments")));
        }
    }

    // ANTS-1248-INV-4: server-side max_results clamp at 500.
    int maxResults = 50;
    const QJsonValue maxVal = req.value("max_results");
    if (maxVal.isDouble()) {
        const int requested = maxVal.toInt();
        if (requested > 0) {
            maxResults = std::min(requested, kWorkspaceSearchMaxResultsCap);
        }
    }

    int context = 0;
    const QJsonValue ctxVal = req.value("context");
    if (ctxVal.isDouble()) {
        const int requested = ctxVal.toInt();
        if (requested > 0) context = std::min(requested, 10);
    }

    const bool isRegex = req.value("regex").toBool(false);
    const QString caseMode = req.value("case").toString(QStringLiteral("smart"));

    // ANTS-1248-INV-3: shell-less argv. Every flag is a separate
    // QString in the argv list — QProcess does not invoke a shell.
    // Two-argument start() overload (QString program, QStringList args).
    QStringList argv;
    argv << QStringLiteral("--json")
         << QStringLiteral("--no-heading")
         << QStringLiteral("--line-number")
         << QStringLiteral("--max-columns") << QString::number(kWorkspaceSearchMaxColumns)
         << QStringLiteral("--threads")     << QStringLiteral("1");
    if (caseMode == QLatin1String("smart"))           argv << QStringLiteral("--smart-case");
    else if (caseMode == QLatin1String("insensitive")) argv << QStringLiteral("--ignore-case");
    else if (caseMode == QLatin1String("sensitive"))   argv << QStringLiteral("--case-sensitive");
    if (!isRegex) argv << QStringLiteral("--fixed-strings");
    if (context > 0) argv << QStringLiteral("--context") << QString::number(context);
    if (!glob.isEmpty()) argv << QStringLiteral("--glob") << glob;
    argv << QStringLiteral("--") << pattern << laneAbs;

    QProcess rg;
    rg.setWorkingDirectory(rootCanonical);
    rg.setProcessChannelMode(QProcess::SeparateChannels);
    // ANTS-1248-INV-3: QProcess::start(QString, QStringList) — argv
    // form. No shell, no single-string overload.
    rg.start(QStringLiteral("rg"), argv);
    if (!rg.waitForStarted(500)) {
        return QJsonDocument(wsErr("rg_failed",
            QStringLiteral("workspace-search: rg failed to start (is ripgrep installed?)")));
    }

    // ANTS-1248-INV-5: 2 s hard kill via kWorkspaceSearchHardKillMs
    // (waitForFinished returns false on timeout). On timeout we
    // terminate(), then grant 200 ms grace, then kill().
    const bool finished = rg.waitForFinished(kWorkspaceSearchHardKillMs);
    bool hardKilled = false;
    if (!finished) {
        hardKilled = true;
        rg.terminate();
        if (!rg.waitForFinished(kWorkspaceSearchKillGraceMs)) {
            rg.kill();
            rg.waitForFinished(kWorkspaceSearchKillGraceMs);
        }
    }

    // ANTS-1248-INV-8: stderr cap. Read up to 4 KiB; emit only on
    // the error branch to avoid path-enumeration leaks on success.
    QByteArray stderrTail = rg.readAllStandardError();
    if (stderrTail.size() > kWorkspaceSearchStderrCapBytes) {
        stderrTail.truncate(kWorkspaceSearchStderrCapBytes);
    }

    const QByteArray stdoutBytes = rg.readAllStandardOutput();

    // rg --json emits one event per line. We want type=="match" events.
    // Each match event has data.path.text, data.line_number, and
    // data.lines.text. NDJSON-style parse: split on '\n', QJsonDocument
    // per line.
    QJsonArray matches;
    int seenMatchEvents = 0;
    bool truncated = false;
    const QList<QByteArray> lines = stdoutBytes.split('\n');
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) continue;
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const QJsonObject ev = doc.object();
        if (ev.value("type").toString() != QLatin1String("match")) continue;
        ++seenMatchEvents;
        if (matches.size() >= maxResults) { truncated = true; continue; }

        const QJsonObject data = ev.value("data").toObject();
        QString path = data.value("path").toObject().value("text").toString();
        // Trim absolute prefix back to project-relative if possible —
        // callers want stable, short paths.
        if (path.startsWith(rootCanonical + QLatin1Char('/'))) {
            path = path.mid(rootCanonical.size() + 1);
        }
        const int lineNo = data.value("line_number").toInt();
        QString text = data.value("lines").toObject().value("text").toString();
        // Strip a single trailing newline that rg includes in `lines.text`.
        if (text.endsWith(QLatin1Char('\n'))) text.chop(1);

        QJsonObject m;
        m["file"] = path;
        m["line"] = lineNo;
        m["text"] = text;
        matches.append(m);
    }

    if (rg.exitStatus() != QProcess::NormalExit && !hardKilled) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg crashed (exit status not normal)"));
        if (!stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(stderrTail);
        return QJsonDocument(o);
    }
    // rg exit codes: 0 = matches found, 1 = no matches (still ok),
    // 2 = error. Anything ≥ 2 (or hard-kill) is treated as failure
    // only when no matches were parsed.
    if (rg.exitCode() >= 2 && matches.isEmpty() && !hardKilled) {
        QJsonObject o = wsErr("rg_failed",
            QStringLiteral("workspace-search: rg returned non-zero exit code %1")
                .arg(rg.exitCode()));
        if (!stderrTail.isEmpty()) o["stderr"] = QString::fromUtf8(stderrTail);
        return QJsonDocument(o);
    }
    if (hardKilled && matches.isEmpty()) {
        // No partial results — surface the hard kill rather than
        // pretending the search finished cleanly.
        return QJsonDocument(wsErr("rg_failed",
            QStringLiteral("workspace-search: rg exceeded 2 s wall budget, hard-killed")));
    }
    // ANTS-1248-INV-4: post-cap detection — truncated iff we either
    // saw more match events than max_results, or the hard kill cut
    // us off mid-stream.
    if (seenMatchEvents > matches.size() || hardKilled) truncated = true;

    QJsonObject out;
    out["ok"]         = true;
    out["pattern"]    = pattern;
    out["matches"]    = matches;
    out["truncated"]  = truncated;
    out["elapsed_ms"] = static_cast<int>(wall.elapsed());
    // ANTS-1248-INV-6: stateless — no cache, no member-state mutation.
    // ANTS-1248-INV-10: reachability gated by the existing UDS +
    // MCP-socket trust model (SO_PEERCRED UID + 0700 + S_ISSOCK).
    // Nothing extra to do here.
    // ANTS-1248-INV-7: tools/list schema declared in claudeintegration.cpp
    // (this body's contract; the schema lives at the wire boundary).
    return QJsonDocument(out);
}

// ANTS-1249: file_outline — structured file outline (header_doc +
// symbols[]). Replaces a full Read of a 5 000-line file with a ~1 K
// token orientation envelope. Path-escape guarded by canonical-path
// startswith (mirrors ANTS-1248's lane check). The regex-scanner
// work itself lives in fileoutline.cpp — this body validates input,
// resolves the path, and delegates.
QJsonDocument RemoteControl::cmdFileOutline(const QJsonObject &req) {
    // ANTS-1249-INV-2: empty path → bad_path; non-existent path
    // returns not_found (set further down by FileOutline::compute).
    const QString rawPath = req.value("path").toString();
    if (rawPath.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: missing or empty \"path\"");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    // ANTS-1249-INV-1: path canonicalisation + project-root guard.
    // Same logic as ANTS-1248's lane check (canonicalFilePath +
    // startsWith). ANTS-1253 will consolidate these into a shared
    // helper.
    QString rootCwd;
    if (auto *t = m_main->currentTerminal()) {
        rootCwd = t->shellCwd();
    }
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    const QString rootCanonical = QFileInfo(rootCwd).canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: project root \"%1\" does not exist").arg(rootCwd);
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    // NFC + reject control / backslash before canonicalising.
    const QString nfc = rawPath.normalized(QString::NormalizationForm_C);
    if (hasControlOrBackslash(nfc)) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: \"path\" contains control or backslash characters");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    QString joined;
    if (QFileInfo(nfc).isAbsolute()) {
        joined = nfc;
    } else {
        joined = QDir(rootCanonical).filePath(nfc);
    }
    const QString resolved = QFileInfo(joined).canonicalFilePath();
    if (resolved.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: \"%1\" does not exist").arg(rawPath);
        o["code"]  = QStringLiteral("not_found");
        return QJsonDocument(o);
    }
    if (!(resolved == rootCanonical ||
          resolved.startsWith(rootCanonical + QLatin1Char('/')))) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: \"path\" escapes project root");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    // ANTS-1249: mode + flags. Delegate to fileoutline.cpp for the
    // actual scan.
    const FileOutline::Mode mode = FileOutline::parseMode(
        req.value("mode").toString());
    const bool includeDoc = req.value("include_doc_comment").toBool(true);
    int maxSymbols = req.value("max_symbols").toInt(200);

    QJsonObject result = FileOutline::compute(resolved, mode,
                                              includeDoc, maxSymbols);

    // Reframe the path back to project-relative so callers get stable
    // paths regardless of where the binary was launched.
    if (result.value("ok").toBool()) {
        QString abs = result.value("path").toString();
        if (abs.startsWith(rootCanonical + QLatin1Char('/'))) {
            result["path"] = abs.mid(rootCanonical.size() + 1);
        }
    }
    // ANTS-1249-INV-10: reachability gate — UDS / MCP socket
    // SO_PEERCRED UID match (same as ANTS-1248). Nothing extra here.
    return QJsonDocument(result);
}

// ANTS-1250: git_state — single tool collapsing status / log / diff
// behind an `op` discriminator. Saves ~240 permanent schema tokens
// vs three separate tools (cold-eyes pass 2). All git invocations go
// through gitwrap (shell-less argv, 5 s + 200 ms kill, 4 KiB stderr
// cap). Argv-injection guards: strict regex on `range`, `--`
// separator before user-derived positional args, `./` prefix on
// `-`-leading paths.
namespace {
constexpr int kGitLogMaxN          = 100;   // ANTS-1250-INV-3
constexpr int kGitLogBodyCapBytes  = 1024;  // ANTS-1250-INV-3

QJsonObject gitErr(const char *code, const QString &message,
                   const QByteArray &stderrTail = {}) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = QString::fromLatin1(code);
    if (!stderrTail.isEmpty()) {
        o["stderr"] = QString::fromUtf8(stderrTail);
    }
    return o;
}

// ANTS-1250-INV-4: stricter range regex — first char of each
// rev-component MUST NOT be `-` (closes leading-flag injection).
// Subsequent chars allow `-` so `HEAD~3..HEAD-1` style is still valid.
bool isValidRange(const QString &range) {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^[a-zA-Z0-9._/^~][a-zA-Z0-9._/^~\-]*)"
            R"((\.\.\.?[a-zA-Z0-9._/^~][a-zA-Z0-9._/^~\-]*)?$)"));
    const auto m = re.match(range);
    return m.hasMatch() && m.capturedLength(0) == range.size();
}

// Project root resolution mirrors cmdWorkspaceSearch / cmdFileOutline:
// focused tab's shellCwd, fall back to QDir::current. Empty string
// returned when canonicalisation fails (caller maps to bad_path or
// not_git_repo depending on context).
QString resolveRootCanonical(MainWindow *main) {
    QString rootCwd;
    if (auto *t = main->currentTerminal()) {
        rootCwd = t->shellCwd();
    }
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    return QFileInfo(rootCwd).canonicalFilePath();
}

// ANTS-1250-INV-8: per-call path validation. Empty path → empty
// QString returned (caller treats absence as "no path filter"). On
// rejection returns nullopt-equivalent: bad=true and err contains
// the error envelope.
struct PathCheck {
    bool        bad = false;
    QJsonObject err;        // populated when bad
    QString     argvForm;   // path as it should appear in argv (./ prefix when starts with -)
};

PathCheck validatePath(const QString &rawPath, const QString &rootCanonical) {
    PathCheck pc;
    if (rawPath.isEmpty()) return pc;
    const QString nfc = rawPath.normalized(QString::NormalizationForm_C);
    if (hasControlOrBackslash(nfc)) {
        pc.bad = true;
        pc.err = gitErr("bad_path",
            QStringLiteral("git_state: \"path\" contains control or "
                           "backslash characters"));
        return pc;
    }
    QString joined;
    if (QFileInfo(nfc).isAbsolute()) {
        joined = nfc;
    } else {
        joined = QDir(rootCanonical).filePath(nfc);
    }
    const QString resolved = QFileInfo(joined).canonicalFilePath();
    // We do NOT require the path to exist (git log/diff for a deleted
    // file is meaningful). But if it DOES resolve, it must canonicalise
    // back inside root. If it does NOT resolve, fall back to a lexical
    // root-prefix check on the joined path so a non-existent path
    // outside root still gets rejected.
    if (!resolved.isEmpty()) {
        if (!(resolved == rootCanonical ||
              resolved.startsWith(rootCanonical + QLatin1Char('/')))) {
            pc.bad = true;
            pc.err = gitErr("bad_path",
                QStringLiteral("git_state: \"path\" escapes project root"));
            return pc;
        }
    } else {
        const QString cleaned = QDir::cleanPath(joined);
        if (!(cleaned == rootCanonical ||
              cleaned.startsWith(rootCanonical + QLatin1Char('/')))) {
            pc.bad = true;
            pc.err = gitErr("bad_path",
                QStringLiteral("git_state: \"path\" escapes project root"));
            return pc;
        }
    }
    // Use the user-supplied form (relative or absolute) verbatim for
    // argv. ANTS-1250-INV-5: prefix `./` so a leading `-` cannot be
    // misread by git as a flag.
    QString form = nfc;
    if (form.startsWith(QLatin1Char('-'))) {
        form = QStringLiteral("./") + form;
    }
    pc.argvForm = form;
    return pc;
}

// Status header line: `## branch...origin/branch [ahead 1, behind 2]`
// or `## branch` for an untracked branch.
void parseStatusHeader(const QString &headerLine, QJsonObject &out) {
    // Strip leading "## ".
    QString rest = headerLine;
    if (rest.startsWith(QStringLiteral("## "))) rest = rest.mid(3);
    // Split off the bracketed ahead/behind suffix if present.
    int aheadN = 0;
    int behindN = 0;
    int bracket = rest.indexOf(QLatin1Char('['));
    if (bracket >= 0) {
        const int close = rest.indexOf(QLatin1Char(']'), bracket);
        if (close > bracket) {
            const QString inside = rest.mid(bracket + 1, close - bracket - 1);
            // tokens: "ahead N", "behind N", or "ahead N, behind N"
            const QStringList parts = inside.split(QLatin1Char(','),
                                                   Qt::SkipEmptyParts);
            for (const QString &raw : parts) {
                const QString t = raw.trimmed();
                if (t.startsWith(QStringLiteral("ahead "))) {
                    aheadN = QStringView{t}.mid(6).toInt();
                } else if (t.startsWith(QStringLiteral("behind "))) {
                    behindN = QStringView{t}.mid(7).toInt();
                }
            }
            rest.truncate(bracket);
            rest = rest.trimmed();
        }
    }
    // rest is now "branch" or "branch...upstream" or "branch...upstream"
    // or just "HEAD (no branch)" for detached.
    QString branch  = rest;
    QString upstream;
    const int dots = rest.indexOf(QStringLiteral("..."));
    if (dots >= 0) {
        branch   = rest.left(dots);
        upstream = rest.mid(dots + 3);
    }
    out["branch"]   = branch;
    out["upstream"] = upstream;
    out["ahead"]    = aheadN;
    out["behind"]   = behindN;
}

QJsonObject runStatusOp(MainWindow *main) {
    const QString rootCanonical = resolveRootCanonical(main);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    QStringList argv;
    argv << QStringLiteral("status")
         << QStringLiteral("--porcelain=v1")
         << QStringLiteral("-b");
    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git status exceeded 5 s wall budget"),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git status crashed"),
            g.stderrTail);
    }
    if (g.exitCode != 0) {
        // ANTS-1250-INV-11: not-a-git-repo → distinct code.
        const QString s = QString::fromUtf8(g.stderrTail);
        if (s.contains(QStringLiteral("not a git repository"),
                       Qt::CaseInsensitive)) {
            return gitErr("not_git_repo",
                QStringLiteral("git_state: not a git repository"));
        }
        return gitErr("git_failed",
            QStringLiteral("git_state: git status exit %1").arg(g.exitCode),
            g.stderrTail);
    }

    QJsonObject out;
    out["ok"]   = true;
    out["op"]   = QStringLiteral("status");
    QJsonArray files;
    QJsonArray untracked;
    const QList<QByteArray> lines = g.stdoutBytes.split('\n');
    for (const QByteArray &lineBytes : lines) {
        if (lineBytes.isEmpty()) continue;
        const QString line = QString::fromUtf8(lineBytes);
        if (line.startsWith(QStringLiteral("## "))) {
            parseStatusHeader(line, out);
            continue;
        }
        // porcelain v1 format: "XY path"
        if (line.size() < 3) continue;
        const QString xy   = line.left(2);
        const QString path = line.mid(3);
        if (xy == QStringLiteral("??")) {
            untracked.append(path);
            continue;
        }
        QJsonObject f;
        f["path"]     = path;
        f["index"]    = QString(xy.at(0));
        f["worktree"] = QString(xy.at(1));
        files.append(f);
    }
    // Backstop fields if header missing (e.g. detached HEAD without
    // -b output — porcelain emits at least the branch line, but be safe).
    if (!out.contains("branch"))   out["branch"]   = QString();
    if (!out.contains("upstream")) out["upstream"] = QString();
    if (!out.contains("ahead"))    out["ahead"]    = 0;
    if (!out.contains("behind"))   out["behind"]   = 0;
    out["files"]     = files;
    out["untracked"] = untracked;
    return out;
}

QJsonObject runLogOp(MainWindow *main, const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(main);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    // ANTS-1250-INV-3: server-clamp n to [1, 100].
    int n = 10;
    const QJsonValue nVal = req.value("n");
    if (nVal.isDouble()) {
        const int requested = nVal.toInt();
        if (requested > 0) n = std::min(requested, kGitLogMaxN);
    }
    const bool wantBody = req.value("body").toBool(false);

    PathCheck pc = validatePath(req.value("path").toString(), rootCanonical);
    if (pc.bad) return pc.err;

    // Format: SHA<US>SUBJECT<US>DATE(<US>BODY)?<RS>
    // Use 0x1f (US) between fields, 0x1e (RS) between commits.
    const QChar US(0x1f);
    const QChar RS(0x1e);
    const QString fmtNoBody = QStringLiteral("%h\x1f%s\x1f%cs");
    const QString fmtWith   = QStringLiteral("%h\x1f%s\x1f%cs\x1f%b");

    QStringList argv;
    argv << QStringLiteral("log")
         << QStringLiteral("--no-color")
         << QStringLiteral("-z")  // null-terminate per commit (we use RS)
         ;
    // -z emits NUL between commits. Override the inter-commit separator
    // by adding %x1e at end of format and splitting on RS.
    const QString fmt = (wantBody ? fmtWith : fmtNoBody) + QChar(0x1e);
    argv << QStringLiteral("--pretty=format:") + fmt;
    // Fetch n+1 to detect truncation (INV-3).
    argv << QStringLiteral("-n") << QString::number(n + 1);
    // ANTS-1250-INV-5: argv -- separator before user-derived path.
    argv << QStringLiteral("--");
    if (!pc.argvForm.isEmpty()) argv << pc.argvForm;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git log exceeded 5 s wall budget"),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git log crashed"),
            g.stderrTail);
    }
    if (g.exitCode != 0) {
        const QString s = QString::fromUtf8(g.stderrTail);
        if (s.contains(QStringLiteral("not a git repository"),
                       Qt::CaseInsensitive)) {
            return gitErr("not_git_repo",
                QStringLiteral("git_state: not a git repository"));
        }
        return gitErr("git_failed",
            QStringLiteral("git_state: git log exit %1").arg(g.exitCode),
            g.stderrTail);
    }

    // Parse: split on 0x1e, then per record split on 0x1f.
    const QString stdoutAll = QString::fromUtf8(g.stdoutBytes);
    const QStringList rawCommits = stdoutAll.split(RS, Qt::SkipEmptyParts);
    QJsonArray commits;
    for (const QString &rec : rawCommits) {
        // Trim leading NUL that -z inserts between records.
        QString r = rec;
        while (!r.isEmpty() && r.at(0) == QLatin1Char('\0')) r = r.mid(1);
        if (r.isEmpty()) continue;
        const QStringList fields = r.split(US);
        if (fields.size() < 3) continue;
        QJsonObject c;
        c["sha"]     = fields.value(0);
        c["subject"] = fields.value(1);
        c["date"]    = fields.value(2);
        if (wantBody && fields.size() >= 4) {
            QString body = fields.value(3);
            // Strip trailing NUL (-z emits one between records that ends
            // up after the body in the final field of all but the last).
            while (!body.isEmpty() && body.endsWith(QLatin1Char('\0'))) {
                body.chop(1);
            }
            const QByteArray b = body.toUtf8();
            if (b.size() > kGitLogBodyCapBytes) {
                body = QString::fromUtf8(b.left(kGitLogBodyCapBytes - 1)) +
                       QChar(0x2026);  // ellipsis
            }
            c["body"] = body;
        }
        commits.append(c);
    }
    bool truncated = false;
    if (commits.size() > n) {
        truncated = true;
        // Drop the n+1th probe commit.
        while (commits.size() > n) commits.removeLast();
    }
    QJsonObject out;
    out["ok"]        = true;
    out["op"]        = QStringLiteral("log");
    out["commits"]   = commits;
    out["truncated"] = truncated;
    return out;
}

QJsonObject runDiffOp(MainWindow *main, const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(main);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    const QString range = req.value("range").toString();
    if (range.isEmpty()) {
        return gitErr("bad_range",
            QStringLiteral("git_state: \"range\" required for op:diff"));
    }
    // ANTS-1250-INV-4: strict regex; first char excludes `-`.
    if (!isValidRange(range)) {
        return gitErr("bad_range",
            QStringLiteral("git_state: \"range\" failed validation"));
    }
    PathCheck pc = validatePath(req.value("path").toString(), rootCanonical);
    if (pc.bad) return pc.err;

    QStringList argv;
    argv << QStringLiteral("diff")
         << QStringLiteral("--no-color")
         << QStringLiteral("--numstat")
         << range
         << QStringLiteral("--");
    if (!pc.argvForm.isEmpty()) argv << pc.argvForm;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git diff exceeded 5 s wall budget"),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git diff crashed"),
            g.stderrTail);
    }
    if (g.exitCode != 0) {
        const QString s = QString::fromUtf8(g.stderrTail);
        if (s.contains(QStringLiteral("not a git repository"),
                       Qt::CaseInsensitive)) {
            return gitErr("not_git_repo",
                QStringLiteral("git_state: not a git repository"));
        }
        if (s.contains(QStringLiteral("unknown revision"),
                       Qt::CaseInsensitive) ||
            s.contains(QStringLiteral("bad revision"),
                       Qt::CaseInsensitive)) {
            return gitErr("bad_range",
                QStringLiteral("git_state: range refers to unknown revision"),
                g.stderrTail);
        }
        return gitErr("git_failed",
            QStringLiteral("git_state: git diff exit %1").arg(g.exitCode),
            g.stderrTail);
    }

    QJsonArray files;
    int totalAdded   = 0;
    int totalRemoved = 0;
    const QList<QByteArray> lines = g.stdoutBytes.split('\n');
    for (const QByteArray &lineBytes : lines) {
        if (lineBytes.isEmpty()) continue;
        const QString line = QString::fromUtf8(lineBytes);
        // numstat format: "<added>\t<removed>\t<path>"
        // Binary files appear as "-\t-\t<path>".
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 3) continue;
        bool addOk = false;
        bool remOk = false;
        const int added   = parts.at(0).toInt(&addOk);
        const int removed = parts.at(1).toInt(&remOk);
        QJsonObject f;
        f["path"] = parts.mid(2).join(QLatin1Char('\t'));
        if (addOk) {
            f["added"]    = added;
            totalAdded   += added;
        } else {
            f["added"]    = QJsonValue();  // null for binary
        }
        if (remOk) {
            f["removed"]  = removed;
            totalRemoved += removed;
        } else {
            f["removed"]  = QJsonValue();
        }
        files.append(f);
    }
    QJsonObject totals;
    totals["added"]   = totalAdded;
    totals["removed"] = totalRemoved;
    totals["files"]   = files.size();

    QJsonObject out;
    out["ok"]     = true;
    out["op"]     = QStringLiteral("diff");
    out["range"]  = range;
    out["files"]  = files;
    out["totals"] = totals;
    return out;
}
}  // namespace

QJsonDocument RemoteControl::cmdGitState(const QJsonObject &req) {
    // ANTS-1250-INV-1: dispatch on op ∈ {status, log, diff}.
    const QString op = req.value("op").toString();
    if (op == QLatin1String("status")) {
        return QJsonDocument(runStatusOp(m_main));
    }
    if (op == QLatin1String("log")) {
        return QJsonDocument(runLogOp(m_main, req));
    }
    if (op == QLatin1String("diff")) {
        return QJsonDocument(runDiffOp(m_main, req));
    }
    return QJsonDocument(gitErr("bad_op",
        QStringLiteral("git_state: \"op\" must be one of "
                       "{status, log, diff}, got \"%1\"").arg(op)));
    // ANTS-1250-INV-2: parseStatusHeader handles `## branch...upstream
    //                  [ahead N, behind M]`.
    // ANTS-1250-INV-7: stderr cap enforced inside GitWrap::run.
    // ANTS-1250-INV-9: gitwrap.started=false → git_missing.
    // ANTS-1250-INV-10: non-zero exit → git_failed with stderr.
    // ANTS-1250-INV-13: reachability gate inherits from UDS + MCP socket
    //                  (SO_PEERCRED UID + 0700 + S_ISSOCK).
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
    // Read until newline or disconnect. ANTS-1169: cap the receive
    // buffer at 1 MiB to mirror the server's frame cap. Without this
    // a same-UID malicious local process could answer the client
    // (set $ANTS_REMOTE_SOCKET to its own listener) and reply with a
    // multi-hundred-MB body that saturates this client process.
    constexpr qint64 kMaxResponseBytes = 1 * 1024 * 1024;
    QByteArray resp;
    while (socket.waitForReadyRead(2000)) {
        resp += socket.readAll();
        if (resp.contains('\n')) break;
        if (resp.size() > kMaxResponseBytes) {
            fprintf(stderr,
                    "ants-terminal --remote: response exceeds %lld bytes; "
                    "aborting (suspect socket hijack)\n",
                    static_cast<long long>(kMaxResponseBytes));
            return 1;
        }
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

// =====================================================================
// ANTS-1251 — subsystem (consolidated; map / files / recent_changes)
// =====================================================================

namespace {

QJsonObject subsystemErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]      = false;
    o["code"]    = code;
    o["error"]   = message;
    return o;
}

// Locate `CLAUDE.md` for the focused tab's project: walk up from the
// shellCwd looking for a file named CLAUDE.md; stop at filesystem root.
// Returns absolute path or empty string.
QString findClaudeMdForRoot(const QString &startDir) {
    if (startDir.isEmpty()) return {};
    QDir d(startDir);
    while (true) {
        const QString candidate = d.filePath(QStringLiteral("CLAUDE.md"));
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).canonicalFilePath();
        }
        if (!d.cdUp()) break;
    }
    return {};
}

QJsonObject lanesAsJson(const QVector<SubsystemMap::Lane> &lanes) {
    QJsonObject root;
    QJsonArray arr;
    for (const auto &l : lanes) {
        QJsonObject o;
        o["name"]    = l.name;
        o["summary"] = l.summary;
        arr.append(o);
    }
    root["lanes"] = arr;
    return root;
}

QJsonArray laneNamesArray(const QVector<SubsystemMap::Lane> &lanes) {
    QJsonArray arr;
    for (const auto &l : lanes) arr.append(l.name);
    return arr;
}

bool laneIsKnown(const QString &name, const QVector<SubsystemMap::Lane> &lanes) {
    for (const auto &l : lanes) {
        if (l.name == name) return true;
    }
    return false;
}

// ANTS-1251-INV-4: enumerate `src/<lane>*` files; canonical-startswith
// re-check each result against project root before yielding it.
QStringList resolveLaneFiles(const QString &lane, const QString &rootCanonical) {
    QStringList out;
    if (rootCanonical.isEmpty()) return out;
    QDir srcDir(QDir(rootCanonical).filePath(QStringLiteral("src")));
    if (!srcDir.exists()) return out;
    const QStringList filters{ lane + QStringLiteral("*") };
    const QFileInfoList entries = srcDir.entryInfoList(
        filters, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &fi : entries) {
        const QString resolved = fi.canonicalFilePath();
        if (resolved.isEmpty()) continue;
        // Defence in depth — even though `lane` came from a trusted
        // CLAUDE.md membership check, a malicious symlink inside src/
        // could redirect outside root.
        if (!(resolved == rootCanonical ||
              resolved.startsWith(rootCanonical + QLatin1Char('/')))) {
            continue;
        }
        // Emit repo-relative paths.
        QString rel = resolved;
        rel.remove(0, rootCanonical.size());
        if (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

QJsonDocument RemoteControl::cmdSubsystem(const QJsonObject &req) {
    const QString op = req.value("op").toString();
    if (op != QLatin1String("map") &&
        op != QLatin1String("files") &&
        op != QLatin1String("recent_changes")) {
        return QJsonDocument(subsystemErr("bad_op",
            QStringLiteral("subsystem: \"op\" must be one of "
                           "{map, files, recent_changes}, got \"%1\"").arg(op)));
    }

    const QString rootCanonical = resolveRootCanonical(m_main);
    const QString claudeMdPath  = findClaudeMdForRoot(rootCanonical);
    // Note: cachedLanes returns empty when the file is missing; that
    // collapses to an empty `lanes[]` for op:"map" (INV-7) and a
    // unknown_lane error for the other ops.
    const QVector<SubsystemMap::Lane> lanes =
        SubsystemMap::cachedLanes(claudeMdPath);

    if (op == QLatin1String("map")) {
        QJsonObject ok;
        ok["ok"]     = true;
        ok["op"]     = "map";
        ok["source"] = "CLAUDE.md";
        ok["lanes"]  = lanesAsJson(lanes).value("lanes");
        return QJsonDocument(ok);
    }

    // ANTS-1251-INV-1: lane validation precedes any filesystem call.
    const QString lane = req.value("lane").toString();
    if (lane.isEmpty() || !laneIsKnown(lane, lanes)) {
        QJsonObject err = subsystemErr("unknown_lane",
            QStringLiteral("subsystem: \"lane\" \"%1\" is not in the "
                           "parsed Module map").arg(lane));
        err["lanes"] = laneNamesArray(lanes);
        return QJsonDocument(err);
    }

    if (op == QLatin1String("files")) {
        // ANTS-1251-INV-4 inside resolveLaneFiles.
        const QStringList files = resolveLaneFiles(lane, rootCanonical);
        QJsonObject ok;
        ok["ok"]   = true;
        ok["op"]   = "files";
        ok["lane"] = lane;
        QJsonArray arr;
        for (const QString &f : files) arr.append(f);
        ok["files"] = arr;
        return QJsonDocument(ok);
    }

    // op == "recent_changes"
    // ANTS-1251-INV-5: compose cmdGitState({op:"log", ...}) per file
    // resolved for the lane; merge by sha; keep top n by date.
    int n = 10;
    if (req.contains("n") && req.value("n").isDouble()) {
        n = req.value("n").toInt();
    }
    if (n < 1)   n = 1;
    if (n > 100) n = 100;

    const QStringList files = resolveLaneFiles(lane, rootCanonical);
    if (files.isEmpty()) {
        QJsonObject ok;
        ok["ok"]      = true;
        ok["op"]      = "recent_changes";
        ok["lane"]    = lane;
        ok["commits"] = QJsonArray{};
        return QJsonDocument(ok);
    }

    // sha → commit object; preserve insertion order for tie-breaks.
    QHash<QString, QJsonObject>           bySha;
    QVector<QString>                      shaOrder;
    for (const QString &f : files) {
        QJsonObject sub;
        sub["op"]   = "log";
        sub["n"]    = n;
        sub["path"] = f;
        const QJsonObject r = cmdGitState(sub).object();
        if (!r.value("ok").toBool()) continue;
        const QJsonArray commits = r.value("commits").toArray();
        for (const QJsonValue &v : commits) {
            const QJsonObject c = v.toObject();
            const QString sha = c.value("sha").toString();
            if (sha.isEmpty() || bySha.contains(sha)) continue;
            bySha.insert(sha, c);
            shaOrder.push_back(sha);
        }
    }

    // Sort merged commits by date desc, fall back to insertion order
    // when dates tie.
    std::sort(shaOrder.begin(), shaOrder.end(),
              [&](const QString &a, const QString &b) {
                  return bySha.value(a).value("date").toString() >
                         bySha.value(b).value("date").toString();
              });
    if (shaOrder.size() > n) shaOrder.resize(n);

    QJsonArray commits;
    for (const QString &sha : shaOrder) commits.append(bySha.value(sha));

    QJsonObject ok;
    ok["ok"]      = true;
    ok["op"]      = "recent_changes";
    ok["lane"]    = lane;
    ok["commits"] = commits;
    return QJsonDocument(ok);
    // ANTS-1251-INV-6: reachability gate inherits from UDS + MCP socket.
}
