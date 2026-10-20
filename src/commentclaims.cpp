// ANTS-5786 — see commentclaims.h. Contract:
// tests/features/mcp_comment_claims/spec.md.
//
// Each file's comments are harvested by its comment syntax, then each block
// is searched for four claim shapes. Findings are only what a lookup
// decides; a name the lookup cannot reach (a Qt or std symbol, a bare file
// name, a capped walk) is skipped or counted, never reported as dead.

#include "commentclaims.h"

#include "codewalk.h"
#include "projectsettings.h"
#include "quotationcheckverb.h"
#include "symbolquery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace CommentClaims {
namespace {

inline constexpr int kDefaultLimit = 500;
inline constexpr int kMaxLimit = 5000;
inline constexpr int kMinQuotation = 12;

// True when the text between a file name and a quotation attributes the one
// to the other: nothing, punctuation, or one linking word ("src/a.cpp says").
// Mere nearness is not enough: a comment naming a spec beside a quoted
// phrase is not quoting the spec.
bool attributes(QString between) {
    static const QRegularExpression punct(QStringLiteral("[\\s:,()\u2014\u2013-]+"));
    between.replace(punct, QStringLiteral(" "));
    between = between.trimmed().toLower();
    static const QSet<QString> kLinks = {
        QStringLiteral("says"), QStringLiteral("said"), QStringLiteral("reads"),
        QStringLiteral("states"), QStringLiteral("holds"), QStringLiteral("has"),
        QStringLiteral("contains"), QStringLiteral("quotes"), QStringLiteral("writes"),
        QStringLiteral("prints"), QStringLiteral("emits"), QStringLiteral("in"),
        QStringLiteral("from")};
    return between.isEmpty() || kLinks.contains(between);
}

enum class Family { None, CLike, Hash, Lua };

Family familyOf(const QString &fileName) {
    if (fileName == QLatin1String("CMakeLists.txt") || fileName == QLatin1String("Makefile"))
        return Family::Hash;
    static const QSet<QString> kC = {
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("cxx"),
        QStringLiteral("h"), QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx"),
        QStringLiteral("java"), QStringLiteral("js"), QStringLiteral("ts"), QStringLiteral("go"),
        QStringLiteral("rs"), QStringLiteral("kt"), QStringLiteral("swift"), QStringLiteral("cs"),
        QStringLiteral("glsl"), QStringLiteral("vert"), QStringLiteral("frag"), QStringLiteral("comp"),
        QStringLiteral("qml"), QStringLiteral("m"), QStringLiteral("mm"), QStringLiteral("zig")};
    static const QSet<QString> kHash = {
        QStringLiteral("py"), QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("cmake"),
        QStringLiteral("yml"), QStringLiteral("yaml"), QStringLiteral("toml"), QStringLiteral("rb"),
        QStringLiteral("pl"), QStringLiteral("r"), QStringLiteral("nix")};
    const QString s = QFileInfo(fileName).suffix().toLower();
    if (kC.contains(s)) return Family::CLike;
    if (kHash.contains(s)) return Family::Hash;
    if (s == QLatin1String("lua")) return Family::Lua;
    return Family::None;
}

class Harvester {
public:
    Harvester(const QString &text, Family f) : m_t(text), m_f(f) {}

    QList<Block> run() {
        const int n = m_t.size();
        if (m_f == Family::Hash && m_t.startsWith(QLatin1String("#!"))) skipToEol();
        while (m_i < n) {
            const QChar c = m_t.at(m_i);
            if (c == QLatin1Char('\n')) { ++m_line; ++m_i; continue; }
            if (m_f == Family::CLike) {
                if (at("//")) { lineComment(2); continue; }
                if (at("/*")) { blockComment(QStringLiteral("*/"), 2); continue; }
                if (c == QLatin1Char('"')) { cString(); continue; }
                if (c == QLatin1Char('\'')) { charLiteral(); continue; }
            } else if (m_f == Family::Hash) {
                if (c == QLatin1Char('#') && (m_i == 0 || m_t.at(m_i - 1).isSpace())) {
                    lineComment(1);
                    continue;
                }
                if (at("\"\"\"") || at("'''")) { skipPast(m_t.mid(m_i, 3), 3); continue; }
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quoted(c); continue; }
            } else if (m_f == Family::Lua) {
                if (at("--[[")) { blockComment(QStringLiteral("]]"), 4); continue; }
                if (at("--")) { lineComment(2); continue; }
                if (at("[[")) { skipPast(QStringLiteral("]]"), 2); continue; }
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) { quoted(c); continue; }
            }
            ++m_i;
        }
        return m_blocks;
    }

