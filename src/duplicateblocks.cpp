// ANTS-5785 — see duplicateblocks.h. Contract:
// tests/features/mcp_duplicate_blocks/spec.md.
//
// Three steps. Every file becomes its compared lines, each interned to an
// id. Every pair of places sharing a short seed run is extended to its
// longest match, skipping a few edited lines while the match stays similar
// enough. Pair matches over the same code are then joined into groups, so
// three copies are one group rather than three pairs.

#include "duplicateblocks.h"

#include "codewalk.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace DuplicateBlocks {
namespace {

inline constexpr int kSeed = 3;          // lines in a seed run
inline constexpr int kMaxGap = 3;        // edited lines skipped on one side
inline constexpr int kMaxBucket = 30;    // a seed in more places is boilerplate
inline constexpr int kMaxDiff = 20;
inline constexpr int kDefaultMinLines = 5;
inline constexpr int kDefaultTop = 50;
inline constexpr double kDefaultSimilarity = 0.8;

struct File {
    QString rel;
    QStringList raw;      // each compared line as written, trimmed
    QStringList norm;     // what is compared
    QList<int> lineNo;    // its 1-based line in the file
    QList<int> ids;       // norm, interned
};

// The line without a trailing `//` comment, when the `//` is outside quotes.
QString dropTrailingComment(const QString &t) {
    bool inStr = false;
    for (int i = 0; i + 1 < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == QLatin1Char('\\')) { ++i; continue; }
        if (c == QLatin1Char('"')) inStr = !inStr;
        if (!inStr && c == QLatin1Char('/') && t.at(i + 1) == QLatin1Char('/')
            && (i == 0 || t.at(i - 1).isSpace()))
            return t.left(i).trimmed();
    }
    return t;
}

bool compared(const QString &t) {
    static const QRegularExpression punctOnly(QStringLiteral("^[\\s{}()\\[\\];,.:]*$"));
    static const QRegularExpression elseLine(QStringLiteral("^\\}?\\s*else\\s*\\{?$"));
    static const QRegularExpression access(
        QStringLiteral("^(?:public|private|protected)(?:\\s+(?:slots|Q_SLOTS))?\\s*:$"));
    static const QRegularExpression import(
        QStringLiteral("^(?:import\\s|from\\s+\\S+\\s+import\\s|using\\s+namespace\\s|package\\s)"));
    if (t.isEmpty()) return false;
    for (const char *lead : {"//", "/*", "*", "#", "--"})
        if (t.startsWith(QLatin1String(lead))) return false;
    return !punctOnly.match(t).hasMatch() && !elseLine.match(t).hasMatch()
        && !access.match(t).hasMatch() && !import.match(t).hasMatch();
}

QString normaliseNames(const QString &t) {
    static const QRegularExpression str(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\"|'(?:\\\\.|[^'\\\\])*'"));
    static const QRegularExpression num(QStringLiteral("\\b\\d[\\w.]*\\b"));
    static const QRegularExpression ident(QStringLiteral("\\b[A-Za-z_]\\w*\\b"));
    static const QSet<QString> kKeywords = {
        QStringLiteral("if"), QStringLiteral("else"), QStringLiteral("for"), QStringLiteral("while"),
        QStringLiteral("do"), QStringLiteral("switch"), QStringLiteral("case"), QStringLiteral("default"),
        QStringLiteral("return"), QStringLiteral("break"), QStringLiteral("continue"),
        QStringLiteral("const"), QStringLiteral("auto"), QStringLiteral("static"), QStringLiteral("new"),
        QStringLiteral("delete"), QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("nullptr"),
        QStringLiteral("this"), QStringLiteral("void"), QStringLiteral("int"), QStringLiteral("bool"),
        QStringLiteral("def"), QStringLiteral("in"), QStringLiteral("not"), QStringLiteral("and"),
        QStringLiteral("or"), QStringLiteral("None"), QStringLiteral("self"), QStringLiteral("try"),
        QStringLiteral("catch"), QStringLiteral("except"), QStringLiteral("throw"), QStringLiteral("raise")};
    QString s = t;
    s.replace(str, QStringLiteral("S"));
    s.replace(num, QStringLiteral("N"));
    QString out;
    int last = 0;
    for (auto it = ident.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        const QString w = m.captured(0);
        out += (kKeywords.contains(w) || w == QLatin1String("S") || w == QLatin1String("N"))
                   ? w : QStringLiteral("I");
        last = int(m.capturedEnd());
    }
    out += s.mid(last);
    return out;
}

