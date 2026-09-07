// Re-implementations of shellQuote outside its canonical home
// (src/shellutils.h). Each is a second source of truth that will drift
// from the first. These are what the rule exists to catch.

#include <QString>

// @expect refactor_shell_quote_duplicate
static QString shellQuote(const QString &s) {
    return QLatin1Char('\'') + QString(s).replace('\'', "'\\''") + QLatin1Char('\'');
}

void buildCommand(const QString &path) {
    // @expect refactor_shell_quote_duplicate
    auto shellQuote = [](const QString &v) { return "'" + v + "'"; };
    (void)shellQuote(path);
}
