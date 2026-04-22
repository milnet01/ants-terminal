#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringLiteral>

// Shell-quote a string for safe inclusion in a shell command line. Returns
// the value wrapped in single quotes (with any embedded single quotes
// backslash-escaped) when it contains any character the shell might treat
// specially; empty strings become ''. Strings with no shell specials are
// returned as-is so ordinary paths don't accumulate cosmetic quoting.
inline QString shellQuote(const QString &s) {
    if (s.isEmpty()) return QStringLiteral("''");
    static const QRegularExpression kShellSpecials(
        QStringLiteral("[\\s'\"\\\\$`!#&|;(){}]"));
    if (!s.contains(kShellSpecials))
        return s;
    QString quoted = s;
    quoted.replace(QChar('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'") + quoted + QStringLiteral("'");
}
