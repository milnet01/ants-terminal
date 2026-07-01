// ANTS-1305 — similar_code pattern matcher. See similarcode.h header.

#include "similarcode.h"

#include "fileoutline.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace SimilarCode {

namespace {

constexpr int kDefaultMaxFiles = 5000;
constexpr int kDefaultResults  = 3;
constexpr int kMaxResultsCap   = 20;
constexpr int kMaxShapeLen     = 512;
constexpr int kMinTokenLen     = 2;
constexpr int kPerFileSymbols  = 1000;  // FileOutline's own cap ceiling

// ANTS-3380 — a signature that shares exactly ONE token with the query is
// an incidental match: token-set Jaccard rewards a short signature with a
// single common word as highly as a genuinely richer signature (a
// one-token `add` tied a 13-token `Settings` struct at 0.0909, then won the
// (file,line) tiebreak). Halving the score on a single-token overlap
// demotes it below any 2+-token-overlap match without dropping it. Note the
// self-correcting property: when the QUERY itself is trivial (every
// candidate shares exactly one token) the penalty applies uniformly, so
// relative ranking is unchanged — it only bites when some candidates share
// ≥2 tokens and others share 1.
constexpr double kIncidentalOverlapPenalty = 0.5;

bool isAsciiAlnum(QChar c) {
    const ushort u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
           (u >= '0' && u <= '9');
}

// Lowercased extension → language family; Auto == "skip".
Lang langForExt(const QString &ext) {
    if (ext == QLatin1String("cpp") || ext == QLatin1String("cc")  ||
        ext == QLatin1String("cxx") || ext == QLatin1String("c")   ||
        ext == QLatin1String("h")   || ext == QLatin1String("hpp") ||
        ext == QLatin1String("hh")  || ext == QLatin1String("hxx")) {
        return Lang::Cpp;
    }
    if (ext == QLatin1String("py") || ext == QLatin1String("pyi")) return Lang::Py;
    return Lang::Auto;
}

FileOutline::Mode modeFor(Lang l) {
    return (l == Lang::Py) ? FileOutline::Mode::Py : FileOutline::Mode::Cpp;
}

const char *langStr(Lang l) { return (l == Lang::Py) ? "py" : "cpp"; }

double roundScore(double x) { return std::round(x * 10000.0) / 10000.0; }

// ANTS-3380 — count of tokens shared between two token lists (|a ∩ b| on
// the deduped sets). Mirrors jaccard's intersection step; used to detect
// the incidental single-token overlap that jaccard alone cannot punish.
int sharedTokenCount(const QStringList &a, const QStringList &b) {
    const QSet<QString> sa(a.begin(), a.end());
    const QSet<QString> sb(b.begin(), b.end());
    int inter = 0;
    for (const QString &t : sa)
        if (sb.contains(t)) ++inter;
    return inter;
}

// Ranking order: higher score first, then (file, line) ascending.
bool ranksBefore(const Match &a, const Match &b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.file != b.file)   return a.file < b.file;
    return a.line < b.line;
}

struct ScanState {
    int  rootPrefixLen = 0;
    Lang langFilter = Lang::Auto;
    int  maxFiles = kDefaultMaxFiles;
    int  filesScanned = 0;
    bool walkCapped = false;

    QStringList queryTokens;
    int  resultCap = kDefaultResults;
    int  matchesTotal = 0;
    QVector<Match> matches;  // bounded top-K (size ≤ resultCap)
};

// Insert into the bounded top-K, evicting the lowest-ranked entry when
// full and the newcomer beats it. Deterministic w.r.t. ranksBefore.
void offer(ScanState &st, const Match &m) {
    ++st.matchesTotal;
    if (st.matches.size() < st.resultCap) {
        st.matches.append(m);
        return;
    }
    int worst = 0;
    for (int i = 1; i < st.matches.size(); ++i)
        if (ranksBefore(st.matches[worst], st.matches[i])) worst = i;
    if (ranksBefore(m, st.matches[worst])) st.matches[worst] = m;
}

void scanFile(ScanState &st, const QFileInfo &fi, Lang lang) {
    const QJsonObject out = FileOutline::compute(
        fi.absoluteFilePath(), modeFor(lang), /*includeDocComment=*/false,
        kPerFileSymbols);
    if (!out.value(QStringLiteral("ok")).toBool()) return;  // unopenable → not counted
    ++st.filesScanned;

    const QString rel = fi.absoluteFilePath().mid(st.rootPrefixLen);
    const QJsonArray syms = out.value(QStringLiteral("symbols")).toArray();
    for (const QJsonValue &v : syms) {
        const QJsonObject s = v.toObject();
        const QString sig = s.value(QStringLiteral("signature")).toString();
        const QStringList sigTokens = tokenize(sig);
        double score = jaccard(st.queryTokens, sigTokens);
        if (score <= 0.0) continue;
        // ANTS-3380 — demote an incidental single-token overlap so a
        // trivial signature can't tie/outrank a genuinely richer match.
        if (sharedTokenCount(st.queryTokens, sigTokens) == 1)
            score *= kIncidentalOverlapPenalty;
        Match m;
        m.file      = rel;
        m.line      = s.value(QStringLiteral("line")).toInt();
        m.signature = sig;
        m.kind      = s.value(QStringLiteral("kind")).toString();
        m.lang      = QString::fromLatin1(langStr(lang));
        m.score     = roundScore(score);
        offer(st, m);
    }
}

