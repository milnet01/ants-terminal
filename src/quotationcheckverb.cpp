// quotation_check (ANTS-5502). Contract:
// docs/specs/ANTS-5502-quotation-check.md.

#include "quotationcheckverb.h"

#include "gitwrap.h"
#include "pathvalidation.h"
#include "remotecontrol.h"  // capJsonArrayToBytes, header-inline

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace QuotationCheck {

namespace {

bool isPosixSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Keeps text[i] and its source offset.
void keep(Normalised &out, const Normalised &in, qsizetype i)
{
    out.text.append(in.text[i]);
    out.source.push_back(in.source[i]);
}

// Step 1, STRIP: s/^[[:space:]]*>\{1,\} \?// on each line.
Normalised strip(const QByteArray &raw)
{
    Normalised out;
    out.text.reserve(raw.size());
    out.source.reserve(raw.size());
    qsizetype i = 0;
    const qsizetype n = raw.size();
    while (i < n) {
        qsizetype end = raw.indexOf('\n', i);
        if (end < 0)
            end = n;
        qsizetype p = i;
        while (p < end && isPosixSpace(raw[p]))
            ++p;
        qsizetype from = i;
        if (p < end && raw[p] == '>') {
            while (p < end && raw[p] == '>')
                ++p;
            if (p < end && raw[p] == ' ')
                ++p;
            from = p;
        }
        for (qsizetype k = from; k < end; ++k) {
            out.text.append(raw[k]);
            out.source.push_back(k);
        }
        if (end < n) {
            out.text.append('\n');
            out.source.push_back(end);
        }
        i = end + 1;
    }
    return out;
}

// Steps 2 and 3: CR, tab and newline become a space; runs of spaces squeeze.
Normalised flatten(const Normalised &in)
{
    Normalised out;
    out.text.reserve(in.text.size());
    out.source.reserve(in.text.size());
    for (qsizetype i = 0; i < in.text.size(); ++i) {
        char c = in.text[i];
        if (c == '\r' || c == '\t' || c == '\n')
            c = ' ';
        if (c == ' ' && !out.text.isEmpty() && out.text.back() == ' ')
            continue;
        out.text.append(c);
        out.source.push_back(in.source[i]);
    }
    return out;
}

// Step 4, LINK: s/\[([^]]*)\]\([^)]*\)/\1/g. From a `[`, `[^]]*` cannot
// pass the first `]`, so a match exists exactly when that `]` is followed by
// `(` and a `)` follows somewhere after it.
Normalised renderLinks(const Normalised &in)
{
    Normalised out;
    const QByteArray &t = in.text;
    const qsizetype n = t.size();
    qsizetype i = 0;
    while (i < n) {
        if (t[i] == '[') {
            const qsizetype close = t.indexOf(']', i + 1);
            if (close >= 0 && close + 1 < n && t[close + 1] == '(') {
                const qsizetype paren = t.indexOf(')', close + 2);
                if (paren >= 0) {
                    for (qsizetype k = i + 1; k < close; ++k)
                        keep(out, in, k);
                    i = paren + 1;
                    continue;
                }
            }
        }
        keep(out, in, i);
        ++i;
    }
    return out;
}

// Step 5, MARK: delete every `**`, then every backquote.
Normalised dropMarks(const Normalised &in)
{
    Normalised bold;
    const QByteArray &t = in.text;
    for (qsizetype i = 0; i < t.size(); ++i) {
        if (t[i] == '*' && i + 1 < t.size() && t[i + 1] == '*') {
            ++i;
            continue;
        }
        keep(bold, in, i);
    }
    Normalised out;
    for (qsizetype i = 0; i < bold.text.size(); ++i) {
        if (bold.text[i] != '`')
            keep(out, bold, i);
    }
    return out;
}

// `failed` memoises the (glob index, path index) states already proved not to
// match. Without it a caller's `allowed` glob such as `*a*a*a*a*a*a*b` against
// a long path backtracks exponentially in the number of stars; with it every
// state is tried once, so the cost is bounded by g.size() * p.size().
bool globMatchAt(const QString &g, qsizetype gi, const QString &p, qsizetype pi,
                 std::vector<bool> &failed)
{
    const size_t key = size_t(gi) * size_t(p.size() + 1) + size_t(pi);
    if (failed[key])
        return false;
    while (gi < g.size()) {
        const QChar c = g[gi];
        if (c == '*') {
            const bool deep = gi + 1 < g.size() && g[gi + 1] == '*';
            const qsizetype rest = gi + (deep ? 2 : 1);
            for (qsizetype k = pi; k <= p.size(); ++k) {
                if (globMatchAt(g, rest, p, k, failed))
                    return true;
                if (k < p.size() && !deep && p[k] == '/')
                    break;
            }
            failed[key] = true;
            return false;
        }
        if (pi >= p.size())
            break;
        if (c == '?') {
            if (p[pi] == '/')
                break;
        } else if (c != p[pi]) {
            break;
        }
        ++gi;
        ++pi;
    }
    if (gi >= g.size() && pi == p.size())
        return true;
    failed[key] = true;
    return false;
}

} // namespace

Normalised normalise(const QByteArray &raw)
{
    return dropMarks(renderLinks(flatten(strip(raw))));
}

QByteArray normaliseQuotation(const QByteArray &raw)
{
    QByteArray q = normalise(raw).text;
    qsizetype b = 0;
    qsizetype e = q.size();
    while (b < e && q[b] == ' ')
        ++b;
    while (e > b && q[e - 1] == ' ')
        --e;
    return q.mid(b, e - b);
}

bool globMatch(const QString &glob, const QString &path)
{
    // The memo is one bit per state; a glob and path whose product passes 64M
    // states (8 MiB) is not a path glob, and matches nothing.
    const size_t states = size_t(glob.size() + 1) * size_t(path.size() + 1);
    if (states > (size_t(1) << 26))
        return false;
    std::vector<bool> failed(states, false);
    return globMatchAt(glob, 0, path, 0, failed);
}

namespace {

// One subject, read once per (ref, path) per call (INV-8).
struct Subject {
    QString reason;  // non-empty: the read failed, and this is the not_run reason
    QByteArray raw;
    Normalised norm;
    std::vector<qsizetype> newlines;  // ANTS-5732: offsets of '\n' in raw, for lineOf
};

// ANTS-5732 — a JSON number holding an integer of at least `min`. "3" and
// 1.5 are not one.
bool isIntAtLeast(const QJsonValue &v, int min)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    return d == double(qint64(d)) && d >= min && d <= INT_MAX;
}

QJsonObject refuse(const QString &message)
{
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), QStringLiteral("bad_args")},
                       {QStringLiteral("error"), QStringLiteral("quotation_check: ") + message}};
}

