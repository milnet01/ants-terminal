// ANTS-3368 — co_change_family pure seam. See header for the contract and
// docs/specs/ANTS-3368-co-change-family.md for the reasoning.

#include "cochangefamily.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace CoChangeFamily {
namespace {

bool isWordChar(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isSeparator(QChar c) {
    return c == QLatin1Char('_') || c == QLatin1Char('-') ||
           c == QLatin1Char('.');
}

// INV-4 — frozen in the spec; kept sorted for reading, not for lookup.
const QStringList &stopwords() {
    static const QStringList kWords = {
        QStringLiteral("count"),  QStringLiteral("data"),
        QStringLiteral("disabled"), QStringLiteral("enabled"),
        QStringLiteral("flag"),   QStringLiteral("get"),
        QStringLiteral("has"),    QStringLiteral("index"),
        QStringLiteral("is"),     QStringLiteral("m"),
        QStringLiteral("mode"),   QStringLiteral("name"),
        QStringLiteral("off"),    QStringLiteral("on"),
        QStringLiteral("p"),      QStringLiteral("set"),
        QStringLiteral("size"),   QStringLiteral("type"),
        QStringLiteral("value"),
    };
    return kWords;
}

// Clip to a UTF-8 byte budget without splitting a code point.
QString clipUtf8(const QString &s, int maxBytes) {
    if (maxBytes <= 0 || s.toUtf8().size() <= maxBytes) return s;
    QString out = s;
    while (!out.isEmpty() && out.toUtf8().size() > maxBytes) out.chop(1);
    return out;
}

}  // namespace

const char *roleStr(Role r) {
    switch (r) {
        case Role::JsonKey:  return "json_key";
        case Role::Member:   return "member";
        case Role::Mutator:  return "mutator";
        case Role::Signal:   return "signal";
        case Role::Type:     return "type";
        case Role::Reference: break;
    }
    return "reference";
}

QStringList splitWords(const QString &s) {
    QStringList out;
    QString cur;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (isSeparator(c)) {
            if (!cur.isEmpty()) { out.append(cur); cur.clear(); }
            continue;
        }
        // A lower->upper boundary starts a new word (`mcpEnabled`), but an
        // all-caps run does not (`MCP_ENABLED` splits on the underscore).
        if (!cur.isEmpty() && c.isUpper() && s.at(i - 1).isLower()) {
            out.append(cur);
            cur.clear();
        }
        cur.append(c.toLower());
    }
    if (!cur.isEmpty()) out.append(cur);
    return out;
}

bool isStopword(const QString &word) {
    return stopwords().contains(word.toLower());
}

bool allStopwords(const QStringList &words) {
    if (words.isEmpty()) return false;
    for (const QString &w : words)
        if (!isStopword(w)) return false;
    return true;
}

bool isValidStem(const QString &stem) {
    if (stem.isEmpty()) return false;
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.\\-]+$"));
    return re.match(stem).hasMatch();
}

int defaultMinRun(int stemWordCount) {
    if (stemWordCount <= 0) return 1;
    return std::min(2, stemWordCount);
}

int clampMinRun(int requested, int stemWordCount) {
    if (stemWordCount <= 0) return 1;
    return std::max(1, std::min(requested, stemWordCount));
}

Run longestRun(const QStringList &stemWords, const QStringList &candWords) {
    Run best;
    for (int i = 0; i < stemWords.size(); ++i) {
        for (int j = 0; j < candWords.size(); ++j) {
            int k = 0;
            while (i + k < stemWords.size() && j + k < candWords.size() &&
                   stemWords.at(i + k) == candWords.at(j + k)) {
                ++k;
            }
            if (k <= best.len) continue;
            // INV-4 — a run of nothing but stopwords carries no signal, so
            // it never becomes the best run. Skipping it here (rather than
            // filtering afterwards) lets a shorter distinctive run win.
            if (allStopwords(stemWords.mid(i, k))) continue;
            best.len       = k;
            best.stemStart = i;
        }
    }
    return best;
}

QString scanPattern(const QStringList &stemWords, int minRun) {
    if (stemWords.isEmpty()) return {};
    const QString sep = QStringLiteral("[_.\\-]?");

    QStringList alts;
    for (int i = 0; i + 1 < stemWords.size(); ++i) {
        alts.append(QRegularExpression::escape(stemWords.at(i)) + sep +
                    QRegularExpression::escape(stemWords.at(i + 1)));
    }
    // INV-2 — min_run widens the SCAN, not merely the filter. A pairs-only
    // pattern cannot produce a one-word match, so without this a min_run:1
    // call would return exactly what the default returned.
    if (minRun <= 1 || stemWords.size() < 2) {
        for (const QString &w : stemWords) {
            const QString esc = QRegularExpression::escape(w);
            if (!alts.contains(esc)) alts.append(esc);
        }
    }
    if (alts.isEmpty()) return {};
    return QStringLiteral("(?i)(?:") + alts.join(QLatin1Char('|')) +
           QStringLiteral(")");
}