private:
    bool at(const char *s) const { return m_t.mid(m_i, int(qstrlen(s))) == QLatin1String(s); }

    void skipToEol() {
        while (m_i < m_t.size() && m_t.at(m_i) != QLatin1Char('\n')) ++m_i;
    }

    void addLine(const QString &text, bool lineKind) {
        const CommentLine cl{m_line, text.trimmed()};
        if (lineKind && m_lastLineComment == m_line - 1 && !m_blocks.isEmpty())
            m_blocks.last().append(cl);
        else
            m_blocks.append(Block{cl});
        m_lastLineComment = lineKind ? m_line : -1;
    }

    void lineComment(int markerLen) {
        const int start = m_i + markerLen;
        skipToEol();
        addLine(m_t.mid(start, m_i - start), true);
    }

    void blockComment(const QString &close, int markerLen) {
        const int start = m_i + markerLen;
        int end = m_t.indexOf(close, start);
        if (end < 0) end = m_t.size();
        const QStringList parts = m_t.mid(start, end - start).split(QLatin1Char('\n'));
        static const QRegularExpression lead(QStringLiteral("^\\s*\\*(?!/)"));
        Block b;
        for (int k = 0; k < parts.size(); ++k) {
            QString p = parts.at(k);
            p.remove(lead);
            b.append({m_line + k, p.trimmed()});
        }
        m_blocks.append(b);
        m_line += int(parts.size()) - 1;
        m_lastLineComment = -1;
        m_i = qMin(int(m_t.size()), end + int(close.size()));
    }

    // Moves past `close`, counting the lines it crosses.
    void skipPast(const QString &close, int openLen) {
        int end = m_t.indexOf(close, m_i + openLen);
        if (end < 0) end = m_t.size();
        m_line += int(m_t.mid(m_i, end - m_i).count(QLatin1Char('\n')));
        m_i = qMin(int(m_t.size()), end + int(close.size()));
    }

    void quoted(QChar q) {
        for (++m_i; m_i < m_t.size() && m_t.at(m_i) != q && m_t.at(m_i) != QLatin1Char('\n'); ++m_i)
            if (m_t.at(m_i) == QLatin1Char('\\')) ++m_i;
        if (m_i < m_t.size() && m_t.at(m_i) == q) ++m_i;
    }

    void cString() {
        // A raw string R"delim( … )delim" may hold `//` and newlines.
        if (m_i > 0 && m_t.at(m_i - 1) == QLatin1Char('R')) {
            const int open = m_t.indexOf(QLatin1Char('('), m_i);
            if (open > 0) {
                const QString close = QLatin1Char(')') + m_t.mid(m_i + 1, open - m_i - 1) + QLatin1Char('"');
                skipPast(close, open - m_i + 1);
                return;
            }
        }
        quoted(QLatin1Char('"'));
    }

    void charLiteral() {
        // A digit separator (1'000) is not a character literal.
        if (m_i > 0 && m_t.at(m_i - 1).isLetterOrNumber()) { ++m_i; return; }
        quoted(QLatin1Char('\''));
    }

    const QString &m_t;
    Family m_f;
    int m_i = 0;
    int m_line = 1;
    int m_lastLineComment = -1;
    QList<Block> m_blocks;
};

struct Row {
    QString file;
    int line = 0;
    QString kind;
    QString quote;
    QString why;
    bool finding = true;
};

// A "see X" naming a symbol, resolved in one walk once every file is read.
struct PendingSymbol {
    Row row;
    QString name;
};

