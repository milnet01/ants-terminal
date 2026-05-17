// ANTS-1397 v1 — test_audit_* trio engine.
// See header for v1 scope + deferred items.

#include "testauditengine.h"

#include "falseposledger.h"
#include "pathvalidation.h"
#include "roadmapfoldin.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <algorithm>

namespace TestAuditEngine {

namespace {

// ────────────────────────── INV-6 (canonical dimension list) ──
const QStringList &g_kDimensions() {
    static const QStringList v = {
        QStringLiteral("performance"),
        QStringLiteral("flakiness"),
        QStringLiteral("duplication"),
        QStringLiteral("isolation"),
        QStringLiteral("determinism"),
        QStringLiteral("accuracy"),
        QStringLiteral("security"),
        QStringLiteral("verbosity"),
        QStringLiteral("naming"),
        QStringLiteral("coverage_gaps"),
        QStringLiteral("splitting"),
        QStringLiteral("fixtures"),
        QStringLiteral("assertions"),
        QStringLiteral("hardcoded_data"),
        QStringLiteral("setup_teardown"),
        QStringLiteral("parametrisation"),
        QStringLiteral("error_handling"),
        QStringLiteral("doc_strings"),
    };
    return v;
}

// ────────────────────────── INV-9 (chunk_size clamping) ──
constexpr int kChunkSizeMin = 4;
constexpr int kChunkSizeMax = 30;
// ────────────────────────── INV-7 (pre-pass cap) ──
constexpr int kPrePassPerChunkCap = 20;
// ────────────────────────── INV-15 (mtime recheck rate limit) ──
constexpr qint64 kMtimeRecheckRateLimitMs = 5'000;
// ────────────────────────── Partition cache LRU ──
constexpr int kPartitionCacheCap = 16;
// (Pagination soft cap is enforced caller-side via offset/limit
// — v1 trusts the caller to page; v2 measures the rendered
// envelope size like AuditRunner's INV-13 cascade does.)

// ────────────────────────── Framework probes ──
struct FrameworkProbe {
    QString     name;
    QStringList signalFiles;       // any present → matched
    QStringList testGlobs;
};

const QVector<FrameworkProbe> &g_kFrameworks() {
    static const QVector<FrameworkProbe> v = {
        {QStringLiteral("pytest"),
         {QStringLiteral("pyproject.toml"), QStringLiteral("pytest.ini"),
          QStringLiteral("setup.py"), QStringLiteral("tox.ini")},
         {QStringLiteral("tests/**/*.py"), QStringLiteral("test_*.py"),
          QStringLiteral("*_test.py")}},
        {QStringLiteral("jest"),
         {QStringLiteral("package.json"), QStringLiteral("jest.config.js"),
          QStringLiteral("jest.config.ts")},
         {QStringLiteral("**/*.spec.js"),  QStringLiteral("**/*.test.js"),
          QStringLiteral("**/*.spec.ts"),  QStringLiteral("**/*.test.ts")}},
        {QStringLiteral("ctest"),
         {QStringLiteral("CMakeLists.txt")},
         {QStringLiteral("tests/**/*.cpp"), QStringLiteral("test_*.cpp")}},
        {QStringLiteral("cargo"),
         {QStringLiteral("Cargo.toml")},
         {QStringLiteral("tests/**/*.rs")}},
        {QStringLiteral("go"),
         {QStringLiteral("go.mod")},
         {QStringLiteral("**/*_test.go")}},
    };
    return v;
}

// ────────────────────────── Pre-pass pattern set ──
struct PrePattern {
    QString id;             // stable id ("sleep_call", "datetime_now", ...)
    QString dimension;
    QString regex;          // PCRE2
};

const QVector<PrePattern> &g_kPrePatterns() {
    // Small built-in set for v1. Hardcoded; v2 ships JSON resource.
    static const QVector<PrePattern> v = {
        {QStringLiteral("sleep_call"),     QStringLiteral("flakiness"),
            QStringLiteral("\\btime\\.sleep\\(")},
        {QStringLiteral("datetime_now"),   QStringLiteral("determinism"),
            QStringLiteral("\\bdatetime\\.now\\(")},
        {QStringLiteral("hardcoded_password"), QStringLiteral("security"),
            QStringLiteral("\\bpassword\\s*=\\s*[\"']")},
        {QStringLiteral("hardcoded_api_key"),  QStringLiteral("security"),
            QStringLiteral("\\bapi_key\\s*=\\s*[\"']")},
        {QStringLiteral("real_network"),       QStringLiteral("isolation"),
            QStringLiteral("\\b(requests|urlopen|fetch)\\(['\"]?https?://")},
    };
    return v;
}

// ────────────────────────── Partition LRU cache ──
QHash<QString, PartitionResult> g_partitionCache;
QStringList                     g_partitionCacheLru;
QMutex                          g_partitionCacheMutex;
QHash<QString, qint64>          g_lastMtimeRecheckByToken;
QMutex                          g_recheckMutex;

void cachePartition(const PartitionResult &p) {
    QMutexLocker lk(&g_partitionCacheMutex);
    if (g_partitionCache.contains(p.partitionToken)) {
        g_partitionCacheLru.removeAll(p.partitionToken);
    }
    g_partitionCache.insert(p.partitionToken, p);
    g_partitionCacheLru.append(p.partitionToken);
    while (g_partitionCacheLru.size() > kPartitionCacheCap) {
        const QString evict = g_partitionCacheLru.takeFirst();
        g_partitionCache.remove(evict);
    }
}

// ────────────────────────── Helpers ──
QString detectFramework(const QString &projectRoot,
                        QStringList *globsOut) {
    for (const auto &fw : g_kFrameworks()) {
        for (const QString &signal : fw.signalFiles) {
            if (QFileInfo::exists(projectRoot + QLatin1Char('/') + signal)) {
                if (globsOut) *globsOut = fw.testGlobs;
                return fw.name;
            }
        }
    }
    return QString();
}

// ANTS-1455 — manual glob→regex conversion. Qt's
// QRegularExpression::wildcardToRegularExpression doesn't implement
// globstar (`**`); `tests/**/*.py` would not match `tests/test_a.py`
// directly. This helper handles:
//   `**/` → `(?:[^/]+/)*` (zero-or-more directory segments)
//   `**`  (no trailing `/`) → `.*`
//   `*`   → `[^/]*`
//   `?`   → `[^/]`
//   `[…]` / `[a-z]` → passed through to QRegularExpression natively
//   Other regex metachars → QRegularExpression::escape'd
QRegularExpression globToRegex(const QString &glob) {
    QString rx;
    rx.reserve(glob.size() * 2 + 4);
    rx += QStringLiteral("\\A");
    int i = 0;
    const int n = glob.size();
    while (i < n) {
        const QChar c = glob.at(i);
        if (c == QLatin1Char('*')) {
            const bool doubled = (i + 1 < n
                && glob.at(i + 1) == QLatin1Char('*'));
            if (doubled) {
                const bool trailingSlash = (i + 2 < n
                    && glob.at(i + 2) == QLatin1Char('/'));
                if (trailingSlash) {
                    rx += QStringLiteral("(?:[^/]+/)*");
                    i += 3;
                } else {
                    rx += QStringLiteral(".*");
                    i += 2;
                }
            } else {
                rx += QStringLiteral("[^/]*");
                i += 1;
            }
        } else if (c == QLatin1Char('?')) {
            rx += QStringLiteral("[^/]");
            i += 1;
        } else if (c == QLatin1Char('[')) {
            // Pass character class through verbatim until the
            // matching ']' (Qt's regex engine understands these).
            const int close = glob.indexOf(QLatin1Char(']'), i + 1);
            if (close < 0) {
                // No close — treat as literal '['.
                rx += QRegularExpression::escape(QString(c));
                i += 1;
            } else {
                rx += glob.mid(i, close - i + 1);
                i = close + 1;
            }
        } else if (c == QLatin1Char('/')) {
            rx += QLatin1Char('/');
            i += 1;
        } else {
            rx += QRegularExpression::escape(QString(c));
            i += 1;
        }
    }
    rx += QStringLiteral("\\z");
    return QRegularExpression(rx);
}

// ANTS-1455 — derive the leading path prefix from a glob, up to the
// first `**` or the last `/` if no `**`. Returns empty for globs
// without a path component (`test_*.py`, `*_test.py`, `**/test.py`).
QString globPathPrefix(const QString &glob) {
    const int starStar = glob.indexOf(QStringLiteral("**"));
    if (starStar > 0) {
        // Trim to last `/` at-or-before starStar so we don't
        // include a partial segment.
        const QString head = glob.left(starStar);
        const int lastSlash = head.lastIndexOf(QLatin1Char('/'));
        return lastSlash >= 0 ? head.left(lastSlash + 1) : QString();
    }
    if (starStar == 0) return QString();  // leading `**/`
    const int lastSlash = glob.lastIndexOf(QLatin1Char('/'));
    if (lastSlash < 0) return QString();  // no `/`
    return glob.left(lastSlash + 1);
}

// Walk the project tree honouring the test_globs.
QStringList walkTestFiles(const QString &projectRoot,
                          const QStringList &testGlobs,
                          const QString &scope) {
    QStringList result;
    QString scopeRoot = projectRoot;
    if (scope.startsWith(QLatin1String("path:"))) {
        const QString sub = scope.mid(5);
        scopeRoot = projectRoot + QLatin1Char('/') + sub;
    } else if (scope.startsWith(QLatin1String("files:"))) {
        // Each comma-separated path — validate each + emit.
        const QStringList parts = scope.mid(6).split(QLatin1Char(','));
        for (const QString &p : parts) {
            const QString t = p.trimmed();
            if (t.isEmpty() || t.contains(QLatin1Char('\\'))) continue;
            const QString full = projectRoot + QLatin1Char('/') + t;
            if (QFileInfo::exists(full)) result.append(full);
        }
        return result;
    }

    // ANTS-1451: single source of truth for build-tree + tooling
    // exclusions.
    static const QRegularExpression excludeRx(QStringLiteral(
        "/(node_modules|\\.venv|__pycache__|build[^/]*|dist|_deps"
        "|CMakeFiles|autogen)/"));

    // ANTS-1455: per-glob walk with path-prefix optimisation +
    // full-glob re-filter.
    QSet<QString> seen;
    for (const QString &glob : testGlobs) {
        if (glob.isEmpty()) continue;
        const bool hasSlash = glob.contains(QLatin1Char('/'));
        const QString prefix = globPathPrefix(glob);
        const QString walkRoot = prefix.isEmpty()
            ? scopeRoot
            : QDir::cleanPath(scopeRoot + QLatin1Char('/') + prefix);
        if (!QFileInfo(walkRoot).isDir()) continue;
        const QRegularExpression rx = globToRegex(glob);
        QDirIterator it(walkRoot, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            if (excludeRx.match(p).hasMatch()) continue;
            if (seen.contains(p)) continue;
            // For bare-basename globs (no `/`), match against the
            // candidate's basename. For path-bearing globs, match
            // against the candidate-relative path under scopeRoot.
            QString matchTarget;
            if (hasSlash) {
                if (!p.startsWith(scopeRoot + QLatin1Char('/'))) continue;
                matchTarget = p.mid(scopeRoot.size() + 1);
            } else {
                matchTarget = QFileInfo(p).fileName();
            }
            if (!rx.match(matchTarget).hasMatch()) continue;
            seen.insert(p);
            result.append(p);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

qint64 walkMaxMtime(const QStringList &files) {
    qint64 maxMs = 0;
    for (const QString &f : files) {
        const qint64 ms = QFileInfo(f).lastModified().toMSecsSinceEpoch();
        if (ms > maxMs) maxMs = ms;
    }
    return maxMs;
}

QString computeToken(const QString &callerCwd, const QString &scope,
                     const QStringList &dimensions, qint64 maxMtime) {
    // qHash combine; format as 8 hex chars.
    QStringList sorted = dimensions;
    sorted.sort();
    const uint h = qHash(callerCwd)
                 ^ qHash(scope)
                 ^ qHash(sorted.join(QLatin1Char(',')))
                 ^ qHash(QString::number(maxMtime));
    return QStringLiteral("%1").arg(h, 8, 16, QLatin1Char('0'));
}

QStringList resolveDimensions(const QString &arg) {
    if (arg.isEmpty() || arg == QLatin1String("auto"))
        return g_kDimensions();
    if (arg.startsWith(QLatin1String("csv:"))) {
        return arg.mid(4).split(QLatin1Char(','),
                                Qt::SkipEmptyParts);
    }
    return {};  // unknown shape
}

// ANTS-1491 — strip C/C++ string literals and comments before pre-pass
// regex matching, so e.g. a `sleep(...)` pattern doesn't match inside a
// C++ raw-string literal holding a Python child-process script. Mirrors
// the comment/string filter step in auditdialog's static-analysis
// pipeline. Replaces content with spaces (not deletes) to preserve
// column positions; newlines are preserved verbatim so the line-number
// map stays exact. Only applied to C/C++ extensions — other languages
// would need their own state machine.
bool isCxxPath(const QString &path) {
    static const QStringList kExt = {
        QStringLiteral(".cpp"), QStringLiteral(".cxx"),
        QStringLiteral(".cc"),  QStringLiteral(".c"),
        QStringLiteral(".h"),   QStringLiteral(".hpp"),
        QStringLiteral(".hh"),  QStringLiteral(".hxx"),
        QStringLiteral(".ipp"), QStringLiteral(".tcc"),
    };
    for (const QString &e : kExt) {
        if (path.endsWith(e, Qt::CaseInsensitive)) return true;
    }
    return false;
}

QString stripCxxLiteralsAndComments(const QString &src) {
    QString out;
    out.reserve(src.size());
    enum State { Normal, LineComment, BlockComment, StringLit,
                 CharLit,  RawString };
    State st = Normal;
    QString rawDelim;             // captured between R"<delim>( and )<delim>"
    int rawDelimMatched = 0;      // progress into )<delim>" closer
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        const QChar n = (i + 1 < src.size()) ? src[i+1] : QChar();
        switch (st) {
        case Normal: {
            // Detect raw-string prefix: R"delim( ... )delim"
            // (Or u8R, uR, UR, LR — accept letter prefix before R).
            // Cheap probe: at index `i` we're on 'R' and next is '"'.
            int rawStart = -1;
            if (c == QLatin1Char('R') && n == QLatin1Char('"')) {
                rawStart = i;
            } else if (i + 1 < src.size() &&
                       src[i+1] == QLatin1Char('R') &&
                       i + 2 < src.size() &&
                       src[i+2] == QLatin1Char('"') &&
                       (c == QLatin1Char('u') || c == QLatin1Char('U') ||
                        c == QLatin1Char('L'))) {
                rawStart = i + 1;
            } else if (c == QLatin1Char('u') && n == QLatin1Char('8') &&
                       i + 3 < src.size() &&
                       src[i+2] == QLatin1Char('R') &&
                       src[i+3] == QLatin1Char('"')) {
                rawStart = i + 2;
            }
            if (rawStart >= 0) {
                // Find delim between '"' and '('.
                int j = rawStart + 2;
                rawDelim.clear();
                while (j < src.size() && src[j] != QLatin1Char('(')) {
                    rawDelim.append(src[j]);
                    ++j;
                }
                if (j < src.size()) {
                    // Emit spaces for the entire R"delim( header.
                    for (int k = i; k <= j; ++k) {
                        out.append(src[k] == QLatin1Char('\n')
                                   ? QLatin1Char('\n')
                                   : QLatin1Char(' '));
                    }
                    i = j;            // loop ++i moves past '('
                    rawDelimMatched = 0;
                    st = RawString;
                    continue;
                }
                // fall through — treat as normal
            }
            if (c == QLatin1Char('/') && n == QLatin1Char('/')) {
                st = LineComment;
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('/') && n == QLatin1Char('*')) {
                st = BlockComment;
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('"')) {
                st = StringLit;
                out.append(QLatin1Char(' '));
            } else if (c == QLatin1Char('\'')) {
                st = CharLit;
                out.append(QLatin1Char(' '));
            } else {
                out.append(c);
            }
            break;
        }
        case LineComment:
            // Preserve newline so line numbers stay exact.
            if (c == QLatin1Char('\n')) { st = Normal; out.append(c); }
            else                         out.append(QLatin1Char(' '));
            break;
        case BlockComment:
            if (c == QLatin1Char('*') && n == QLatin1Char('/')) {
                st = Normal;
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('\n')) {
                out.append(c);
            } else {
                out.append(QLatin1Char(' '));
            }
            break;
        case StringLit:
            if (c == QLatin1Char('\\') && n != QChar()) {
                // Preserve a newline that follows an escape (line cont.).
                out.append(QLatin1Char(' '));
                out.append(n == QLatin1Char('\n')
                           ? QLatin1Char('\n') : QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('"')) {
                st = Normal;
                out.append(QLatin1Char(' '));
            } else if (c == QLatin1Char('\n')) {
                // Unterminated string literal — be liberal and end on EOL.
                st = Normal;
                out.append(c);
            } else {
                out.append(QLatin1Char(' '));
            }
            break;
        case CharLit:
            if (c == QLatin1Char('\\') && n != QChar()) {
                out.append(QLatin1Char(' '));
                out.append(n == QLatin1Char('\n')
                           ? QLatin1Char('\n') : QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('\'')) {
                st = Normal;
                out.append(QLatin1Char(' '));
            } else if (c == QLatin1Char('\n')) {
                st = Normal;
                out.append(c);
            } else {
                out.append(QLatin1Char(' '));
            }
            break;
        case RawString: {
            // Watch for )delim" — track progress without rescanning.
            if (rawDelimMatched == 0 && c == QLatin1Char(')')) {
                rawDelimMatched = 1;
                out.append(QLatin1Char(' '));
            } else if (rawDelimMatched >= 1 &&
                       rawDelimMatched <= rawDelim.size()) {
                if (rawDelimMatched - 1 < rawDelim.size() &&
                    c == rawDelim[rawDelimMatched - 1]) {
                    ++rawDelimMatched;
                    out.append(QLatin1Char(' '));
                } else if (c == QLatin1Char('"')) {
                    // Edge: ')' followed immediately by '"' with empty delim.
                    if (rawDelim.isEmpty()) {
                        st = Normal;
                        out.append(QLatin1Char(' '));
                        rawDelimMatched = 0;
                    } else {
                        rawDelimMatched = 0;
                        out.append(c == QLatin1Char('\n')
                                   ? QLatin1Char('\n') : QLatin1Char(' '));
                    }
                } else {
                    rawDelimMatched = 0;
                    out.append(c == QLatin1Char('\n')
                               ? QLatin1Char('\n') : QLatin1Char(' '));
                }
            } else if (rawDelimMatched == rawDelim.size() + 1 &&
                       c == QLatin1Char('"')) {
                st = Normal;
                rawDelimMatched = 0;
                out.append(QLatin1Char(' '));
            } else {
                rawDelimMatched = 0;
                out.append(c == QLatin1Char('\n')
                           ? QLatin1Char('\n') : QLatin1Char(' '));
            }
            break;
        }
        }
    }
    return out;
}

// Pre-pass scan one file against the pattern set; return up to N
// findings as JSON objects (file, line, pattern_id, dimension).
QJsonArray prePassFile(const QString &path,
                       const QVector<QRegularExpression> &rxs,
                       int capRemaining) {
    QJsonArray out;
    if (capRemaining <= 0) return out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QString raw = QString::fromUtf8(f.readAll());
    // ANTS-1491 — strip C/C++ string literals and comments before
    // matching, so patterns don't fire inside fixture-string Python
    // scripts. Newlines/columns preserved so line-number reporting
    // stays exact.
    const QString text = isCxxPath(path)
        ? stripCxxLiteralsAndComments(raw) : raw;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int idx = 0; idx < lines.size() && out.size() < capRemaining; ++idx) {
        const int lineNo = idx + 1;
        const QString &line = lines.at(idx);
        for (int i = 0; i < rxs.size() && out.size() < capRemaining; ++i) {
            if (rxs[i].match(line).hasMatch()) {
                QJsonObject o;
                o["file"]       = path;
                o["line"]       = lineNo;
                o["pattern_id"] = g_kPrePatterns().at(i).id;
                o["dimension"]  = g_kPrePatterns().at(i).dimension;
                out.append(o);
            }
        }
    }
    return out;
}

}  // namespace

const QStringList &kDimensions() { return g_kDimensions(); }

namespace internal {
const PartitionResult *lookupPartition(const QString &token) {
    QMutexLocker lk(&g_partitionCacheMutex);
    auto it = g_partitionCache.find(token);
    return (it == g_partitionCache.end()) ? nullptr : &it.value();
}

// ANTS-1451 — thin forwarder so the regression test can exercise the
// exclusion list against a fixture tree without going through the
// full partition() pipeline (which also requires a framework probe).
QStringList walkTestFiles(const QString &projectRoot,
                          const QStringList &testGlobs,
                          const QString &scope) {
    return ::TestAuditEngine::walkTestFiles(projectRoot, testGlobs, scope);
}
}  // namespace internal

PartitionResult partition(const PartitionRequest &req) {
    PartitionResult r;
    // INV-14 — caller_cwd → canonical projectRoot.
    const QString canon = QFileInfo(req.callerCwd).canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_partition: caller_cwd \"%1\" does not "
            "canonicalise to an existing directory").arg(req.callerCwd);
        return r;
    }
    // INV-14 — scope:"files:..." backslash check.
    if (req.scope.startsWith(QLatin1String("files:")) &&
        req.scope.contains(QLatin1Char('\\'))) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_partition: scope files list contains "
            "backslash — rejected");
        return r;
    }
    // INV-6 — dimensions.
    const QStringList dims = resolveDimensions(req.dimensions);
    if (dims.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("unknown_dimension");
        r.error = QStringLiteral(
            "test_audit_partition: dimensions arg \"%1\" not "
            "recognised — expected \"auto\" or \"csv:<d1,d2,...>\""
            ).arg(req.dimensions);
        return r;
    }
    for (const QString &d : dims) {
        if (!g_kDimensions().contains(d)) {
            r.ok = false; r.code = QStringLiteral("unknown_dimension");
            r.error = QStringLiteral(
                "test_audit_partition: dimension \"%1\" not in "
                "kDimensions").arg(d);
            return r;
        }
    }
    // Framework detect.
    QStringList globs;
    r.framework = detectFramework(canon, &globs);
    if (r.framework.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("no_tests_found");
        r.error = QStringLiteral(
            "test_audit_partition: no test framework detected at "
            "\"%1\"").arg(canon);
        return r;
    }
    r.testGlobs = globs;
    // Walk test files.
    const QStringList files = walkTestFiles(canon, globs, req.scope);
    if (files.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("no_tests_found");
        r.error = QStringLiteral(
            "test_audit_partition: framework=%1 detected but no "
            "test files matched (scope=%2)")
                .arg(r.framework).arg(req.scope);
        return r;
    }
    r.totalFiles = files.size();
    // INV-9 — clamp chunk_size.
    int chunkSize = req.chunkSize;
    if (chunkSize < kChunkSizeMin) chunkSize = kChunkSizeMin;
    if (chunkSize > kChunkSizeMax) chunkSize = kChunkSizeMax;
    // Pack chunks depth-first.
    QVector<Chunk> chunks;
    for (int i = 0; i < files.size(); i += chunkSize) {
        Chunk c;
        c.id = QStringLiteral("c-%1")
            .arg(chunks.size() + 1, 3, 10, QLatin1Char('0'));
        for (int j = i; j < std::min(i + chunkSize,
                                     static_cast<int>(files.size())); ++j) {
            c.paths.append(files.at(j));
        }
        chunks.append(c);
    }
    r.chunksCount = chunks.size();
    r.total       = chunks.size();
    // INV-4 — token from canonical inputs + max mtime.
    const qint64 maxMtime = walkMaxMtime(files);
    r.partitionToken = computeToken(canon, req.scope, dims, maxMtime);
    r.mtimeWalkComputedAt = QDateTime::currentDateTimeUtc();
    r.dimensionsActive = dims;
    // Pre-compile patterns.
    QVector<QRegularExpression> rxs;
    for (const auto &p : g_kPrePatterns()) rxs.append(QRegularExpression(p.regex));
    // Pre-pass per chunk; cap at kPrePassPerChunkCap per chunk.
    for (const Chunk &c : chunks) {
        QJsonArray findings;
        int remaining = kPrePassPerChunkCap;
        for (const QString &path : c.paths) {
            const QJsonArray fileF = prePassFile(path, rxs, remaining);
            for (const QJsonValue &v : fileF) {
                findings.append(v);
            }
            remaining = kPrePassPerChunkCap - findings.size();
            if (remaining <= 0) break;
        }
        if (!findings.isEmpty()) {
            // Compute dimension hints from observed pattern dimensions.
            QSet<QString> hints;
            for (const QJsonValue &v : findings) {
                hints.insert(v.toObject().value(
                    QStringLiteral("dimension")).toString());
            }
            Chunk withHints = c;
            withHints.prePassDimensions = hints.values();
            std::sort(withHints.prePassDimensions.begin(),
                      withHints.prePassDimensions.end());
            const QString hk = withHints.id;
            r.prePassFindingsByChunk[hk] = findings;
            // Replace chunk-with-hints back.
            r.chunks.append(withHints);
        } else {
            r.chunks.append(c);
        }
    }
    // INV-10 — pagination. If caller passed offset/limit, slice.
    r.offset = req.offset;
    r.limit  = req.limit;
    if (req.offset > 0 || req.limit > 0) {
        const int total = r.chunks.size();
        const int off = std::min(req.offset, total);
        const int lim = (req.limit > 0) ? req.limit : (total - off);
        QVector<Chunk> page = r.chunks.mid(off, lim);
        r.chunks = page;
        r.truncated = (off + page.size() < total);
        if (r.truncated) r.nextOffset = off + page.size();
        // Filter prePassFindingsByChunk to retained chunk ids.
        QHash<QString, QJsonArray> filtered;
        for (const Chunk &c : r.chunks) {
            if (r.prePassFindingsByChunk.contains(c.id))
                filtered.insert(c.id, r.prePassFindingsByChunk.value(c.id));
        }
        r.prePassFindingsByChunk = filtered;
        // Page 2+ → omit pre_pass_findings_by_chunk via cached marker.
        if (off > 0) {
            r.prePassCached = true;
            r.prePassFindingsByChunk.clear();
        }
    }
    // Cache partition for brief/synth lookup.
    cachePartition(r);
    // Byte count (informational).
    QJsonObject env;
    env["framework"]    = r.framework;
    env["chunks_count"] = r.chunksCount;
    r.byteCount =
        QJsonDocument(env).toJson(QJsonDocument::Compact).size();
    return r;
}