bool underRoot(const QString &abs, const QString &root)
{
    return abs == root || abs.startsWith(root + QLatin1Char('/'));
}

bool badRef(const QString &ref)
{
    if (ref.isEmpty() || ref.startsWith(QLatin1Char('-')))
        return true;
    for (const QChar c : ref) {
        if (c.isSpace() || c.category() == QChar::Other_Control)
            return true;
    }
    return false;
}

Subject readWorkingTree(const QString &abs)
{
    Subject s;
    const QFileInfo fi(abs);
    if (!fi.exists()) {
        s.reason = QStringLiteral("not_found");
        return s;
    }
    if (!fi.isFile()) {
        s.reason = QStringLiteral("not_a_file");
        return s;
    }
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) {
        s.reason = QStringLiteral("unreadable");
        return s;
    }
    if (fi.size() > kMaxSubjectBytes) {
        s.reason = QStringLiteral("too_large");
        return s;
    }
    s.raw = f.read(kMaxSubjectBytes + 1);
    if (f.error() != QFileDevice::NoError)
        s.reason = QStringLiteral("unreadable");
    else if (s.raw.size() > kMaxSubjectBytes)
        s.reason = QStringLiteral("too_large");
    return s;
}

Subject readBlob(const QString &root, const QString &ref, const QString &rel)
{
    Subject s;
    const GitWrap::Result r = GitWrap::run(
        root, {QStringLiteral("cat-file"), QStringLiteral("blob"),
               ref + QStringLiteral(":./") + rel},
        int(kMaxSubjectBytes + 1));
    if (r.stdoutTruncated || r.stdoutBytes.size() > kMaxSubjectBytes)
        s.reason = QStringLiteral("too_large");
    else if (!r.started || r.hardKilled || r.crashed || r.exitCode != 0)
        s.reason = QStringLiteral("ref_unreadable");
    else
        s.raw = r.stdoutBytes;
    return s;
}

