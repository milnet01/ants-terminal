#include "remotecontrol.h"
#include "coldeyesengine.h"
#include "debtsweepengine.h"
#include "fileoutline.h"
#include "gitwrap.h"
#include "claudeintegration.h"
#include "indiereviewengine.h"
#include "mainwindow.h"
#include "pathvalidation.h"
#include "plantemplateengine.h"
#include "remotecontrolgate.h"
#include "resolvedroot.h"
#include "sessionmemoryengine.h"
#include "tokenusageengine.h"
#include "roadmapdialog.h"
#include "roadmapfoldin.h"
#include "subsystemmap.h"
#include "terminalwidget.h"
#include "verifyengine.h"
#include "verifytrust.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
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
#include <cmath>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QCryptographicHash>
#include <QScopeGuard>
#include <QTimer>

// safeToUnlinkLocalSocket lives in secureio.h as of ANTS-1132 (0.7.66)
// so the Claude hook + MCP server start paths can share the same
// helper. The file-scope static here was unified with that lift.

namespace {
// Forward decl for early callers (ANTS-1347 cmdLaunch / cmdNewTab,
// post-bundle-A). Definition lives in the anonymous namespace below
// next to the rest of the git_state helpers. The two anon-namespace
// blocks in this TU share internal linkage so this forward decl
// resolves at the same `resolveRootCanonical` symbol.
QString resolveRootCanonical(MainWindow *main);
// ANTS-1391 — read-verb overload: prefer caller_cwd in the request
// body over the focused-tab default. Definition next to the legacy
// one below.
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req);
}  // namespace

RemoteControl::RemoteControl(MainWindow *main, QObject *parent)
    : QObject(parent), m_main(main) {}

void RemoteControl::setVerifyTrustClient(
        std::unique_ptr<VerifyTrust::Client> c) {
    m_verifyTrustClient = std::move(c);
}

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
            const int gscRet = ::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                                            SO_PEERCRED, &cred, &len);
            // If getsockopt failed OR returned a truncated struct, treat
            // as a hostile peer rather than logging cred.uid==0 (which
            // would surface as a fake "root tried to connect" alarm).
            if (gscRet != 0 || len != sizeof(cred) ||
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
        if (requested > 0) lines = std::min(requested, 10000);
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
            const QString root = resolveRootCanonical(m_main);
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
            const QString root = resolveRootCanonical(m_main);
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

    // ANTS-1287-INV-1: optional `section` slug. Empty/missing → full-file
    // path (existing behaviour, INV-6).
    const QString section = req.value(QStringLiteral("section")).toString();

    // ANTS-1398-INV-1: `include_section_headers` opt-in. Default false
    // — section-rollup bullets (empty id + empty headline, status emoji
    // only) are dropped from `bullets[]` server-side so clients don't
    // have to scan for them. Pass true to retain the legacy shape for
    // any back-compat caller that wants them.
    const bool hasIncludeHeadersArg =
        req.contains(QStringLiteral("include_section_headers"));
    const bool includeSectionHeaders =
        req.value(QStringLiteral("include_section_headers")).toBool(false);

    // ANTS-1398-INV-2: rollup predicate. A bullet is a section rollup
    // iff its `id` and `headline` are both empty — the unambiguous
    // signature of `parseBullets`'s status-only summary cards.
    auto isRollupBullet = [](const QJsonValue &v) {
        const QJsonObject o = v.toObject();
        return o.value(QStringLiteral("id")).toString().isEmpty()
            && o.value(QStringLiteral("headline")).toString().isEmpty();
    };

    // ANTS-1391: when caller_cwd is present, derive the ROADMAP.md path
    // under that root (matching MainWindow::refreshRoadmapButton's
    // case-variant search) instead of relying on the focused tab's
    // pre-discovered m_roadmapPath. Falls back to the focused tab when
    // absent (back-compat).
    QString path;
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (!callerRaw.isEmpty()) {
        const QString callerCanonical = QFileInfo(callerRaw).canonicalFilePath();
        if (!callerCanonical.isEmpty()) {
            const QStringList candidates = {
                QStringLiteral("ROADMAP.md"),
                QStringLiteral("roadmap.md"),
                QStringLiteral("Roadmap.md"),
            };
            for (const QString &name : candidates) {
                const QString candidate =
                    callerCanonical + QLatin1Char('/') + name;
                if (QFileInfo::exists(candidate)) {
                    path = candidate;
                    break;
                }
            }
        }
    }
    if (path.isEmpty() && callerRaw.isEmpty()) {
        path = m_main->roadmapPathForRemote();
    }
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
    // ANTS-1287-INV-5/8: section index + section-bullet cache share
    // the same freshness predicate.
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
        // ANTS-1287-INV-5: stale cache → wipe both bullet caches AND
        // the heading index. Both regenerate lazily below.
        m_roadmapIndex.clear();
        m_roadmapSectionCache.clear();
        m_roadmapSectionLru.clear();   // ANTS-1346 — keep INV-2 in sync.
        m_roadmapCachePath = path;
        m_roadmapCacheMtimeMs = mtime;
        m_roadmapCacheStampMs = nowMs;

        if (section.isEmpty()) {  // ANTS-1287-INV-6 — full-file path
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
            m_roadmapCacheBullets = arr;
        } else {
            // ANTS-1287-INV-9 — section mode does not pre-fill the
            // full bullets cache; that path is taken lazily on the
            // next no-section call. Build the index once.
            m_roadmapCacheBullets = QJsonArray();
            m_roadmapIndex = RoadmapIndex::buildIndex(markdown);
        }
    }

    // ANTS-1287 — section branch.
    if (!section.isEmpty()) {
        // INV-9: ensure we have an index even on a cache HIT taken
        // earlier in section-less mode (and vice versa).
        if (m_roadmapIndex.isEmpty()) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                out["ok"] = false;
                out["error"] = QStringLiteral(
                    "could not open %1 for reading").arg(path);
                out["code"] = QStringLiteral("read_failed");
                return QJsonDocument(out);
            }
            const QString markdown = QString::fromUtf8(f.readAll());
            m_roadmapIndex = RoadmapIndex::buildIndex(markdown);
        }
        const auto *sec = RoadmapIndex::findBySlug(m_roadmapIndex, section);
        if (!sec) {
            // ANTS-1287-INV-10 — bad_section, hygiene parity with INV-11.
            QString verbatim = section;
            if (verbatim.size() > 64) verbatim.truncate(64);
            for (int i = 0; i < verbatim.size(); ++i) {
                if (verbatim.at(i).unicode() < 0x20) verbatim[i] = QChar('?');
            }
            out["ok"] = false;
            out["error"] = QStringLiteral("unknown section: %1").arg(verbatim);
            out["code"] = QStringLiteral("bad_section");
            return QJsonDocument(out);
        }

        QJsonArray sectionBullets;
        if (m_roadmapSectionCache.contains(sec->slug)) {
            sectionBullets = m_roadmapSectionCache.value(sec->slug);
            // ANTS-1346 — bump to MRU front.
            m_roadmapSectionLru.removeOne(sec->slug);
            m_roadmapSectionLru.prepend(sec->slug);
        } else {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                out["ok"] = false;
                out["error"] = QStringLiteral(
                    "could not open %1 for reading").arg(path);
                out["code"] = QStringLiteral("read_failed");
                return QJsonDocument(out);
            }
            const QString markdown = QString::fromUtf8(f.readAll());
            const QString slice = RoadmapIndex::sliceSection(markdown, *sec);
            const auto bullets = RoadmapDialog::parseBullets(slice);
            for (const auto &b : bullets) {
                QJsonObject o;
                o["id"] = b.id;
                o["status"] = b.status;
                o["headline"] = b.headline;
                o["kind"] = b.kind;
                QJsonArray lanes;
                for (const QString &l : b.lanes) lanes.append(l);
                o["lanes"] = lanes;
                // ANTS-1287-INV-7 — overwrite sectionSlug so all
                // bullets in section-mode carry the requested slug,
                // regardless of slice-local uniqueSlug state.
                o["section_slug"] = sec->slug;
                sectionBullets.append(o);
            }
            m_roadmapSectionCache.insert(sec->slug, sectionBullets);
            // ANTS-1346 — push slug to MRU front and evict tail if
            // the cap is exceeded. removeOne is a no-op on first
            // insert; harmless on duplicate-key re-insert path.
            m_roadmapSectionLru.removeOne(sec->slug);
            m_roadmapSectionLru.prepend(sec->slug);
            while (m_roadmapSectionLru.size() > kRoadmapSectionCacheCap) {
                const QString evicted = m_roadmapSectionLru.takeLast();
                m_roadmapSectionCache.remove(evicted);
            }
        }

        // ANTS-1287-INV-8: status filter applies post-section.
        QJsonArray filtered;
        if (filter == QLatin1String("all")) {
            filtered = sectionBullets;
        } else {
            const QString plannedEmoji  = QString::fromUtf8("\xF0\x9F\x93\x8B");
            const QString progressEmoji = QString::fromUtf8("\xF0\x9F\x9A\xA7");
            const QString doneEmoji     = QString::fromUtf8("\xE2\x9C\x85");
            for (const auto &v : std::as_const(sectionBullets)) {
                const QString s = v.toObject().value(QStringLiteral("status")).toString();
                const bool keep =
                    (filter == QLatin1String("active")  && (s == plannedEmoji || s == progressEmoji)) ||
                    (filter == QLatin1String("shipped") && (s == doneEmoji));
                if (keep) filtered.append(v);
            }
        }
        // ANTS-1398-INV-3b: section-mode emission path drops rollup
        // bullets post-status filter unless the caller opts in.
        if (!includeSectionHeaders) {
            QJsonArray pruned;
            for (const auto &v : std::as_const(filtered)) {
                if (!isRollupBullet(v)) pruned.append(v);
            }
            filtered = pruned;
        }
        out["ok"] = true;
        out["bullets"] = filtered;
        out["path"] = path;
        out["count"] = filtered.size();
        out["filter"] = filter;
        out["section"] = sec->slug;
        // ANTS-1398-INV-5: echo the opt-in only when the caller set it.
        if (hasIncludeHeadersArg) {
            out["include_section_headers"] = includeSectionHeaders;
        }
        return QJsonDocument(out);
    }

    // Full-file path may need to fill the bullets cache lazily if
    // an earlier hit took the section path.
    if (m_roadmapCacheBullets.isEmpty() && (m_roadmapCachePath == path) &&
        (m_roadmapCacheMtimeMs == mtime)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
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
            m_roadmapCacheBullets = arr;
        }
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
    // ANTS-1398-INV-3a: full-file emission path drops rollup bullets
    // post-status filter unless the caller opts in. Runs after status
    // filter so the rollup card's status emoji can't leak through.
    if (!includeSectionHeaders) {
        QJsonArray pruned;
        for (const auto &v : std::as_const(filtered)) {
            if (!isRollupBullet(v)) pruned.append(v);
        }
        filtered = pruned;
    }

    out["ok"] = true;
    out["bullets"] = filtered;
    out["path"] = path;
    // ANTS-1247-INV-10: count is post-filter bullets.size().
    out["count"] = filtered.size();
    // ANTS-1247-INV-7: filter echo (canonicalised lowercase).
    out["filter"] = filter;
    // ANTS-1398-INV-5: echo the opt-in only when the caller set it
    // so the default-false case stays trim on the wire.
    if (hasIncludeHeadersArg) {
        out["include_section_headers"] = includeSectionHeaders;
    }
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
// Forward decl — definition in the second anonymous namespace below
// (it lives next to the rest of the git_state helpers). Both
// unnamed-namespace blocks in this TU share linkage.
QString resolveRootCanonical(MainWindow *main);
// ANTS-1391 — read-verb overload (see top-of-file forward decl).
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req);
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
    // ANTS-1391: prefer the caller_cwd-rooted project when present so a
    // Claude session in project B searches project B, not whichever tab
    // is focused in Ants.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    QString rootCwd;
    if (!callerRaw.isEmpty()) {
        rootCwd = callerRaw;
    } else if (auto *t = m_main->currentTerminal()) {
        rootCwd = t->shellCwd();
    }
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    const QFileInfo rootInfo(rootCwd);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        // ANTS-1295: unified to bad_path with the rest of the
        // anchor-failure envelope; bad_lane is retired.
        return QJsonDocument(wsErr("bad_path",
            QStringLiteral("workspace-search: project root \"%1\" does not exist").arg(rootCwd)));
    }

    // ANTS-1248-INV-2 / ANTS-1295: lane validation through the central
    // PathValidation chokepoint. The historical contract requires the
    // lane to exist on disk (ripgrep's cwd), so reject when
    // check.resolved is empty even though validatePath would otherwise
    // accept via the lexical-fallback branch.
    const QString laneRaw = req.value("lane").toString();
    QString laneAbs = rootCanonical;
    if (!laneRaw.isEmpty()) {
        const auto check = PathValidation::validatePath(
            laneRaw, rootCanonical,
            QStringLiteral("workspace-search"),
            QStringLiteral("lane"));
        if (check.bad) return QJsonDocument(check.err);
        if (check.resolved.isEmpty()) {
            return QJsonDocument(wsErr("bad_path",
                QStringLiteral("workspace-search: \"lane\" does not exist")));
        }
        laneAbs = check.resolved;
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

    // ANTS-1249-INV-1 / ANTS-1295: anchor through the central
    // PathValidation chokepoint. file_outline requires the path to
    // exist (we can't outline a file we can't read), so reject when
    // check.resolved is empty with a `not_found` code distinct from
    // the anchor-fail `bad_path` envelope.
    // ANTS-1391: prefer caller_cwd's project root over the focused tab.
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    const auto check = PathValidation::validatePath(
        rawPath, rootCanonical,
        QStringLiteral("file_outline"),
        QStringLiteral("path"));
    if (check.bad) return QJsonDocument(check.err);
    if (check.resolved.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("file_outline: \"%1\" does not exist").arg(rawPath);
        o["code"]  = QStringLiteral("not_found");
        return QJsonDocument(o);
    }
    const QString resolved = check.resolved;

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

// ANTS-1391: read-verb overload. When the request body carries
// `caller_cwd`, anchor the read to that cwd's project instead of the
// focused tab's. Use case: a Claude session in project B asks Ants
// "what's in ROADMAP?" — without this, the focused-tab default would
// reply with project A's ROADMAP whenever the user's attention is on
// a different tab. Empty/absent caller_cwd preserves back-compat (use
// focused tab). Present-but-unresolvable caller_cwd returns "" so
// callers' existing bad_path envelope fires — no new error code needed.
// Mutating verbs continue to enforce strict match via RcGate (ANTS-1372);
// read verbs just route, without refusing on mismatch.
//
// ANTS-1401 refactor: body is now a wrapper around `resolveCallerCwdRoot`,
// the single source of truth shared with `MainWindow::terminalForCaller`
// and the `caller_cwd_info` MCP verb (ANTS-1400). Pre-refactor mapping:
//   EmptyFallback              → focused-tab root
//   ExplicitMatch / NoMatch    → canonical caller_cwd (no tab walk —
//                                this overload trusts the caller's
//                                claim; tab-finding is the other
//                                wrapper's job)
//   Unresolvable               → empty string
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req) {
    const QString rawCaller =
        req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr =
        ants::resolveCallerCwdRoot(main, rawCaller);
    switch (rr.source) {
        case ants::ResolvedRoot::Source::EmptyFallback:
            return resolveRootCanonical(main);
        case ants::ResolvedRoot::Source::ExplicitMatch:
        case ants::ResolvedRoot::Source::NoMatch:
            return rr.cwd;
        case ants::ResolvedRoot::Source::Unresolvable:
            return QString();
    }
    return QString();  // -Wreturn-type
}

}  // namespace (anonymous from line 1320 — closed early so the
   // `ants::resolveCallerCwdRoot` definition below has external
   // linkage and matches its declaration in resolvedroot.h).