BriefResult brief(const BriefRequest &req) {
    BriefResult r;
    const QString canon = QFileInfo(req.callerCwd).canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_brief: caller_cwd \"%1\" does not "
            "canonicalise to an existing directory").arg(req.callerCwd);
        return r;
    }
    if (req.partitionToken.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("missing_field");
        r.error = QStringLiteral(
            "test_audit_brief: partition_token is required");
        return r;
    }
    const PartitionResult *p =
        internal::lookupPartition(req.partitionToken);
    if (!p) {
        r.ok = false; r.code = QStringLiteral("stale_partition");
        r.error = QStringLiteral(
            "test_audit_brief: partition_token \"%1\" not found — "
            "re-run partition").arg(req.partitionToken);
        return r;
    }
    // INV-15 — shallow recheck rate-limit.
    qint64 lastRecheck = 0;
    {
        QMutexLocker lk(&g_recheckMutex);
        lastRecheck = g_lastMtimeRecheckByToken.value(
            req.partitionToken, 0);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastRecheck > kMtimeRecheckRateLimitMs) {
        QMutexLocker lk(&g_recheckMutex);
        g_lastMtimeRecheckByToken[req.partitionToken] = now;
        // (Recursive recheck would walk the test_globs roots; v1
        // skips — documented as part of INV-15's deep-tree gap.)
    }
    // Locate the chunk.
    const Chunk *chunk = nullptr;
    for (const Chunk &c : p->chunks) {
        if (c.id == req.chunkId) { chunk = &c; break; }
    }
    if (!chunk) {
        r.ok = false; r.code = QStringLiteral("bad_chunk_id");
        r.error = QStringLiteral(
            "test_audit_brief: chunk_id \"%1\" not in partition")
                .arg(req.chunkId);
        return r;
    }
    r.chunkId      = chunk->id;
    r.sourcePaths  = chunk->paths;
    r.dimensions   = p->dimensionsActive;
    QJsonObject fc;
    fc["framework"] = p->framework;
    r.frameworkContext = fc;
    r.prePassFindings  = p->prePassFindingsByChunk.value(chunk->id);
    // ANTS-1457 — prior false-positive findings filtered by
    // review_kind=test-audit. lane filter is empty because
    // test-audit partitions by chunk-id rather than lane name;
    // INV-9's bidirectional empty-match covers both lane-tagged
    // and untagged entries.
    {
        const auto fpEntries = ants::falsepos::filter(
            ants::falsepos::loadEntries(canon),
            QStringLiteral("test-audit"), QString());
        r.priorFalsePositives = ants::falsepos::formatForJsonArray(fpEntries);
    }
    // Byte count (informational).
    QJsonObject env;
    env["chunk_id"]    = r.chunkId;
    env["source_paths"] = QJsonArray::fromStringList(r.sourcePaths);
    env["dimensions"]   = QJsonArray::fromStringList(r.dimensions);
    env["pre_pass_findings"] = r.prePassFindings;
    env["prior_false_positives"] = r.priorFalsePositives;
    r.byteCount = QJsonDocument(env).toJson(QJsonDocument::Compact).size();
    return r;
}

