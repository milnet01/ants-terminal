// ANTS-1300 — `test_results` MCP cache.

#include "testrescache.h"

#include "secureio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>

namespace TestResCache {

namespace {

constexpr int  kMaxFailures      = 50;
constexpr int  kFullExcerptCap   = 8'000;
constexpr int  kExcerptCap       = 4'000;
constexpr int  kLineCap          = 1'000;
constexpr int  kSchemaVersion    = 1;
const char    *kFileName         = "tests.json";

// Same path the AuditCache GUI/runner uses (see auditcache.h);
// inlined to keep testrescache off the ants_audit_lib link path.
QString cacheDirLocal(const QString &canonProject) {
    if (canonProject.isEmpty()) return {};
    return canonProject + QLatin1String("/.audit_cache");
}

// Per-test status line (see § 3.4 of ANTS-1300.md).
//   "   1/1221 Test #N: TestName ...   Passed    1.03 sec"
//   "   1/1221 Test #N: TestName ...***Failed   1.03 sec"
const QRegularExpression &perTestRe() {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.+\s+(?:\*\*\*)?\s*(\S+)\s+\d+\.\d+\s+sec\s*$)"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression &summaryRe() {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^\d+% tests passed,\s+(\d+) tests failed out of\s+(\d+)\s*$)"),
        QRegularExpression::MultilineOption);
    return re;
}

const QRegularExpression &totalTimeRe() {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^Total Test time \(real\) =\s+(\d+\.\d+)\s*sec\s*$)"),
        QRegularExpression::MultilineOption);
    return re;
}

bool isFailureStatus(const QString &tok) {
    return tok == QLatin1String("Failed") ||
           tok == QLatin1String("Subprocess") ||
           tok == QLatin1String("Timeout") ||
           tok == QLatin1String("Exception");
}

// Truncate per-line to kLineCap, join with \n, then truncate the
// joined value to kExcerptCap.
QString clampExcerpt(const QStringList &lines) {
    QStringList capped;
    capped.reserve(lines.size());
    for (const QString &l : lines) {
        QString s = l;
        // strip trailing whitespace per § 3.2
        while (!s.isEmpty() && s.back().isSpace()) s.chop(1);
        if (s.size() > kLineCap) s.truncate(kLineCap);
        capped.append(s);
    }
    QString joined = capped.join(QLatin1Char('\n'));
    if (joined.size() > kExcerptCap) joined.truncate(kExcerptCap);
    return joined;
}

}  // namespace

QString cachePath(const QString &canonProject) {
    const QString dir = cacheDirLocal(canonProject);
    if (dir.isEmpty()) return {};
    return dir + QLatin1Char('/') + QLatin1String(kFileName);
}

