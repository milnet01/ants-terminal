#pragma once

#include <QObject>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>

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

private slots:
    void onNewConnection();

private:
    QJsonDocument dispatch(const QJsonObject &req);
    QJsonDocument cmdLs();
    QJsonDocument cmdSendText(const QJsonObject &req);
    QJsonDocument cmdNewTab(const QJsonObject &req);
    QJsonDocument cmdSelectWindow(const QJsonObject &req);
    QJsonDocument cmdSetTitle(const QJsonObject &req);
    QJsonDocument cmdGetText(const QJsonObject &req);
    QJsonDocument cmdLaunch(const QJsonObject &req);

    QLocalServer *m_server = nullptr;
    MainWindow *m_main;  // non-owning; MainWindow owns us via QObject parent
};