// Byte offsets of each UTF-8 character start in `q`, plus q.size().
std::vector<qsizetype> charBoundaries(const QByteArray &q)
{
    std::vector<qsizetype> b;
    for (qsizetype i = 0; i < q.size(); ++i) {
        if ((uchar(q[i]) & 0xC0) != 0x80)
            b.push_back(i);
    }
    b.push_back(q.size());
    return b;
}

// ANTS-5732 — the 1-based subject line holding original byte `src`.
int lineOf(const Subject &s, qsizetype src)
{
    return int(std::lower_bound(s.newlines.begin(), s.newlines.end(), src)
               - s.newlines.begin()) + 1;
}

// Fills a miss's matched_prefix_chars, line and nearest (§ 2.3). A prefix
// that occurs implies every shorter one does, so the length is searched.
void describeMiss(QJsonObject &f, const QByteArray &needle, const Subject &s)
{
    const std::vector<qsizetype> b = charBoundaries(needle);
    qsizetype lo = 0;                      // chars known to occur
    qsizetype hi = qsizetype(b.size()) - 1; // the whole needle does not
    while (lo + 1 < hi) {
        const qsizetype mid = (lo + hi) / 2;
        if (s.norm.text.indexOf(needle.left(b[mid])) >= 0)
            lo = mid;
        else
            hi = mid;
    }
    f[QStringLiteral("matched_prefix_chars")] = int(lo);
    if (lo == 0)
        return;
    const qsizetype at = s.norm.text.indexOf(needle.left(b[lo]));
    const qsizetype src = s.norm.source[size_t(at)];
    // lastIndexOf(c, -1) searches from the end, so offset 0 is its own case.
    const qsizetype lineStart = src == 0 ? 0 : s.raw.lastIndexOf('\n', src - 1) + 1;
    f[QStringLiteral("line")] = lineOf(s, src);
    qsizetype end = qMin(s.raw.size(), lineStart + kNearestBytes);
    while (end > lineStart && end < s.raw.size() && (uchar(s.raw[end]) & 0xC0) == 0x80)
        --end;
    f[QStringLiteral("nearest")] = QString::fromUtf8(s.raw.mid(lineStart, end - lineStart));
}

// ANTS-5732 (§ 2.3) — the line of the occurrence nearest `cited`, the earlier
// on a tie, and whether it starts within `tolerance` of it. Every occurrence
// is weighed, since a quotation can sit in the file more than once.
std::pair<int, bool> nearestOccurrence(const QByteArray &needle, const Subject &s,
                                       int cited, int tolerance)
{
    int best = 0;
    int bestDistance = INT_MAX;
    for (qsizetype at = s.norm.text.indexOf(needle); at >= 0;
         at = s.norm.text.indexOf(needle, at + 1)) {
        const int line = lineOf(s, s.norm.source[size_t(at)]);
        const int distance = std::abs(line - cited);
        if (distance <= tolerance)
            return {line, true};
        if (distance < bestDistance) {
            bestDistance = distance;
            best = line;
        }
    }
    return {best, false};
}

} // namespace