// The block's text joined with spaces, and the offset each line starts at,
// so a match anywhere maps back to its line.
struct Joined {
    QString text;
    QList<int> starts;
    QList<int> lines;
    int lineAt(int offset) const {
        int k = int(std::upper_bound(starts.cbegin(), starts.cend(), offset) - starts.cbegin()) - 1;
        return lines.at(qMax(0, k));
    }
};

Joined join(const Block &b) {
    Joined j;
    for (const CommentLine &cl : b) {
        if (!j.text.isEmpty()) j.text += QLatin1Char(' ');
        j.starts.append(int(j.text.size()));
        j.lines.append(cl.line);
        j.text += cl.text;
    }
    return j;
}

QString readText(const QString &abs) {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

QJsonObject refusal(const QString &code, const QString &error) {
    QJsonObject e;
    e[QStringLiteral("ok")] = false;
    e[QStringLiteral("code")] = code;
    e[QStringLiteral("error")] = error;
    return e;
}

class Checker {
public:
    Checker(const QString &root, const QSet<QString> &ids, const QSet<QString> &prefixes)
        : m_root(root), m_ids(ids), m_prefixes(prefixes) {}

    void file(const QString &rel, const QString &text) {
        m_dir = QFileInfo(rel).path();
        m_rel = rel;
        for (const Block &b : harvest(QFileInfo(rel).fileName(), text)) {
            ++comments;
            const Joined j = join(b);
            quotations(j);
            seeReferences(j);
            tickets(j);
            census(j);
        }
    }

    QList<Row> rows;
    QList<PendingSymbol> pending;
    int comments = 0;

private:
    // `p` as a root-relative path, trying the root then the comment's own
    // directory; empty when neither holds it.
    QString resolve(const QString &p) const {
        const QDir root(m_root);
        if (QFileInfo::exists(root.filePath(p))) return QDir::cleanPath(p);
        const QString near = QDir::cleanPath(m_dir + QLatin1Char('/') + p);
        if (!near.startsWith(QLatin1String("..")) && QFileInfo::exists(root.filePath(near))) return near;
        return QString();
    }

    void add(const Joined &j, int offset, const QString &kind, const QString &quote,
             const QString &why, bool finding = true) {
        rows.append({m_rel, j.lineAt(offset), kind, quote, why, finding});
    }

    void quotations(const Joined &j) {
        static const QRegularExpression pathTok(QStringLiteral(
            "(?<![\\w/.-])((?:[\\w.-]+/)*[\\w-][\\w.-]*\\.[A-Za-z][A-Za-z0-9]{0,5})(?![\\w/])"));
        static const QRegularExpression quote(QStringLiteral("\"([^\"]+)\"|“([^”]+)”"));
        struct Named { int at; int len; QString rel; };
        QList<Named> named;
        for (auto it = pathTok.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            const QString rel = resolve(m.captured(1));
            if (!rel.isEmpty() && QFileInfo(QDir(m_root).filePath(rel)).isFile())
                named.append({int(m.capturedStart(1)), int(m.capturedLength(1)), rel});
        }
        if (named.isEmpty()) return;
        for (auto it = quote.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            const int g = m.captured(1).isNull() ? 2 : 1;
            const QString q = m.captured(g);
            if (q.size() < kMinQuotation || q.contains(QLatin1String("...")) || q.contains(QChar(0x2026)))
                continue;
            QStringList files;
            for (const Named &nm : std::as_const(named)) {
                const int start = int(m.capturedStart(0)), end = int(m.capturedEnd(0));
                if (nm.at + nm.len > start && nm.at < end) continue;   // named inside the quotation
                const QString between = nm.at < start
                    ? j.text.mid(nm.at + nm.len, start - nm.at - nm.len)
                    : j.text.mid(end, nm.at - end);
                if (attributes(between) && !files.contains(nm.rel)) files.append(nm.rel);
            }
            if (files.isEmpty()) continue;
            const QByteArray needle = QuotationCheck::normaliseQuotation(q.toUtf8());
            bool found = false;
            for (const QString &f : std::as_const(files)) found = found || contains(f, needle);
            if (!found)
                add(j, int(m.capturedStart(g)), QStringLiteral("missing_quotation"), q,
                    QStringLiteral("%1 does not contain this text").arg(files.join(QStringLiteral(", "))));
        }
    }

    bool contains(const QString &rel, const QByteArray &needle) {
        if (!m_normalised.contains(rel))
            m_normalised.insert(rel, QuotationCheck::normalise(
                readText(QDir(m_root).filePath(rel)).toUtf8()).text);
        return m_normalised.value(rel).contains(needle);
    }

    void seeReferences(const Joined &j) {
        static const QRegularExpression see(QStringLiteral("\\b[Ss]ee\\s+(?:also\\s+)?(`[^`]+`|\\S+)"));
        static const QRegularExpression symbol(QStringLiteral("^[A-Za-z_]\\w*(?:::[A-Za-z_]\\w*)*$"));
        static const QRegularExpression camel(QStringLiteral("[a-z][A-Z]"));
        static const QRegularExpression lower(QStringLiteral("[a-z]"));
        static const QRegularExpression foreign(QStringLiteral("^(?:Q[A-Z]|std::|boost::|testing::)"));
        static const QRegularExpression suffix(QStringLiteral(
            "\\.(?:md|cpp|cc|c|h|hpp|py|sh|json|txt|cmake|ya?ml|lua|qml|ui|qrc|toml)$"));
        for (auto it = see.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            // A "see" inside a code span is an example being shown, not a
            // reference being made.
            if (j.text.left(int(m.capturedStart(0))).count(QLatin1Char('`')) % 2 == 1) continue;
            QString tok = m.captured(1);
            const bool ticked = tok.startsWith(QLatin1Char('`'));
            tok.remove(QLatin1Char('`'));
            while (!tok.isEmpty() && QStringLiteral("(\"'").contains(tok.front())) tok.remove(0, 1);
            // `()` is part of a symbol, so it is kept while other closing
            // punctuation is trimmed.
            while (!tok.isEmpty() && !tok.endsWith(QLatin1String("()"))
                   && QStringLiteral(".,;:)\"'").contains(tok.back()))
                tok.chop(1);
            const int hash = tok.indexOf(QLatin1Char('#'));
            if (hash > 0) tok.truncate(hash);
            static const QRegularExpression lineRef(QStringLiteral(":\\d+(?:-\\d+)?$"));
            tok.remove(lineRef);
            if (tok.isEmpty()) continue;
            const int at = int(m.capturedStart(1));
            if (tok.contains(QLatin1Char('/'))) {
                if (tok.contains(QLatin1String("://")) || tok.startsWith(QLatin1Char('~'))
                    || tok.startsWith(QLatin1Char('/')) || tok.contains(QLatin1Char('*'))
                    || tok.contains(QLatin1Char('<')) || tok.contains(QLatin1Char('{'))
                    || tok.contains(QLatin1Char('$')))
                    continue;
                if (resolve(tok).isEmpty())
                    add(j, at, QStringLiteral("dead_reference"), m.captured(0).trimmed(),
                        QStringLiteral("no file or directory %1 under the project").arg(tok));
                continue;
            }
            if (suffix.match(tok).hasMatch()) continue;   // a bare file name: not looked up
            const bool parens = tok.endsWith(QLatin1String("()"));
            if (parens) tok.chop(2);
            if (!symbol.match(tok).hasMatch() || foreign.match(tok).hasMatch()) continue;
            if (!lower.match(tok).hasMatch()) continue;   // a macro or CMake variable
            // A bare word is too often a library or enum name; it needs `::`,
            // `()` or backticks to be read as the project's own symbol.
            if (!tok.contains(QLatin1String("::")) && !parens && !ticked) continue;
            if (!tok.contains(QLatin1String("::")) && !camel.match(tok).hasMatch()
                && !tok.contains(QLatin1Char('_')))
                continue;
            const QString last = tok.section(QLatin1String("::"), -1);
            pending.append({{m_rel, j.lineAt(at), QStringLiteral("dead_reference"),
                             m.captured(0).trimmed(),
                             QStringLiteral("no definition of %1 found").arg(last), true},
                            last});
        }
    }

    void tickets(const Joined &j) {
        if (m_prefixes.isEmpty()) return;
        static const QRegularExpression id(QStringLiteral("\\b([A-Z][A-Z0-9]*)-(\\d+)\\b"));
        for (auto it = id.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            if (!m_prefixes.contains(m.captured(1)) || m_ids.contains(m.captured(0))) continue;
            add(j, int(m.capturedStart(0)), QStringLiteral("unknown_ticket"), m.captured(0),
                QStringLiteral("%1 is not in the roadmap").arg(m.captured(0)));
        }
    }

    void census(const Joined &j) {
        static const QRegularExpression approx(QStringLiteral("~\\s?\\d[\\d,]*(?:[- ][A-Za-z]+)?"));
        static const QRegularExpression nLine(QStringLiteral("(?<![\\w.-])\\d+-line\\b"));
        static const QRegularExpression plural(QStringLiteral(
            "(?<![\\w.\\-/#:~])(\\d{1,3}(?:,\\d{3})+|\\d+)(?![\\d.:%/,-])\\s+(?:[A-Za-z][\\w-]*\\s+)?([a-z]{3,}s)\\b"));
        static const QSet<QString> kTime = {
            QStringLiteral("seconds"), QStringLiteral("minutes"), QStringLiteral("hours"),
            QStringLiteral("days"), QStringLiteral("weeks"), QStringLiteral("months"),
            QStringLiteral("years"), QStringLiteral("milliseconds"), QStringLiteral("microseconds"),
            QStringLiteral("nanoseconds"), QStringLiteral("secs"), QStringLiteral("mins")};
        const QString why = QStringLiteral("a count that goes stale when the code changes");
        QSet<int> taken;
        for (auto it = approx.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            taken.insert(int(m.capturedStart(0)) + 1);
            add(j, int(m.capturedStart(0)), QStringLiteral("census_number"), m.captured(0), why, false);
        }
        for (auto it = nLine.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            if (taken.contains(int(m.capturedStart(0)))) continue;
            taken.insert(int(m.capturedStart(0)));
            add(j, int(m.capturedStart(0)), QStringLiteral("census_number"), m.captured(0), why, false);
        }
        for (auto it = plural.globalMatch(j.text); it.hasNext();) {
            const auto m = it.next();
            if (taken.contains(int(m.capturedStart(1))) || kTime.contains(m.captured(2))) continue;
            QString digits = m.captured(1);
            digits.remove(QLatin1Char(','));
            if (digits.toLongLong() < 2) continue;
            add(j, int(m.capturedStart(1)), QStringLiteral("census_number"), m.captured(0), why, false);
        }
    }

    QString m_root;
    QSet<QString> m_ids;
    QSet<QString> m_prefixes;
    QString m_dir;
    QString m_rel;
    QHash<QString, QByteArray> m_normalised;
};

// Every item id the roadmap files declare, read from the bullet that opens
// each item (`- 📋 [ANTS-1] …` or `- **ANTS-1** …`).
QSet<QString> roadmapIds(const QString &rootCanonical) {
    const QDir root(rootCanonical);
    const ProjectSettings::Settings settings = ProjectSettings::load(rootCanonical);
    QStringList files{settings.roadmap.value_or(QStringLiteral("ROADMAP.md"))};
    const QDir archive(root.filePath(QStringLiteral("docs/roadmap")));
    for (const QString &f : archive.entryList({QStringLiteral("*.md")}, QDir::Files))
        files.append(QStringLiteral("docs/roadmap/") + f);
    static const QRegularExpression item(
        QStringLiteral("^\\s*[-*]\\s+(?:\\S+\\s+)?(?:\\[|\\*\\*)([A-Z][A-Z0-9]*-\\d+)(?:\\]|\\*\\*)"),
        QRegularExpression::MultilineOption);
    QSet<QString> ids;
    for (const QString &f : std::as_const(files))
        for (auto it = item.globalMatch(readText(root.filePath(f))); it.hasNext();)
            ids.insert(it.next().captured(1));
    return ids;
}

} // namespace