File load(const QString &rel, const QString &text, bool names, QHash<QString, int> *vocab) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    File f;
    f.rel = rel;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = dropTrailingComment(lines.at(i).trimmed());
        if (!compared(t)) continue;
        QString n = QString(t).replace(ws, QStringLiteral(" "));
        if (names) n = normaliseNames(n);
        f.raw.append(t);
        f.norm.append(n);
        f.lineNo.append(i + 1);
        auto known = vocab->constFind(n);
        f.ids.append(known != vocab->cend() ? known.value()
                                            : vocab->insert(n, int(vocab->size())).value());
    }
    return f;
}

struct DiffLine {
    int file = 0;
    int index = 0;   // a compared-line index
};

// One pair of places and how far they match, in compared-line indices.
struct Match {
    int fa = 0, sa = 0, ea = 0;   // [sa, ea)
    int fb = 0, sb = 0, eb = 0;
    int matched = 0;
    double similarity = 1.0;
    bool diverged = false;
    QList<DiffLine> diff;
};

class Finder {
public:
    Finder(const QList<File> &files, int minLines, double similarity, bool names)
        : m_files(files), m_minLines(minLines), m_similarity(similarity), m_names(names) {}

    QList<Match> run() {
        const int k = qMin(kSeed, m_minLines);
        QHash<QByteArray, QList<QPair<int, int>>> seeds;
        for (int f = 0; f < m_files.size(); ++f) {
            const QList<int> &ids = m_files.at(f).ids;
            for (int p = 0; p + k <= ids.size(); ++p) {
                // A line repeated over and over is not code written twice.
                bool same = true;
                for (int i = 1; i < k; ++i) same = same && ids.at(p + i) == ids.at(p);
                if (same && k > 1) continue;
                QByteArray key;
                for (int i = 0; i < k; ++i) key += QByteArray::number(ids.at(p + i)) + ',';
                seeds[key].append({f, p});
            }
        }
        QList<Match> out;
        for (auto it = seeds.cbegin(); it != seeds.cend(); ++it) {
            const QList<QPair<int, int>> &b = it.value();
            if (b.size() < 2 || b.size() > kMaxBucket) continue;
            for (int x = 0; x < b.size(); ++x)
                for (int y = x + 1; y < b.size(); ++y) {
                    Match m;
                    if (extend(b.at(x), b.at(y), k, &m)) out.append(m);
                }
        }
        return out;
    }

private:
    int id(int f, int p) const { return m_files.at(f).ids.at(p); }
    int size(int f) const { return int(m_files.at(f).ids.size()); }

    // Grows the seed at a and b into the longest match. False when it is not
    // a match's first seed, overlaps itself, or stays under min_lines.
    bool extend(QPair<int, int> a, QPair<int, int> b, int k, Match *m) const {
        const int fa = a.first, fb = b.first;
        int pa = a.second, pb = b.second;
        if (fa == fb && pa > pb) std::swap(pa, pb);
        if (fa == fb && pb - pa < k) return false;
        if (pa > 0 && pb > 0 && id(fa, pa - 1) == id(fb, pb - 1)) return false;
        // In one file the first copy must end before the second begins.
        const int limitA = fa == fb ? pb : size(fa);
        int ea = pa, eb = pb, matched = 0;
        const auto run = [&](int fromA, int fromB) {
            int n = 0;
            while (fromA + n < limitA && fromB + n < size(fb) && id(fa, fromA + n) == id(fb, fromB + n)) {
                if (m_names && m_files.at(fa).raw.at(fromA + n) != m_files.at(fb).raw.at(fromB + n)) {
                    m->diverged = true;
                    if (m->diff.size() < kMaxDiff) m->diff.append({fa, fromA + n});
                    if (m->diff.size() < kMaxDiff) m->diff.append({fb, fromB + n});
                }
                ++n;
            }
            return n;
        };
        const int first = run(pa, pb);
        matched = first;
        ea = pa + first;
        eb = pb + first;
        for (bool grew = true; grew;) {
            grew = false;
            for (int total = 1; total <= 2 * kMaxGap && !grew; ++total)
                for (int ga = 0; ga <= qMin(total, kMaxGap) && !grew; ++ga) {
                    const int gb = total - ga;
                    if (gb > kMaxGap) continue;
                    const int na = ea + ga, nb = eb + gb;
                    if (na + 1 >= limitA || nb + 1 >= size(fb)) continue;
                    if (id(fa, na) != id(fb, nb) || id(fa, na + 1) != id(fb, nb + 1)) continue;
                    // Measure the resumed run before recording anything.
                    int n = 0;
                    while (na + n < limitA && nb + n < size(fb) && id(fa, na + n) == id(fb, nb + n)) ++n;
                    const int spanA = na + n - pa, spanB = nb + n - pb;
                    const double sim = double(matched + n) / double(qMax(spanA, spanB));
                    if (sim < m_similarity) continue;
                    for (int i = ea; i < na; ++i)
                        if (m->diff.size() < kMaxDiff) m->diff.append({fa, i});
                    for (int i = eb; i < nb; ++i)
                        if (m->diff.size() < kMaxDiff) m->diff.append({fb, i});
                    run(na, nb);
                    matched += n;
                    ea = na + n;
                    eb = nb + n;
                    m->diverged = true;
                    m->similarity = sim;
                    grew = true;
                }
        }
        if (matched < m_minLines) return false;
        m->fa = fa; m->sa = pa; m->ea = ea;
        m->fb = fb; m->sb = pb; m->eb = eb;
        m->matched = matched;
        return true;
    }