ParsedTests parseCtestOutput(const QString &output) {
    ParsedTests t;
    t.outputByteSize = output.toUtf8().size();
    if (output.isEmpty()) return t;

    // First pass: collect per-test status lines.
    struct StatusHit {
        int     start{};
        int     end{};
        QString name;
        QString token;
    };
    QList<StatusHit> hits;
    {
        auto it = perTestRe().globalMatch(output);
        while (it.hasNext()) {
            const auto m = it.next();
            StatusHit h;
            h.start = m.capturedStart();
            h.end   = m.capturedEnd();
            h.name  = m.captured(1).trimmed();
            h.token = m.captured(2);
            // Strip trailing dot-fill from the name (defence-in-depth;
            // the regex already excludes the `\.+` separator).
            while (h.name.endsWith(QLatin1Char('.'))) h.name.chop(1);
            hits.append(h);
        }
    }

    // Count by status token.
    for (const StatusHit &h : hits) {
        if (h.token == QLatin1String("Passed")) {
            ++t.passed;
        } else if (h.token == QLatin1String("Skipped")) {
            ++t.skipped;
        } else if (isFailureStatus(h.token)) {
            ++t.failed;
        }
    }

    // Summary footer overrides totals when present (authoritative).
    bool summaryMatched = false;
    {
        const auto m = summaryRe().match(output);
        if (m.hasMatch()) {
            summaryMatched = true;
            const int sumFailed = m.captured(1).toInt();
            const int sumTotal  = m.captured(2).toInt();
            // Trust the summary's total; recompute passed if our
            // per-test count came up short.
            if (sumTotal > t.passed + t.failed + t.skipped) {
                t.passed = sumTotal - sumFailed - t.skipped;
                t.failed = sumFailed;
            } else if (sumFailed > t.failed) {
                t.failed = sumFailed;
            }
        }
    }

    t.total = t.passed + t.failed + t.skipped;
    t.recognised = summaryMatched || !hits.isEmpty();
    if (!t.recognised) return t;

    // Total time footer
    {
        const auto m = totalTimeRe().match(output);
        if (m.hasMatch()) {
            const double sec = m.captured(1).toDouble();
            if (sec > 0) {
                t.durationMs = static_cast<qint64>(sec * 1000.0);
            }
        }
    }

    // Per-failure capture blocks. Each failing status hit starts a
    // block; the block ends at the next per-test hit OR the summary
    // footer, whichever comes first.
    int summaryStart = output.size();
    {
        const auto m = summaryRe().match(output);
        if (m.hasMatch()) summaryStart = m.capturedStart();
    }
    for (int i = 0; i < hits.size(); ++i) {
        if (!isFailureStatus(hits[i].token)) continue;
        if (t.failingTests.size() >= kMaxFailures) {
            t.failingTestsTruncated = true;
            break;
        }
        const int blockStart = hits[i].start;
        int blockEnd = summaryStart;
        if (i + 1 < hits.size() && hits[i + 1].start < blockEnd) {
            blockEnd = hits[i + 1].start;
        }
        QString block = output.mid(blockStart, blockEnd - blockStart);
        // Cap full body at 8000 chars (tail-truncate so the failure
        // detail at the end of the block is preserved).
        if (block.size() > kFullExcerptCap) {
            block = block.right(kFullExcerptCap);
        }
        // Tail 20 lines of the block → excerpt
        const QStringList allLines = block.split(QLatin1Char('\n'),
                                                 Qt::KeepEmptyParts);
        const int tailStart = std::max(0, static_cast<int>(allLines.size()) - 20);
        const QStringList tail = allLines.mid(tailStart);

        ParsedFailure pf;
        pf.name        = hits[i].name;
        pf.excerpt     = clampExcerpt(tail);
        pf.fullExcerpt = block;
        t.failingTests.append(pf);
    }

    return t;
}

bool recordTests(const QString &canonProject, ParsedTests &tests) {
    const QString dir = cacheDirLocal(canonProject);
    if (dir.isEmpty()) return false;
    if (!QFileInfo(dir).isDir()) {
        if (!QDir().mkpath(dir)) return false;
    }

    tests.recordedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (tests.startedAtMs > 0 && tests.finishedAtMs > 0 &&
        tests.durationMs < 0) {
        tests.durationMs = tests.finishedAtMs - tests.startedAtMs;
    }

    const QJsonObject obj = toJsonOnDisk(tests);
    const QByteArray  body = QJsonDocument(obj).toJson(QJsonDocument::Indented);

    QSaveFile sf(cachePath(canonProject));
    if (!sf.open(QIODevice::WriteOnly)) return false;
    setOwnerOnlyPerms(sf);
    if (sf.write(body) != body.size()) return false;
    if (!sf.commit()) return false;
    setOwnerOnlyPerms(cachePath(canonProject));
    return true;
}

std::optional<ParsedTests> loadTests(const QString &canonProject) {
    const QString p = cachePath(canonProject);
    if (p.isEmpty()) return std::nullopt;
    QFile f(p);
    if (!f.exists()) return std::nullopt;
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    bool ok = false;
    ParsedTests t = fromJson(doc.object(), &ok);
    if (!ok) return std::nullopt;
    return t;
}

