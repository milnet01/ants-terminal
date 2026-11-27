#include "regexharden.h"

#include <QRegularExpression>

namespace ants::regex {

bool isCatastrophicRegex(const QString &pattern) {
    // BEST-EFFORT (ANTS-1758): catches the two grouped shapes below.
    // Ungrouped consecutive quantified atoms (`a+a+`) and some deep
    // nesting are NOT caught — `hardenUserRegex`'s (*LIMIT_MATCH) is the
    // real runtime backstop. See the header for the full caveat.
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
    // Always prefixed: PCRE2 uses the lower of two settings of one limit, so
    // a caller's own (*LIMIT_MATCH=…) can only lower ours, and a
    // (*LIMIT_HEAP=…) or (*LIMIT_DEPTH=…) prefix no longer skips it.
    return QStringLiteral("(*LIMIT_MATCH=100000)") + pattern;
}

}  // namespace ants::regex
