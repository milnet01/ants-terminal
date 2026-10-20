// ANTS-5602 — see sourcescrapes.h. Contract:
// tests/features/mcp_source_scrapes/spec.md.
//
// A test file is cut into statements (comments, preprocessor lines and
// string contents removed, each literal replaced by a placeholder), then
// walked once: a statement that reads a path binds the variable it assigns,
// and the literal arguments of a scrape call on a reading value are
// anchors. Deliberately looser than a C++ parser; what it cannot follow is
// counted or named in not_covered, never guessed.

#include "sourcescrapes.h"

#include "buildtargets.h"
#include "codewalk.h"
#include "pathvalidation.h"
#include "projectsettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace SourceScrapes {
namespace {

constexpr QChar kLitOpen = QChar(0x01);
constexpr QChar kLitClose = QChar(0x02);
inline constexpr int kDefaultLimit = 500;
inline constexpr int kMaxLimit = 5000;

struct Literal {
    QString text;
    int line = 0;
};

struct Event {
    enum Kind { Stmt, Open, Close } kind = Stmt;
    QString code;
    int line = 0;
};

bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

// Cuts `text` into statements and brace events. A brace inside parentheses
// (a lambda or an initialiser list in an argument) stays in its statement.
void lex(const QString &text, QList<Event> *events, QList<Literal> *lits) {
    const int n = text.size();
    int line = 1;
    int paren = 0;
    QString code;
    int codeLine = 0;
    bool lineStart = true;
    const auto put = [&](const QString &s) {
        if (code.trimmed().isEmpty() && !s.trimmed().isEmpty()) codeLine = line;
        code += s;
    };
    const auto emitStmt = [&](Event::Kind kind) {
        if (kind == Event::Stmt && code.trimmed().isEmpty()) {
            code.clear();
            return;
        }
        events->append({kind, code, codeLine ? codeLine : line});
        code.clear();
        codeLine = 0;
    };
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n')) {
            ++line;
            lineStart = true;
            code += QLatin1Char(' ');
            continue;
        }
        if (lineStart && c.isSpace()) continue;
        if (lineStart && c == QLatin1Char('#')) {
            // A preprocessor line, with its backslash continuations.
            while (i < n && text.at(i) != QLatin1Char('\n')) {
                if (text.at(i) == QLatin1Char('\\') && i + 1 < n && text.at(i + 1) == QLatin1Char('\n')) {
                    ++line;
                    ++i;
                }
                ++i;
            }
            --i;
            continue;
        }
        lineStart = false;
        if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('/')) {
            while (i + 1 < n && text.at(i + 1) != QLatin1Char('\n')) ++i;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('*')) {
            for (i += 2; i + 1 < n && !(text.at(i) == QLatin1Char('*') && text.at(i + 1) == QLatin1Char('/')); ++i)
                if (text.at(i) == QLatin1Char('\n')) ++line;
            ++i;
            code += QLatin1Char(' ');
            continue;
        }
        if (c == QLatin1Char('"')) {
            // The prefix (R, u8, u8R, L…) is taken off the statement text so a
            // placeholder reads the same whatever kind of literal it was.
            int p = code.size();
            while (p > 0 && isIdentChar(code.at(p - 1))) --p;
            const QString prefix = code.mid(p);
            static const QStringList kRaw = {QStringLiteral("R"), QStringLiteral("u8R"),
                                             QStringLiteral("uR"), QStringLiteral("UR"),
                                             QStringLiteral("LR")};
            static const QStringList kPlain = {QStringLiteral("u8"), QStringLiteral("u"),
                                               QStringLiteral("U"), QStringLiteral("L")};
            const bool raw = kRaw.contains(prefix);
            if (raw || kPlain.contains(prefix)) code.chop(prefix.size());
            Literal lit;
            lit.line = line;
            if (raw) {
                const int open = text.indexOf(QLatin1Char('('), i + 1);
                if (open < 0) break;
                const QString close = QLatin1Char(')') + text.mid(i + 1, open - i - 1) + QLatin1Char('"');
                const int end = text.indexOf(close, open + 1);
                if (end < 0) break;
                lit.text = text.mid(open + 1, end - open - 1);
                line += int(lit.text.count(QLatin1Char('\n')));
                i = end + close.size() - 1;
            } else {
                int j = i + 1;
                while (j < n && text.at(j) != QLatin1Char('"') && text.at(j) != QLatin1Char('\n')) {
                    if (text.at(j) == QLatin1Char('\\') && j + 1 < n) ++j;
                    ++j;
                }
                lit.text = text.mid(i + 1, j - i - 1);
                i = j;
            }
            put(QString(kLitOpen) + QString::number(lits->size()) + kLitClose);
            lits->append(lit);
            continue;
        }
        if (c == QLatin1Char('\'')) {
            // A digit separator (1'000) is not a character literal.
            if (!code.isEmpty() && code.back().isLetterOrNumber()) continue;
            int j = i + 1;
            while (j < n && text.at(j) != QLatin1Char('\'') && text.at(j) != QLatin1Char('\n')) {
                if (text.at(j) == QLatin1Char('\\') && j + 1 < n) ++j;
                ++j;
            }
            put(QStringLiteral("' '"));
            i = j;
            continue;
        }
        if (c == QLatin1Char('(')) ++paren;
        if (c == QLatin1Char(')') && paren > 0) --paren;
        if (paren == 0 && c == QLatin1Char('{')) {
            emitStmt(Event::Open);
            continue;
        }
        if (paren == 0 && c == QLatin1Char('}')) {
            emitStmt(Event::Stmt);
            events->append({Event::Close, QString(), line});
            continue;
        }
        if (paren == 0 && c == QLatin1Char(';')) {
            emitStmt(Event::Stmt);
            continue;
        }
        put(QString(c));
    }
    emitStmt(Event::Stmt);
}

