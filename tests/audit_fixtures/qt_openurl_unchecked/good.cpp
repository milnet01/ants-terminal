// Safe patterns — MUST NOT match the rule's literal pattern. The audit-dialog
// filter additionally drops lines containing "scheme() ==", "validScheme",
// "allowScheme", "isLocal", "https://", or "QUrl::TolerantMode" even when the
// raw regex matches — but this fixture tests the regex arm, so we avoid the
// literal entirely and route all link opens through an internal wrapper that
// front-loads the check.

#include <QUrl>
#include <QString>

static bool isAllowedScheme(const QUrl &u) {
    const QString s = u.scheme().toLower();
    return s == "http" || s == "https" || s == "mailto";
}

void safe_open(const QString &raw) {
    QUrl u(raw);
    if (!isAllowedScheme(u)) return;
    // Delegated to a wrapper `openAllowedUrl()` defined elsewhere — the
    // wrapper is the only place in the project that calls the real Qt API.
}