SynthResult synthesize(const SynthRequest &req) {
    SynthResult r;
    const QString canon = QFileInfo(req.callerCwd).canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: caller_cwd \"%1\" does "
            "not canonicalise to an existing directory")
                .arg(req.callerCwd);
        return r;
    }
    if (req.partitionToken.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("missing_field");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: partition_token required");
        return r;
    }
    if (!internal::lookupPartition(req.partitionToken)) {
        r.ok = false; r.code = QStringLiteral("stale_partition");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: partition_token not found");
        return r;
    }
    if (req.reportsDir.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: reports_dir required");
        return r;
    }
    // ANTS-1455 — route through PathValidation::validatePath (single
    // source of truth for NFC + control + backslash + canonicalisation).
    const auto pv = PathValidation::validatePath(
        req.reportsDir, canon,
        QStringLiteral("test_audit_synthesis_prompt"),
        QStringLiteral("reports_dir"),
        /*allowOutsideRoot=*/req.allowOutsideProject);
    if (pv.bad) {
        const QJsonObject &e = pv.err;
        r.ok = false;
        // Anchor-failure under default mode → rename code from the
        // pre-fix `reports_dir_missing` to `reports_dir_outside_root`
        // (≤ 24 chars, see docs/standards/mcp-error-codes.md § 1).
        if (e.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("escapes project root"))
            && !req.allowOutsideProject) {
            r.code = QStringLiteral("reports_dir_outside_root");
            r.error = QStringLiteral(
                "test_audit_synthesis_prompt: reports_dir \"%1\" "
                "resolves outside project root; pass "
                "allow_outside_project:true to override")
                    .arg(req.reportsDir);
        } else {
            r.code = e.value(QStringLiteral("code")).toString();
            r.error = e.value(QStringLiteral("error")).toString();
        }
        return r;
    }
    const QString canonDir = pv.resolved;
    if (canonDir.isEmpty() || !QFileInfo(canonDir).isDir()) {
        r.ok = false; r.code = QStringLiteral("reports_dir_unreadable");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: reports_dir \"%1\" does not "
            "exist, is not a directory, or is unreadable")
                .arg(req.reportsDir);
        return r;
    }
    // ANTS-1455 — mode validation; default "summary" (breaking change
    // ack: v1 always returned the full-fenced bundle, which exceeded
    // tool-result caps on real projects).
    // ANTS-1486 — "hybrid" added: summary + top-N highest-finding-count
    // chunks verbatim, so callers can decide-and-read in one round-trip
    // for heavy chunks without paging through all of mode:full.
    QString mode = req.mode;
    if (mode.isEmpty()) mode = QStringLiteral("summary");
    if (mode != QStringLiteral("summary")
        && mode != QStringLiteral("full")
        && mode != QStringLiteral("hybrid")) {
        r.ok = false; r.code = QStringLiteral("bad_mode");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: mode \"%1\" not in "
            "{summary, full, hybrid}").arg(req.mode);
        return r;
    }
    r.mode = mode;

    // Read *.md / *.json top-level. ANTS-1485 — accept .json reports
    // too: subagent harnesses sometimes drop structured JSON instead
    // of markdown. Downstream passes are regex-over-text so JSON
    // bodies still match dimension keywords and `[SEV]` bullets; the
    // experience degrades when a chunk uses pure JSON-object shape
    // with no prose tags, but that's no worse than receiving fewer
    // chunks — and dropping the file via reports_dir_empty was worse.
    QDir dir(canonDir);
    QStringList md = dir.entryList({QStringLiteral("*.md"),
                                    QStringLiteral("*.json")},
                                   QDir::Files);
    if (md.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("reports_dir_empty");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: reports_dir \"%1\" contains "
            "no *.md or *.json files at top level").arg(req.reportsDir);
        return r;
    }
    r.chunksTotal = md.size();

    // Two-pass: pass 1 reads every chunk to compute dimension counts
    // + file-reference index (cheap, both modes need it). Pass 2 emits
    // the fenced bundle for `mode:"full"` only.
    QHash<QString, int> perDimensionCount;
    QHash<QString, int> fileRefCount;
    static const QRegularExpression fileRefRx(QStringLiteral(
        "([\\w./-]+\\.(?:py|cpp|h|hpp|cc|js|ts|tsx|go|rs|rb|java)):"
        "(\\d+)"));
    // ANTS-1488 — per-dimension severity histograms. Pre-seed every
    // dimension to {0,0,0,0,0} so callers see the full lane list even
    // when a dimension surfaced no findings (orchestrator can iterate
    // without null-checks).
    QHash<QString, QHash<QString, int>> sevHist;
    static const QStringList kSevs = {
        QStringLiteral("crit"), QStringLiteral("high"),
        QStringLiteral("med"),  QStringLiteral("low"),
        QStringLiteral("info"),
    };
    for (const QString &d : g_kDimensions()) {
        for (const QString &s : kSevs) sevHist[d][s] = 0;
    }
    // Header pattern: `## <emoji>? <Dimension Name> (N)` — the leading
    // `##` (or `###`) plus the dimension keyword anywhere in the
    // header line. We canonicalise by lowercasing + comparing against
    // kDimensions so cosmetic differences in skill prose ("Performance"
    // vs "performance") don't fragment counts.
    static const QRegularExpression headerRx(QStringLiteral(
        "^#{2,3}\\s+"));
    // Finding bullet shape: `- [SEV] …` or `- [SEV-tag] …`. Match the
    // canonical 5 severities and the "MEDIUM" full-word form too.
    static const QRegularExpression findingRx(QStringLiteral(
        "^\\s*-\\s*\\[(CRIT|CRITICAL|HIGH|MED|MEDIUM|LOW|INFO)\\]"));
    // ANTS-1486 — per-chunk total finding count, for hybrid mode's
    // "top-N by max-finding-count" pick. Computed during pass 1 so we
    // don't re-scan.
    QList<int> findingCountByChunk;
    findingCountByChunk.reserve(md.size());
    QStringList contentsByChunk;
    contentsByChunk.reserve(md.size());
    for (const QString &m : md) {
        QFile f(dir.absoluteFilePath(m));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            contentsByChunk.append(QString());
            findingCountByChunk.append(0);
            continue;
        }
        const QByteArray raw = f.readAll().left(64 * 1024);
        QString contents = QString::fromUtf8(raw);
        contents.replace(QStringLiteral("</chunk_report>"),
                         QStringLiteral("&lt;/chunk_report&gt;"));
        contentsByChunk.append(contents);
        for (const QString &d : g_kDimensions()) {
            if (contents.contains(d, Qt::CaseInsensitive)) {
                ++perDimensionCount[d];
            }
        }
        auto it = fileRefRx.globalMatch(contents);
        while (it.hasNext()) {
            const QRegularExpressionMatch m2 = it.next();
            ++fileRefCount[m2.captured(1)];
        }
        // ANTS-1488 — walk lines, track current dimension header,
        // attribute each finding's severity to that dimension. Lines
        // outside a recognised dimension header still count toward the
        // dimension's bucket if the dimension keyword appears in the
        // header line, even if the prose styled it differently.
        QString curDim;
        int chunkFindings = 0;
        const QStringList lines = contents.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (headerRx.match(line).hasMatch()) {
                curDim.clear();
                for (const QString &d : g_kDimensions()) {
                    if (line.contains(d, Qt::CaseInsensitive)) {
                        curDim = d;
                        break;
                    }
                }
                continue;
            }
            const auto fm = findingRx.match(line);
            if (fm.hasMatch()) {
                ++chunkFindings;
                if (!curDim.isEmpty()) {
                    QString sev = fm.captured(1).toLower();
                    if (sev == QLatin1String("critical")) sev = QStringLiteral("crit");
                    if (sev == QLatin1String("medium"))   sev = QStringLiteral("med");
                    ++sevHist[curDim][sev];
                }
            }
        }
        findingCountByChunk.append(chunkFindings);
    }

    // Dimension summaries (legacy field) + top_dimensions envelope.
    QJsonObject summaries;
    QList<QPair<QString, int>> dimRanked;
    for (auto it = perDimensionCount.constBegin();
         it != perDimensionCount.constEnd(); ++it) {
        QJsonObject s; s["count"] = it.value();
        summaries[it.key()] = s;
        dimRanked.append({it.key(), it.value()});
    }
    r.dimensionSummaries = summaries;
    std::sort(dimRanked.begin(), dimRanked.end(),
              [](const auto &a, const auto &b) {
                  return a.second > b.second;
              });
    QJsonArray topDims;
    const int kTopDimCap = 10;
    for (int i = 0; i < dimRanked.size() && i < kTopDimCap; ++i) {
        QJsonObject o;
        o["dimension"] = dimRanked.at(i).first;
        o["count"] = dimRanked.at(i).second;
        topDims.append(o);
    }
    if (dimRanked.size() > kTopDimCap) r.truncated = true;
    r.topDimensions = topDims;

    // ANTS-1461 — dedup file_index by basename before ranking.
    // Chunk subagents cite the same logical file inconsistently
    // (e.g. `test_X.py` vs `tests/test_X.py`); the raw QHash keeps
    // them as two entries and double-counts the file in the top-N
    // display. Two-pass merge: (1) per basename, pick the longest
    // path as the canonical display key (the directory-prefixed
    // form is the repo-root-relative truth the walker delivered);
    // (2) sum counts under the canonical key.
    QHash<QString, QString> canonicalByBase;
    for (auto it = fileRefCount.constBegin();
         it != fileRefCount.constEnd(); ++it) {
        const QString &raw = it.key();
        const QString base = raw.section('/', -1);
        if (!canonicalByBase.contains(base) ||
            raw.length() > canonicalByBase.value(base).length()) {
            canonicalByBase[base] = raw;
        }
    }
    QHash<QString, int> mergedRefCount;
    for (auto it = fileRefCount.constBegin();
         it != fileRefCount.constEnd(); ++it) {
        const QString base = it.key().section('/', -1);
        mergedRefCount[canonicalByBase.value(base)] += it.value();
    }
    QList<QPair<QString, int>> fileRanked;
    for (auto it = mergedRefCount.constBegin();
         it != mergedRefCount.constEnd(); ++it) {
        fileRanked.append({it.key(), it.value()});
    }
    std::sort(fileRanked.begin(), fileRanked.end(),
              [](const auto &a, const auto &b) {
                  return a.second > b.second;
              });
    QJsonArray fileIdx;
    const int kFileIdxCap = 30;
    for (int i = 0; i < fileRanked.size() && i < kFileIdxCap; ++i) {
        QJsonObject o;
        o["file"] = fileRanked.at(i).first;
        o["dimension_hits_total"] = fileRanked.at(i).second;
        fileIdx.append(o);
    }
    if (fileRanked.size() > kFileIdxCap) r.truncated = true;
    r.fileIndex = fileIdx;

    // ANTS-1488 — assemble per-dimension severity histogram envelope.
    // Only include dimensions that surfaced at least one finding (keeps
    // the envelope tight; the orchestrator can probe dimensions absent
    // from this map as "no findings").
    QJsonObject sevEnv;
    for (auto it = sevHist.constBegin(); it != sevHist.constEnd(); ++it) {
        int total = 0;
        for (const QString &s : kSevs) total += it.value().value(s);
        if (total == 0) continue;
        QJsonObject row;
        for (const QString &s : kSevs) row[s] = it.value().value(s);
        sevEnv[it.key()] = row;
    }
    r.severityHistograms = sevEnv;

    QString prompt;
    QTextStream ts(&prompt);
    ts << "# Test-Audit Synthesis\n\n";
    ts << "Mode: " << mode << "\n";
    ts << "Reports: " << md.size() << " chunk(s) at "
       << req.reportsDir << "\n\n";

    // Shared helper: render summary stats block (top_dimensions +
    // file_index + severity histograms + chunk inventory). Used by
    // both summary and hybrid modes.
    auto renderSummaryBlock = [&]() {
        ts << "## Top dimensions (by chunk-keyword hit count)\n\n";
        for (const auto &entry : topDims) {
            const QJsonObject o = entry.toObject();
            ts << "- **" << o.value("dimension").toString()
               << "** — " << o.value("count").toInt()
               << " chunk(s)\n";
        }
        // ANTS-1488 — severity histograms.
        if (!sevEnv.isEmpty()) {
            ts << "\n## Severity histograms (per dimension)\n\n";
            for (const QString &d : sevEnv.keys()) {
                const QJsonObject row = sevEnv.value(d).toObject();
                ts << "- **" << d << "** — "
                   << "crit:" << row.value("crit").toInt() << " "
                   << "high:" << row.value("high").toInt() << " "
                   << "med:"  << row.value("med").toInt() << " "
                   << "low:"  << row.value("low").toInt() << " "
                   << "info:" << row.value("info").toInt() << "\n";
            }
        }
        ts << "\n## Most-referenced source files\n\n";
        for (const auto &entry : fileIdx) {
            const QJsonObject o = entry.toObject();
            ts << "- `" << o.value("file").toString()
               << "` — " << o.value("dimension_hits_total").toInt()
               << " ref(s)\n";
        }
        if (r.truncated) {
            ts << "\n*(top_dimensions and/or file_index truncated; "
                  "pass mode:\"full\" for the verbatim chunk reports.)*\n";
        }
        ts << "\n## Chunk inventory\n\n";
        for (int i = 0; i < md.size(); ++i) {
            ts << "- " << md.at(i)
               << " (" << findingCountByChunk.at(i) << " findings)\n";
        }
    };

    if (mode == QStringLiteral("full")) {
        // ANTS-1455 — pagination. Default limit=5 (NOT -1) prevents
        // v1 callers transitioning to mode:"full" from re-creating
        // the 112 KB-blob bug. Explicit limit:-1 opts into "all".
        int limit = req.limit;
        if (limit == 0) limit = 5;           // default for unset
        const int offset = qMax(0, req.offset);
        const int end = (limit < 0)
            ? md.size()
            : qMin(md.size(), offset + limit);
        ts << "Per-chunk reports (each fenced for prompt-injection "
              "defence — INV-8):\n\n";
        for (int i = offset; i < end; ++i) {
            const QString &m = md.at(i);
            ts << "<chunk_report file=\"" << m.toHtmlEscaped() << "\">\n"
               << contentsByChunk.at(i) << "\n</chunk_report>\n\n";
            ++r.reportsRead;
        }
        r.chunksReturned = end - offset;
        if (end < md.size()) {
            r.nextOffset = end;
            r.truncatedByLimit = true;
        } else {
            r.nextOffset = -1;
        }
    } else if (mode == QStringLiteral("hybrid")) {
        // ANTS-1486 — summary stats + top-N highest-finding-count
        // chunks verbatim. Default N=3; caller can override via
        // `limit`. Chunks selected from `findingCountByChunk` desc; ties
        // broken by chunk index (so output order is stable).
        renderSummaryBlock();
        int topN = req.limit;
        if (topN <= 0) topN = 3;
        QList<QPair<int /*count*/, int /*idx*/>> ranked;
        ranked.reserve(md.size());
        for (int i = 0; i < md.size(); ++i)
            ranked.append({findingCountByChunk.at(i), i});
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto &a, const auto &b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });
        const int n = qMin(topN, static_cast<int>(ranked.size()));
        ts << "\n## Top " << n
           << " chunk(s) verbatim (by finding count)\n\n";
        ts << "Per-chunk reports (each fenced for prompt-injection "
              "defence — INV-8):\n\n";
        for (int i = 0; i < n; ++i) {
            const int idx = ranked.at(i).second;
            const QString &m = md.at(idx);
            ts << "<chunk_report file=\"" << m.toHtmlEscaped() << "\">\n"
               << contentsByChunk.at(idx) << "\n</chunk_report>\n\n";
            ++r.reportsRead;
        }
        r.chunksReturned = n;
        r.nextOffset = -1;
    } else {
        // mode:"summary" — counts + top-N pointers + severity
        // histograms; no fenced bundle. Output target ≤ 16 KiB.
        renderSummaryBlock();
        r.reportsRead = md.size();
        r.chunksReturned = 0;
        r.nextOffset = -1;
    }

    if (!req.calibrationAnchor.isEmpty()) {
        ts << "\n## Calibration anchor\n"
           << "Prior raw: " << req.calibrationAnchor.value(
                QStringLiteral("raw")).toInt() << "\n"
           << "Prior actionable: " << req.calibrationAnchor.value(
                QStringLiteral("actionable")).toInt() << "\n";
    }
    ts.flush();
    r.prompt = prompt;
    r.byteCount = prompt.toUtf8().size();
    return r;
}

