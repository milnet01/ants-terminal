#pragma once

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "auditengine.h"  // ANTS-1254 — AuditSummary value member below

class QLocalServer;
class QLocalSocket;
class MainWindow;

// Remote-control server for Ants Terminal. Kitty-style JSON envelopes
// over a Unix domain socket — unlocks scripting, IDE integration, CI.
// See ROADMAP.md § 0.8.0 > 🎨 Features — multiplexing for the full
// command list; this first slice implements only `ls`, with the socket
// + envelope + client infrastructure in place for the next commands
// (`send-text`, `set-title`, `select-window`, `get-text`, `new-tab`,
// `launch`) to land one-by-one.
//
// Protocol: one JSON object per line (LF-terminated). Request shape:
//   {"cmd": "<name>", ...args}
// Response shape on success:
//   {"ok": true, ...result-fields}
// Response shape on error:
//   {"ok": false, "error": "<message>"}
//
// Socket path resolution (in order):
//   1. `$ANTS_REMOTE_SOCKET` env var — explicit override, used by
//      client + server together for multi-instance scenarios
//   2. `$XDG_RUNTIME_DIR/ants-terminal.sock` — XDG standard dir,
//      user-scoped, survives tmp-cleaner sweeps
//   3. `/tmp/ants-terminal-<uid>.sock` — fallback when XDG runtime
//      dir is unset (very unusual on modern Linux, but keeps the
//      fallback deterministic instead of failing silently)
//
// Server-side: if `listen()` fails because the path is already in use
// (another Ants instance owns it), we log and give up — remote-control
// is optional, we don't want to take the main window down with us.
// A future enhancement could fall back to a per-PID path; for now
// single-instance-per-user is the documented behaviour, and
// multi-instance users set the env var explicitly.
class RemoteControl : public QObject {
    Q_OBJECT

public:
    explicit RemoteControl(MainWindow *main, QObject *parent = nullptr);
    ~RemoteControl() override;

    // Start listening. Returns true on success; false if another
    // instance already owns the socket. Either way, MainWindow
    // construction continues (remote-control is non-critical).
    bool start();

    // Default socket path — see header doc for resolution order.
    static QString defaultSocketPath();

    // Client entry point — connects, sends one JSON request, reads
    // one JSON response, writes it to stdout. Called from main.cpp
    // when `--remote <cmd>` is passed. Returns process exit code
    // (0 on success, 1 on connect/parse error, 2 on server error
    // response).
    //
    // `command` is the raw command name (e.g. `"ls"`); `args` is an
    // already-constructed JSON object that will be merged into the
    // envelope under the `cmd` field at runtime.
    static int runClient(const QString &command,
                         const QJsonObject &args,
                         const QString &socketPath);

    // Strip C0 control bytes from a `send-text` payload to block
    // local-UID keystroke-injection attacks (ESC-based bracketed-paste
    // toggles, OSC 52 clipboard overwrites, cursor reprogramming).
    // Preserves HT (0x09), LF (0x0A), CR (0x0D) — those are regular
    // keystrokes in a PTY stream. C1 control codepoints (U+0080..U+009F)
    // are not stripped here: at the UTF-8 byte level they manifest as
    // continuation bytes (0x80..0xBF) inside multi-byte sequences for
    // ordinary characters, so a byte-oriented strip would mangle them.
    // Stripping C1 is the AI-dialog layer's job (`aidialog.cpp`), which
    // operates on QChar codepoints, not raw bytes.
    //
    // Returns the filtered payload. `out_stripped`, if non-null, is
    // set to the number of bytes removed — callers surface this in
    // the `stripped` response field.
    //
    // The `send-text` request JSON may carry `"raw": true` to bypass
    // this filter; see tests/features/remote_control_opt_in/spec.md.
    //
    // Defined inline so feature tests can exercise it without pulling
    // in the full MainWindow dep chain.
    static inline QByteArray filterControlChars(const QByteArray &in,
                                                int *out_stripped = nullptr) {
        QByteArray out;
        out.reserve(in.size());
        int removed = 0;
        for (char c : in) {
            const unsigned char b = static_cast<unsigned char>(c);
            const bool isAllowedWhitespace = (b == 0x09 || b == 0x0A || b == 0x0D);
            const bool isC0Bad = (b < 0x20) && !isAllowedWhitespace;
            const bool isDel = (b == 0x7F);
            if (isC0Bad || isDel) {
                ++removed;
                continue;
            }
            out.append(c);
        }
        if (out_stripped) *out_stripped = removed;
        return out;
    }

