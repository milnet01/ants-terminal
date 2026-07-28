// ANTS-3660 — doc_dedup engine. See docdedup.h for the design and
// docs/specs/ANTS-3660.md for the measurements every constant came from.

#include "docdedup.h"

#include "markdownscan.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace DocDedup {
namespace {

// Segmentation classes, spec § 2.1. Kept byte-identical in meaning to the
// ANTS-3666 calibration script, because every constant in the spec is that
// script's output and a segmentation that differs by one rule moves the
// flagship pair by 23x.
const QRegularExpression &headingRe() {
    static const QRegularExpression re(QStringLiteral("^#{1,6} "));
    return re;
}
const QRegularExpression &listRe() {
    static const QRegularExpression re(QStringLiteral("^\\s*(?:[-*+]|\\d+\\.)\\s"));
    return re;
}
const QRegularExpression &tableRe() {
    static const QRegularExpression re(QStringLiteral("^\\s*\\|"));
    return re;
}
// A continuation line: leading whitespace then something. Matches the
// reference's `^\s+\S`, so a blank line ends a list item's continuations.
const QRegularExpression &indentRe() {
    static const QRegularExpression re(QStringLiteral("^\\s+\\S"));
    return re;
}

}  // namespace

QVector<QPair<int, QString>> segment(const QStringList &lines) {
    const QVector<bool> fence = MarkdownScan::fenceMask(lines);
    QVector<QPair<int, QString>> out;
    QStringList cur;
    int start = 0;

    const auto flush = [&] {
        if (!cur.isEmpty()) out.append({start, cur.join(QLatin1Char(' '))});
        cur.clear();
        start = 0;
    };

    const int n = lines.size();
    int i = 0;
    while (i < n) {
        const QString &ln = lines.at(i);
        // A masked fence line acts as a blank: it BREAKS a run rather than
        // joining it. A code sample is not a sentence in the following
        // paragraph.
        const bool breaks = ln.trimmed().isEmpty() || fence.at(i) ||
                            headingRe().match(ln).hasMatch() ||
                            tableRe().match(ln).hasMatch();
        if (breaks) {
            flush();
            ++i;
            continue;
        }
        if (listRe().match(ln).hasMatch()) {
            // A list item is ONE passage: its marker line joined with the
            // indented continuations beneath it, ending at the next marker, the
            // next unindented line, or a fence.
            flush();
            start = i + 1;
            cur << ln.trimmed();
            ++i;
            while (i < n && !fence.at(i) &&
                   indentRe().match(lines.at(i)).hasMatch() &&
                   !listRe().match(lines.at(i)).hasMatch()) {
                cur << lines.at(i).trimmed();
                ++i;
            }
            flush();
            continue;
        }
        if (start == 0) start = i + 1;
        cur << ln.trimmed();
        ++i;
    }
    flush();
    return out;
}