    const QList<File> &m_files;
    int m_minLines;
    double m_similarity;
    bool m_names;
};

// A site: one place a group's code sits, in compared-line indices.
struct Site {
    int file = 0, start = 0, end = 0;   // [start, end)
};

class Grouper {
public:
    // The node for a range: an existing site in the same file overlapping at
    // least half of the shorter range, widened to cover both, else a new one.
    int node(int file, int start, int end) {
        for (int i = 0; i < m_sites.size(); ++i) {
            Site &s = m_sites[i];
            if (s.file != file) continue;
            const int overlap = qMin(s.end, end) - qMax(s.start, start);
            if (overlap * 2 >= qMin(s.end - s.start, end - start)) {
                s.start = qMin(s.start, start);
                s.end = qMax(s.end, end);
                return i;
            }
        }
        m_sites.append({file, start, end});
        m_parent.append(int(m_parent.size()));
        return int(m_sites.size()) - 1;
    }
    int find(int i) { return m_parent[i] == i ? i : (m_parent[i] = find(m_parent[i])); }
    void unite(int a, int b) { m_parent[find(a)] = find(b); }
    const QList<Site> &sites() const { return m_sites; }

private:
    QList<Site> m_sites;
    QList<int> m_parent;
};

QJsonObject refusal(const QString &error) {
    QJsonObject e;
    e[QStringLiteral("ok")] = false;
    e[QStringLiteral("code")] = QStringLiteral("bad_args");
    e[QStringLiteral("error")] = error;
    return e;
}

} // namespace

