#include "regexharden.h"

#include <QRegularExpression>

namespace ants::regex {

bool isCatastrophicRegex(const QString &pattern) {
    // Shape A — quantifier-under-quantifier inside a group: `(.+)+`, `(a*)+`,
    // `(a+b)*`, etc.
    static const QRegularExpression nestedQuant(
        QStringLiteral(R"(\([^()]*[+*][^()]*\)[?*+])"));
    // Shape B — alternation-under-quantifier inside a group: `(a|b)+`,
    // `(a|aa)*`, `(x|y|z)+`.
    static const QRegularExpression altQuant(
        QStringLiteral(R"(\([^()]*\|[^()]*\)[?*+])"));
    return nestedQuant.match(pattern).hasMatch()
           || altQuant.match(pattern).hasMatch();
}

QString hardenUserRegex(const QString &pattern) {
    if (pattern.isEmpty()) return {};
    if (pattern.startsWith(QStringLiteral("(*LIMIT_"))) return pattern;
    return QStringLiteral("(*LIMIT_MATCH=100000)") + pattern;
}

}  // namespace ants::regex
