#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <mutex>

// DebugLog — centralized debug logging for Ants Terminal.
//
// Enabled via env var `ANTS_DEBUG=<categories>` at launch, or
// toggled at runtime via Tools → Debug menu. Writes timestamped
// lines to `~/.local/share/ants-terminal/debug.log` (plus stderr
// when the env var was set — keeps `2>` redirection useful).
//
// Categories are an enum bitmask; multiple can be active at once.
// Log sites use the ANTS_LOG(category, format, ...) macro so the
// hot path is a single bit-test when the category is disabled.
class DebugLog {
public:
    enum Category : quint32 {
        None       = 0,
        Paint      = 1u << 0,  // QPaintEvent / UpdateRequest / LayoutRequest
        Events     = 1u << 1,  // focus / key / mouse / resize / general QEvent
        Input      = 1u << 2,  // keystrokes + mouse events routed to terminal
        Pty        = 1u << 3,  // PTY reads/writes/resize
        Vt         = 1u << 4,  // VT parser actions + grid mutations
        Render     = 1u << 5,  // paintEvent latency + glyph cache stats
        Plugins    = 1u << 6,  // Lua plugin events + calls
        Network    = 1u << 7,  // AI endpoint requests, SSH, git subprocess
        Config     = 1u << 8,  // config load/save/change
        Audit      = 1u << 9,  // audit-dialog tool invocations + findings
        Claude     = 1u << 10, // Claude Code integration (hooks, detection, transcript)
        Signals    = 1u << 11, // Qt signal-slot firings of tracked signals
        Shell      = 1u << 12, // OSC 133 / shell integration / HMAC verification
        Session    = 1u << 13, // session persistence save/restore
        Perf       = 1u << 14, // event-loop stalls, slow handlers, budget overruns
        Scrollback = 1u << 15, // ANTS-1118 — scrollback pin / m_scrollOffset / overwrite-during-stream traces
        All        = 0xFFFFFFFFu,
    };

    // Parse a comma-separated list of category names (case-insensitive,
    // "all" enables everything) into a bitmask. Recognised tokens match
    // the lowercased enum names.
    static quint32 parseCategories(const QString &spec);

    // Active categories. Modified at runtime via the menu.
    static quint32 active();
    static void setActive(quint32 mask);
    static bool enabled(Category c) { return (active() & c) != 0; }

    // Log file path (created on first write). Fixed location:
    // ~/.local/share/ants-terminal/debug.log.
    static QString logFilePath();

    // Clear the log (truncates the file). Safe to call at any time.
    static void clear();

    // Write one line. Do not call directly; use ANTS_LOG.
    static void write(Category c, const QString &message);

    // Available category names (for the menu builder).
    static QStringList categoryNames();
    static Category categoryFor(const QString &name);
    static const char *nameFor(Category c);

private:
    static std::mutex s_mutex;
    static QFile s_file;
    static quint32 s_active;
    static bool s_alsoStderr;
};

// Single-point macro so sites are zero-cost when the category is off.
// Usage: ANTS_LOG(DebugLog::Paint, "widget=%s rect=%dx%d", cls, w, h);
#define ANTS_LOG(cat, fmt, ...) \
    do { \
        if (DebugLog::enabled(cat)) { \
            DebugLog::write(cat, QString::asprintf(fmt, ##__VA_ARGS__)); \
        } \
    } while (0)

// Convenience: log a message always (ignores category filter).
#define ANTS_LOG_ALWAYS(fmt, ...) \
    DebugLog::write(DebugLog::None, QString::asprintf(fmt, ##__VA_ARGS__))