// The literal indices of an argument made only of literals ("a" "b"), else
// empty.
QList<int> literalsOnly(const QString &arg) {
    static const QRegularExpression re(QStringLiteral("^\\s*(?:\\x01\\d+\\x02\\s*)+$"));
    static const QRegularExpression wrapped(QStringLiteral(
        "^\\s*(?:QStringLiteral|QLatin1String|QLatin1StringView|QByteArrayLiteral|QString|"
        "QByteArray|std::string|std::string_view)\\s*\\((.*)\\)\\s*$"));
    QString a = arg;
    const auto w = wrapped.match(a);
    if (w.hasMatch()) a = w.captured(1);
    if (!re.match(a).hasMatch()) return {};
    QList<int> out;
    static const QRegularExpression idx(QStringLiteral("\\x01(\\d+)\\x02"));
    for (auto it = idx.globalMatch(a); it.hasNext();) out.append(it.next().captured(1).toInt());
    return out;
}

// The literals inside a regex construction in `arg`, else empty.
QList<int> regexLiterals(const QString &arg) {
    static const QRegularExpression re(
        QStringLiteral("(?:std::w?regex|QRegularExpression)\\s*[({]\\s*(.*)$"));
    const auto m = re.match(arg);
    if (!m.hasMatch()) return {};
    QList<int> out;
    static const QRegularExpression idx(QStringLiteral("\\x01(\\d+)\\x02"));
    for (auto it = idx.globalMatch(m.captured(1)); it.hasNext();)
        out.append(it.next().captured(1).toInt());
    return out;
}

// Top-level arguments of the call whose '(' is at `open`; `*end` gets the
// index of its ')'.
QStringList callArgs(const QString &code, int open, int *end) {
    QStringList args;
    int depth = 0;
    int start = open + 1;
    for (int i = open; i < code.size(); ++i) {
        const QChar c = code.at(i);
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            if (--depth == 0) {
                args.append(code.mid(start, i - start));
                *end = i;
                return args;
            }
        } else if (c == QLatin1Char(',') && depth == 1) {
            args.append(code.mid(start, i - start));
            start = i + 1;
        }
    }
    *end = code.size();
    return args;
}