FoldInResult foldIn(const FoldInRequest &req) {
    FoldInResult r;
    const QString canon = QFileInfo(req.callerCwd).canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_fold_in: caller_cwd does not canonicalise");
        return r;
    }
    if (req.actionable.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("missing_field");
        r.error = QStringLiteral(
            "test_audit_fold_in: actionable[] is required");
        return r;
    }
    // INV-3 — single batched allocate + insertBlock.
    const QString heading = QStringLiteral("### 🧪 Test Audit %1")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                .left(10));
    QString block;
    QTextStream bs(&block);
    bs << heading << "\n\n"
       << "Framework: " << req.framework
       << " · Files scanned: " << req.filesScanned
       << " · Dimensions: " << req.dimensions.join(QLatin1String(", "))
       << " · Raw: " << req.rawFindings
       << " · Actionable: " << req.actionable.size() << "\n\n";
    // Allocate all IDs upfront — single counter touch (INV-3).
    const int n = req.actionable.size();
    const QList<int> allocatedInts = RoadmapFoldIn::allocateIds(canon, n);
    if (allocatedInts.size() != n) {
        // ANTS-1490 — surface the counter-file path in the error so the
        // caller can clear a stale `.lock` sibling or inspect the file.
        const QString counterPath = RoadmapFoldIn::counterFilePath(canon);
        r.ok = false; r.code = QStringLiteral("id_counter_failed");
        r.error = QStringLiteral(
            "test_audit_fold_in: allocateIds returned %1 of %2 "
            "(flock/IO failure on counter %3 — check for a stale "
            "%3.lock sibling)")
                .arg(allocatedInts.size()).arg(n)
                .arg(counterPath.isEmpty()
                     ? QStringLiteral("(unresolvable)") : counterPath);
        return r;
    }
    QStringList allocated;
    for (int v : allocatedInts) {
        allocated.append(QStringLiteral("ANTS-%1").arg(v));
    }
    r.allocatedIds = allocated;
    // Render per-finding bullets.
    for (int i = 0; i < n; ++i) {
        const QJsonObject f = req.actionable.at(i).toObject();
        bs << "- 📋 [" << allocated.at(i) << "] **"
           << f.value(QStringLiteral("summary")).toString() << ".**\n"
           << "  - File: " << f.value(QStringLiteral("file")).toString()
           << ":" << f.value(QStringLiteral("line")).toInt() << "\n"
           << "  - Dimension: "
           << f.value(QStringLiteral("dimension")).toString() << "\n"
           << "  - Severity: "
           << f.value(QStringLiteral("severity")).toString() << "\n"
           << "  - Fix: "
           << f.value(QStringLiteral("fix")).toString() << "\n";
    }
    bs.flush();
    // Single insertBlock per call (INV-3). Find or fall back to the
    // active release heading via the helper.
    const QString release =
        RoadmapFoldIn::findActiveReleaseHeading(canon);
    const bool wrote =
        RoadmapFoldIn::insertBlock(canon, release, block);
    if (!wrote) {
        r.ok = false;
        r.code  = QStringLiteral("write_failed");
        r.error = QStringLiteral(
            "test_audit_fold_in: insertBlock failed (heading not "
            "found OR IO error) — release heading was \"%1\"")
                .arg(release);
        r.failedCount = n;
        return r;
    }
    r.block               = block;
    r.bytesWritten        = block.toUtf8().size();
    r.writtenCount        = n;
    r.failedCount         = 0;
    r.partial             = false;
    r.releaseBlockHeading = release;
    r.written             = canon + QLatin1String("/ROADMAP.md");
    return r;
}

}  // namespace TestAuditEngine
