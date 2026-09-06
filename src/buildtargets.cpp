// ANTS-3745 — see buildtargets.h.

#include "buildtargets.h"

#include <QRegularExpression>
#include <QSet>

namespace BuildTargets {
namespace {

// The three declaring commands, and the kind each yields. `ants_add_*_bundle`
// is matched by shape rather than by listing the two wrappers, so a third
// wrapper added later is picked up without editing this file — the property
// that stops this parser going stale silently.
const QRegularExpression &declRe() {
    static const QRegularExpression re(QStringLiteral(
        R"(^[ \t]*(add_library|add_executable|ants_add_[A-Za-z0-9_]*bundle)[ \t]*\(\s*([A-Za-z0-9_.:+-]+))"));
    return re;
}

// A token is a source when it carries a directory separator and a C/C++
// suffix. The separator is what keeps `STATIC`, `SOURCES`, `ants_core_lib` and
// a bare `GTest::gtest` out without a keyword state machine.
bool looksLikeSource(const QString &tok) {
    if (!tok.contains(QLatin1Char('/'))) return false;
    static const QStringList kSuffixes = {
        QStringLiteral(".cpp"), QStringLiteral(".cc"),  QStringLiteral(".cxx"),
        QStringLiteral(".c"),   QStringLiteral(".hpp"), QStringLiteral(".h")};
    for (const QString &s : kSuffixes)
        if (tok.endsWith(s)) return true;
    return false;
}

// A generator expression can WRAP a path — `$<$<BOOL:${X}>:src/a.cpp>` — so
// the brackets are peeled rather than the whole token dropped. A path still
// carrying `$` after peeling is variable-driven and is left unresolved, which
// buildtargets.h states.
QString peel(QString tok) {
    tok.remove(QLatin1Char('"'));
    const int colon = tok.lastIndexOf(QLatin1Char(':'));
    if (tok.startsWith(QLatin1String("$<")) && colon > 0)
        tok = tok.mid(colon + 1);
    while (tok.endsWith(QLatin1Char('>'))) tok.chop(1);
    return tok.trimmed();
}

}  // namespace

QList<Target> parse(const QString &cmakeText) {
    const QStringList lines = cmakeText.split(QLatin1Char('\n'));
    QList<Target> out;

    for (int i = 0; i < lines.size(); ++i) {
        const auto m = declRe().match(lines.at(i));
        if (!m.hasMatch()) continue;

        Target t;
        t.command = m.captured(1);
        t.name    = m.captured(2);
        t.line    = i + 1;
        t.kind    = t.command == QLatin1String("add_library")
                        ? QStringLiteral("library")
                    : t.command == QLatin1String("add_executable")
                        ? QStringLiteral("executable")
                        : QStringLiteral("bundle");
        // A name that is itself a variable reference is a wrapper's own
        // expansion (`add_executable(${name} …)` inside the function body),
        // not a target this project declares.
        if (t.name.startsWith(QLatin1String("${"))) continue;

        // Walk to the matching close paren, counting depth from the declaring
        // line's own text so a one-line `add_library(x STATIC src/a.cpp)`
        // terminates correctly.
        int depth = 0;
        for (int j = i; j < lines.size(); ++j) {
            QString line = lines.at(j);
            const int hash = line.indexOf(QLatin1Char('#'));
            if (hash >= 0) line = line.left(hash);

            for (const QChar c : line) {
                if (c == QLatin1Char('(')) ++depth;
                else if (c == QLatin1Char(')')) --depth;
            }

            static const QRegularExpression kTokSep(
                QStringLiteral(R"([\s()]+)"));
            const QStringList toks =
                line.split(kTokSep, Qt::SkipEmptyParts);
            for (const QString &raw : toks) {
                const QString tok = peel(raw);
                if (looksLikeSource(tok) && !tok.contains(QLatin1Char('$')))
                    t.sources.append(tok);
            }
            if (depth <= 0) break;
        }
        out.append(t);
    }
    return out;
}

QList<Target> ownersOf(const QList<Target> &targets, const QString &relPath) {
    QList<Target> out;
    for (const Target &t : targets)
        if (t.sources.contains(relPath)) out.append(t);
    return out;
}

QStringList gtestSuites(const QString &sourceText) {
    // Anchored at line start so a suite named inside a comment's prose or a
    // string does not count; `TEST_F` / `TEST_P` share the form.
    static const QRegularExpression re(
        QStringLiteral(R"(^[ \t]*TEST(?:_F|_P)?[ \t]*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)"),
        QRegularExpression::MultilineOption);
    QStringList out;
    QSet<QString> seen;
    auto it = re.globalMatch(sourceText);
    while (it.hasNext()) {
        const QString s = it.next().captured(1);
        if (!seen.contains(s)) { seen.insert(s); out.append(s); }
    }
    return out;
}

}  // namespace BuildTargets
