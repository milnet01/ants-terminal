// ANTS-1301 — recent_errors scrollback parser.
//
// Pure function: given a terminal-scrollback string, extract a
// structured errors[] list (file:line + message + category) with a
// per-language regex bank — GCC/clang `error:`, ruff/flake8-style
// `file:line:col: CODE`, `lua: file:line:`, ctest `(Failed)` /
// `***Failed`, and multi-line Python tracebacks.
//
// Powers the `recent_errors` MCP tool: the handler reads the focused
// terminal's recent scrollback (TerminalWidget::recentOutput) and
// hands the string here. No GUI / terminal / filesystem deps — lives
// in ants_core_lib next to fileoutline / buildcache.
//
// See docs/specs/ANTS-1301.md.

#ifndef ANTS_SCROLLBACKERRORS_H
#define ANTS_SCROLLBACKERRORS_H

#include <QString>
#include <QVector>

namespace ScrollbackErrors {

struct ErrorEntry {
    QString category;   // "compiler" | "lint" | "lua" | "test" | "python"
    QString file;       // captured path, or empty
    int     line = 0;   // captured source line, or 0
    int     column = 0; // captured column, or 0
    QString message;    // the error message
    QString text;       // matched scrollback line (trimmed); anchor line for multi-line
};

// maxResults: ≤0 ⇒ default 50; >1000 ⇒ clamped to 1000.
struct Options { int maxResults = 0; };

struct Result {
    QVector<ErrorEntry> errors;
    int  errorsTotal  = 0;   // pre-cap count of all matches
    int  linesScanned = 0;
    bool truncated    = false;
};

// Single forward pass. Splits on '\n', strips a trailing '\r' per
// line. When more than the (clamped) maxResults are found, keeps the
// LAST maxResults (the newest), sets truncated, and errorsTotal
// carries the full pre-cap count.
Result parse(const QString &scrollback, const Options &opts);

}  // namespace ScrollbackErrors

#endif  // ANTS_SCROLLBACKERRORS_H
