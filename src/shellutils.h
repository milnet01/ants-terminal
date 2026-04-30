#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringLiteral>

// Shell-quote a string for safe inclusion in a shell command line. Returns
// the value wrapped in single quotes (with any embedded single quotes
// backslash-escaped) when it contains any character outside the safe set;
// empty strings become ''. Strings made entirely of safe characters are
// returned as-is so ordinary paths don't accumulate cosmetic quoting.
//
// 0.7.57 (2026-04-30 indie-review HIGH ANTS-1047) — switched from a
// denylist regex to a whitelist. The previous denylist
// `[\s'"\\$`!#&|;(){}]` missed glob wildcards (`*`, `?`), redirection
// (`<`, `>`), and bracket-expansion specials (`[`, `]`), so a path
// containing `*` reached the shell unquoted and underwent globbing.
// Whitelist closes the class — anything not `[A-Za-z0-9_\-./:@%+,]+`
// gets quoted. Single quotes inside the value are escaped as `'\''`
// (close-quote, escaped quote, reopen-quote) — POSIX shell standard.
inline QString shellQuote(const QString &s) {
    if (s.isEmpty()) return QStringLiteral("''");
    static const QRegularExpression kShellSafe(
        QStringLiteral("^[A-Za-z0-9_\\-./:@%+,]+$"));
    if (kShellSafe.match(s).hasMatch())
        return s;
    QString quoted = s;
    quoted.replace(QChar('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'") + quoted + QStringLiteral("'");
}
