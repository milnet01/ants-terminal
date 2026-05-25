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
        // ANTS-1624 — libcheck C-unit detection. Probe before ctest so a
        // project that ships both CMake (main build) and Makefile.test
        // (libcheck suite) gets routed to libcheck and its .c globs
        // rather than ctest's .cpp-only globs.
        {QStringLiteral("libcheck"),
         {QStringLiteral("Makefile.test")},
         {QStringLiteral("tests/**/test_*.c"),
          QStringLiteral("**/test_*.c"),
          QStringLiteral("**/tst_*.c")}},
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
// PrePattern is the public TestAuditEngine::PrePassPattern (exposed for
// the ANTS-1450 drift-guard); aliased here to keep the table literal
// terse.
using PrePattern = PrePassPattern;

const QVector<PrePattern> &g_kPrePatterns() {
    // Pattern set widened ANTS-1616 — v1 was Python-centric and missed
    // every real C/C++/Qt test smell in this codebase (zero-hit rate
    // across 19 chunks reported in 2026-05-18 cross-session reports).
    // v2 ships JSON resource (see Source: in ANTS-1616 follow-up); the
    // built-in set below stays language-broad so the orchestrator's
    // pre_pass_chunk_ids[] optimisation actually fires on mixed
    // codebases.
    static const QVector<PrePattern> v = {
        // ── Python (v1 originals) ──
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
        // ── C/C++ sleep + threading ──
        {QStringLiteral("cpp_sleep_for"),       QStringLiteral("flakiness"),
            QStringLiteral("\\bstd::this_thread::sleep_for\\b")},
        {QStringLiteral("qt_msleep"),           QStringLiteral("flakiness"),
            QStringLiteral("\\bQThread::(?:msleep|sleep|usleep)\\(")},
        {QStringLiteral("posix_sleep"),         QStringLiteral("flakiness"),
            QStringLiteral("\\b(?:usleep|nanosleep|sleep)\\(")},
        // ── C/C++ exit/abort in tests ──
        {QStringLiteral("cpp_exit"),            QStringLiteral("error_handling"),
            QStringLiteral("\\b(?:std::exit|::exit|exit|abort|std::abort)\\(")},
        // ── stderr noise / FAIL-prints ──
        {QStringLiteral("stderr_fail_print"),   QStringLiteral("verbosity"),
            QStringLiteral("(?:std::fprintf|fprintf)\\s*\\(\\s*stderr\\b")},
        {QStringLiteral("cpp_cerr"),            QStringLiteral("verbosity"),
            QStringLiteral("\\bstd::cerr\\s*<<")},
        // ── Env-mutation without restore (isolation breach) ──
        {QStringLiteral("qputenv_call"),        QStringLiteral("isolation"),
            QStringLiteral("\\bqputenv\\s*\\(")},
        {QStringLiteral("setenv_call"),         QStringLiteral("isolation"),
            QStringLiteral("\\b(?:setenv|putenv|_putenv)\\s*\\(")},
        // ── External process / shell-out from tests ──
        {QStringLiteral("system_shell_out"),    QStringLiteral("isolation"),
            QStringLiteral("\\b(?:std::system|::system|system|popen)\\s*\\(")},
        // ── Non-deterministic seed ──
        {QStringLiteral("cpp_rand"),            QStringLiteral("determinism"),
            QStringLiteral("\\b(?:std::rand|qrand|rand)\\s*\\(\\s*\\)")},
        {QStringLiteral("cpp_srand"),           QStringLiteral("determinism"),
            QStringLiteral("\\b(?:std::srand|srand)\\s*\\(")},
        // ── Always-true assertion smell ──
        {QStringLiteral("tautological_expect"), QStringLiteral("assertions"),
            QStringLiteral("\\b(?:EXPECT_TRUE|ASSERT_TRUE|expect)\\s*\\(\\s*(?:true|1)\\s*\\)")},
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
QString detectFramework(const QString &probeRoot,
                        QStringList *globsOut) {
    for (const auto &fw : g_kFrameworks()) {
        for (const QString &signal : fw.signalFiles) {
            if (QFileInfo::exists(probeRoot + QLatin1Char('/') + signal)) {
                if (globsOut) *globsOut = fw.testGlobs;
                return fw.name;
            }
        }
    }
    return QString();
}

// ANTS-1623 — list every framework whose signal file exists at
// `probeRoot`, in probe-table order. Used by the polyglot path to
// fill `additional_frameworks[]` so callers see all signals at once.
QStringList detectAllFrameworks(const QString &probeRoot) {
    QStringList out;
    for (const auto &fw : g_kFrameworks()) {
        for (const QString &signal : fw.signalFiles) {
            if (QFileInfo::exists(probeRoot + QLatin1Char('/') + signal)) {
                out.append(fw.name);
                break;
            }
        }
    }
    return out;
}

// ANTS-1623 — extract `<sub>` from a `path:<sub>` scope, with
// trailing-slash normalisation. Returns empty for non-path scopes.
QString scopeSubdir(const QString &scope) {
    if (!scope.startsWith(QLatin1String("path:"))) return QString();
    QString s = scope.mid(5);
    while (s.endsWith(QLatin1Char('/'))) s.chop(1);
    return s;
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

// ANTS-1627 — strip Python string literals (single, double, triple-
// quoted, plus r/b/u/f/rb/br/fr/rf prefixes) and `#` line comments
// before pre-pass regex matching. Mirrors stripCxxLiteralsAndComments
// for .py files. Replaces stripped content with spaces (preserving
// columns) and keeps newlines verbatim (preserving line numbers).
//
// Covers the false-positive shapes reported on RetroDB Issue 5 +
// Vestige Issue 6 (2026-05-17/18):
//   * `sleep_call` matched inside a string literal that's the search-
//     needle of a `test_no_bare_time_sleep_in_jobs` negative-grep.
//   * `hardcoded_password` matched inside a string literal asserted
//     NOT to be present (`'password: admin'` in `assert ... not in`).
//   * `sleep_call` matched inside a module docstring or `#` comment
//     describing the bug being fixed.
//
// All four collapse to "match is inside a string literal or comment"
// — the same shape the C/C++ strip already handles for raw-string
// fixture scripts.
bool isPyPath(const QString &path) {
    return path.endsWith(QStringLiteral(".py"), Qt::CaseInsensitive);
}

// Skip past Python string-prefix letters (r, b, u, f, rb, br, fr, rf
// and case-variants) immediately preceding a quote. Returns the index
// of the opening quote, or -1 if `i` is not a prefix start.
int matchPyStringPrefix(const QString &src, int i) {
    int n = src.size();
    int j = i;
    int letters = 0;
    while (j < n && letters < 2) {
        const QChar c = src[j];
        const QChar lc = c.toLower();
        if (lc == QLatin1Char('r') || lc == QLatin1Char('b') ||
            lc == QLatin1Char('u') || lc == QLatin1Char('f')) {
            ++j;
            ++letters;
        } else {
            break;
        }
    }
    if (letters == 0) return -1;
    if (j < n && (src[j] == QLatin1Char('"') ||
                  src[j] == QLatin1Char('\''))) {
        return j;
    }
    return -1;
}

// Word-boundary check — the char before `i` must not be a Python
// identifier char, otherwise `bar()` would look like a `b`-prefixed
// string when followed by `"` later. Cheap one-char lookback.
bool isPyPrefixBoundary(const QString &src, int i) {
    if (i == 0) return true;
    const QChar p = src[i - 1];
    if (p.isLetterOrNumber() || p == QLatin1Char('_')) return false;
    return true;
}

QString stripPythonLiteralsAndComments(const QString &src) {
    QString out;
    out.reserve(src.size());
    enum State { Normal, LineComment,
                 StrS, StrD,            // single-quoted, double-quoted (single-line)
                 TripleS, TripleD };    // ''' and """
    State st = Normal;
    const int n = src.size();
    int i = 0;
    while (i < n) {
        const QChar c = src[i];
        const QChar n1 = (i + 1 < n) ? src[i + 1] : QChar();
        const QChar n2 = (i + 2 < n) ? src[i + 2] : QChar();
        switch (st) {
        case Normal: {
            // Python string prefix (r/b/u/f and pairs).
            if (isPyPrefixBoundary(src, i)) {
                const int q = matchPyStringPrefix(src, i);
                if (q >= 0) {
                    // Replace prefix letters with spaces.
                    for (int k = i; k < q; ++k) out.append(QLatin1Char(' '));
                    i = q;
                    continue;  // re-enter Normal at the quote char
                }
            }
            if (c == QLatin1Char('#')) {
                st = LineComment;
                out.append(QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('"') && n1 == QLatin1Char('"') &&
                       n2 == QLatin1Char('"')) {
                st = TripleD;
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                i += 3;
            } else if (c == QLatin1Char('\'') && n1 == QLatin1Char('\'') &&
                       n2 == QLatin1Char('\'')) {
                st = TripleS;
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                i += 3;
            } else if (c == QLatin1Char('"')) {
                st = StrD;
                out.append(QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('\'')) {
                st = StrS;
                out.append(QLatin1Char(' '));
                ++i;
            } else {
                out.append(c);
                ++i;
            }
            break;
        }
        case LineComment:
            if (c == QLatin1Char('\n')) { st = Normal; out.append(c); }
            else                         out.append(QLatin1Char(' '));
            ++i;
            break;
        case StrS:
        case StrD: {
            const QChar quote = (st == StrS) ? QLatin1Char('\'')
                                             : QLatin1Char('"');
            if (c == QLatin1Char('\\') && n1 != QChar()) {
                // Preserve newline if escape is line-continuation.
                out.append(QLatin1Char(' '));
                out.append(n1 == QLatin1Char('\n')
                           ? QLatin1Char('\n') : QLatin1Char(' '));
                i += 2;
            } else if (c == quote) {
                st = Normal;
                out.append(QLatin1Char(' '));
                ++i;
            } else if (c == QLatin1Char('\n')) {
                // Unterminated single-line string — be liberal, end on EOL.
                st = Normal;
                out.append(c);
                ++i;
            } else {
                out.append(QLatin1Char(' '));
                ++i;
            }
            break;
        }
        case TripleS:
        case TripleD: {
            const QChar quote = (st == TripleS) ? QLatin1Char('\'')
                                                : QLatin1Char('"');
            if (c == QLatin1Char('\\') && n1 != QChar()) {
                out.append(QLatin1Char(' '));
                out.append(n1 == QLatin1Char('\n')
                           ? QLatin1Char('\n') : QLatin1Char(' '));
                i += 2;
            } else if (c == quote && n1 == quote && n2 == quote) {
                st = Normal;
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                out.append(QLatin1Char(' '));
                i += 3;
            } else if (c == QLatin1Char('\n')) {
                // Preserve newline so line numbers stay exact.
                out.append(c);
                ++i;
            } else {
                out.append(QLatin1Char(' '));
                ++i;
            }
            break;
        }
        }
    }
    return out;
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
    // scripts. ANTS-1627 extends the same strip to .py files
    // (docstrings + comments + negative-grep needles like
    // `'time.sleep('` inside a `not in` assertion). Newlines/columns
    // preserved so line-number reporting stays exact.
    QString text;
    if      (isCxxPath(path)) text = stripCxxLiteralsAndComments(raw);
    else if (isPyPath(path))  text = stripPythonLiteralsAndComments(raw);
    else                      text = raw;
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

const QVector<PrePassPattern> &prePassPatterns() { return g_kPrePatterns(); }

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
    // Framework detect. ANTS-1623 — when scope is `path:<sub>`, probe
    // inside that subdir FIRST so polyglot projects (e.g. pytest
    // backend + vitest frontend) route the framework to the scoped
    // tree rather than the repo root's first-match. Falls back to
    // root if the subdir has no signal file (e.g. the subdir is just
    // a test-name filter inside a single-framework project).
    QStringList globs;
    const QString sub = scopeSubdir(req.scope);
    QString probedRoot = canon;
    if (!sub.isEmpty()) {
        const QString subAbs = canon + QLatin1Char('/') + sub;
        if (QFileInfo(subAbs).isDir()) {
            const QString subFw = detectFramework(subAbs, &globs);
            if (!subFw.isEmpty()) {
                r.framework = subFw;
                probedRoot  = subAbs;
            }
        }
    }
    if (r.framework.isEmpty()) {
        r.framework = detectFramework(canon, &globs);
    }
    if (r.framework.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("no_tests_found");
        r.error = QStringLiteral(
            "test_audit_partition: no test framework detected at "
            "\"%1\"").arg(canon);
        return r;
    }
    r.testGlobs = globs;

    // ANTS-1623 — polyglot probe. Collect every framework whose
    // signal sits at the project root OR at a top-level subdir, then
    // exclude the one we already picked. Emitted as
    // {name, root_rel} pairs for the caller's second-pass dispatch.
    // Subdir scan caps at the immediate children of canon (depth-1)
    // — deeper polyglot layouts are rare; the caller can call again
    // with scope:path:<...> for those.
    {
        QSet<QString> seenNames;
        seenNames.insert(r.framework);
        QJsonArray extras;
        // Root-level peers.
        for (const QString &name : detectAllFrameworks(canon)) {
            if (seenNames.contains(name)) continue;
            seenNames.insert(name);
            QJsonObject o;
            o["name"]     = name;
            o["root_rel"] = QString();    // project root
            extras.append(o);
        }
        // Top-level subdirs.
        QDir dirIter(canon);
        const QFileInfoList children = dirIter.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &child : children) {
            // Skip the build-tree + tooling exclusions used in
            // walkTestFiles so probes don't fire on `build/` etc.
            static const QSet<QString> kSkip = {
                QStringLiteral("node_modules"), QStringLiteral(".venv"),
                QStringLiteral("__pycache__"),  QStringLiteral("dist"),
                QStringLiteral("_deps"),        QStringLiteral("CMakeFiles"),
                QStringLiteral("autogen"),      QStringLiteral(".git"),
            };
            if (kSkip.contains(child.fileName())) continue;
            if (child.fileName().startsWith(QStringLiteral("build"))) continue;
            for (const QString &name : detectAllFrameworks(
                     child.absoluteFilePath())) {
                if (seenNames.contains(name)) continue;
                seenNames.insert(name);
                QJsonObject o;
                o["name"]     = name;
                o["root_rel"] = child.fileName();
                extras.append(o);
            }
        }
        r.additionalFrameworks = extras;
    }
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
    // Pre-compile patterns. ANTS-1647 — g_kPrePatterns() is a fixed static
    // table, so compile the regexes once for the process rather than on every
    // partition() call.
    static const QVector<QRegularExpression> rxs = []{
        QVector<QRegularExpression> v;
        for (const auto &p : g_kPrePatterns()) v.append(QRegularExpression(p.regex));
        return v;
    }();
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
    // ANTS-1617 — alternative finding shape: `### [SEV] dimension: <name>`
    // (RetroDB convention). The `###` doubles as a per-finding header
    // (not a section header); count once and read the dimension out of
    // the `dimension: <name>` tail if present.
    static const QRegularExpression headingFindingRx(QStringLiteral(
        "^#{3}\\s+\\[(CRIT|CRITICAL|HIGH|MED|MEDIUM|LOW|INFO)\\]"));
    static const QRegularExpression headingDimensionRx(QStringLiteral(
        "dimension:\\s*([\\w_]+)"));
    // ANTS-1617 — `## Findings (JSON)` block opener (3D Engine
    // convention). The next fenced code block is parsed as a JSON
    // array of finding objects with `severity` + optional `dimension`.
    static const QRegularExpression findingsJsonOpenerRx(QStringLiteral(
        "^#{2}\\s+Findings\\s*\\(JSON\\)\\s*$"));
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
        // Escape BOTH the open `<chunk_report` and close `</chunk_report>`
        // markers so a hostile chunk report can't forge a report frame in
        // the synthesis prompt — mirrors indiereviewengine's <lane_report>
        // hardening (ANTS-1445). indie-review-2026-05-21.
        contents.replace(QStringLiteral("</chunk_report>"),
                         QStringLiteral("&lt;/chunk_report&gt;"));
        contents.replace(QStringLiteral("<chunk_report"),
                         QStringLiteral("&lt;chunk_report"));
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
        // ANTS-1617 — also recognise `### [SEV] dimension: <name>`
        // (heading-finding form) and `## Findings (JSON)` blocks.
        auto sevCanonical = [](QString sev) -> QString {
            sev = sev.toLower();
            if (sev == QLatin1String("critical")) return QStringLiteral("crit");
            if (sev == QLatin1String("medium"))   return QStringLiteral("med");
            return sev;
        };
        QString curDim;
        int chunkFindings = 0;
        // 0 = normal; 1 = saw `## Findings (JSON)` header, awaiting
        // ``` fence opener; 2 = inside fence, accumulating body.
        int jsonFenceState = 0;
        QString findingsJsonBuf;
        auto parseFindingsJson = [&](const QString &buf) {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(
                buf.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isArray()) return;
            const QJsonArray arr = doc.array();
            for (const QJsonValue &v : arr) {
                if (!v.isObject()) continue;
                const QJsonObject obj = v.toObject();
                ++chunkFindings;
                const QString sev = sevCanonical(
                    obj.value(QStringLiteral("severity")).toString());
                QString dim = obj.value(QStringLiteral("dimension"))
                                .toString().toLower();
                if (dim.isEmpty()) dim = curDim;
                if (!dim.isEmpty() && !sev.isEmpty()
                    && g_kDimensions().contains(dim)
                    && kSevs.contains(sev)) {
                    ++sevHist[dim][sev];
                }
            }
        };
        const QStringList lines = contents.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            // — Findings(JSON) fenced-block state machine.
            if (jsonFenceState == 2) {
                if (line.trimmed().startsWith(QStringLiteral("```"))) {
                    parseFindingsJson(findingsJsonBuf);
                    jsonFenceState = 0;
                    findingsJsonBuf.clear();
                } else {
                    findingsJsonBuf += line + QLatin1Char('\n');
                }
                continue;
            }
            if (jsonFenceState == 1) {
                if (line.trimmed().startsWith(QStringLiteral("```"))) {
                    // Fence opener — flip into accumulation mode. The
                    // opener line itself isn't part of the JSON body.
                    jsonFenceState = 2;
                    continue;
                }
                // Non-blank non-fence line before opener — abandon the
                // pending Findings(JSON) state; treat the line normally
                // by falling through into the regular handlers below.
                if (!line.trimmed().isEmpty()) {
                    jsonFenceState = 0;
                }
                // (else: blank lines between header and opener are fine)
                if (jsonFenceState == 1) continue;
            }
            // — `## Findings (JSON)` opener → expect a ```json fence next.
            if (findingsJsonOpenerRx.match(line).hasMatch()) {
                jsonFenceState = 1;
                findingsJsonBuf.clear();
                continue;
            }
            // — Heading-finding form: `### [SEV] dimension: <name> — ...`
            {
                const auto hfm = headingFindingRx.match(line);
                if (hfm.hasMatch()) {
                    ++chunkFindings;
                    const QString sev = sevCanonical(hfm.captured(1));
                    QString dim;
                    const auto dimMatch = headingDimensionRx.match(line);
                    if (dimMatch.hasMatch()) {
                        dim = dimMatch.captured(1).toLower();
                    } else {
                        // Fall back to current section's dimension.
                        dim = curDim;
                    }
                    if (!dim.isEmpty() && g_kDimensions().contains(dim)
                        && kSevs.contains(sev)) {
                        ++sevHist[dim][sev];
                    }
                    continue;
                }
            }
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
                    const QString sev = sevCanonical(fm.captured(1));
                    ++sevHist[curDim][sev];
                }
            }
        }
        // Trailing-buffer flush if a Findings(JSON) block was never closed.
        if (jsonFenceState == 2 && !findingsJsonBuf.trimmed().isEmpty()) {
            parseFindingsJson(findingsJsonBuf);
        }
        // ANTS-1689 — last-resort inline `[SEVERITY]` fallback. The
        // structured passes above recognise `- [SEV]` bullets,
        // `### [SEV] dimension:` headings, and `## Findings (JSON)`
        // blocks. The default /test-audit chunk agent often emits
        // prose markdown instead — `[HIGH]` / `[MEDIUM]` tags inline
        // on a line that is neither a dash-bullet nor a heading (e.g.
        // `**[HIGH]** weak assertion at foo.py:42`). Those reports came
        // back as `0 findings` + empty severity_histograms even though
        // the data was right there (cross-session-report 2026-05-19,
        // Music_Production). Gate on chunkFindings==0 so this never
        // double-counts the structured shapes (which are line-anchored
        // and would already have fired). Re-track the current dimension
        // header so inline tags attribute to the right bucket; uppercase
        // severity tokens only (matches the structured regexes) to keep
        // narrative prose like "the [info] section" from registering.
        if (chunkFindings == 0) {
            static const QRegularExpression inlineSevRx(QStringLiteral(
                "\\[(CRIT|CRITICAL|HIGH|MED|MEDIUM|LOW|INFO)\\]"));
            QString fbDim;
            for (const QString &line : lines) {
                if (headerRx.match(line).hasMatch()) {
                    fbDim.clear();
                    for (const QString &d : g_kDimensions()) {
                        if (line.contains(d, Qt::CaseInsensitive)) {
                            fbDim = d;
                            break;
                        }
                    }
                    continue;
                }
                auto sevIt = inlineSevRx.globalMatch(line);
                while (sevIt.hasNext()) {
                    const QRegularExpressionMatch sm = sevIt.next();
                    ++chunkFindings;
                    if (!fbDim.isEmpty()) {
                        const QString sev = sevCanonical(sm.captured(1));
                        if (kSevs.contains(sev)) ++sevHist[fbDim][sev];
                    }
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
    // ANTS-1635 — narrative-mode short-circuit. Caller supplies pre-
    // rendered markdown under the `### 🧪 Test Audit YYYY-MM-DD`
    // heading; engine inserts it verbatim, skipping ID allocation and
    // per-finding bullet rendering entirely. Useful when the natural
    // shape is "prose subsection grouped by Closed-inline / Deferred /
    // False-positives", not 30 structured bullets.
    const QString heading = QStringLiteral("### 🧪 Test Audit %1")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                .left(10));
    if (req.narrativeMode) {
        const QString narrative = req.narrativeMd.trimmed();
        if (narrative.isEmpty()) {
            r.ok = false;
            r.code  = QStringLiteral("narrative_md_required");
            r.error = QStringLiteral(
                "test_audit_fold_in: narrative_mode=true requires "
                "non-empty narrative_md");
            return r;
        }
        QString block;
        block.reserve(heading.size() + narrative.size() + 8);
        block += heading;
        block += QStringLiteral("\n\n");
        block += narrative;
        if (!block.endsWith(QChar('\n'))) block += QChar('\n');
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
            return r;
        }
        r.block               = block;
        r.bytesWritten        = block.toUtf8().size();
        // Narrative mode does not allocate IDs and the on-roadmap
        // bullet count is opaque to the engine — leave writtenCount=0
        // (the standard "no per-bullet allocation" signal). Callers
        // that care about a paragraph-vs-bullet count parse the
        // returned `block` themselves.
        r.writtenCount        = 0;
        r.failedCount         = 0;
        r.partial             = false;
        r.releaseBlockHeading = release;
        r.written             = canon + QLatin1String("/ROADMAP.md");
        return r;
    }
    if (req.actionable.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("missing_field");
        r.error = QStringLiteral(
            "test_audit_fold_in: actionable[] is required "
            "(or pass narrative_mode=true + narrative_md=\"…\" — "
            "ANTS-1635)");
        return r;
    }
    // INV-3 — single batched allocate + insertBlock.
    QString block;
    QTextStream bs(&block);
    bs << heading << "\n\n"
       << "Framework: " << req.framework
       << " · Files scanned: " << req.filesScanned
       << " · Dimensions: " << req.dimensions.join(QLatin1String(", "))
       << " · Raw: " << req.rawFindings
       << " · Actionable: " << req.actionable.size() << "\n\n";
    const int n = req.actionable.size();
    // ANTS-1615 — pre-validate headlines BEFORE allocating IDs so a caller
    // that forgot to supply `summary`/`headline`/`claim` gets a loud
    // `bad_actionable` refusal instead of permanently burning N counter
    // values with nothing written to ROADMAP. (Moved above allocateIds —
    // indie-review-2026-05-21.) Accept any of the three field names;
    // truncate to ~120 chars with " …" suffix.
    QStringList headlines;
    headlines.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QJsonObject f = req.actionable.at(i).toObject();
        QString h;
        for (const auto &k : {QStringLiteral("headline"),
                              QStringLiteral("summary"),
                              QStringLiteral("claim")}) {
            const QString v = f.value(k).toString().trimmed();
            if (!v.isEmpty()) { h = v; break; }
        }
        if (h.isEmpty()) {
            r.ok = false; r.code = QStringLiteral("bad_actionable");
            r.error = QStringLiteral(
                "test_audit_fold_in: actionable[%1] has no headline / "
                "summary / claim field (keys present: %2)")
                .arg(i).arg(f.keys().join(QLatin1String(", ")));
            r.failedCount = n;
            return r;
        }
        if (h.size() > 120) h = h.left(120).trimmed() + QStringLiteral(" …");
        headlines.append(h);
    }
    // Allocate all IDs upfront — single counter touch (INV-3).
    const QList<int> allocatedInts = RoadmapFoldIn::allocateIds(canon, n);
    if (allocatedInts.size() != n) {
        // ANTS-1490 — surface the counter-file path in the error so the
        // caller can clear a stale `.lock` sibling or inspect the file.
        // ANTS-1527 — also populate `counterPath` as a structured
        // field so callers can extract it without parsing the prose.
        // ANTS-1618 — call inspectCounter post-failure to compose a
        // state-specific message; the prior unconditional "stale .lock
        // sibling" hint misled cross-session reports when the real
        // cause was corrupt content or permission failure.
        const QString counterPath = RoadmapFoldIn::counterFilePath(canon);
        const auto ins = RoadmapFoldIn::inspectCounter(canon);
        r.ok = false; r.code = QStringLiteral("id_counter_failed");
        r.counterPath = counterPath;
        const QString counterPathOrUnknown = counterPath.isEmpty()
            ? QStringLiteral("(unresolvable)") : counterPath;
        QString stateHint;
        switch (ins.state) {
        case RoadmapFoldIn::CounterState::Corrupt:
            stateHint = QStringLiteral(
                "counter contains non-integer data (preview: \"%1\") "
                "— fix the file and retry").arg(ins.preview);
            break;
        case RoadmapFoldIn::CounterState::PermissionDenied:
            stateHint = QStringLiteral(
                "no read permission on counter file");
            break;
        case RoadmapFoldIn::CounterState::PathRefused:
            stateHint = QStringLiteral(
                "counter path escapes project root (symlink guard "
                "tripped — ANTS-1342)");
            break;
        case RoadmapFoldIn::CounterState::EmptyOrAbsent:
        case RoadmapFoldIn::CounterState::Ok:
            // Ok/Empty here means inspect saw a valid counter, so the
            // failure was elsewhere — flock contention or the O_EXCL
            // rename-lock fallback (ANTS-1490) both gave up after the
            // 5 s budget. Retain the legacy hint.
            stateHint = QStringLiteral(
                "flock + O_EXCL rename-lock both timed out — check "
                "for a stale %1.lock sibling or a concurrent fold-in")
                .arg(counterPathOrUnknown);
            break;
        }
        r.error = QStringLiteral(
            "test_audit_fold_in: allocateIds returned %1 of %2 on "
            "counter %3 — %4")
                .arg(allocatedInts.size()).arg(n)
                .arg(counterPathOrUnknown).arg(stateHint);
        return r;
    }
    QStringList allocated;
    for (int v : allocatedInts) {
        allocated.append(QStringLiteral("ANTS-%1").arg(v));
    }
    r.allocatedIds = allocated;
    // Render per-finding bullets (headlines validated above, pre-allocation).
    for (int i = 0; i < n; ++i) {
        const QJsonObject f = req.actionable.at(i).toObject();
        bs << "- 📋 [" << allocated.at(i) << "] **"
           << headlines.at(i) << ".**\n"
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

// ANTS-1513 — recheck a deferred finding's cite. Read-only; confines all
// file reads to under the project root (a hostile ROADMAP cite to an
// absolute/outside path is reported as fileExists:false, never read).
RecheckResult recheck(const RecheckRequest &req) {
    RecheckResult r;
    if (req.findingId.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("missing_field");
        r.error = QStringLiteral("test_audit_recheck: finding_id is required");
        return r;
    }
    const QString canon = QFileInfo(req.callerCwd).canonicalFilePath();
    if (canon.isEmpty()) {
        r.ok = false; r.code = QStringLiteral("no_project");
        r.error = QStringLiteral("test_audit_recheck: caller_cwd does not "
                                 "resolve to an existing directory");
        return r;
    }
    const QString roadmapPath = canon + QLatin1String("/ROADMAP.md");
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        r.ok = false; r.code = QStringLiteral("no_roadmap");
        r.error = QStringLiteral("test_audit_recheck: no readable ROADMAP.md "
                                 "at project root");
        return r;
    }
    const QStringList lines =
        QString::fromUtf8(rf.readAll()).split(QChar('\n'));
    rf.close();

    // Locate the bullet carrying `[<findingId>]`.
    const QString idToken = QLatin1Char('[') + req.findingId + QLatin1Char(']');
    int bulletIdx = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).contains(idToken)) { bulletIdx = i; break; }
    }
    if (bulletIdx < 0) {
        r.ok = true; r.found = false;  // not an error — caller can branch
        return r;
    }
    r.found = true;

    // Accumulate the bullet body: its line + continuation lines until the
    // next top-level bullet or heading.
    QString body = lines.at(bulletIdx);
    for (int j = bulletIdx + 1; j < lines.size(); ++j) {
        const QString &ln = lines.at(j);
        if (ln.startsWith(QStringLiteral("- ")) ||
            ln.startsWith(QStringLiteral("#"))) break;
        body += QLatin1Char('\n') + ln;
    }

    // First `path.ext:line` cite in the body.
    static const QRegularExpression citeRx(QStringLiteral(
        "([A-Za-z0-9_][\\w./\\-]*\\.[A-Za-z0-9_]+):(\\d+)"));
    const QRegularExpressionMatch cm = citeRx.match(body);
    if (!cm.hasMatch()) {
        r.ok = true;  // bullet found, but no file:line to recheck
        return r;
    }
    r.citedFile = cm.captured(1);
    r.citedLine = cm.captured(2).toInt();

    // Resolve under the project root only (security: never read outside).
    const QString resolved =
        QFileInfo(QDir(canon).absoluteFilePath(r.citedFile)).canonicalFilePath();
    const bool underProject = !resolved.isEmpty() &&
        (resolved == canon || resolved.startsWith(canon + QLatin1Char('/')));
    r.fileExists = underProject && QFileInfo::exists(resolved);

    if (r.fileExists) {
        QFile cf(resolved);
        if (cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QStringList flines =
                QString::fromUtf8(cf.readAll()).split(QChar('\n'));
            cf.close();
            if (r.citedLine >= 1 && r.citedLine <= flines.size()) {
                r.lineExists = true;
                const QString raw = flines.at(r.citedLine - 1);
                r.currentLineText = raw.trimmed();
                // Re-match against every pre-pass pattern; first hit wins.
                for (const PrePassPattern &p : prePassPatterns()) {
                    const QRegularExpression rx(p.regex);
                    if (rx.isValid() && rx.match(raw).hasMatch()) {
                        r.lineStillMatchesPattern = true;
                        r.matchedPatternId  = p.id;
                        r.matchedDimension  = p.dimension;
                        break;
                    }
                }
            }
        }
    }
    // NB: the best-effort git rename `drift_hint` for a missing file is
    // computed by the MCP registration lambda (mainwindow.cpp), NOT here
    // — the test-audit engine stays shell-free / pure (trio INV-1).
    r.ok = true;
    return r;
}

}  // namespace TestAuditEngine