QJsonObject run(const QString &rootCanonical, const QJsonObject &args) {
    const QString verb = QStringLiteral("duplicate_blocks");
    const int minLines = args.value(QStringLiteral("min_lines")).toInt(kDefaultMinLines);
    if (args.contains(QStringLiteral("min_lines")) && (minLines < 2 || minLines > 200))
        return refusal(QStringLiteral("duplicate_blocks: \"min_lines\" must be 2 to 200"));
    const double similarity = args.value(QStringLiteral("similarity")).toDouble(kDefaultSimilarity);
    if (similarity < 0.5 || similarity > 1.0)
        return refusal(QStringLiteral("duplicate_blocks: \"similarity\" must be 0.5 to 1"));
    const int top = args.value(QStringLiteral("top")).toInt(kDefaultTop);
    if (args.contains(QStringLiteral("top")) && (top < 1 || top > 500))
        return refusal(QStringLiteral("duplicate_blocks: \"top\" must be 1 to 500"));
    const bool names = args.value(QStringLiteral("normalise_names")).toBool(false);

    CodeWalk::Options walkOpts;
    walkOpts.includeTests = args.value(QStringLiteral("include_tests")).toBool(false);
    walkOpts.includeGenerated = args.value(QStringLiteral("include_generated")).toBool(false);
    const CodeWalk::Result walked = CodeWalk::collect(
        rootCanonical, args.value(QStringLiteral("paths")).toArray(), walkOpts, verb);
    if (!walked.refusal.isEmpty()) return walked.refusal;

    const QDir root(rootCanonical);
    QHash<QString, int> vocab;
    QList<File> files;
    int linesCompared = 0;
    for (const QString &abs : walked.files) {
        QFile in(abs);
        if (!in.open(QIODevice::ReadOnly)) continue;
        files.append(load(root.relativeFilePath(abs), QString::fromUtf8(in.readAll()), names, &vocab));
        linesCompared += int(files.last().ids.size());
    }

    const QList<Match> matches = Finder(files, minLines, similarity, names).run();
    Grouper g;
    QList<QPair<int, int>> matchNodes;
    for (const Match &m : matches) {
        const int a = g.node(m.fa, m.sa, m.ea);
        const int b = g.node(m.fb, m.sb, m.eb);
        matchNodes.append({a, b});
    }
    for (const auto &ab : std::as_const(matchNodes)) g.unite(ab.first, ab.second);

    struct Group {
        QList<int> sites;
        int lines = 0;
        double similarity = 1.0;
        bool diverged = false;
        QList<DiffLine> diff;
    };
    QHash<int, Group> groups;
    for (int i = 0; i < g.sites().size(); ++i) groups[g.find(i)].sites.append(i);
    for (int i = 0; i < matches.size(); ++i) {
        const Match &m = matches.at(i);
        Group &gr = groups[g.find(matchNodes.at(i).first)];
        gr.lines = qMax(gr.lines, m.matched);
        gr.similarity = qMin(gr.similarity, m.similarity);
        gr.diverged = gr.diverged || m.diverged;
        for (const DiffLine &d : m.diff) {
            bool seen = false;
            for (const DiffLine &e : std::as_const(gr.diff))
                seen = seen || (e.file == d.file && e.index == d.index);
            if (!seen && gr.diff.size() < kMaxDiff) gr.diff.append(d);
        }
    }

    // A site widened after another was made can come to overlap it; the two
    // are one place, so they merge before copies are counted.
    QList<Site> merged;
    QList<Group> ordered;
    for (Group &gr : groups) {
        std::sort(gr.sites.begin(), gr.sites.end(), [&](int x, int y) {
            const Site &a = g.sites().at(x), &b = g.sites().at(y);
            return a.file != b.file ? files.at(a.file).rel < files.at(b.file).rel : a.start < b.start;
        });
        QList<int> kept;
        for (int si : std::as_const(gr.sites)) {
            const Site &s = g.sites().at(si);
            if (!kept.isEmpty()) {
                Site &last = merged[kept.last()];
                if (last.file == s.file && s.start <= last.end) {
                    last.end = qMax(last.end, s.end);
                    continue;
                }
            }
            merged.append(s);
            kept.append(int(merged.size()) - 1);
        }
        gr.sites = kept;
        if (gr.sites.size() >= 2) ordered.append(gr);
    }
    std::sort(ordered.begin(), ordered.end(), [&](const Group &a, const Group &b) {
        if (a.lines != b.lines) return a.lines > b.lines;
        if (a.sites.size() != b.sites.size()) return a.sites.size() > b.sites.size();
        const Site &sa = merged.at(a.sites.first()), &sb = merged.at(b.sites.first());
        if (sa.file != sb.file) return files.at(sa.file).rel < files.at(sb.file).rel;
        return sa.start < sb.start;
    });

    QJsonArray out;
    for (int i = 0; i < ordered.size() && i < top; ++i) {
        const Group &gr = ordered.at(i);
        QJsonArray sites;
        for (int si : gr.sites) {
            const Site &s = merged.at(si);
            const File &f = files.at(s.file);
            QJsonObject o;
            o[QStringLiteral("file")] = f.rel;
            o[QStringLiteral("start")] = f.lineNo.at(s.start);
            o[QStringLiteral("end")] = f.lineNo.at(s.end - 1);
            sites.append(o);
        }
        const Site &first = merged.at(gr.sites.first());
        const QString text = files.at(first.file).norm.mid(first.start, first.end - first.start)
                                 .join(QLatin1Char('\n'));
        QJsonArray diff;
        for (const DiffLine &d : gr.diff) {
            QJsonObject o;
            o[QStringLiteral("file")] = files.at(d.file).rel;
            o[QStringLiteral("line")] = files.at(d.file).lineNo.at(d.index);
            o[QStringLiteral("text")] = files.at(d.file).raw.at(d.index);
            diff.append(o);
        }
        QJsonObject o;
        o[QStringLiteral("id")] = QStringLiteral("dup:") + QString::fromLatin1(
            QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
        o[QStringLiteral("lines")] = gr.lines;
        o[QStringLiteral("copies")] = int(gr.sites.size());
        o[QStringLiteral("diverged")] = gr.diverged;
        o[QStringLiteral("finding_shape")] = gr.sites.size() >= 3 || gr.diverged;
        o[QStringLiteral("similarity")] = gr.similarity;
        o[QStringLiteral("sites")] = sites;
        o[QStringLiteral("diff")] = diff;
        out.append(o);
    }

    QJsonObject scanned = CodeWalk::scannedSummary(walked);
    scanned[QStringLiteral("lines_compared")] = linesCompared;
    scanned[QStringLiteral("groups_total")] = int(ordered.size());
    scanned[QStringLiteral("rows_truncated")] = ordered.size() > top;
    QJsonObject env;
    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("groups")] = out;
    env[QStringLiteral("top")] = top;
    env[QStringLiteral("scanned")] = scanned;
    return env;
}

} // namespace DuplicateBlocks