class Pass {
public:
    Pass(const QString &text, const Readers &r, const std::function<bool(const QString &)> &existing,
         QHash<QString, QSet<QString>> *exportHelpers)
        : m_r(r), m_existing(existing), m_export(exportHelpers) {
        lex(text, &m_events, &m_lits);
        static const QRegularExpression ident(QStringLiteral("\\b[A-Za-z_]\\w*\\b"));
        for (const Event &e : std::as_const(m_events))
            for (auto it = ident.globalMatch(e.code); it.hasNext();) {
                const QString id = it.next().captured(0);
                if (m_r.dirTokens.contains(id)) m_dirs.insert(m_r.dirTokens.value(id));
            }
    }

    FileResult run() {
        for (const Event &e : std::as_const(m_events)) {
            if (e.kind == Event::Open) open(e);
            else if (e.kind == Event::Close) close();
            else statement(e.code, e.line);
        }
        return m_out;
    }

private:
    struct Item {
        QString test;
        QString function;
        QSet<QString> returnReads;
    };

    void open(const Event &e) {
        static const QRegularExpression ns(QStringLiteral("^\\s*(?:inline\\s+)?namespace\\b|^\\s*extern\\s*\\x01"));
        const bool isNs = ns.match(e.code).hasMatch();
        if (!isNs && m_itemDepth == 0) {
            m_item = Item();
            m_locals.clear();
            m_localRegex.clear();
            m_localLits.clear();
            static const QRegularExpression test(
                QStringLiteral("^\\s*TEST(?:_F|_P)?\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)"));
            static const QRegularExpression lambda(QStringLiteral("(\\w+)\\s*=\\s*\\["));
            static const QRegularExpression fn(QStringLiteral("(\\w+)\\s*\\("));
            const auto t = test.match(e.code);
            if (t.hasMatch()) {
                m_item.test = t.captured(1) + QLatin1Char('.') + t.captured(2);
            } else if (const auto l = lambda.match(e.code); l.hasMatch()) {
                m_item.function = l.captured(1);
            } else if (const auto f = fn.match(e.code); f.hasMatch() && !e.code.contains(QLatin1Char('='))) {
                m_item.function = f.captured(1);
            }
        } else if (!isNs) {
            statement(e.code, e.line);
        }
        m_braces.append(isNs);
        if (!isNs) ++m_itemDepth;
    }

    void close() {
        if (m_braces.isEmpty()) return;
        const bool isNs = m_braces.takeLast();
        if (isNs) return;
        if (--m_itemDepth > 0) return;
        if (!m_item.function.isEmpty() && !m_item.returnReads.isEmpty()) {
            m_fileHelpers.insert(m_item.function, m_item.returnReads);
            if (m_export) m_export->insert(m_item.function, m_item.returnReads);
        }
        m_item = Item();
        m_locals.clear();
        m_localRegex.clear();
        m_localLits.clear();
    }

    QHash<QString, QSet<QString>> &bindings() { return m_itemDepth == 0 ? m_fileBindings : m_locals; }
    QHash<QString, QList<int>> &regexVars() { return m_itemDepth == 0 ? m_fileRegex : m_localRegex; }
    QHash<QString, QList<int>> &literalVars() { return m_itemDepth == 0 ? m_fileLits : m_localLits; }

    QSet<QString> boundPaths(const QString &id) const {
        if (m_locals.contains(id)) return m_locals.value(id);
        return m_fileBindings.value(id);
    }

    QList<int> regexVar(const QString &id) const {
        if (m_localRegex.contains(id)) return m_localRegex.value(id);
        return m_fileRegex.value(id);
    }