    // ANTS-1244: read-only verbs promoted to public so the MCP server
    // in ClaudeIntegration can delegate to them without duplicating
    // bodies. Provider lambdas in MainWindow::setupClaudeMcpProviders
    // call these on the existing m_remoteControl instance, sharing
    // the roadmap-query cache (INV-7 in the spec).
    //
    // ANTS-1247: cmdRoadmapQuery accepts an optional `status` filter
    // in `req`. Zero-arg-equivalent (empty req) is back-compat with
    // ANTS-1244 callers — returns the full unfiltered array.
    QJsonDocument cmdRoadmapQuery(const QJsonObject &req = {});
    QJsonDocument cmdTabList();
    QJsonDocument cmdGetText(const QJsonObject &req);

    // ANTS-1248: ripgrep wrapper. Public for the same reason as the
    // ANTS-1244 trio — MCP server lambda in MainWindow delegates here
    // so the body is reused across IPC + MCP transports. Argv-only
    // QProcess::start (no shell), 2 s hard-kill via constant
    // kWorkspaceSearchHardKillMs + 200 ms grace, server-clamped to
    // 500 results. See docs/specs/ANTS-1248.md.
    QJsonDocument cmdWorkspaceSearch(const QJsonObject &req);

    // ANTS-1249: file outline (regex scanner over a file, returns
    // header_doc + symbols[] for cpp / py / md / unknown). Shares
    // the pathInRepoRoot helper with cmdWorkspaceSearch.
    // See docs/specs/ANTS-1249.md.
    QJsonDocument cmdFileOutline(const QJsonObject &req);

    // ANTS-1250: git_state — single tool, dispatches on `op` field
    // (status / log / diff). Wraps gitwrap.cpp's shell-less QProcess
    // helper. Argv-only, --separator + ./ prefix on -leading paths,
    // strict regex on the diff range (rejects leading -). Public so
    // the MCP provider lambda in MainWindow delegates here.
    // See docs/specs/ANTS-1250.md.
    QJsonDocument cmdGitState(const QJsonObject &req);

    // ANTS-1251: subsystem — single tool, dispatches on `op` field
    // (map / files / recent_changes). Parses the project's CLAUDE.md
    // Module map, returns per-lane file lists, and (via cmdGitState
    // composition) per-lane git history. Public so the MCP provider
    // lambda in MainWindow delegates here.
    // See docs/specs/ANTS-1251.md.
    QJsonDocument cmdSubsystem(const QJsonObject &req);

    // ANTS-1254: last_audit_summary — opens latest .audit_cache/audit-*.sarif
    // and returns compact summary (counts + top_findings). Single-entry
    // mtime-keyed cache; SARIF parsing delegated to
    // AuditEngine::summariseSarif. See docs/specs/ANTS-1254.md.
    QJsonDocument cmdLastAuditSummary(const QJsonObject &req);

private slots:
    void onNewConnection();

private:
    QJsonDocument dispatch(const QJsonObject &req);
    QJsonDocument cmdLs();
    QJsonDocument cmdSendText(const QJsonObject &req);
    QJsonDocument cmdNewTab(const QJsonObject &req);
    QJsonDocument cmdSelectWindow(const QJsonObject &req);
    QJsonDocument cmdSetTitle(const QJsonObject &req);
    QJsonDocument cmdLaunch(const QJsonObject &req);

    // Cached parse of `m_main->roadmapPathForRemote()` content. Refreshed
    // on `roadmap-query` when EITHER the mtime advances OR the wall-clock
    // age of the cache exceeds `kRoadmapCacheTtlMs`. INV-10 contract is
    // both bounds ANDed: "≤ 100 ms cache lifetime" plus mtime detection
    // for the rare edit-then-re-edit-within-the-same-tick case.
    // ANTS-1123 indie-review F1 fold-in.
    mutable QString m_roadmapCachePath;
    mutable qint64 m_roadmapCacheMtimeMs = 0;
    mutable qint64 m_roadmapCacheStampMs = 0;  // epoch ms of last refresh
    mutable QJsonArray m_roadmapCacheBullets;
    static constexpr qint64 kRoadmapCacheTtlMs = 100;

    QLocalServer *m_server = nullptr;
    MainWindow *m_main;  // non-owning; MainWindow owns us via QObject parent

    // ANTS-1254 — single-entry summary cache. Keyed on
    // (path, mtime, topN, floor) per spec INV-2.
    mutable QString  m_auditSummaryPath;
    mutable qint64   m_auditSummaryMtimeMs    = 0;
    mutable AuditEngine::AuditSummary m_auditSummaryCache;
    mutable int      m_auditSummaryCachedTopN = -1;
    mutable QString  m_auditSummaryCachedFloor;
};