// ANTS-1401 — Central `caller_cwd` resolution helper. Single source of
// truth for the four-case decision tree introduced in ANTS-1396 and now
// shared with `MainWindow::terminalForCaller`,
// `resolveRootCanonical(main, req)`, the `caller_cwd_info` MCP verb
// (ANTS-1400), and the per-tool contract dispatcher (ANTS-1404).
namespace ants {

ResolvedRoot resolveCallerCwdRoot(const MainWindow *main,
                                  const QString &callerCwd) {
    ResolvedRoot rr;
    if (!main) {
        // Defensive: shouldn't happen — MCP dispatch always has a
        // MainWindow. Match the "no useful answer" shape.
        rr.source = ResolvedRoot::Source::EmptyFallback;
        return rr;
    }
    if (callerCwd.isEmpty()) {
        // Case 1 — empty caller_cwd → focused fallback.
        rr.source = ResolvedRoot::Source::EmptyFallback;
        if (auto *t = main->focusedTerminal()) {
            const QString cwd = t->shellCwd();
            if (!cwd.isEmpty()) {
                rr.cwd = QFileInfo(cwd).canonicalFilePath();
            }
        }
        const int idx = main->currentTabIndexForRemote();
        if (idx >= 0) rr.tabIndex = idx;
        return rr;
    }
    const QString wantCanonical =
        QFileInfo(callerCwd).canonicalFilePath();
    if (wantCanonical.isEmpty()) {
        // Case 4 — present but unresolvable.
        rr.source = ResolvedRoot::Source::Unresolvable;
        return rr;
    }
    // INV-5 — deterministic lowest-index tie-break. for-loop walks
    // indices ascending; first match wins.
    for (int i = 0; i < main->tabCount(); ++i) {
        TerminalWidget *t = main->terminalAtTab(i);
        if (!t) continue;
        const QString tabCwd = t->shellCwd();
        if (tabCwd.isEmpty()) continue;
        const QString tabCanonical =
            QFileInfo(tabCwd).canonicalFilePath();
        if (!tabCanonical.isEmpty() &&
            tabCanonical == wantCanonical) {
            // Case 2 — explicit caller_cwd hits an open tab.
            rr.source   = ResolvedRoot::Source::ExplicitMatch;
            rr.cwd      = wantCanonical;
            rr.tabIndex = i;
            return rr;
        }
    }
    // Case 3 — explicit caller_cwd, no open tab matches.
    rr.source = ResolvedRoot::Source::NoMatch;
    rr.cwd    = wantCanonical;
    return rr;
}

}  // namespace ants