QJsonObject run(const QString &rootIn, const QJsonObject &args)
{
    // Shape (§ 2.1): a wrong shape refuses the whole call.
    const QJsonValue itemsV = args.value(QStringLiteral("items"));
    if (!itemsV.isArray())
        return refuse(QStringLiteral("\"items\" must be an array of 1 to %1 objects").arg(kMaxItems));
    const QJsonArray items = itemsV.toArray();
    if (items.isEmpty() || items.size() > kMaxItems)
        return refuse(QStringLiteral("\"items\" holds %1 entries; 1 to %2 are accepted")
                          .arg(items.size()).arg(kMaxItems));
    for (qsizetype i = 0; i < items.size(); ++i) {
        const QJsonValue v = items.at(i);
        const QJsonObject o = v.toObject();
        QString why;
        if (!v.isObject())
            why = QStringLiteral("is not an object");
        else if (!o.value(QStringLiteral("path")).isString()
                 || o.value(QStringLiteral("path")).toString().isEmpty())
            why = QStringLiteral("\"path\" is not a non-empty string");
        else if (!o.value(QStringLiteral("text")).isString())
            why = QStringLiteral("\"text\" is not a string");
        else if (o.contains(QStringLiteral("ref")) && !o.value(QStringLiteral("ref")).isString())
            why = QStringLiteral("\"ref\" is not a string");
        else if (o.contains(QStringLiteral("line")) && !isIntAtLeast(o.value(QStringLiteral("line")), 1))
            why = QStringLiteral("\"line\" is not an integer of 1 or more");
        else if (o.contains(QStringLiteral("tolerance"))
                 && !isIntAtLeast(o.value(QStringLiteral("tolerance")), 0))
            why = QStringLiteral("\"tolerance\" is not an integer of 0 or more");
        if (!why.isEmpty())
            return refuse(QStringLiteral("items[%1] %2").arg(i).arg(why));
    }
    // ANTS-5732 — default 3, measured (§ 2.3).
    int callTolerance = 3;
    if (args.contains(QStringLiteral("tolerance"))) {
        if (!isIntAtLeast(args.value(QStringLiteral("tolerance")), 0))
            return refuse(QStringLiteral("\"tolerance\" is not an integer of 0 or more"));
        callTolerance = args.value(QStringLiteral("tolerance")).toInt();
    }
    QStringList allowed;
    const bool hasAllowed = args.contains(QStringLiteral("allowed"));
    if (hasAllowed) {
        const QJsonValue a = args.value(QStringLiteral("allowed"));
        if (!a.isArray())
            return refuse(QStringLiteral("\"allowed\" must be an array of non-empty strings"));
        const QJsonArray arr = a.toArray();
        for (qsizetype i = 0; i < arr.size(); ++i) {
            if (!arr.at(i).isString() || arr.at(i).toString().isEmpty())
                return refuse(QStringLiteral("allowed[%1] is not a non-empty string").arg(i));
            allowed.append(arr.at(i).toString());
        }
    }

    const QString root = QFileInfo(rootIn).canonicalFilePath();
    const QDir rootDir(root);
    std::map<std::pair<QString, QString>, Subject> subjects;
    int filesRead = 0;
    int nHit = 0, nDisplaced = 0, nMiss = 0, nNotRun = 0, nOutside = 0;
    QJsonArray results, findings, checkErrors;

    for (qsizetype i = 0; i < items.size(); ++i) {
        const QJsonObject o = items.at(i).toObject();
        const QString path = o.value(QStringLiteral("path")).toString();
        const QString text = o.value(QStringLiteral("text")).toString();
        const bool hasRef = o.contains(QStringLiteral("ref"));
        const QString ref = o.value(QStringLiteral("ref")).toString();

        QString result;
        QString reason;
        QJsonObject finding;
        QByteArray needle;
        QString rel;
        QString abs;

        // bad_path: validatePath's reasons other than escaping the root.
        const PathValidation::Check chk = PathValidation::validatePath(
            path, root, QStringLiteral("quotation_check"), QStringLiteral("path"), true);
        if (chk.bad) {
            result = QStringLiteral("not_run");
            reason = QStringLiteral("bad_path");
        }
        if (result.isEmpty()) {
            const QString lexical = QDir::cleanPath(rootDir.absoluteFilePath(path));
            abs = lexical;
            if (!hasRef) {
                const QString canon = QFileInfo(lexical).canonicalFilePath();
                if (!canon.isEmpty())
                    abs = canon;
            }
            rel = rootDir.relativeFilePath(abs);
            bool ok = underRoot(abs, root);
            if (ok && hasAllowed) {
                ok = false;
                for (const QString &g : allowed) {
                    if (globMatch(g, rel)) {
                        ok = true;
                        break;
                    }
                }
            }
            if (!ok)
                result = QStringLiteral("outside_allowed");
        }
        if (result.isEmpty()) {
            const QByteArray utf8 = text.toUtf8();
            if (utf8.size() > kMaxTextBytes) {
                reason = QStringLiteral("text_too_long");
            } else {
                needle = normaliseQuotation(utf8);
                if (needle.isEmpty())
                    reason = QStringLiteral("empty_text");
            }
            if (reason.isEmpty() && hasRef && badRef(ref))
                reason = QStringLiteral("bad_ref");
            if (!reason.isEmpty())
                result = QStringLiteral("not_run");
        }
        if (result.isEmpty()) {
            const auto key = std::make_pair(hasRef ? ref : QString(), rel);
            auto it = subjects.find(key);
            if (it == subjects.end()) {
                Subject s = hasRef ? readBlob(root, ref, rel) : readWorkingTree(abs);
                if (s.reason.isEmpty()) {
                    s.norm = normalise(s.raw);
                    for (qsizetype n = s.raw.indexOf('\n'); n >= 0; n = s.raw.indexOf('\n', n + 1))
                        s.newlines.push_back(n);
                    ++filesRead;
                }
                it = subjects.emplace(key, std::move(s)).first;
            }
            const Subject &s = it->second;
            if (!s.reason.isEmpty()) {
                result = QStringLiteral("not_run");
                reason = s.reason;
            } else if (s.norm.text.indexOf(needle) >= 0) {
                result = QStringLiteral("hit");
                if (o.contains(QStringLiteral("line"))) {
                    const int tolerance = o.contains(QStringLiteral("tolerance"))
                                              ? o.value(QStringLiteral("tolerance")).toInt()
                                              : callTolerance;
                    const auto [line, near] = nearestOccurrence(
                        needle, s, o.value(QStringLiteral("line")).toInt(), tolerance);
                    if (!near) {
                        result = QStringLiteral("hit_displaced");
                        finding[QStringLiteral("line")] = line;
                    }
                }
            } else {
                result = QStringLiteral("miss");
                describeMiss(finding, needle, s);
            }
        }

        results.append(QJsonObject{{QStringLiteral("index"), int(i)},
                                   {QStringLiteral("result"), result}});
        if (result == QLatin1String("hit")) {
            ++nHit;
            continue;
        }
        if (result == QLatin1String("hit_displaced"))
            ++nDisplaced;
        else if (result == QLatin1String("miss"))
            ++nMiss;
        else if (result == QLatin1String("outside_allowed"))
            ++nOutside;
        else
            ++nNotRun;
        finding[QStringLiteral("index")] = int(i);
        finding[QStringLiteral("path")] = path;
        if (hasRef)
            finding[QStringLiteral("ref")] = ref;
        finding[QStringLiteral("kind")] = result;
        if (!reason.isEmpty()) {
            finding[QStringLiteral("reason")] = reason;
            checkErrors.append(QJsonObject{{QStringLiteral("index"), int(i)},
                                           {QStringLiteral("reason"), reason}});
        }
        findings.append(finding);
    }

    QJsonObject env{
        {QStringLiteral("ok"), true},
        {QStringLiteral("items_checked"), int(items.size())},
        {QStringLiteral("counts"), QJsonObject{{QStringLiteral("hit"), nHit},
                                               {QStringLiteral("hit_displaced"), nDisplaced},
                                               {QStringLiteral("miss"), nMiss},
                                               {QStringLiteral("not_run"), nNotRun},
                                               {QStringLiteral("outside_allowed"), nOutside}}},
        {QStringLiteral("files_read"), filesRead},
        {QStringLiteral("normaliser"), QString::fromLatin1(kNormaliser)},
        {QStringLiteral("results"), results},
        {QStringLiteral("findings"), findings},
        {QStringLiteral("check_errors"), checkErrors},
    };
    // § 2.4: max_bytes trims `results` only; findings is never trimmed.
    const auto cap = RemoteControl::capJsonArrayToBytes(
        env, QStringLiteral("results"), QStringLiteral("results_dropped"),
        args.value(QStringLiteral("max_bytes")).toInt(0));
    if (cap.capClamped)
        env[QStringLiteral("bytes_cap_clamped")] = true;
    return env;
}

} // namespace QuotationCheck