QList<Block> harvest(const QString &fileName, const QString &text) {
    const Family f = familyOf(fileName);
    if (f == Family::None) return {};
    return Harvester(text, f).run();
}

QJsonObject run(const QString &rootCanonical, const QJsonObject &args) {
    const QString verb = QStringLiteral("comment_claims");
    int limit = kDefaultLimit;
    if (args.contains(QStringLiteral("limit"))) {
        limit = args.value(QStringLiteral("limit")).toInt(0);
        if (limit < 1 || limit > kMaxLimit)
            return refusal(QStringLiteral("bad_args"),
                           QStringLiteral("comment_claims: \"limit\" must be 1 to %1").arg(kMaxLimit));
    }
    CodeWalk::Options walkOpts;
    walkOpts.includeTests = args.value(QStringLiteral("include_tests")).toBool(false);
    walkOpts.includeGenerated = args.value(QStringLiteral("include_generated")).toBool(false);
    const CodeWalk::Result files = CodeWalk::collect(
        rootCanonical, args.value(QStringLiteral("paths")).toArray(), walkOpts, verb);
    if (!files.refusal.isEmpty()) return files.refusal;

    const QSet<QString> ids = roadmapIds(rootCanonical);
    QSet<QString> prefixes;
    for (const QString &id : ids) prefixes.insert(id.section(QLatin1Char('-'), 0, 0));

    const QDir root(rootCanonical);
    Checker checker(rootCanonical, ids, prefixes);
    for (const QString &abs : files.files) checker.file(root.relativeFilePath(abs), readText(abs));

    QList<Row> rows = checker.rows;
    int unchecked = 0;
    if (!checker.pending.isEmpty()) {
        QStringList names;
        for (const PendingSymbol &p : std::as_const(checker.pending)) names.append(p.name);
        names.removeDuplicates();
        SymbolQuery::Options opts;
        opts.maxResults = 1;
        const QHash<QString, SymbolQuery::DefResult> defs =
            SymbolQuery::findDefinitions(rootCanonical, names, opts);
        for (const PendingSymbol &p : std::as_const(checker.pending)) {
            const SymbolQuery::DefResult r = defs.value(p.name);
            if (!r.ok || !r.definitions.isEmpty()) continue;
            if (r.walkCapped) { ++unchecked; continue; }
            rows.append(p.row);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.file != b.file) return a.file < b.file;
        return a.line < b.line;
    });

    QJsonArray findings, candidates;
    QHash<QString, int> seq;
    int findingsTotal = 0, candidatesTotal = 0;
    for (const Row &r : std::as_const(rows)) {
        const QString key = QStringLiteral("%1:%2:%3").arg(r.file).arg(r.line).arg(r.kind);
        QJsonObject o;
        o[QStringLiteral("id")] = key + QLatin1Char(':') + QString::number(seq[key]++);
        o[QStringLiteral("file")] = r.file;
        o[QStringLiteral("line")] = r.line;
        o[QStringLiteral("kind")] = r.kind;
        o[QStringLiteral("quote")] = r.quote;
        o[QStringLiteral("why")] = r.why;
        if (r.finding) {
            if (findingsTotal++ < limit) findings.append(o);
        } else if (candidatesTotal++ < limit) {
            candidates.append(o);
        }
    }

    QJsonObject scanned = CodeWalk::scannedSummary(files);
    scanned[QStringLiteral("comments")] = checker.comments;
    scanned[QStringLiteral("findings_total")] = findingsTotal;
    scanned[QStringLiteral("candidates_total")] = candidatesTotal;
    scanned[QStringLiteral("symbols_unchecked")] = unchecked;
    // True when `limit` cut either bucket; `truncated` is the file walk's cap.
    scanned[QStringLiteral("rows_truncated")] = findingsTotal > limit || candidatesTotal > limit;
    QJsonObject env;
    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("findings")] = findings;
    env[QStringLiteral("candidates")] = candidates;
    env[QStringLiteral("limit")] = limit;
    env[QStringLiteral("scanned")] = scanned;
    env[QStringLiteral("not_covered")] = QJsonArray{
        QStringLiteral("a bare file name after \"see\" (no directory), which could live anywhere"),
        QStringLiteral("Qt, std and other library names, which the project does not define"),
        QStringLiteral("a quotation not directly beside the file it is attributed to"),
        QStringLiteral("a bare name after \"see\" with no ::, () or backticks")};
    return env;
}

} // namespace CommentClaims
