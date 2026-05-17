// ANTS-1397 v1 — test_audit_* trio engine.
// See header for v1 scope + deferred items.

#include "testauditengine.h"

#include "falseposledger.h"
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
    // Convert simple ** globs to QDirIterator + filter.
    QSet<QString> seen;
    for (const QString &glob : testGlobs) {
        QStringList filters;
        if (glob.contains(QLatin1String("**"))) {
            // Recursive — derive the basename pattern.
            const int idx = glob.lastIndexOf(QLatin1Char('/'));
            filters.append(idx >= 0 ? glob.mid(idx + 1) : glob);
        } else {
            filters.append(glob);
        }
        QDirIterator it(scopeRoot, filters, QDir::Files,
                        QDirIterator::Subdirectories);
        // ANTS-1451: single source of truth for build-tree + tooling
        // exclusions. `build[^/]*` covers build/, build-asan/,
        // build-workstation/, build-debug/, future presets. _deps/,
        // CMakeFiles/, autogen/ cover ctest/CMake autogen subtrees
        // that previously surfaced moc_*.cpp + mocs_compilation.cpp
        // as tests.
        static const QRegularExpression excludeRx(QStringLiteral(
            "/(node_modules|\\.venv|__pycache__|build[^/]*|dist|_deps"
            "|CMakeFiles|autogen)/"));
        while (it.hasNext()) {
            const QString p = it.next();
            if (excludeRx.match(p).hasMatch()) continue;
            if (!seen.contains(p)) {
                seen.insert(p);
                result.append(p);
            }
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

// Pre-pass scan one file against the pattern set; return up to N
// findings as JSON objects (file, line, pattern_id, dimension).
QJsonArray prePassFile(const QString &path,
                       const QVector<QRegularExpression> &rxs,
                       int capRemaining) {
    QJsonArray out;
    if (capRemaining <= 0) return out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream ts(&f);
    int lineNo = 0;
    while (!ts.atEnd() && out.size() < capRemaining) {
        ++lineNo;
        const QString line = ts.readLine();
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
            withHints.dimensionHints = hints.values();
            std::sort(withHints.dimensionHints.begin(),
                      withHints.dimensionHints.end());
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
    if (req.reportsDir.isEmpty() ||
        req.reportsDir.contains(QStringLiteral(".."))) {
        r.ok = false; r.code = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: reports_dir invalid");
        return r;
    }
    const QString fullDir = canon + QLatin1Char('/') + req.reportsDir;
    const QString canonDir = QFileInfo(fullDir).canonicalFilePath();
    if (canonDir.isEmpty() || !canonDir.startsWith(canon) ||
        !QFileInfo(canonDir).isDir()) {
        r.ok = false; r.code = QStringLiteral("reports_dir_missing");
        r.error = QStringLiteral(
            "test_audit_synthesis_prompt: reports_dir \"%1\" does "
            "not resolve under project root").arg(req.reportsDir);
        return r;
    }
    // Read *.md top-level only; fence each report.
    QDir dir(canonDir);
    const QStringList md = dir.entryList({QStringLiteral("*.md")},
                                         QDir::Files);
    QString prompt;
    QTextStream ts(&prompt);
    ts << "# Test-Audit Synthesis\n\n";
    ts << "Per-chunk reports (each fenced for prompt-injection "
          "defence — INV-8):\n\n";
    QHash<QString, int> perDimensionCount;
    for (const QString &m : md) {
        QFile f(dir.absoluteFilePath(m));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QByteArray raw = f.readAll().left(64 * 1024);
        QString contents = QString::fromUtf8(raw);
        // Escape nested fence markers.
        contents.replace(QStringLiteral("</chunk_report>"),
                         QStringLiteral("&lt;/chunk_report&gt;"));
        ts << "<chunk_report file=\"" << m.toHtmlEscaped() << "\">\n"
           << contents << "\n</chunk_report>\n\n";
        ++r.reportsRead;
        // Heuristic dimension-summary count: any line containing
        // a known dimension name.
        for (const QString &d : g_kDimensions()) {
            if (contents.contains(d, Qt::CaseInsensitive)) {
                ++perDimensionCount[d];
            }
        }
    }
    if (!req.calibrationAnchor.isEmpty()) {
        ts << "## Calibration anchor\n"
           << "Prior raw: " << req.calibrationAnchor.value(
                QStringLiteral("raw")).toInt() << "\n"
           << "Prior actionable: " << req.calibrationAnchor.value(
                QStringLiteral("actionable")).toInt() << "\n";
    }
    ts.flush();
    QJsonObject summaries;
    for (auto it = perDimensionCount.constBegin();
         it != perDimensionCount.constEnd(); ++it) {
        QJsonObject s;
        s["count"] = it.value();
        summaries[it.key()] = s;
    }
    r.prompt = prompt;
    r.dimensionSummaries = summaries;
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
        r.ok = false; r.code = QStringLiteral("id_counter_failed");
        r.error = QStringLiteral(
            "test_audit_fold_in: allocateIds returned %1 of %2 "
            "(flock/IO failure)")
                .arg(allocatedInts.size()).arg(n);
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