QString normalise(const QString &text) {
    static const QRegularExpression emph(QStringLiteral("[*_`]+"));
    // UseUnicodePropertiesOption so \w means what it means in the reference
    // implementation: an accented letter is a word character, while the em
    // dashes, section signs and arrows this corpus is full of are not.
    static const QRegularExpression punct(
        QStringLiteral("[^\\w\\s]+"), QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression ws(QStringLiteral("\\s+"));

    QString t = text;
    t.remove(emph);                              // inline-code delims + emphasis
    t.replace(punct, QStringLiteral(" "));       // then punctuation
    t.replace(ws, QStringLiteral(" "));          // fold LAST — see the header
    return t.toLower().trimmed();
}

bool isPointerLine(const QString &raw) {
    // Optional list marker, optional "see", then ONE inline link or bare URL,
    // then optional trailing punctuation — and nothing else. A sentence that
    // merely contains a link is prose.
    static const QRegularExpression re(QStringLiteral(
        "^(?:[-*+]\\s+|\\d+\\.\\s+)?(?:[Ss]ee\\s+)?"
        "(?:\\[[^\\]]*\\]\\([^)]*\\)|https?://\\S+)[.,]?$"));
    return re.match(raw.trimmed()).hasMatch();
}

bool isExcludedPath(const QString &relPath, const Options &opts) {
    // Deliberately NOT QRegularExpression::wildcardToRegularExpression: its `*`
    // stops at a path separator (that is what makes it a *glob*), so
    // `*superpowers/*` never matches `docs/superpowers/plans/x.md` — and
    // crossing separators is the entire point of a path glob here.
    // Qt 6.6's NonPathWildcardConversion says exactly this, but this project's
    // floor is Qt 6.2 (docs/standards/dependencies.md), so the two-line
    // translation is written out rather than raising the floor for it.
    for (const QString &glob : opts.excludedPathGlobs) {
        QString rx = QRegularExpression::escape(glob);
        rx.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
        rx.replace(QStringLiteral("\\?"), QStringLiteral("."));
        const QRegularExpression re(QStringLiteral("\\A(?:") + rx +
                                    QStringLiteral(")\\z"));
        if (re.match(relPath).hasMatch()) return true;
    }
    return false;
}

void Accumulator::add(const QString &text, const QString &relPath,
                      const Options &opts) {
    m_opts = opts;
    if (isExcludedPath(relPath, opts)) return;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const auto &p : segment(lines)) {
        if (isPointerLine(p.second)) continue;
        const QStringList words =
            normalise(p.second).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.size() < opts.minWords) continue;
        if (words.size() < opts.shingleSize) continue;

        // Counted BEFORE the cap, so passagesTotal is a true count and the two
        // numbers stay consistent (INV-6). A run that simply stopped walking
        // could not report the first.
        ++m_total;
        if (m_passages.size() >= opts.maxPassages) continue;

        QSet<int> ids;
        for (int i = 0; i + opts.shingleSize <= words.size(); ++i) {
            const QString key =
                words.mid(i, opts.shingleSize).join(QLatin1Char(' '));
            const auto it = m_vocab.constFind(key);
            if (it == m_vocab.constEnd()) {
                const int id = m_vocab.size();   // sequenced before the insert
                m_vocab.insert(key, id);
                ids.insert(id);
            } else {
                ids.insert(*it);
            }
        }
        Entry e;
        e.file     = relPath;
        e.line     = p.first;
        e.shingles = QVector<int>(ids.cbegin(), ids.cend());
        std::sort(e.shingles.begin(), e.shingles.end());
        m_passages.push_back(std::move(e));
    }
}

