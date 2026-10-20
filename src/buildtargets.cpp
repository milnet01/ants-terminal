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

// One command: its name and its arguments, quotes removed and `\"` unescaped.
struct Command {
    QString name;
    QStringList args;
};

// Every `name(args)` in `text`, comments dropped. A quoted argument may hold
// spaces and parentheses; an unquoted one ends at whitespace or a paren.
QList<Command> commands(const QString &text) {
    QList<Command> out;
    const int n = text.size();
    int i = 0;
    auto skipComment = [&](int at) {
        while (at < n && text.at(at) != QLatin1Char('\n')) ++at;
        return at;
    };
    while (i < n) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('#')) { i = skipComment(i); continue; }
        if (!(c.isLetter() || c == QLatin1Char('_'))) { ++i; continue; }
        int j = i;
        while (j < n && (text.at(j).isLetterOrNumber() || text.at(j) == QLatin1Char('_'))) ++j;
        int k = j;
        while (k < n && (text.at(k) == QLatin1Char(' ') || text.at(k) == QLatin1Char('\t'))) ++k;
        if (k >= n || text.at(k) != QLatin1Char('(')) { i = j; continue; }
        Command cmd;
        cmd.name = text.mid(i, j - i).toLower();
        int depth = 1;
        QString tok;
        bool inTok = false;
        auto flush = [&] {
            if (inTok) cmd.args.append(tok);
            tok.clear();
            inTok = false;
        };
        for (k = k + 1; k < n && depth > 0; ++k) {
            const QChar d = text.at(k);
            if (d == QLatin1Char('"') && !inTok) {
                inTok = true;
                for (++k; k < n && text.at(k) != QLatin1Char('"'); ++k) {
                    if (text.at(k) == QLatin1Char('\\') && k + 1 < n) ++k;
                    tok += text.at(k);
                }
                flush();
            } else if (d == QLatin1Char('#') && !inTok) {
                k = skipComment(k);
            } else if (d.isSpace()) {
                flush();
            } else if (d == QLatin1Char('(')) {
                flush();
                ++depth;
            } else if (d == QLatin1Char(')')) {
                flush();
                --depth;
            } else {
                if (d == QLatin1Char('\\') && k + 1 < n) tok += text.at(++k);
                else tok += d;
                inTok = true;
            }
        }
        out.append(cmd);
        i = k;
    }
    return out;
}

bool isRootVar(const QString &v) {
    return v == QLatin1String("CMAKE_SOURCE_DIR") || v == QLatin1String("PROJECT_SOURCE_DIR")
        || v == QLatin1String("CMAKE_CURRENT_SOURCE_DIR");
}

// `${CMAKE_SOURCE_DIR}/src/a.cpp` -> `src/a.cpp`; `${CMAKE_SOURCE_DIR}` -> "".
// A plain relative path is kept; anything else still holding `$` is not a
// path this can name, and comes back null.
QString rootRelative(const QString &value) {
    static const QRegularExpression re(QStringLiteral(R"(^\$\{([A-Za-z_]\w*)\}(.*)$)"));
    const auto m = re.match(value);
    if (m.hasMatch()) {
        if (!isRootVar(m.captured(1))) return QString();
        QString rest = m.captured(2);
        while (rest.startsWith(QLatin1Char('/'))) rest.remove(0, 1);
        return rest.isNull() ? QStringLiteral("") : rest;
    }
    if (value.contains(QLatin1Char('$'))) return QString();
    return value;
}

}  // namespace

QHash<QString, QStringList> pathDefinitions(const QString &cmakeText) {
    QHash<QString, QStringList> vars;
    QHash<QString, QStringList> defs;
    static const QRegularExpression wholeVar(QStringLiteral(R"(^\$\{([A-Za-z_]\w*)\}$)"));
    static const QRegularExpression defRe(
        QStringLiteral(R"re(^(?:-D)?([A-Za-z_]\w*)="?(.*?)"?$)re"));
    const auto expand = [&](const QStringList &items) {
        QStringList out;
        for (const QString &it : items) {
            const auto m = wholeVar.match(it);
            if (m.hasMatch() && vars.contains(m.captured(1))) out += vars.value(m.captured(1));
            else out.append(it);
        }
        return out;
    };

    for (const Command &c : commands(cmakeText)) {
        const QStringList &a = c.args;
        if (c.name == QLatin1String("set") && !a.isEmpty()) {
            vars[a.first()] = expand(a.mid(1));
        } else if (c.name == QLatin1String("list") && a.size() >= 2) {
            const QString op = a.at(0), name = a.at(1);
            if (op == QLatin1String("APPEND")) {
                vars[name] += expand(a.mid(2));
            } else if (op == QLatin1String("TRANSFORM")) {
                // The transform's PREPEND/APPEND text is dropped: the items
                // stay root-relative, which is what a reader names.
                const int out = a.indexOf(QStringLiteral("OUTPUT_VARIABLE"));
                if (out >= 0 && out + 1 < a.size()) vars[a.at(out + 1)] = vars.value(name);
            } else if (op == QLatin1String("JOIN") && a.size() >= 4) {
                vars[a.at(3)] = vars.value(name);
            }
        }
        for (const QString &arg : a) {
            const auto m = defRe.match(arg);
            if (!m.hasMatch()) continue;
            const QString value = m.captured(2);
            QStringList paths;
            const auto var = wholeVar.match(value);
            if (var.hasMatch() && !isRootVar(var.captured(1))) {
                for (const QString &item : vars.value(var.captured(1))) {
                    const QString rel = rootRelative(item);
                    if (!rel.isEmpty()) paths.append(rel);
                }
            } else if (value.startsWith(QLatin1String("${"))) {
                const QString rel = rootRelative(value);
                if (!rel.isNull()) paths.append(rel);
            }
            if (!paths.isEmpty()) defs[m.captured(1)] = paths;
        }
    }
    return defs;
}

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
