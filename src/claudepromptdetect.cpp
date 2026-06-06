// ANTS-1993 — see claudepromptdetect.h for the threat model.

#include "claudepromptdetect.h"

#include <QRegularExpression>
#include <QString>

namespace ClaudePromptDetect {

bool isPermissionPromptStructure(const QStringList &recentLines) {
    // A numbered selection option: optional "❯"/">" cursor, a digit,
    // "." or ")", then "Yes" or "No". Matches both "❯ 1. Yes" and
    // "  2. No, and tell Claude…".
    static const QRegularExpression optionRe(
        QStringLiteral(R"(^[>❯]?\s*[1-9][.)]\s+(?:Yes|No)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    // A bare y/n choice line: "y · yes", "n · no", "y/n", "(y/n)".
    static const QRegularExpression ynRe(
        QStringLiteral(R"(\by\s*[·/]\s*yes\b|\(y/n\)|\by/n\b)"),
        QRegularExpression::CaseInsensitiveOption);

    bool hasAnchor = false;
    bool hasStrong = false;
    bool hasSelection = false;

    for (const QString &raw : recentLines) {
        const QString t = raw.trimmed();
        if (t.isEmpty()) continue;

        const bool tabAccept = t.contains(QLatin1String("Tab to accept"));
        if (tabAccept || t.contains(QLatin1String("Ctrl+e to explain")))
            hasStrong = true;
        if (tabAccept ||
            t.contains(QLatin1String("Do you want to proceed")) ||
            t.contains(QLatin1String("allow access to")) ||
            t.contains(QLatin1String("always allow")))
            hasAnchor = true;
        if (t.startsWith(QChar(0x276F)) /* ❯ */ ||
            t.contains(QLatin1String("Esc to cancel")) ||
            optionRe.match(t).hasMatch() ||
            ynRe.match(t).hasMatch())
            hasSelection = true;
    }

    return hasAnchor && (hasStrong || hasSelection);
}

}  // namespace ClaudePromptDetect