Candidate widenToCandidate(const QString &line, int matchStart, int matchEnd) {
    Candidate c;
    if (line.isEmpty()) return c;
    const int len = static_cast<int>(line.size());
    matchStart = std::max(0, std::min(matchStart, len));
    matchEnd   = std::max(matchStart, std::min(matchEnd, len));

    // Inside a string literal the candidate is the literal's contents: the
    // JSON key uses a separator convention the surrounding C++ does not.
    int quotes = 0;
    int lastOpen = -1;
    for (int i = 0; i < matchStart; ++i) {
        if (line.at(i) == QLatin1Char('\\')) { ++i; continue; }
        if (line.at(i) == QLatin1Char('"')) { ++quotes; lastOpen = i; }
    }
    if ((quotes % 2) == 1 && lastOpen >= 0) {
        int close = -1;
        for (int i = matchEnd; i < line.size(); ++i) {
            if (line.at(i) == QLatin1Char('\\')) { ++i; continue; }
            if (line.at(i) == QLatin1Char('"')) { close = i; break; }
        }
        if (close > lastOpen) {
            c.inLiteral = true;
            c.name      = line.mid(lastOpen + 1, close - lastOpen - 1);
            return c;
        }
    }

    int s = matchStart;
    int e = matchEnd;
    while (s > 0 && isWordChar(line.at(s - 1))) --s;
    while (e < line.size() && isWordChar(line.at(e))) ++e;
    c.name = line.mid(s, e - s);
    return c;
}

Role classifyRole(const QString &line, const Candidate &cand) {
    if (cand.inLiteral) return Role::JsonKey;
    const QString &n = cand.name;
    if (n.isEmpty()) return Role::Reference;

    if (n.startsWith(QLatin1String("m_"))) return Role::Member;
    if (n.size() > 3 && n.startsWith(QLatin1String("set")) &&
        n.at(3).isUpper()) {
        return Role::Mutator;
    }
    if (n.endsWith(QLatin1String("Changed"))) return Role::Signal;
    if (n.at(0).isUpper()) {
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1String("struct ")) ||
            t.startsWith(QLatin1String("class ")) ||
            t.startsWith(QLatin1String("enum "))) {
            return Role::Type;
        }
    }
    return Role::Reference;
}

int clampMaxSites(int requested) {
    return std::max(1, std::min(requested, 1000));
}

Result assemble(const QVector<RawMatch> &matches, const QVector<Stem> &stems,
                const Options &opts) {
    Result out;
    if (stems.isEmpty()) return out;

    // INV-6 — one row per (path, line). A line matching several stems is
    // owned by the longest run, ties by position in `stems`.
    QHash<QString, Site> byKey;
    for (const RawMatch &m : matches) {
        const Candidate cand =
            widenToCandidate(m.text, m.matchStart, m.matchEnd);
        if (cand.name.isEmpty()) continue;
        const QStringList candWords = splitWords(cand.name);
        if (candWords.isEmpty()) continue;

        int bestIdx = -1;
        Run bestRun;
        for (int si = 0; si < stems.size(); ++si) {
            const Run r = longestRun(stems.at(si).words, candWords);
            if (r.len <= 0 || r.len < stems.at(si).minRun) continue;
            if (r.len > bestRun.len) {   // strict: first stem wins a tie
                bestRun = r;
                bestIdx = si;
            }
        }
        if (bestIdx < 0) continue;

        Site s;
        s.path   = m.path;
        s.line   = m.line;
        s.stem   = stems.at(bestIdx).name;
        s.name   = cand.name;
        s.role   = classifyRole(m.text, cand);
        s.run    = stems.at(bestIdx).words.mid(bestRun.stemStart, bestRun.len);
        s.runLen = bestRun.len;
        s.text   = clipUtf8(m.text, opts.maxTextBytes);

        const QString key =
            s.path + QLatin1Char(':') + QString::number(s.line);
        const auto it = byKey.constFind(key);
        if (it == byKey.constEnd() || it->runLen < s.runLen) byKey.insert(key, s);
    }

    QVector<Site> all;
    all.reserve(byKey.size());
    for (auto it = byKey.cbegin(); it != byKey.cend(); ++it) all.append(*it);

    // INV-7 — when the cap binds, retain the highest run_len. Sorting by
    // strength first makes the truncation a prefix of this order.
    std::sort(all.begin(), all.end(), [](const Site &a, const Site &b) {
        if (a.runLen != b.runLen) return a.runLen > b.runLen;
        if (a.path != b.path)     return a.path < b.path;
        return a.line < b.line;
    });
    const int cap = clampMaxSites(opts.maxSites);
    if (all.size() > cap) {
        all.resize(cap);
        out.truncated = true;
    }

    // INV-6 — final order: file by max run_len desc, then path, then line.
    QHash<QString, int> maxRunPerFile;
    for (const Site &s : all) {
        auto it = maxRunPerFile.find(s.path);
        if (it == maxRunPerFile.end()) maxRunPerFile.insert(s.path, s.runLen);
        else if (*it < s.runLen) *it = s.runLen;
    }
    std::sort(all.begin(), all.end(),
              [&maxRunPerFile](const Site &a, const Site &b) {
                  const int ma = maxRunPerFile.value(a.path);
                  const int mb = maxRunPerFile.value(b.path);
                  if (ma != mb)          return ma > mb;
                  if (a.path != b.path)  return a.path < b.path;
                  return a.line < b.line;
              });

    out.sites = all;
    return out;
}

}  // namespace CoChangeFamily