    // A variable holding only a literal (`const std::string call = "f(";`).
    QList<int> literalVar(const QString &id) const {
        if (m_localLits.contains(id)) return m_localLits.value(id);
        return m_fileLits.value(id);
    }

    QSet<QString> helperPaths(const QString &id) const {
        if (m_fileHelpers.contains(id)) return m_fileHelpers.value(id);
        return m_r.helpers.value(id);
    }

    // What `code` reads: tokens, helper calls, bound variables and, where the
    // file names a directory token, source-shaped literals under it.
    QSet<QString> reads(const QString &code, int line, bool record) {
        QSet<QString> out;
        static const QRegularExpression ident(QStringLiteral("\\b[A-Za-z_]\\w*\\b"));
        for (auto it = ident.globalMatch(code); it.hasNext();) {
            const auto m = it.next();
            const QString id = m.captured(0);
            int before = m.capturedStart() - 1;
            while (before >= 0 && code.at(before).isSpace()) --before;
            const bool member = before >= 0
                && (code.at(before) == QLatin1Char('.')
                    || (code.at(before) == QLatin1Char('>') && before > 0
                        && code.at(before - 1) == QLatin1Char('-')));
            if (member) continue;
            int after = m.capturedEnd();
            while (after < code.size() && code.at(after).isSpace()) ++after;
            const bool called = after < code.size() && code.at(after) == QLatin1Char('(');
            QSet<QString> direct;
            if (m_r.fileTokens.contains(id)) {
                const QStringList p = m_r.fileTokens.value(id);
                direct = QSet<QString>(p.cbegin(), p.cend());
            } else if (called) {
                direct = helperPaths(id);
            }
            if (!direct.isEmpty()) {
                out += direct;
                if (record) m_out.reads.append({m_item.test, line, id, direct});
            }
            out += boundPaths(id);
        }
        if (!m_dirs.isEmpty()) {
            static const QRegularExpression idx(QStringLiteral("\\x01(\\d+)\\x02"));
            static const QRegularExpression shape(
                QStringLiteral("^/?[\\w.+-]+(?:/[\\w.+-]+)*\\.[A-Za-z0-9]+$"));
            for (auto it = idx.globalMatch(code); it.hasNext();) {
                const Literal &lit = m_lits.at(it.next().captured(1).toInt());
                if (!shape.match(lit.text).hasMatch()) continue;
                for (const QString &dir : std::as_const(m_dirs)) {
                    QString rel = QDir::cleanPath(dir.isEmpty() ? lit.text : dir + QLatin1Char('/') + lit.text);
                    while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
                    if (rel.startsWith(QLatin1String("..")) || !m_existing(rel)) continue;
                    out.insert(rel);
                    if (record)
                        m_out.reads.append({m_item.test, lit.line,
                                            QLatin1Char('"') + lit.text + QLatin1Char('"'), {rel}});
                }
            }
        }
        return out;
    }

    void addAnchor(const QList<int> &lits, bool regex, const QSet<QString> &paths) {
        if (lits.isEmpty() || paths.isEmpty()) return;
        QString text;
        for (int i : lits) text += m_lits.at(i).text;
        const int line = m_lits.at(lits.first()).line;
        const QString key = QStringLiteral("%1:%2:%3").arg(line).arg(regex).arg(text);
        if (m_seen.contains(key)) return;
        m_seen.insert(key);
        m_out.anchors.append({m_item.test, line, text, regex, paths});
    }

    // One argument used as a search pattern: a literal, a regex built from
    // literals, or a regex variable. False when it is none of these.
    bool anchorArg(const QString &arg, bool regexCall, const QSet<QString> &paths) {
        if (const QList<int> l = literalsOnly(arg); !l.isEmpty()) {
            addAnchor(l, regexCall, paths);
            return true;
        }
        if (const QList<int> l = regexLiterals(arg); !l.isEmpty()) {
            addAnchor(l, true, paths);
            return true;
        }
        static const QRegularExpression bare(QStringLiteral("^\\s*(\\w+)\\s*$"));
        if (const auto m = bare.match(arg); m.hasMatch()) {
            if (const QList<int> l = regexVar(m.captured(1)); !l.isEmpty()) {
                addAnchor(l, true, paths);
                return true;
            }
            if (const QList<int> l = literalVar(m.captured(1)); !l.isEmpty()) {
                addAnchor(l, regexCall, paths);
                return true;
            }
        }
        return false;
    }