namespace {  // reopen the anonymous namespace closed above so the rest
             // of the gitwrap helpers (parseStatusHeader, runStatusOp,
             // etc.) keep their internal-linkage placement.

// ANTS-1250-INV-8 / ANTS-1295: per-call path validation now lives in
// the central PathValidation chokepoint. See src/pathvalidation.{h,cpp}.

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

// ANTS-1391: req carries optional caller_cwd; pass-through to the
// read-verb resolveRootCanonical overload.
QJsonObject runStatusOp(MainWindow *main, const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(main, req);
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
    // ANTS-1391: prefer caller_cwd when present.
    const QString rootCanonical = resolveRootCanonical(main, req);
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

    const auto pc = PathValidation::validatePath(
        req.value("path").toString(), rootCanonical,
        QStringLiteral("git_state"), QStringLiteral("path"));
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
    // ANTS-1391: prefer caller_cwd when present.
    const QString rootCanonical = resolveRootCanonical(main, req);
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
    const auto pc = PathValidation::validatePath(
        req.value("path").toString(), rootCanonical,
        QStringLiteral("git_state"), QStringLiteral("path"));
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
        // ANTS-1391: thread req through so caller_cwd anchors the root.
        return QJsonDocument(runStatusOp(m_main, req));
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
        // ANTS-1295: route the defence-in-depth anchor (malicious
        // symlink inside src/ that points outside root) through the
        // central validator. We treat any failure here as "skip this
        // entry" rather than emitting the validator's envelope, since
        // resolveLaneFiles is a filter and the lane itself is already
        // a parsed-CLAUDE.md member.
        const auto check = PathValidation::validatePath(
            fi.absoluteFilePath(), rootCanonical,
            QStringLiteral("subsystem"), QStringLiteral("file"));
        if (check.bad || check.resolved.isEmpty()) continue;
        // Emit repo-relative paths.
        QString rel = check.resolved;
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

    // ANTS-1391: caller_cwd anchors the root when present.
    const QString rootCanonical = resolveRootCanonical(m_main, req);
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

// ============================================================
// ANTS-1254 — last_audit_summary
// ============================================================
//
// Reads the lex-max audit-*.sarif under {projectRoot}/.audit_cache,
// returns counts + top_findings. Single-entry mtime-keyed cache.
// Reachability gate inherits from UDS + MCP socket (INV-5).

namespace {

QJsonObject lasErr(const QString &code, const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = msg;
    return o;
}

QJsonObject auditSummaryFindingAsJson(
    const AuditEngine::AuditSummaryFinding &f) {
    QJsonObject o;
    o["level"]          = f.level;
    o["severity"]       = f.severity;
    o["ruleId"]         = f.ruleId;
    o["file"]           = f.file;
    o["line"]           = f.line;
    o["message"]        = f.message;
    o["confidence"]     = f.confidence;
    o["highConfidence"] = f.highConfidence;
    return o;
}

QJsonObject buildLasEnvelope(const AuditEngine::AuditSummary &s) {
    QJsonObject ok;
    ok["ok"]         = true;
    ok["run_at"]     = s.runAtIso;
    ok["sarif_path"] = s.sarifPath;
    ok["html_path"]  = s.htmlPath;

    QJsonObject counts;
    counts["error"]      = s.countError;
    counts["warning"]    = s.countWarning;
    counts["note"]       = s.countNote;
    counts["suppressed"] = s.countSuppressed;
    ok["counts"] = counts;

    QJsonArray top;
    for (const auto &f : s.topFindings) top.append(auditSummaryFindingAsJson(f));
    ok["top_findings"] = top;

    return ok;
}

}  // namespace

QJsonDocument RemoteControl::cmdLastAuditSummary(const QJsonObject &req) {
    // INV-8: severity_floor validation runs before disk scanning.
    QString floor = QStringLiteral("warning");
    if (req.contains(QStringLiteral("severity_floor"))) {
        floor = req.value(QStringLiteral("severity_floor")).toString();
    }
    if (floor != QLatin1String("error") &&
        floor != QLatin1String("warning") &&
        floor != QLatin1String("note")) {
        return QJsonDocument(lasErr(QStringLiteral("bad_severity_floor"),
            QStringLiteral("last_audit_summary: \"severity_floor\" "
                           "must be one of {error, warning, note}")));
    }

    // top_n default 5, server-clamp [0, 50].
    int topN = 5;
    if (req.contains(QStringLiteral("top_n")) &&
        req.value(QStringLiteral("top_n")).isDouble()) {
        topN = req.value(QStringLiteral("top_n")).toInt();
    }
    if (topN < 0)  topN = 0;
    if (topN > 50) topN = 50;

    // Discover latest SARIF in {projectRoot}/.audit_cache.
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(lasErr(QStringLiteral("not_audited"),
            QStringLiteral("last_audit_summary: project root unresolved")));
    }
    QDir cacheDir(rootCanonical + QStringLiteral("/.audit_cache"));
    if (!cacheDir.exists()) {
        return QJsonDocument(lasErr(QStringLiteral("not_audited"),
            QStringLiteral("last_audit_summary: no .audit_cache directory")));
    }
    const QStringList sarifNames = cacheDir.entryList(
        QStringList{QStringLiteral("audit-*.sarif")},
        QDir::Files, QDir::Name | QDir::Reversed);
    if (sarifNames.isEmpty()) {
        return QJsonDocument(lasErr(QStringLiteral("not_audited"),
            QStringLiteral("last_audit_summary: no audit-*.sarif found")));
    }
    const QString sarifPath = cacheDir.absoluteFilePath(sarifNames.first());

    // Cache key: (path, mtime, topN, floor).
    const qint64 mtimeMs = QFileInfo(sarifPath).lastModified().toMSecsSinceEpoch();
    const bool hit = (sarifPath == m_auditSummaryPath
                      && mtimeMs == m_auditSummaryMtimeMs
                      && topN    == m_auditSummaryCachedTopN
                      && floor   == m_auditSummaryCachedFloor);
    if (hit) {
        return QJsonDocument(buildLasEnvelope(m_auditSummaryCache));
    }

    // Cache miss — parse.
    auto parsed = AuditEngine::summariseSarif(sarifPath, topN, floor);
    if (!parsed) {
        // Discovery succeeded; failure must be open() or parse.
        QFile f(sarifPath);
        if (!f.open(QIODevice::ReadOnly)) {
            return QJsonDocument(lasErr(QStringLiteral("read_failed"),
                QStringLiteral("last_audit_summary: cannot read SARIF")));
        }
        f.close();
        // INV-10: empty runs[] is treated as not_audited (the user has
        // nothing to consult).
        return QJsonDocument(lasErr(QStringLiteral("parse_failed"),
            QStringLiteral("last_audit_summary: SARIF malformed or "
                           "missing runs[]")));
    }

    m_auditSummaryPath        = sarifPath;
    m_auditSummaryMtimeMs     = mtimeMs;
    m_auditSummaryCachedTopN  = topN;
    m_auditSummaryCachedFloor = floor;
    m_auditSummaryCache       = std::move(*parsed);

    return QJsonDocument(buildLasEnvelope(m_auditSummaryCache));
}

// ----- ANTS-1112 — five `indie_review_*` MCP-tool handlers ---------

namespace {

QJsonObject irErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]      = false;
    o["error"]   = message;
    o["code"]    = code;
    return o;
}

}  // namespace

