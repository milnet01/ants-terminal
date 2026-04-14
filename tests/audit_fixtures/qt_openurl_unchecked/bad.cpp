// Fixture for qt_openurl_unchecked. Pattern: Qt desktop-services URL opens
// without a scheme check. Each @expect marker is a call that doesn't
// front-load an allowlist. The good-side shows the safe patterns. Comments
// intentionally avoid the literal pattern string (it'd be counted as a match
// by the grep even inside `//` since the regex is line-based, not AST-aware).

#include <QDesktopServices>
#include <QUrl>
#include <QString>

void opens_unchecked(const QString &raw) {
    QUrl u(raw);
    QDesktopServices::openUrl(u);  // @expect qt_openurl_unchecked
}

void opens_user_link(const QString &href) {
    QDesktopServices::openUrl(QUrl(href));  // @expect qt_openurl_unchecked
}