    void anchors(const QString &code, const QSet<QString> &stmtReads) {
        static const QRegularExpression call(QStringLiteral("([A-Za-z_]\\w*)\\s*\\("));
        static const QStringList kFinds = {
            QStringLiteral("find"), QStringLiteral("rfind"), QStringLiteral("contains"),
            QStringLiteral("indexOf"), QStringLiteral("lastIndexOf"), QStringLiteral("count")};
        static const QRegularExpression leadIdent(QStringLiteral("^\\s*([A-Za-z_]\\w*)\\b"));
        for (auto it = call.globalMatch(code); it.hasNext();) {
            const auto m = it.next();
            const QString name = m.captured(1);
            int end = 0;
            const QStringList args = callArgs(code, m.capturedEnd() - 1, &end);
            int before = m.capturedStart() - 1;
            while (before >= 0 && code.at(before).isSpace()) --before;
            const bool method = before >= 0
                && (code.at(before) == QLatin1Char('.')
                    || (code.at(before) == QLatin1Char('>') && before > 0
                        && code.at(before - 1) == QLatin1Char('-')));
            if (method) {
                int r = before - (code.at(before) == QLatin1Char('.') ? 1 : 2);
                while (r >= 0 && code.at(r).isSpace()) --r;
                QSet<QString> recv;
                QString recvId;
                if (r >= 0 && code.at(r) == QLatin1Char(')')) {
                    recv = stmtReads;
                } else {
                    int s = r;
                    while (s >= 0 && isIdentChar(code.at(s))) --s;
                    recvId = code.mid(s + 1, r - s);
                    recv = boundPaths(recvId);
                }
                if (kFinds.contains(name) && !recv.isEmpty() && !args.isEmpty()) {
                    if (!anchorArg(args.first(), false, recv)) m_out.unresolved.append(recv);
                } else if ((name == QLatin1String("match") || name == QLatin1String("globalMatch"))
                           && !regexVar(recvId).isEmpty() && !args.isEmpty()) {
                    const auto a = leadIdent.match(args.first());
                    if (a.hasMatch()) addAnchor(regexVar(recvId), true, boundPaths(a.captured(1)));
                }
                continue;
            }
            if (args.size() < 2) continue;
            const auto first = leadIdent.match(args.first());
            if (!first.hasMatch()) continue;
            const QSet<QString> paths = boundPaths(first.captured(1));
            if (paths.isEmpty()) continue;
            const bool regexCall = name.startsWith(QLatin1String("regex_"));
            for (int i = 1; i < args.size(); ++i) anchorArg(args.at(i), regexCall, paths);
        }
    }