void walk(ScanState &st, const QString &dirPath) {
    if (st.walkCapped) return;
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (st.walkCapped) return;
        if (fi.isSymLink()) continue;  // never follow symlinks out of root

        if (fi.isDir()) {
            const QString name = fi.fileName();
            if (name.startsWith(QLatin1Char('.'))) continue;
            if (name == QLatin1String("build") ||
                name.startsWith(QLatin1String("build-"))) continue;
            if (name == QLatin1String("node_modules")) continue;
            walk(st, fi.absoluteFilePath());
            continue;
        }
        if (!fi.isFile()) continue;

        const Lang lang = langForExt(fi.suffix().toLower());
        if (lang == Lang::Auto) continue;  // unknown / unsupported extension
        if (st.langFilter != Lang::Auto && lang != st.langFilter) continue;

        if (st.filesScanned >= st.maxFiles) { st.walkCapped = true; return; }
        scanFile(st, fi, lang);
    }
}

bool rootUsable(const QString &rootCanonical) {
    return !rootCanonical.isEmpty() && QFileInfo(rootCanonical).isDir();
}

}  // namespace

Lang parseLang(const QString &s) {
    if (s.isEmpty() || s == QLatin1String("auto")) return Lang::Auto;
    if (s == QLatin1String("cpp")) return Lang::Cpp;
    if (s == QLatin1String("py"))  return Lang::Py;
    return Lang::Auto;
}

QStringList tokenize(const QString &shape) {
    QStringList out;
    QString cur;
    const QString lower = shape.toLower();
    auto flush = [&] {
        if (cur.size() >= kMinTokenLen) out.append(cur);
        cur.clear();
    };
    for (const QChar c : lower) {
        if (isAsciiAlnum(c)) cur.append(c);
        else                 flush();
    }
    flush();
    return out;
}

double jaccard(const QStringList &a, const QStringList &b) {
    const QSet<QString> sa(a.begin(), a.end());
    const QSet<QString> sb(b.begin(), b.end());
    if (sa.isEmpty() && sb.isEmpty()) return 0.0;
    int inter = 0;
    for (const QString &t : sa)
        if (sb.contains(t)) ++inter;
    const int uni = sa.size() + sb.size() - inter;
    if (uni == 0) return 0.0;
    return static_cast<double>(inter) / static_cast<double>(uni);
}

Result findSimilar(const QString &rootCanonical,
                   const QString &shape,
                   const Options &opts) {
    Result r;
    const QString s = shape.trimmed();
    if (s.isEmpty() || s.size() > kMaxShapeLen) {
        r.ok = false;
        r.code = QStringLiteral("bad_args");
        r.error = QStringLiteral("similar_code: \"shape\" missing, empty, "
                                 "or longer than 512 chars");
        return r;
    }
    const QStringList qTokens = tokenize(s);
    if (qTokens.isEmpty()) {
        r.ok = false;
        r.code = QStringLiteral("bad_args");
        r.error = QStringLiteral("similar_code: \"shape\" has no usable tokens");
        return r;
    }
    if (!rootUsable(rootCanonical)) return r;  // empty ok result

    ScanState st;
    const QString root = QDir::cleanPath(rootCanonical);
    st.rootPrefixLen = root.length() + 1;  // strip "<root>/"
    st.langFilter = opts.lang;
    st.maxFiles = (opts.maxFiles > 0) ? opts.maxFiles : kDefaultMaxFiles;
    st.queryTokens = qTokens;
    int cap = (opts.maxResults > 0) ? opts.maxResults : kDefaultResults;
    if (cap > kMaxResultsCap) cap = kMaxResultsCap;
    st.resultCap = cap;

    walk(st, root);

    std::sort(st.matches.begin(), st.matches.end(), ranksBefore);
    r.matches = st.matches;
    r.matchesTotal = st.matchesTotal;
    r.filesScanned = st.filesScanned;
    r.walkCapped = st.walkCapped;
    r.truncated = st.matchesTotal > r.matches.size();
    return r;
}

}  // namespace SimilarCode