QJsonObject toJsonOnDisk(const ParsedTests &tests) {
    QJsonObject obj;
    obj["schema_version"]   = kSchemaVersion;
    obj["recorded_at_ms"]   = tests.recordedAtMs;
    obj["exit_code"]        = tests.exitCode;
    obj["passed"]           = tests.passed;
    obj["failed"]           = tests.failed;
    obj["skipped"]          = tests.skipped;
    obj["total"]            = tests.total;
    QJsonArray failArr;
    for (const ParsedFailure &pf : tests.failingTests) {
        QJsonObject fo;
        fo["name"]         = pf.name;
        fo["excerpt"]      = pf.excerpt;
        fo["full_excerpt"] = pf.fullExcerpt;  // on-disk only
        failArr.append(fo);
    }
    obj["failing_tests"]            = failArr;
    obj["failing_tests_truncated"]  = tests.failingTestsTruncated;
    if (tests.startedAtMs  > 0) obj["started_at_ms"]  = tests.startedAtMs;
    if (tests.finishedAtMs > 0) obj["finished_at_ms"] = tests.finishedAtMs;
    if (tests.durationMs   >= 0) obj["duration_ms"]   = tests.durationMs;
    obj["output_byte_size"] = tests.outputByteSize;
    return obj;
}

QJsonObject toJsonWire(const ParsedTests &tests) {
    QJsonObject obj = toJsonOnDisk(tests);
    // Strip full_excerpt from each failing_tests[i] — INV-18.
    QJsonArray failArr = obj.value(QStringLiteral("failing_tests")).toArray();
    QJsonArray stripped;
    for (const QJsonValue &v : failArr) {
        QJsonObject fo = v.toObject();
        fo.remove(QStringLiteral("full_excerpt"));
        stripped.append(fo);
    }
    obj["failing_tests"] = stripped;
    return obj;
}

ParsedTests fromJson(const QJsonObject &obj, bool *okOut) {
    ParsedTests t;
    const int v = obj.value(QStringLiteral("schema_version")).toInt(0);
    if (v != kSchemaVersion) {
        if (okOut) *okOut = false;
        return t;
    }
    t.recordedAtMs   = static_cast<qint64>(
        obj.value(QStringLiteral("recorded_at_ms")).toDouble(0));
    t.exitCode       = obj.value(QStringLiteral("exit_code")).toInt(0);
    t.passed         = obj.value(QStringLiteral("passed")).toInt(0);
    t.failed         = obj.value(QStringLiteral("failed")).toInt(0);
    t.skipped        = obj.value(QStringLiteral("skipped")).toInt(0);
    t.total          = obj.value(QStringLiteral("total")).toInt(0);
    const QJsonArray failArr =
        obj.value(QStringLiteral("failing_tests")).toArray();
    for (const QJsonValue &v2 : failArr) {
        const QJsonObject fo = v2.toObject();
        ParsedFailure pf;
        pf.name        = fo.value(QStringLiteral("name")).toString();
        pf.excerpt     = fo.value(QStringLiteral("excerpt")).toString();
        pf.fullExcerpt =
            fo.value(QStringLiteral("full_excerpt")).toString();
        t.failingTests.append(pf);
    }
    t.failingTestsTruncated =
        obj.value(QStringLiteral("failing_tests_truncated")).toBool(false);
    if (obj.contains(QStringLiteral("started_at_ms"))) {
        t.startedAtMs = static_cast<qint64>(
            obj.value(QStringLiteral("started_at_ms")).toDouble(0));
    }
    if (obj.contains(QStringLiteral("finished_at_ms"))) {
        t.finishedAtMs = static_cast<qint64>(
            obj.value(QStringLiteral("finished_at_ms")).toDouble(0));
    }
    if (obj.contains(QStringLiteral("duration_ms"))) {
        t.durationMs = static_cast<qint64>(
            obj.value(QStringLiteral("duration_ms")).toDouble(-1));
    }
    t.outputByteSize = static_cast<qint64>(
        obj.value(QStringLiteral("output_byte_size")).toDouble(0));
    t.recognised = true;
    if (okOut) *okOut = true;
    return t;
}

}  // namespace TestResCache