    // A '=' or '+=' at the statement's own level, with its position, or -1.
    static int assignment(const QString &code, bool *append) {
        int depth = 0;
        for (int i = 0; i < code.size(); ++i) {
            const QChar c = code.at(i);
            if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{') || c == QLatin1Char('<')) ++depth;
            else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}') || c == QLatin1Char('>')) {
                if (depth > 0) --depth;
            } else if (c == QLatin1Char('=') && depth == 0) {
                const QChar prev = i > 0 ? code.at(i - 1) : QChar();
                const QChar next = i + 1 < code.size() ? code.at(i + 1) : QChar();
                if (next == QLatin1Char('=')) { ++i; continue; }
                if (prev == QLatin1Char('!') || prev == QLatin1Char('<') || prev == QLatin1Char('>')
                    || prev == QLatin1Char('=')) continue;
                *append = prev == QLatin1Char('+');
                return *append ? i - 1 : i;
            }
        }
        return -1;
    }

    void statement(const QString &code, int line) {
        const QSet<QString> r = reads(code, line, true);
        anchors(code, r);

        static const QRegularExpression regexDecl(QStringLiteral(
            "\\b(?:std::w?regex|QRegularExpression)\\s+(\\w+)\\s*[({=]"));
        if (const auto m = regexDecl.match(code); m.hasMatch()) {
            QList<int> lits;
            static const QRegularExpression idx(QStringLiteral("\\x01(\\d+)\\x02"));
            for (auto it = idx.globalMatch(code); it.hasNext();) lits.append(it.next().captured(1).toInt());
            if (!lits.isEmpty()) regexVars().insert(m.captured(1), lits);
        }

        static const QRegularExpression ret(QStringLiteral("^\\s*return\\b"));
        if (!m_item.function.isEmpty() && ret.match(code).hasMatch()) m_item.returnReads += r;

        bool append = false;
        const int eq = assignment(code, &append);
        static const QRegularExpression lastIdent(QStringLiteral("([A-Za-z_]\\w*)\\s*$"));
        if (eq >= 0) {
            const QString lhs = code.left(eq);
            const auto m = lastIdent.match(lhs);
            if (!m.hasMatch() || lhs.contains(QLatin1Char('(')) || lhs.contains(QLatin1Char('.'))
                || lhs.contains(QLatin1String("->")))
                return;
            if (const QList<int> l = literalsOnly(code.mid(eq + (append ? 2 : 1))); !append && !l.isEmpty())
                literalVars().insert(m.captured(1), l);
            if (append) bindings()[m.captured(1)] += r;
            else if (!r.isEmpty()) bindings()[m.captured(1)] = r;
            else bindings().remove(m.captured(1));
            return;
        }
        if (r.isEmpty()) return;
        // `std::ifstream f(SRC_X_PATH)` and `ss << f.rdbuf()` carry the read
        // to the variable a later `.str()` assignment takes it from.
        static const QRegularExpression parenDecl(QStringLiteral(
            "^\\s*(?:const\\s+)?[\\w:<>]+\\s*[*&]?\\s+(\\w+)\\s*\\("));
        static const QRegularExpression shift(QStringLiteral("^\\s*(\\w+)\\s*<<"));
        if (const auto m = parenDecl.match(code); m.hasMatch()) bindings()[m.captured(1)] += r;
        else if (const auto s = shift.match(code); s.hasMatch()) bindings()[s.captured(1)] += r;
    }

    const Readers &m_r;
    const std::function<bool(const QString &)> &m_existing;
    QHash<QString, QSet<QString>> *m_export;
    QList<Event> m_events;
    QList<Literal> m_lits;
    QSet<QString> m_dirs;
    QList<bool> m_braces;   // true for a namespace brace
    int m_itemDepth = 0;
    Item m_item;
    QHash<QString, QSet<QString>> m_locals, m_fileBindings, m_fileHelpers;
    QHash<QString, QList<int>> m_localRegex, m_fileRegex, m_localLits, m_fileLits;
    QSet<QString> m_seen;
    FileResult m_out;
};

QJsonObject refusal(const QString &code, const QString &error) {
    QJsonObject e;
    e[QStringLiteral("ok")] = false;
    e[QStringLiteral("code")] = code;
    e[QStringLiteral("error")] = error;
    return e;
}