QJsonDocument RemoteControl::cmdIndieReviewPartition(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(irErr(QStringLiteral("no_window"),
        QStringLiteral("indie_review_partition: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("no_project"),
        QStringLiteral("indie_review_partition: no focused project")));

    const auto lanes = IndieReviewEngine::derivePartition(root);
    QJsonArray arr;
    for (const auto &l : lanes) {
        QJsonObject o;
        o["name"]    = l.name;
        o["summary"] = l.summary;
        QJsonArray sps;
        for (const QString &sp : l.sourcePaths) sps.append(sp);
        o["sourcePaths"] = sps;
        arr.append(o);
    }
    QJsonObject env;
    env["ok"]    = true;
    env["lanes"] = arr;
    // Project-relative path to the partition source (CLAUDE.md or override).
    if (QFileInfo(root + QStringLiteral("/.indie-review/partition.json")).exists()) {
        env["path"] = QStringLiteral(".indie-review/partition.json");
    } else {
        env["path"] = QStringLiteral("CLAUDE.md");
    }
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdIndieReviewBrief(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(irErr(QStringLiteral("no_window"),
        QStringLiteral("indie_review_brief: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("no_project"),
        QStringLiteral("indie_review_brief: no focused project")));

    const QString laneName = req.value(QStringLiteral("lane")).toString().trimmed();
    if (laneName.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("bad_args"),
        QStringLiteral("indie_review_brief: lane required")));

    const auto lanes = IndieReviewEngine::derivePartition(root);
    const IndieReviewEngine::Lane *match = nullptr;
    for (const auto &l : lanes) {
        if (l.name == laneName) { match = &l; break; }
    }
    if (!match) return QJsonDocument(irErr(
        QStringLiteral("not_found"),
        QStringLiteral("indie_review_brief: no such lane")));

    // ANTS-1281: v2 manifest shape — `brief` no longer inlines source
    // bodies; subagent reads them via its Read tool. Per the spec the
    // `brief` field is kept (not renamed to prompt_template_text) to
    // avoid breaking out-of-tree consumers; structured fields are
    // added alongside for programmatic access.
    const auto manifest =
        IndieReviewEngine::assembleBriefManifest(root, *match);
    QJsonArray paths;
    for (const QString &p : manifest.sourcePaths) paths.append(p);
    QJsonArray contractDocs;
    for (const QString &p : manifest.contractDocs) contractDocs.append(p);
    QJsonArray externalSpecs;
    for (const QString &p : manifest.externalSpecs) externalSpecs.append(p);

    QJsonObject env;
    env["ok"]                  = true;
    env["lane"]                = laneName;
    env["brief"]               = manifest.brief;
    env["source_paths"]        = paths;
    env["contract_docs"]       = contractDocs;
    env["external_specs"]      = externalSpecs;
    env["dimension_weighting"] = QJsonObject{};
    env["source_count"]        = manifest.sourcePaths.size();
    env["byte_count"]          = manifest.brief.toUtf8().size();
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdIndieReviewCorroborate(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(irErr(QStringLiteral("no_window"),
        QStringLiteral("indie_review_corroborate: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("no_project"),
        QStringLiteral("indie_review_corroborate: no focused project")));

    // ANTS-1282: accept EITHER `reports` (inline map, v1) OR
    // `reports_dir` (server-side disk read, v2). XOR — exactly one
    // required (INV-1).
    const bool hasReports    = req.contains(QStringLiteral("reports"));
    const bool hasReportsDir = req.contains(QStringLiteral("reports_dir"));
    if (hasReports == hasReportsDir) {
        return QJsonDocument(irErr(
            QStringLiteral("bad_args"),
            QStringLiteral(
                "indie_review_corroborate: provide exactly one of "
                "`reports` (inline map) or `reports_dir` (project-relative "
                "directory of *.md files)")));
    }

    int minLanes = req.value(QStringLiteral("min_lanes")).toInt(2);
    if (minLanes < 1) minLanes = 1;

    QList<IndieReviewEngine::CorroboratedFinding> found;
    QString reportsDir;
    int     reportsRead = 0;
    qint64  totalIn = 0;

    if (hasReportsDir) {
        reportsDir = req.value(QStringLiteral("reports_dir"))
                        .toString().trimmed();
        if (reportsDir.isEmpty()) return QJsonDocument(irErr(
            QStringLiteral("bad_args"),
            QStringLiteral("indie_review_corroborate: reports_dir must be a "
                           "non-empty project-relative path")));
        // ANTS-1295: anchor reports_dir before the engine sees it. The
        // engine has its own anchor as defense-in-depth, but the MCP
        // layer's uniform `bad_path` envelope is more informative than
        // the engine's silent empty-list return.
        const auto check = PathValidation::validatePath(
            reportsDir, root,
            QStringLiteral("indie_review_corroborate"),
            QStringLiteral("reports_dir"));
        if (check.bad) return QJsonDocument(check.err);
        found = IndieReviewEngine::corroboratedFindingsFromDir(
            root, reportsDir, minLanes, &reportsRead);
        // No totalIn tally for the disk path — the orchestrator
        // didn't pay the parent-context cost, which is the whole
        // point of ANTS-1282.
    } else {
        const QJsonObject reportsObj =
            req.value(QStringLiteral("reports")).toObject();
        QHash<QString, QString> reports;
        for (auto it = reportsObj.constBegin();
             it != reportsObj.constEnd(); ++it) {
            const QString r = it.value().toString();
            reports.insert(it.key(), r);
            totalIn += r.toUtf8().size();
        }
        reportsRead = reports.size();
        found = IndieReviewEngine::corroboratedFindings(
            root, reports, minLanes);
    }

    QJsonArray arr;
    for (const auto &f : found) {
        QJsonObject o;
        o["file"] = f.file;
        o["line"] = f.line;
        QJsonArray lns;
        for (const QString &ln : f.citingLanes) lns.append(ln);
        o["citing_lanes"] = lns;
        QJsonArray ctxs;
        for (const QString &c : f.contexts) ctxs.append(c);
        o["contexts"] = ctxs;
        arr.append(o);
    }

    QJsonObject env;
    env["ok"]                 = true;
    env["findings"]           = arr;
    env["total_input_bytes"]  = totalIn;
    env["total_findings"]     = arr.size();
    env["reports_read"]       = reportsRead;
    if (hasReportsDir) env["reports_dir"] = reportsDir;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdIndieReviewSynthesisPrompt(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(irErr(QStringLiteral("no_window"),
        QStringLiteral("indie_review_synthesis_prompt: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("no_project"),
        QStringLiteral("indie_review_synthesis_prompt: no focused project")));

    const QJsonObject reportsObj = req.value(QStringLiteral("reports")).toObject();
    if (reportsObj.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("bad_args"),
        QStringLiteral("indie_review_synthesis_prompt: reports object required")));

    QHash<QString, QString> reports;
    for (auto it = reportsObj.constBegin(); it != reportsObj.constEnd(); ++it) {
        reports.insert(it.key(), it.value().toString());
    }

    const bool incExtras = req.value(QStringLiteral("include_threat_model_extras"))
                              .toBool(true);
    const QString extras = incExtras
        ? IndieReviewEngine::assembleThreatModelExtras(root)
        : QString();

    const QString prompt = IndieReviewEngine::synthesisPrompt(reports, extras);
    QJsonObject env;
    env["ok"]         = true;
    env["prompt"]     = prompt;
    env["byte_count"] = prompt.toUtf8().size();
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdIndieReviewFoldIn(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(irErr(QStringLiteral("no_window"),
        QStringLiteral("indie_review_fold_in: no MainWindow")));
    // ANTS-1372: gate on caller_cwd matching focused tab.
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("indie_review_fold_in"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString root = gate.focused;

    const QJsonArray actArr = req.value(QStringLiteral("actionable")).toArray();
    if (actArr.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("bad_args"),
        QStringLiteral("indie_review_fold_in: actionable array required")));

    QList<IndieReviewEngine::CorroboratedFinding> actionable;
    for (const auto &v : actArr) {
        const auto o = v.toObject();
        IndieReviewEngine::CorroboratedFinding f;
        f.file = o.value(QStringLiteral("file")).toString();
        f.line = o.value(QStringLiteral("line")).toInt(-1);
        for (const auto &lv :
             o.value(QStringLiteral("citing_lanes")).toArray()) {
            f.citingLanes << lv.toString();
        }
        if (f.file.isEmpty()) continue;
        actionable.append(f);
    }
    if (actionable.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("bad_args"),
        QStringLiteral("indie_review_fold_in: no valid actionable entries")));

    QString dateIso = req.value(QStringLiteral("date_iso")).toString();
    if (dateIso.isEmpty()) {
        dateIso = QDate::currentDate().toString(Qt::ISODate);
    }

    const auto ids = RoadmapFoldIn::allocateIds(root, actionable.size());
    if (ids.isEmpty()) return QJsonDocument(irErr(
        QStringLiteral("counter_failed"),
        QStringLiteral("indie_review_fold_in: could not allocate IDs")));

    const QString block = IndieReviewEngine::templateIndieReviewFoldInBlock(
        actionable, ids, dateIso);

    QString heading = req.value(QStringLiteral("release_block_heading")).toString();
    if (heading.isEmpty()) heading = RoadmapFoldIn::findActiveReleaseHeading(root);

    bool written = false;
    if (!heading.isEmpty()) {
        written = RoadmapFoldIn::insertBlock(root, heading, block);
    }

    QJsonObject env;
    env["ok"]            = true;
    env["block"]         = block;
    QJsonArray idsArr;
    for (int id : ids) idsArr.append(id);
    env["allocated_ids"] = idsArr;
    env["written"]       = written;
    if (!heading.isEmpty()) env["release_block_heading"] = heading;
    return QJsonDocument(env);
}

// ---------------------------------------------------------------------------
// ANTS-1113 — debt_sweep_* MCP tools
// ---------------------------------------------------------------------------

namespace {

QJsonObject dsErr(const QString &code, const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = msg;
    o["code"]  = code;
    return o;
}

QJsonObject dsFindingToJson(const DebtSweepEngine::Finding &f) {
    QJsonObject o;
    o["category"]      = f.category;
    o["detector_id"]   = f.detectorId;
    o["file"]          = f.file;
    o["line"]          = f.line;
    o["message"]       = f.message;
    o["suggested_fix"] = f.suggestedFix;
    o["auto_fixable"]  = f.autoFixable;
    return o;
}

DebtSweepEngine::Finding dsJsonToFinding(const QJsonObject &o) {
    DebtSweepEngine::Finding f;
    f.category    = o.value(QStringLiteral("category")).toString();
    f.detectorId  = o.value(QStringLiteral("detector_id")).toString();
    f.file        = o.value(QStringLiteral("file")).toString();
    f.line        = o.value(QStringLiteral("line")).toInt(-1);
    f.message     = o.value(QStringLiteral("message")).toString();
    f.suggestedFix = o.value(QStringLiteral("suggested_fix")).toString();
    f.autoFixable = o.value(QStringLiteral("auto_fixable")).toBool(false);
    return f;
}

}  // namespace

QJsonDocument RemoteControl::cmdDebtSweepScan(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(dsErr(QStringLiteral("no_window"),
        QStringLiteral("debt_sweep_scan: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(dsErr(
        QStringLiteral("no_project"),
        QStringLiteral("debt_sweep_scan: no focused project")));

    DebtSweepEngine::ScanOptions opt;
    opt.sinceRef = req.value(QStringLiteral("since")).toString();
    if (req.contains(QStringLiteral("categories"))) {
        QSet<QString> wanted;
        for (const auto &v : req.value(QStringLiteral("categories")).toArray())
            wanted.insert(v.toString());
        opt.includeCodeDrift     = wanted.contains(QStringLiteral("code_drift"));
        opt.includeTestCoverage  = wanted.contains(QStringLiteral("test_coverage"));
        opt.includeDocDrift      = wanted.contains(QStringLiteral("doc_drift"));
        opt.includePackagingDrift = wanted.contains(QStringLiteral("packaging_drift"));
    }

    const auto findings = DebtSweepEngine::scanAll(root, opt);

    QJsonArray arr;
    QJsonObject by;
    by["code_drift"]       = 0;
    by["test_coverage"]    = 0;
    by["doc_drift"]        = 0;
    by["packaging_drift"]  = 0;
    for (const auto &f : findings) {
        arr.append(dsFindingToJson(f));
        by[f.category] = by.value(f.category).toInt() + 1;
    }

    QJsonObject env;
    env["ok"]              = true;
    env["findings"]        = arr;
    env["total_findings"]  = arr.size();
    env["by_category"]     = by;
    // Resolve since for response transparency.
    QString sinceRes = opt.sinceRef;
    if (sinceRes.isEmpty()) {
        QProcess p;
        p.setWorkingDirectory(root);
        p.start(QStringLiteral("git"),
                {QStringLiteral("describe"), QStringLiteral("--tags"),
                 QStringLiteral("--abbrev=0")});
        if (p.waitForStarted(2000) && p.waitForFinished(5000)
            && p.exitStatus() == QProcess::NormalExit) {
            sinceRes = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        }
        if (sinceRes.isEmpty()) sinceRes = QStringLiteral("HEAD~10");
    }
    env["since_resolved"]  = sinceRes;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdDebtSweepApplyFix(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(dsErr(QStringLiteral("no_window"),
        QStringLiteral("debt_sweep_apply_fix: no MainWindow")));
    // ANTS-1372: gate on caller_cwd matching focused tab.
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("debt_sweep_apply_fix"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString root = gate.focused;

    const QString detectorId = req.value(QStringLiteral("detector_id")).toString();
    const QString file       = req.value(QStringLiteral("file")).toString();
    const int     line       = req.value(QStringLiteral("line")).toInt(-1);
    if (detectorId.isEmpty() || file.isEmpty() || line < 1) {
        return QJsonDocument(dsErr(QStringLiteral("bad_args"),
            QStringLiteral("debt_sweep_apply_fix: detector_id+file+line required")));
    }

    // ANTS-1295: anchor `file` before the engine opens it. The engine
    // does no cwd check of its own — without this, a triple with
    // file="../../etc/passwd" causes QFile::open() at projectPath +
    // "/" + "../../etc/passwd" with the unrelated information-disclosure
    // and write vectors that follow from there.
    const auto check = PathValidation::validatePath(
        file, root,
        QStringLiteral("debt_sweep_apply_fix"),
        QStringLiteral("file"));
    if (check.bad) return QJsonDocument(check.err);

    // Indie-review-2026-05-14 lane-2 H1: pass the canonical resolved
    // form (project-relative) to the engine, not the raw user input.
    // The engine concatenates `projectPath + "/" + finding.file`; if
    // we pass the raw form, a symlink in the path that swaps between
    // validatePath's canonicalisation and the engine's QFile::open()
    // creates a TOCTOU window. The canonical resolved form has all
    // symlinks already followed, closing that window.
    QString safeFile = file;
    if (!check.resolved.isEmpty() &&
        check.resolved.startsWith(root + QLatin1Char('/'))) {
        safeFile = check.resolved.mid(root.size() + 1);
    }

    // Re-synthesise a Finding from the triple. The engine re-validates
    // every claim in §3.9, so this is safe.
    DebtSweepEngine::Finding f;
    f.detectorId  = detectorId;
    f.file        = safeFile;
    f.line        = line;
    // applyMechanicalFix's first guard is `!autoFixable` — for the v1
    // detector that ever sets this, the input must claim autoFixable
    // too. Default false here trips the not_fixable path; callers
    // pass `auto_fixable: true` to opt in.
    f.autoFixable = req.value(QStringLiteral("auto_fixable")).toBool(true);

    const auto v = DebtSweepEngine::applyMechanicalFix(root, f);

    QJsonObject env;
    // ok=false ONLY on hard io_error; recognised no-ops (file_changed,
    // not_fixable) are ok=true with applied=false.
    const bool hardErr = (v.errorCode == QStringLiteral("io_error"));
    env["ok"]      = !hardErr;
    env["applied"] = v.applied;
    if (!v.errorCode.isEmpty()) env["error_code"] = v.errorCode;
    if (!v.errorMessage.isEmpty()) env["error"] = v.errorMessage;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdDebtSweepDefer(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(dsErr(QStringLiteral("no_window"),
        QStringLiteral("debt_sweep_defer: no MainWindow")));
    // ANTS-1372: gate on caller_cwd matching focused tab.
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("debt_sweep_defer"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString root = gate.focused;

    const QJsonArray defArr = req.value(QStringLiteral("deferred")).toArray();
    if (defArr.isEmpty()) return QJsonDocument(dsErr(
        QStringLiteral("bad_args"),
        QStringLiteral("debt_sweep_defer: deferred array required")));

    QList<DebtSweepEngine::Finding> deferred;
    for (const auto &v : defArr) {
        const auto o = v.toObject();
        const auto f = dsJsonToFinding(o);
        if (f.file.isEmpty()) continue;
        deferred.append(f);
    }
    if (deferred.isEmpty()) return QJsonDocument(dsErr(
        QStringLiteral("bad_args"),
        QStringLiteral("debt_sweep_defer: no valid deferred entries")));

    QString dateIso = req.value(QStringLiteral("date_iso")).toString();
    if (dateIso.isEmpty()) {
        dateIso = QDate::currentDate().toString(Qt::ISODate);
    }

    const auto ids = RoadmapFoldIn::allocateIds(root, deferred.size());
    if (ids.isEmpty()) return QJsonDocument(dsErr(
        QStringLiteral("counter_failed"),
        QStringLiteral("debt_sweep_defer: could not allocate IDs")));

    const QString block = DebtSweepEngine::templateDebtSweepFoldInBlock(
        deferred, ids, dateIso);

    QString heading = req.value(QStringLiteral("release_block_heading")).toString();
    if (heading.isEmpty()) heading = RoadmapFoldIn::findActiveReleaseHeading(root);

    bool written = false;
    if (!heading.isEmpty()) {
        written = RoadmapFoldIn::insertBlock(root, heading, block);
    }

    QJsonObject env;
    env["ok"]            = true;
    env["block"]         = block;
    QJsonArray idsArr;
    for (int id : ids) idsArr.append(id);
    env["allocated_ids"] = idsArr;
    env["written"]       = written;
    if (!heading.isEmpty()) env["release_block_heading"] = heading;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdDebtSweepTriagePrompt(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(dsErr(QStringLiteral("no_window"),
        QStringLiteral("debt_sweep_triage_prompt: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(dsErr(
        QStringLiteral("no_project"),
        QStringLiteral("debt_sweep_triage_prompt: no focused project")));

    const QJsonArray fArr = req.value(QStringLiteral("findings")).toArray();
    QList<DebtSweepEngine::Finding> llmShaped;
    for (const auto &v : fArr) {
        llmShaped.append(dsJsonToFinding(v.toObject()));
    }

    const QString prompt = DebtSweepEngine::triagePrompt(llmShaped);
    QJsonObject env;
    env["ok"]         = true;
    env["prompt"]     = prompt;
    env["byte_count"] = prompt.toUtf8().size();
    return QJsonDocument(env);
}

// ---------------------------------------------------------------------------
// ANTS-1289 — verify_changes MCP tool
// ---------------------------------------------------------------------------

namespace {

QJsonObject vcErr(const QString &code, const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = code;
    o["message"] = msg;
    return o;
}

QJsonObject vcGateToJson(const VerifyEngine::GateResult &r) {
    QJsonObject o;
    o["ran"]    = r.ran;
    o["passed"] = r.passed;
    if (!r.ran || !r.skippedReason.isEmpty()) {
        if (!r.skippedReason.isEmpty()) {
            o["skipped_reason"] = r.skippedReason;
        }
    }
    if (r.ran) {
        o["exit_code"]       = r.exitCode;
        // Round duration to 1 decimal.
        const double rounded = std::round(r.durationSec * 10.0) / 10.0;
        o["duration_sec"]    = rounded;
        o["log_tail"]        = r.logTail;
        o["log_truncated"]   = r.logTruncated;
        o["log_total_lines"] = r.logTotalLines;
        if (r.passedCount >= 0 && r.totalCount >= 0) {
            o["passed_count"] = r.passedCount;
            o["total_count"]  = r.totalCount;
        }
        if (!r.failingTests.isEmpty()) {
            QJsonArray a;
            for (const QString &t : r.failingTests) a.append(t);
            o["failing_tests"] = a;
        }
    }
    return o;
}

}  // anonymous

// ANTS-1359 — verify_changes session build-cache helpers. Per
// docs/specs/ANTS-1359.md § 2.3 + § 2.7 the cache key is built from
// projectRoot + git HEAD + git status SHA + trust-outcome SHA +
// ANTS_VERIFY_TRUST_AUTOTRUST + canonicalised options.
namespace {

struct VerifyGitSnapshot {
    bool    valid = false;
    QString head;            // 40-hex commit SHA
    QString statusSha;       // SHA256-hex16 of `git status --porcelain=v1 -z`
};

// Run `git -C <root> <argv...>` and return stdout on exit 0 or {} on
// any failure. 2 s wall-clock cap; merged stderr discarded.
QByteArray runGit(const QString &root, const QStringList &argv) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    QStringList full;
    full << QStringLiteral("-C") << root;
    full.append(argv);
    p.start(QStringLiteral("git"), full);
    if (!p.waitForStarted(1000)) return {};
    if (!p.waitForFinished(2000)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return {};
    }
    return p.readAllStandardOutput();
}

VerifyGitSnapshot collectGitSnapshot(const QString &root) {
    VerifyGitSnapshot s;
    const QByteArray headRaw = runGit(root, {QStringLiteral("rev-parse"),
                                             QStringLiteral("HEAD")});
    if (headRaw.isEmpty()) return s;
    const QString head = QString::fromUtf8(headRaw).trimmed();
    if (head.size() < 7) return s;

    const QByteArray statusRaw = runGit(root,
        {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
         QStringLiteral("-z")});
    // Empty status output is valid (a clean tree). Detect "git failed"
    // separately via the rev-parse already succeeded — if status fails
    // here, the second QProcess returned empty even on success which is
    // indistinguishable from "clean tree" — accept that as the snapshot
    // (the hash of an empty array is deterministic).
    s.head      = head;
    s.statusSha = QString::fromUtf8(
        QCryptographicHash::hash(statusRaw, QCryptographicHash::Sha256)
            .toHex().left(16));
    s.valid = true;
    return s;
}

QJsonObject canonicaliseVerifyOptions(const QJsonObject &req) {
    QJsonObject canon;
    if (req.contains(QStringLiteral("gates"))) {
        const QJsonArray arr =
            req.value(QStringLiteral("gates")).toArray();
        QStringList gates;
        for (const auto &v : arr) gates.append(v.toString());
        gates.sort();
        QJsonArray sorted;
        for (const QString &g : gates) sorted.append(g);
        canon[QStringLiteral("gates")] = sorted;
    }
    if (req.contains(QStringLiteral("max_log_lines"))) {
        canon[QStringLiteral("max_log_lines")] =
            req.value(QStringLiteral("max_log_lines")).toInt();
    }
    if (req.contains(QStringLiteral("timeout_sec"))) {
        canon[QStringLiteral("timeout_sec")] =
            req.value(QStringLiteral("timeout_sec")).toInt();
    }
    return canon;
}

QString verifyCacheKey(const QString &root,
                       const VerifyGitSnapshot &snap,
                       const QString &cfgSource,
                       bool verifyUntrusted,
                       const QByteArray &autoTrustEnv,
                       const QJsonObject &canonOpts) {
    QByteArray trustMaterial;
    trustMaterial += cfgSource.toUtf8();
    trustMaterial += ':';
    trustMaterial += verifyUntrusted ? '1' : '0';
    const QByteArray trustSha =
        QCryptographicHash::hash(trustMaterial,
                                 QCryptographicHash::Sha256)
            .toHex().left(16);

    QByteArray buf;
    buf += root.toUtf8();
    buf += '\0';
    buf += snap.head.toUtf8();
    buf += '\0';
    buf += snap.statusSha.toUtf8();
    buf += '\0';
    buf += trustSha;
    buf += '\0';
    buf += autoTrustEnv;
    buf += '\0';
    buf += QJsonDocument(canonOpts).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(
        QCryptographicHash::hash(buf, QCryptographicHash::Sha256)
            .toHex().left(16));
}

bool anyGateNotNaturallyCompleted(const VerifyEngine::VerifyReport &rep) {
    for (const auto &g : rep.gates) {
        const QString &reason = g.skippedReason;
        if (reason == QLatin1String("command not resolvable")) return true;
        if (reason.startsWith(QLatin1String("timeout after "))) return true;
    }
    return false;
}

}  // anonymous

QJsonDocument RemoteControl::cmdVerifyChanges(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(vcErr(QStringLiteral("no_window"),
        QStringLiteral("verify_changes: no MainWindow")));
    // ANTS-1372: gate on caller_cwd matching focused tab (refuses
    // before the cwd_unreachable check so cross-project intent never
    // gets to the build-spawn path).
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("verify_changes"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    return cmdVerifyChangesImpl(gate.focused, req);
}

QJsonDocument RemoteControl::cmdVerifyChangesWithRoot(
        const QString &root, const QJsonObject &req) {
    // Test seam — bypasses the MainWindow / RcGate path so tests can
    // drive cmdVerifyChanges against a synthetic project root inside
    // a QTemporaryDir without a MainWindow. See spec § 3.
    return cmdVerifyChangesImpl(root, req);
}

QJsonObject RemoteControl::tryGetVerifyCacheForTest(
        const QString &key) const {
    const auto it = m_verifyCache.find(key);
    if (it == m_verifyCache.end()) return {};
    return it->response;
}

void RemoteControl::putVerifyCacheForTest(
        const QString &key, const QJsonObject &response) {
    VerifyChangesCacheEntry e;
    e.stampMs  = QDateTime::currentMSecsSinceEpoch();
    e.key      = key;
    e.response = response;
    if (!m_verifyCache.contains(key)) {
        m_verifyCacheLru.prepend(key);
    } else {
        m_verifyCacheLru.removeOne(key);
        m_verifyCacheLru.prepend(key);
    }
    m_verifyCache.insert(key, e);
    while (m_verifyCacheLru.size() > kVerifyCacheCap) {
        const QString evict = m_verifyCacheLru.takeLast();
        m_verifyCache.remove(evict);
    }
}

QJsonDocument RemoteControl::cmdVerifyChangesImpl(
        const QString &root, const QJsonObject &req) {
    const QFileInfo rootInfo(root);
    if (!rootInfo.isDir()) return QJsonDocument(vcErr(
        QStringLiteral("cwd_unreachable"),
        QStringLiteral("verify_changes: project root not a directory")));

    // INV-9 — incompatible-args gate up front.
    const bool force = req.value(QStringLiteral("force_refresh")).toBool(false);
    const bool probe = req.value(QStringLiteral("cache_only")).toBool(false);
    if (force && probe) {
        return QJsonDocument(vcErr(
            QStringLiteral("incompatible_args"),
            QStringLiteral("force_refresh and cache_only are mutually exclusive")));
    }

    // INV-11 — reentrancy gate with RAII reset.
    if (m_verifyInFlight) {
        return QJsonDocument(vcErr(
            QStringLiteral("verify_in_flight"),
            QStringLiteral("verify_changes: a previous call is still running")));
    }
    m_verifyInFlight = true;
    auto inFlightGuard = qScopeGuard([this]{ m_verifyInFlight = false; });

    // Parse options up front so the canonical-options form is the
    // same on lookup and insert.
    VerifyEngine::VerifyOptions opts;
    if (req.contains(QStringLiteral("gates"))) {
        const QJsonArray arr = req.value(QStringLiteral("gates")).toArray();
        for (const auto &v : arr) {
            const QString s = v.toString();
            if (s == QLatin1String("build")) opts.only.append(VerifyEngine::GateName::Build);
            else if (s == QLatin1String("tests")) opts.only.append(VerifyEngine::GateName::Tests);
            else if (s == QLatin1String("lint"))  opts.only.append(VerifyEngine::GateName::Lint);
        }
    }
    if (req.contains(QStringLiteral("max_log_lines"))) {
        opts.maxLogLines = req.value(QStringLiteral("max_log_lines")).toInt(opts.maxLogLines);
    }
    if (req.contains(QStringLiteral("timeout_sec"))) {
        opts.timeoutSec = req.value(QStringLiteral("timeout_sec")).toInt(opts.timeoutSec);
    }

    // ANTS-1337 — trust client wiring (autotrust env bypass preserved).
    const QByteArray autoTrustEnv =
        qgetenv("ANTS_VERIFY_TRUST_AUTOTRUST");
    if (autoTrustEnv != "1") {
        opts.trustClient = m_verifyTrustClient.get();
    }

    // Step 5 — pre-run git snapshot.
    const VerifyGitSnapshot preSnapshot = collectGitSnapshot(root);
    const bool cacheable = preSnapshot.valid && !force;

    // Step 6 — trust-aware config load. May invoke prompt() at most
    // once per (SHA, session) per verifytrust.cpp:58-97.
    QString cfgSource;
    bool   probedUntrusted = false;
    QList<VerifyEngine::GateConfig> cfg = VerifyEngine::loadGateConfig(
        root, &cfgSource, opts.trustClient, &probedUntrusted);
    (void)cfg;
    if (cfgSource == QLatin1String("bad_config")) {
        // Excluded by INV-4 class 1; early return is sound under
        // inFlightGuard (resets the flag on this return path too).
        return QJsonDocument(vcErr(
            QStringLiteral("bad_config"),
            QStringLiteral("verify.json: malformed JSON or schema mismatch")));
    }

    // Step 7 — compute the cache key now that the trust outcome is
    // known.
    const QJsonObject canonOpts = canonicaliseVerifyOptions(req);
    const QString key = cacheable
        ? verifyCacheKey(root, preSnapshot, cfgSource, probedUntrusted,
                         autoTrustEnv, canonOpts)
        : QString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // Step 8 — cache lookup (skip on force_refresh).
    if (cacheable && !force) {
        const auto it = m_verifyCache.find(key);
        if (it != m_verifyCache.end()
            && (nowMs - it->stampMs) <= kVerifyCacheTtlMs) {
            QJsonObject resp = it->response;
            resp[QStringLiteral("cache_hit")] = true;
            m_verifyCacheLru.removeOne(key);
            m_verifyCacheLru.prepend(key);
            return QJsonDocument(resp);
        }
    }

    // Step 9 — cache_only probe miss.
    if (probe) {
        QJsonObject resp;
        resp[QStringLiteral("ok")]            = true;
        resp[QStringLiteral("cache_hit")]     = false;
        resp[QStringLiteral("cache_miss")]    = true;
        resp[QStringLiteral("project_root")]  = root;
        return QJsonDocument(resp);
    }

    // Step 10 — miss path. runVerify takes (root, opts) and re-calls
    // loadGateConfig internally; the second call is silent for
    // already-decided SHAs (spec § 2.1 rationale).
    const VerifyEngine::VerifyReport rep =
        VerifyEngine::runVerify(root, opts);

    QJsonObject env;
    env[QStringLiteral("ok")]               = true;
    env[QStringLiteral("all_passed")]       = rep.allPassed;
    env[QStringLiteral("project_root")]     = root;
    env[QStringLiteral("config_source")]    = rep.configSource;
    env[QStringLiteral("verify_untrusted")] = rep.verifyUntrusted;
    env[QStringLiteral("cache_hit")]        = false;

    QJsonObject gates;
    for (const auto &g : rep.gates) {
        gates[VerifyEngine::gateKey(g.name)] = vcGateToJson(g);
    }
    env[QStringLiteral("gates")] = gates;

    // Step 10b — post-run snapshot + exclusion-list gate (§ 2.5).
    const VerifyGitSnapshot postSnapshot =
        cacheable ? collectGitSnapshot(root) : VerifyGitSnapshot{};
    const bool snapshotMatched = cacheable
        && postSnapshot.valid
        && postSnapshot.head      == preSnapshot.head
        && postSnapshot.statusSha == preSnapshot.statusSha;
    const bool shouldInsert =
           cacheable
        && snapshotMatched                                  // class 4
        && rep.configSource != QLatin1String("none")        // class 2
        && !rep.verifyUntrusted                              // class 3
        && !anyGateNotNaturallyCompleted(rep);              // class 6
    if (shouldInsert) {
        VerifyChangesCacheEntry e;
        e.stampMs  = nowMs;
        e.key      = key;
        e.response = env;
        m_verifyCache.insert(key, e);
        m_verifyCacheLru.removeOne(key);
        m_verifyCacheLru.prepend(key);
        while (m_verifyCacheLru.size() > kVerifyCacheCap) {
            const QString evict = m_verifyCacheLru.takeLast();
            m_verifyCache.remove(evict);
        }
    }
    return QJsonDocument(env);
}

// ===========================================================================
// ANTS-1290 — plan_template
// ===========================================================================

namespace {

QJsonObject ptErr(const QString &code, const QString &msg,
                  const QString &planPath = QString(),
                  const QString &planMarkdown = QString()) {
    QJsonObject o;
    o["ok"]      = false;
    o["error"]   = code;
    o["message"] = msg;
    if (!planPath.isEmpty())     o["plan_path"]     = planPath;
    if (!planMarkdown.isEmpty()) o["plan_markdown"] = planMarkdown;
    return o;
}

QJsonObject ptConventions() {
    QJsonObject c;
    c["commit_format"]     = QStringLiteral("ANTS-NNNN: description");
    c["test_path_pattern"] = QStringLiteral(
        "tests/features/<feature>/{spec.md,test_<feature>.cpp}");
    c["test_bundle_hint"]  = QStringLiteral(
        "test_chrome | test_audit | test_claude | test_vt | test_dialogs | test_lua");
    c["build_command"]     = QStringLiteral("cmake --build build --quiet");
    c["test_command"]      = QStringLiteral(
        "ctest --test-dir build --output-on-failure");
    c["save_location"]     = QStringLiteral("docs/plans/");
    return c;
}

}  // anonymous

QJsonDocument RemoteControl::cmdPlanTemplate(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ptErr(QStringLiteral("no_window"),
        QStringLiteral("plan_template: no MainWindow")));
    // ANTS-1372: gate on caller_cwd matching focused tab. Dry-run
    // mode still reads the project counter to derive an ANTS-NNNN id,
    // so the gate is unconditional (not save-only).
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("plan_template"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString root = gate.focused;

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName   = req.value(QStringLiteral("feature_name")).toString();
    opts.goal          = req.value(QStringLiteral("goal")).toString();
    opts.architecture  = req.value(QStringLiteral("architecture")).toString();
    opts.techStack     = req.value(QStringLiteral("tech_stack")).toString();
    opts.antsId        = req.value(QStringLiteral("ants_id")).toString();
    if (req.contains(QStringLiteral("task_count_hint"))) {
        opts.taskCountHint = req.value(QStringLiteral("task_count_hint"))
                                .toInt(opts.taskCountHint);
    }
    if (req.contains(QStringLiteral("includes_tests"))) {
        opts.includesTests = req.value(QStringLiteral("includes_tests"))
                                 .toBool(opts.includesTests);
    }
    if (req.contains(QStringLiteral("save"))) {
        opts.save = req.value(QStringLiteral("save")).toBool(opts.save);
    }

    const PlanTemplateEngine::PlanResult r =
        PlanTemplateEngine::buildPlan(root, opts);

    if (!r.ok) {
        return QJsonDocument(ptErr(r.errorCode, r.errorMessage,
                                   r.planPath, r.planMarkdown));
    }

    QJsonObject env;
    env["ok"]             = true;
    env["plan_markdown"]  = r.planMarkdown;
    env["plan_path"]      = r.planPath;
    env["ants_id"]        = r.antsId;
    env["ants_id_source"] = PlanTemplateEngine::antsIdSourceKey(r.antsIdSource);
    env["saved"]          = r.saved;
    env["task_count"]     = r.taskCount;
    env["conventions"]    = ptConventions();
    return QJsonDocument(env);
}

// =============================================================
// ANTS-1284 — token_usage
// =============================================================
//
// Reads the in-process TokenUsageEngine::Tracker on
// ClaudeIntegration; returns the per-tool dispatch report
// (sorted by est_tokens_saved desc) + total_saved. Optional
// reset:true clears counters AFTER building the snapshot, so a
// caller can read-and-clear in one round-trip.
// See docs/specs/ANTS-1284.md.

// ANTS-1422 — tuErr() helper retired: both error branches now
// emit inline envelopes with diagnostic `debug` fields. Keeping
// the envelope construction inline is the minimum-magic path
// to root-cause the live no_claude_integration regression.

QJsonDocument RemoteControl::cmdTokenUsage(const QJsonObject &req) {
    if (!m_main) {
        // ANTS-1422 — never observed in practice but defensive against
        // a never-initialised RemoteControl. Diagnostic dump of the
        // RemoteControl object pointer so we can spot a stale instance
        // if it ever fires.
        QJsonObject env;
        env["ok"]      = false;
        env["error"]   = QStringLiteral("no_main");
        env["code"]    = QStringLiteral("no_main");
        env["message"] = QStringLiteral("token_usage: no main window");
        QJsonObject dbg;
        dbg["m_main_ptr"]     = QStringLiteral("0x0");
        dbg["this_rc_ptr"]    =
            QString::number(reinterpret_cast<quintptr>(this), 16);
        env["debug"] = dbg;
        return QJsonDocument(env);
    }
    auto *ci = m_main->claudeIntegration();
    if (!ci) {
        // ANTS-1422 — observed 2026-05-16 on a live, otherwise-healthy
        // Ants Terminal (mcp_trace / caller_cwd_info / roadmap_query all
        // returned proper data; only token_usage refused). Static
        // analysis shows m_claudeIntegration is assigned once at
        // mainwindow.cpp:3598 and never re-nulled, so reaching this
        // branch is structurally impossible from the MCP-lambda
        // dispatch path (the lambda is registered BY m_claudeIntegration
        // itself). Surfacing pointer values in the envelope so a fresh
        // repro can be root-caused without an out-of-band stderr capture.
        QJsonObject env;
        env["ok"]      = false;
        env["error"]   = QStringLiteral("no_claude_integration");
        env["code"]    = QStringLiteral("no_claude_integration");
        env["message"] = QStringLiteral(
            "token_usage: claude integration unavailable");
        QJsonObject dbg;
        dbg["m_main_ptr"] =
            QString::number(reinterpret_cast<quintptr>(m_main), 16);
        dbg["this_rc_ptr"] =
            QString::number(reinterpret_cast<quintptr>(this), 16);
        dbg["ci_via_getter_null"] = true;
        env["debug"] = dbg;
        return QJsonDocument(env);
    }

    const bool wantsReset  = req.value(QStringLiteral("reset")).toBool(false);
    const bool includeZero = req.value(QStringLiteral("include_zero")).toBool(false);

    // Snapshot first; reset (if requested) only AFTER the snapshot
    // exists in the response — INV-9 (read-and-clear atomicity).
    const TokenUsageEngine::Snapshot snap = ci->tokenUsageReport(includeZero);
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
        calls.append(c);
    }
    env["calls"] = calls;
    return QJsonDocument(env);
}

// ---------------------------------------------------------------------------
// ANTS-1319 — cold_eyes_* MCP tools
// ---------------------------------------------------------------------------

namespace {

QJsonObject ceErr(const QString &code, const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = msg;
    o["code"]  = code;
    return o;
}

// ANTS-1319 INV-11: cap user-supplied echo at 64 bytes + substitute
// control characters with '?'. Matches the cmdRoadmapQuery hygiene
// block used for the bad_section error code.
QString ceSanitiseEcho(const QString &raw) {
    QString verbatim = raw;
    verbatim.truncate(64);
    QString out;
    out.reserve(verbatim.size());
    for (int i = 0; i < verbatim.size(); ++i) {
        out.append(verbatim.at(i).unicode() < 0x20 ? QChar('?')
                                                  : verbatim.at(i));
    }
    return out;
}

QJsonObject ceFindingToJson(
    const IndieReviewEngine::CorroboratedFinding &f) {
    QJsonObject o;
    o["file"] = f.file;
    o["line"] = f.line;
    QJsonArray lns;
    for (const QString &ln : f.citingLanes) lns.append(ln);
    o["citing_lanes"] = lns;
    QJsonArray ctxs;
    for (const QString &c : f.contexts) ctxs.append(c);
    o["contexts"] = ctxs;
    return o;
}

QJsonArray ceLaneArrayToJson(const QList<ColdEyesEngine::Lane> &lanes) {
    QJsonArray arr;
    for (const auto &l : lanes) {
        QJsonObject o;
        o["name"]    = l.name;
        o["summary"] = l.summary;
        QJsonArray dps;
        for (const QString &p : l.docPaths) dps.append(p);
        o["doc_paths"] = dps;
        arr.append(o);
    }
    return arr;
}

}  // namespace

QJsonDocument RemoteControl::cmdColdEyesPartition(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_partition: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_partition: no focused project")));

    const QString scopeRaw = req.value(QStringLiteral("scope")).toString();
    ColdEyesEngine::Scope scope = ColdEyesEngine::Scope::Default;
    if (!ColdEyesEngine::parseScope(scopeRaw, &scope)) {
        QJsonObject err = ceErr(
            QStringLiteral("bad_scope"),
            QStringLiteral("cold_eyes_partition: scope must be one of "
                           "\"default\", \"docs_only\", \"contracts_only\""));
        err["echo"] = ceSanitiseEcho(scopeRaw);
        return QJsonDocument(err);
    }

    // INV-12 mtime-cache (5 s TTL). Cache hit needs path+scope match
    // AND stamp within TTL. Miss → regenerate.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_coldEyesCachePath != root || m_coldEyesCacheScope != scope
        || now - m_coldEyesCacheStampMs > kColdEyesCacheTtlMs) {
        m_coldEyesCache       = ColdEyesEngine::derivePartition(root, scope);
        m_coldEyesCachePath   = root;
        m_coldEyesCacheScope  = scope;
        m_coldEyesCacheStampMs = now;
    }

    QJsonObject env;
    env["ok"]            = true;
    env["lanes"]         = ceLaneArrayToJson(m_coldEyesCache.lanes);
    env["path"]          = m_coldEyesCache.overridePath;
    env["scope"]         = !scopeRaw.isEmpty() ? scopeRaw
                                              : QStringLiteral("default");
    env["scoped_count"]  = m_coldEyesCache.scopedCount;
    env["truncated"]     = m_coldEyesCache.truncated;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdColdEyesBrief(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_brief: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_brief: no focused project")));

    const QString laneNameRaw = req.value(QStringLiteral("lane")).toString();
    const QString laneName    = laneNameRaw.trimmed();
    if (laneName.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral("cold_eyes_brief: lane required")));

    // Reuse the partition cache (INV-12). On miss, regenerate with
    // Default scope — the caller wants a specific lane and didn't pass
    // a scope arg here, so Default is the right baseline.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_coldEyesCachePath != root
        || m_coldEyesCacheScope != ColdEyesEngine::Scope::Default
        || now - m_coldEyesCacheStampMs > kColdEyesCacheTtlMs) {
        m_coldEyesCache       = ColdEyesEngine::derivePartition(
            root, ColdEyesEngine::Scope::Default);
        m_coldEyesCachePath   = root;
        m_coldEyesCacheScope  = ColdEyesEngine::Scope::Default;
        m_coldEyesCacheStampMs = now;
    }

    const ColdEyesEngine::Lane *match = nullptr;
    for (const auto &l : m_coldEyesCache.lanes) {
        if (l.name == laneName) { match = &l; break; }
    }
    if (!match) {
        QJsonObject err = ceErr(
            QStringLiteral("not_found"),
            QStringLiteral("cold_eyes_brief: no such lane"));
        err["echo"] = ceSanitiseEcho(laneNameRaw);
        return QJsonDocument(err);
    }

    const auto m = ColdEyesEngine::assembleBriefManifest(root, *match);

    QJsonArray dps;
    for (const QString &p : m.docPaths) dps.append(p);
    QJsonArray xref;
    for (const QString &p : m.crossReferenceDocs) xref.append(p);
    QJsonArray code;
    for (const QString &p : m.citedCodePaths) code.append(p);

    QJsonObject env;
    env["ok"]                    = true;
    env["lane"]                  = laneName;
    env["brief"]                 = m.brief;
    env["doc_paths"]             = dps;
    env["cross_reference_docs"]  = xref;
    env["cited_code_paths"]      = code;
    env["byte_count"]            = m.brief.toUtf8().size();
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdColdEyesCrossDocDiff(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_cross_doc_diff: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_cross_doc_diff: no focused project")));

    const QString reportsDirRaw =
        req.value(QStringLiteral("reports_dir")).toString();
    const QString reportsDir = reportsDirRaw.trimmed();
    if (reportsDir.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral("cold_eyes_cross_doc_diff: reports_dir must be a "
                       "non-empty project-relative path")));

    // ANTS-1295: anchor reports_dir before reaching the engine, same
    // reasoning as indie_review_corroborate.
    const auto check = PathValidation::validatePath(
        reportsDir, root,
        QStringLiteral("cold_eyes_cross_doc_diff"),
        QStringLiteral("reports_dir"));
    if (check.bad) return QJsonDocument(check.err);

    int minLanes = req.value(QStringLiteral("min_lanes")).toInt(2);
    if (minLanes < 1) minLanes = 1;

    int reportsRead = 0;
    const auto findings = ColdEyesEngine::crossDocDiffFromDir(
        root, reportsDir, minLanes, &reportsRead);

    QJsonArray arr;
    for (const auto &f : findings) arr.append(ceFindingToJson(f));

    QJsonObject env;
    env["ok"]              = true;
    env["findings"]        = arr;
    env["total_findings"]  = arr.size();
    env["reports_read"]    = reportsRead;
    env["reports_dir"]     = reportsDir;
    env["min_lanes"]       = minLanes;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdColdEyesFoldIn(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_fold_in: no MainWindow")));
    // ANTS-1372: gate on caller_cwd matching focused tab.
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("cold_eyes_fold_in"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString root = gate.focused;

    const QJsonArray actArr =
        req.value(QStringLiteral("actionable")).toArray();
    if (actArr.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral("cold_eyes_fold_in: actionable array required")));

    QList<IndieReviewEngine::CorroboratedFinding> actionable;
    for (const auto &v : actArr) {
        const auto o = v.toObject();
        IndieReviewEngine::CorroboratedFinding f;
        f.file = o.value(QStringLiteral("file")).toString();
        f.line = o.value(QStringLiteral("line")).toInt(-1);
        for (const auto &lv :
             o.value(QStringLiteral("citing_lanes")).toArray()) {
            f.citingLanes << lv.toString();
        }
        if (f.file.isEmpty()) continue;
        actionable.append(f);
    }
    if (actionable.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral("cold_eyes_fold_in: no valid actionable entries")));

    QString dateIso = req.value(QStringLiteral("date_iso")).toString();
    if (dateIso.isEmpty()) {
        dateIso = QDate::currentDate().toString(Qt::ISODate);
    }

    const auto ids = RoadmapFoldIn::allocateIds(root, actionable.size());
    if (ids.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("counter_failed"),
        QStringLiteral("cold_eyes_fold_in: could not allocate IDs")));

    const QString block = ColdEyesEngine::templateColdEyesFoldInBlock(
        actionable, ids, dateIso);

    QString heading = req.value(QStringLiteral("release_block_heading"))
                          .toString();
    if (heading.isEmpty()) heading =
        RoadmapFoldIn::findActiveReleaseHeading(root);

    bool written = false;
    if (!heading.isEmpty()) {
        written = RoadmapFoldIn::insertBlock(root, heading, block);
    }

    QJsonObject env;
    env["ok"]            = true;
    env["block"]         = block;
    QJsonArray idsArr;
    for (int id : ids) idsArr.append(id);
    env["allocated_ids"] = idsArr;
    env["written"]       = written;
    if (!heading.isEmpty()) env["release_block_heading"] = heading;
    return QJsonDocument(env);
}

// ---------------------------------------------------------------------------
// ANTS-1283 — session_memory MCP tool
// ---------------------------------------------------------------------------
//
// Per-cwd key-value persistence backed by
// ~/.cache/ants-terminal/mcp-state/<cwd-hash>.json. Pure delegation to
// SessionMemoryEngine::execute. INV-12 echo hygiene applied to every
// user-supplied string echoed in error responses. See
// docs/specs/ANTS-1283.md.

namespace {

QString smSanitiseEcho(const QString &raw) {
    QString verbatim = raw;
    verbatim.truncate(64);
    QString out;
    out.reserve(verbatim.size());
    for (int i = 0; i < verbatim.size(); ++i) {
        out.append(verbatim.at(i).unicode() < 0x20 ? QChar('?')
                                                  : verbatim.at(i));
    }
    return out;
}

QJsonObject smErr(const QString &code, const QString &msg,
                  const QString &opStr, const QString &keyEcho) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = msg;
    if (!opStr.isEmpty())   o["op"]   = opStr;
    if (!keyEcho.isEmpty()) o["echo"] = smSanitiseEcho(keyEcho);
    return o;
}

}  // namespace

QJsonDocument RemoteControl::cmdSessionMemory(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(smErr(
        QStringLiteral("no_window"),
        QStringLiteral("session_memory: no MainWindow"),
        QString(), QString()));

    // Parse op early — get/list are read-only and skip the ANTS-1372
    // caller-cwd gate; set/delete mutate the project session-memory
    // store and require the gate.
    const QString opRaw = req.value(QStringLiteral("op")).toString();
    SessionMemoryEngine::Op op = SessionMemoryEngine::Op::Get;
    if (!SessionMemoryEngine::parseOp(opRaw, &op)) {
        return QJsonDocument(smErr(
            QStringLiteral("bad_op"),
            QStringLiteral("session_memory: op must be one of "
                           "\"get\", \"set\", \"delete\", \"list\""),
            QString(), opRaw));
    }

    // ANTS-1336: every op (get / list / set / delete) routes through
    // RcGate. caller_cwd is the only project-scope source. Pre-fix,
    // get/list accepted a user-supplied `cwd` arg, which let a
    // session in project A read project B's bucket via the
    // ~/.cache/.../mcp-state/<sha256(cwd)>.json hash path. ANTS-1372
    // closed this for set/delete but preserved it for reads under
    // INV-7 ("survey project B from A"); the 2026-05-14 indie review
    // (lane-5 HI-1) reclassified the same capability as a tenancy
    // bypass. INV-7 is now amended — session_memory is the unique
    // read-only verb that reads from a tenant-hashed cache path, so
    // it joins the gated set. See docs/specs/ANTS-1336.md.
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("session_memory"));
    if (!gate.ok) {
        // Materialise via smErr to keep the existing 4-field shape
        // (path/extra come back empty for gate refusals).
        return QJsonDocument(smErr(gate.errorCode, gate.error,
                                   opRaw, QString()));
    }
    const QString cwd = gate.focused;

    const QString    key   = req.value(QStringLiteral("key")).toString();
    const QJsonValue value = req.value(QStringLiteral("value"));

    // INV-9 — handler-side check for required key/value past schema.
    const bool needsKey = (op != SessionMemoryEngine::Op::List);
    if (needsKey && key.isEmpty()) {
        return QJsonDocument(smErr(
            QStringLiteral("bad_key"),
            QStringLiteral("session_memory: key required for get/set/delete"),
            opRaw, key));
    }
    if (op == SessionMemoryEngine::Op::Set && value.isUndefined()) {
        return QJsonDocument(smErr(
            QStringLiteral("bad_value"),
            QStringLiteral("session_memory: value required for set"),
            opRaw, key));
    }

    const SessionMemoryEngine::OpResult r =
        SessionMemoryEngine::execute(cwd, op, key, value);

    if (!r.ok) {
        QJsonObject env = smErr(r.code, r.error, r.op, r.key);
        if (!r.path.isEmpty()) env["path"] = r.path;
        return QJsonDocument(env);
    }

    QJsonObject env;
    env["ok"]          = true;
    env["op"]          = r.op;
    env["path"]        = r.path;
    env["total_bytes"] = static_cast<qint64>(r.totalBytes);
    switch (op) {
        case SessionMemoryEngine::Op::Get:
            env["key"]   = r.key;
            env["found"] = r.found;
            if (r.found) env["value"] = r.value;
            break;
        case SessionMemoryEngine::Op::Set:
            env["key"]           = r.key;
            env["bytes_written"] = static_cast<qint64>(r.bytesWritten);
            break;
        case SessionMemoryEngine::Op::Delete:
            env["key"]   = r.key;
            env["found"] = r.found;
            break;
        case SessionMemoryEngine::Op::List:
            env["keys"] = r.keys;
            break;
    }
    return QJsonDocument(env);
}