Result Accumulator::finish() {
    Result r;
    r.passagesTotal    = m_total;
    r.passagesCompared = int(m_passages.size());
    r.truncated        = m_total > r.passagesCompared;

    // shingle -> passage ids. This plus the per-passage sets is the whole
    // run-scoped footprint (spec § 4); both are needed, because candidate
    // gathering reads the first and exact scoring reads the second.
    QHash<int, QVector<int>> postings;
    for (int i = 0; i < m_passages.size(); ++i)
        for (int s : m_passages.at(i).shingles) postings[s].push_back(i);

    r.distinctShingles = int(postings.size());
    for (auto it = postings.constBegin(); it != postings.constEnd(); ++it) {
        r.totalPostings += int(it->size());
        r.maxPostingLength = qMax(r.maxPostingLength, int(it->size()));
    }

    // (a-index, b-index) alongside pairs, so the clustering can union-find on
    // indices without re-deriving them from (file, line).
    QVector<QPair<int, int>> pairIdx;

    for (int a = 0; a < m_passages.size(); ++a) {
        const QVector<int> &A = m_passages.at(a).shingles;

        // Candidate gathering — and ONLY this step honours maxPostings.
        QSet<int> candSet;
        for (int s : A) {
            const auto it = postings.constFind(s);
            if (it == postings.constEnd()) continue;
            if (it->size() > m_opts.maxPostings) continue;   // INV-7
            for (int b : *it)
                if (b > a) candSet.insert(b);
        }
        QVector<int> cand(candSet.cbegin(), candSet.cend());
        std::sort(cand.begin(), cand.end());   // determinism, not performance

        for (int b : cand) {
            ++r.candidatePairsConsidered;
            const QVector<int> &B = m_passages.at(b).shingles;

            // Exact Jaccard over the FULL sets: a pruned shingle is counted in
            // both the intersection and the union. Scoring never sees the
            // prune (INV-1 / spec § 2.2).
            int inter = 0;
            for (int i = 0, j = 0; i < A.size() && j < B.size();) {
                if (A.at(i) == B.at(j)) { ++inter; ++i; ++j; }
                else if (A.at(i) < B.at(j)) { ++i; }
                else { ++j; }
            }
            const int uni = A.size() + B.size() - inter;
            const double sim = uni > 0 ? double(inter) / uni : 0.0;
            if (sim < m_opts.minSimilarity) continue;

            // INV-4 — canonicalise so `a` precedes `b` in (file, line) order.
            // Not free: the walk order is the caller's, and a composer that
            // adds documents unsorted would otherwise emit them either way
            // round.
            const Entry &ea = m_passages.at(a);
            const Entry &eb = m_passages.at(b);
            const bool aFirst = ea.file != eb.file ? ea.file < eb.file
                                                   : ea.line < eb.line;
            Pair p;
            p.a = {aFirst ? ea.file : eb.file, aFirst ? ea.line : eb.line};
            p.b = {aFirst ? eb.file : ea.file, aFirst ? eb.line : ea.line};
            p.similarity = sim;
            r.pairs.push_back(p);
            pairIdx.push_back({a, b});
        }
    }

    // Stable report order: strongest first, then by location.
    QVector<int> order(r.pairs.size());
    for (int i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        const Pair &px = r.pairs.at(x);
        const Pair &py = r.pairs.at(y);
        if (px.similarity != py.similarity) return px.similarity > py.similarity;
        if (px.a.file != py.a.file) return px.a.file < py.a.file;
        if (px.a.line != py.a.line) return px.a.line < py.a.line;
        if (px.b.file != py.b.file) return px.b.file < py.b.file;
        return px.b.line < py.b.line;
    });
    QVector<Pair> sortedPairs;
    QVector<QPair<int, int>> sortedIdx;
    sortedPairs.reserve(order.size());
    sortedIdx.reserve(order.size());
    for (int i : order) {
        sortedPairs.push_back(r.pairs.at(i));
        sortedIdx.push_back(pairIdx.at(i));
    }
    r.pairs = sortedPairs;
    pairIdx = sortedIdx;

    for (const Pair &p : std::as_const(r.pairs)) {
        DocFinding::Finding f;
        f.verb    = QStringLiteral("doc_dedup");
        f.kind    = QStringLiteral("near_duplicate");
        f.file    = p.a.file;      // anchored at the EARLIER passage
        f.line    = p.a.line;
        f.message = QStringLiteral("near-duplicate of %1:%2 (similarity %3)")
                        .arg(p.b.file)
                        .arg(p.b.line)
                        .arg(p.similarity, 0, 'f', 3);
        f.autoFixable = false;     // report-only, always — INV-5
        f.emissionIndex = int(r.findings.size());
        r.findings.push_back(f);
    }

    // Clusters: connected components over the REPORTED pairs. N documents
    // sharing one stanza are one thing to fix, not N(N-1)/2 findings.
    QHash<int, int> parent;
    const std::function<int(int)> findRoot = [&](int x) -> int {
        if (!parent.contains(x)) parent.insert(x, x);
        while (parent.value(x) != x) {
            parent[x] = parent.value(parent.value(x));
            x = parent.value(x);
        }
        return x;
    };
    for (const auto &pi : std::as_const(pairIdx)) {
        const int ra = findRoot(pi.first);
        const int rb = findRoot(pi.second);
        if (ra != rb) parent[ra] = rb;
    }
    QHash<int, QVector<int>> groups;
    for (auto it = parent.constBegin(); it != parent.constEnd(); ++it)
        groups[findRoot(it.key())].push_back(it.key());

    QHash<int, double> best;
    for (int i = 0; i < pairIdx.size(); ++i) {
        const int root = findRoot(pairIdx.at(i).first);
        best[root] = qMax(best.value(root, 0.0), r.pairs.at(i).similarity);
    }
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        Cluster c;
        QVector<int> members = it.value();
        std::sort(members.begin(), members.end(), [&](int x, int y) {
            const Entry &ex = m_passages.at(x);
            const Entry &ey = m_passages.at(y);
            return ex.file != ey.file ? ex.file < ey.file : ex.line < ey.line;
        });
        for (int m : std::as_const(members))
            c.passages.push_back({m_passages.at(m).file, m_passages.at(m).line});
        c.maxSimilarity = best.value(it.key(), 0.0);
        r.clusters.push_back(c);
    }
    std::sort(r.clusters.begin(), r.clusters.end(),
              [](const Cluster &x, const Cluster &y) {
                  if (x.maxSimilarity != y.maxSimilarity)
                      return x.maxSimilarity > y.maxSimilarity;
                  if (x.passages.size() != y.passages.size())
                      return x.passages.size() > y.passages.size();
                  return x.passages.first().file < y.passages.first().file;
              });
    return r;
}

}  // namespace DocDedup