bool isCppSource(const QString &rel) {
    static const QStringList kSuffixes = {
        QStringLiteral("cpp"), QStringLiteral("cc"), QStringLiteral("cxx"), QStringLiteral("h"),
        QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx")};
    return kSuffixes.contains(QFileInfo(rel).suffix());
}

bool isHeader(const QString &rel) {
    const QString s = QFileInfo(rel).suffix();
    return s.startsWith(QLatin1Char('h'));
}

QString readText(const QString &abs) {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

void collectHelpers(const QString &text, Readers *r) {
    const std::function<bool(const QString &)> none = [](const QString &) { return false; };
    Pass(text, *r, none, &r->helpers).run();
}

FileResult analyse(const QString &text, const Readers &r,
                   const std::function<bool(const QString &)> &existing) {
    return Pass(text, r, existing, nullptr).run();
}

QJsonObject run(const QString &rootCanonical, const QJsonObject &args) {
    const QString verb = QStringLiteral("source_scrapes");
    QJsonArray asked = args.value(QStringLiteral("paths")).toArray();
    if (args.contains(QStringLiteral("path"))) asked.prepend(args.value(QStringLiteral("path")));
    if (asked.isEmpty())
        return refusal(QStringLiteral("bad_args"),
                       QStringLiteral("source_scrapes: give \"path\" or \"paths\""));
    int limit = kDefaultLimit;
    if (args.contains(QStringLiteral("limit"))) {
        limit = args.value(QStringLiteral("limit")).toInt(0);
        if (limit < 1 || limit > kMaxLimit)
            return refusal(QStringLiteral("bad_args"),
                           QStringLiteral("source_scrapes: \"limit\" must be 1 to %1").arg(kMaxLimit));
    }
    const QDir root(rootCanonical);
    QStringList wanted;
    for (const QJsonValue v : std::as_const(asked)) {
        if (!v.isString())
            return refusal(QStringLiteral("bad_args"),
                           QStringLiteral("source_scrapes: \"paths\" entries must be strings"));
        const PathValidation::Check check =
            PathValidation::validatePath(v.toString(), rootCanonical, verb, QStringLiteral("paths"));
        if (check.bad) return check.err;
        const QFileInfo fi(check.resolved);
        if (check.resolved.isEmpty() || !fi.exists())
            return refusal(QStringLiteral("bad_path"),
                           QStringLiteral("source_scrapes: \"%1\" does not exist").arg(v.toString()));
        wanted.append(root.relativeFilePath(fi.canonicalFilePath()));
    }
    const auto matching = [&](const QSet<QString> &paths) {
        QStringList out;
        for (const QString &p : paths)
            for (const QString &w : std::as_const(wanted))
                if (p == w || w == QLatin1String(".") || p.startsWith(w + QLatin1Char('/'))) {
                    out.append(p);
                    break;
                }
        std::sort(out.begin(), out.end());
        return out;
    };

    Readers readers;
    const QHash<QString, QStringList> defs =
        BuildTargets::pathDefinitions(readText(root.filePath(QStringLiteral("CMakeLists.txt"))));
    for (auto it = defs.cbegin(); it != defs.cend(); ++it) {
        const QString first = it.value().first();
        if (first.isEmpty() || QFileInfo(root.filePath(first)).isDir())
            readers.dirTokens.insert(it.key(), first);
        else
            readers.fileTokens.insert(it.key(), it.value());
    }

    const ProjectSettings::Settings settings = ProjectSettings::load(rootCanonical);
    QJsonArray testRoots;
    for (const QString &t : settings.testRoots.value_or(QStringList{QStringLiteral("tests")}))
        if (QFileInfo(root.filePath(t)).isDir()) testRoots.append(t);
    CodeWalk::Result walk;
    if (!testRoots.isEmpty()) {
        CodeWalk::Options opts;
        opts.includeTests = true;
        walk = CodeWalk::collect(rootCanonical, testRoots, opts, verb);
        if (!walk.refusal.isEmpty()) return walk.refusal;
    }
    QStringList files;
    for (const QString &abs : std::as_const(walk.files))
        if (isCppSource(abs)) files.append(abs);

    QHash<QString, QString> texts;
    for (const QString &abs : std::as_const(files)) {
        texts.insert(abs, readText(abs));
        if (isHeader(abs)) collectHelpers(texts.value(abs), &readers);
    }
    const std::function<bool(const QString &)> existing = [&](const QString &rel) {
        return QFileInfo(root.filePath(rel)).isFile();
    };

    QJsonArray scrapes, readRows;
    int total = 0, unresolved = 0;
    struct Row {
        QString path, file;
        int line;
        QJsonObject obj;
    };
    QList<Row> rows, readerRows;
    QSet<QString> readerSeen;
    for (const QString &abs : std::as_const(files)) {
        const QString rel = root.relativeFilePath(abs);
        const FileResult fr = analyse(texts.value(abs), readers, existing);
        for (const Anchor &a : fr.anchors)
            for (const QString &p : matching(a.paths)) {
                QJsonObject o;
                o[QStringLiteral("path")] = p;
                o[QStringLiteral("test")] = a.test.isEmpty() ? QJsonValue() : QJsonValue(a.test);
                o[QStringLiteral("file")] = rel;
                o[QStringLiteral("line")] = a.line;
                o[QStringLiteral("anchor")] = a.anchor;
                o[QStringLiteral("kind")] = a.regex ? QStringLiteral("regex") : QStringLiteral("literal");
                rows.append({p, rel, a.line, o});
            }
        for (const Read &r : fr.reads)
            for (const QString &p : matching(r.paths)) {
                const QString key = p + QLatin1Char('\n') + rel + QLatin1Char('\n') + r.test
                                    + QLatin1Char('\n') + r.via;
                if (readerSeen.contains(key)) continue;
                readerSeen.insert(key);
                QJsonObject o;
                o[QStringLiteral("path")] = p;
                o[QStringLiteral("test")] = r.test.isEmpty() ? QJsonValue() : QJsonValue(r.test);
                o[QStringLiteral("file")] = rel;
                o[QStringLiteral("line")] = r.line;
                o[QStringLiteral("via")] = r.via;
                readerRows.append({p, rel, r.line, o});
            }
        for (const QSet<QString> &u : fr.unresolved)
            if (!matching(u).isEmpty()) ++unresolved;
    }
    const auto order = [](const Row &a, const Row &b) {
        if (a.path != b.path) return a.path < b.path;
        if (a.file != b.file) return a.file < b.file;
        return a.line < b.line;
    };
    std::sort(rows.begin(), rows.end(), order);
    std::sort(readerRows.begin(), readerRows.end(), order);
    total = int(rows.size());
    for (int i = 0; i < rows.size() && i < limit; ++i) scrapes.append(rows.at(i).obj);
    for (int i = 0; i < readerRows.size() && i < limit; ++i) readRows.append(readerRows.at(i).obj);

    QJsonObject scanned;
    scanned[QStringLiteral("test_files")] = int(files.size());
    scanned[QStringLiteral("definitions")] = int(defs.size());
    scanned[QStringLiteral("helpers")] = int(readers.helpers.size());
    scanned[QStringLiteral("scrapes_total")] = total;
    scanned[QStringLiteral("readers_total")] = int(readerRows.size());
    scanned[QStringLiteral("unresolved_anchors")] = unresolved;
    scanned[QStringLiteral("truncated")] = walk.truncated;
    // True when `limit` cut either list; `truncated` is the file walk's cap.
    scanned[QStringLiteral("rows_truncated")] = total > limit || readerRows.size() > limit;
    QJsonObject env;
    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("paths")] = QJsonArray::fromStringList(wanted);
    env[QStringLiteral("scrapes")] = scrapes;
    env[QStringLiteral("readers")] = readRows;
    env[QStringLiteral("limit")] = limit;
    env[QStringLiteral("scanned")] = scanned;
    env[QStringLiteral("not_covered")] = QJsonArray{
        QStringLiteral("a path built at run time, such as a directory token joined to a variable"),
        QStringLiteral("definitions in a CMakeLists.txt other than the root one"),
        QStringLiteral("a file reached through an environment variable")};
    return env;
}

} // namespace SourceScrapes
