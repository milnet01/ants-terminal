// ANTS-1299 — `build_status` MCP cache.

#include "buildcache.h"

#include "secureio.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

namespace BuildCache {

namespace {

constexpr int  kMaxErrors        = 50;
constexpr int  kMessageCharCap   = 240;
constexpr int  kStaleWalkCap     = 5'000;
constexpr int  kSchemaVersion    = 1;
const char    *kFileName         = "build.json";

// Same path the AuditCache GUI/runner uses (see auditcache.h);
// inlined to keep buildcache off the ants_audit_lib link path.
QString cacheDirLocal(const QString &canonProject) {
    if (canonProject.isEmpty()) return {};
    return canonProject + QLatin1String("/.audit_cache");
}

// path:line:col: (error|warning|note): message
// Column is optional (some clang/make/ld lines omit it). Path matches
// up to the first colon followed by a digit.
const QRegularExpression &gccLineRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(^([^:\n]+):(\d+)(?::(\d+))?:\s+(error|warning|note):\s*(.+?)\s*$)"),
        QRegularExpression::MultilineOption);
    return re;
}

void appendOrFoldError(QList<ParsedError> &errors,
                       int &errorsCount,
                       int &warningsCount,
                       const ParsedError &candidate,
                       QString &lastSeverity) {
    // Continuation rule: a note: line that immediately follows an
    // error: line (no blank/non-matching gap) is folded into the
    // parent error's message rather than counted as a warning.
    if (candidate.severity == QLatin1String("note") &&
        lastSeverity == QLatin1String("error") &&
        !errors.isEmpty()) {
        QString &m = errors.back().message;
        const QString suffix = QStringLiteral(" / ") + candidate.message;
        m.append(suffix);
        if (m.size() > kMessageCharCap) {
            m.truncate(kMessageCharCap);
        }
        // lastSeverity remains "error" so a subsequent note also folds
        return;
    }

    if (candidate.severity == QLatin1String("error")) {
        ++errorsCount;
        if (errors.size() < kMaxErrors) {
            ParsedError clipped = candidate;
            if (clipped.message.size() > kMessageCharCap) {
                clipped.message.truncate(kMessageCharCap);
            }
            errors.append(clipped);
        }
        lastSeverity = QStringLiteral("error");
    } else {
        ++warningsCount;
        lastSeverity = QStringLiteral("warning");
    }
}

}  // namespace

QString cachePath(const QString &canonProject) {
    const QString dir = cacheDirLocal(canonProject);
    if (dir.isEmpty()) return {};
    return dir + QLatin1Char('/') + QLatin1String(kFileName);
}

ParsedBuild parseBuildOutput(const QString &output) {
    ParsedBuild b;
    b.outputByteSize = output.toUtf8().size();
    if (output.isEmpty()) return b;

    const auto &re = gccLineRe();

    QString lastSeverity;  // tracks immediate-prior matched line's severity
    int  prevLineEnd = -1;

    auto it = re.globalMatch(output);
    while (it.hasNext()) {
        const auto m = it.next();

        // Continuation must follow IMMEDIATELY — any non-matching
        // bytes between this match and the previous one breaks the
        // chain. Detect by checking whether the match starts right
        // after the previous match's end (allowing one trailing
        // newline). If anything else interposed, reset lastSeverity.
        if (prevLineEnd >= 0) {
            // Allow one newline between matches; anything more is a gap
            const int gap = m.capturedStart() - prevLineEnd;
            if (gap > 1) lastSeverity.clear();
        }
        prevLineEnd = m.capturedEnd();

        ParsedError e;
        e.file     = m.captured(1).trimmed();
        e.line     = m.captured(2).toInt();
        e.severity = m.captured(4);
        e.message  = m.captured(5).trimmed();
        appendOrFoldError(b.errors, b.errorsCount, b.warningsCount,
                          e, lastSeverity);
    }
    b.errorsTruncated = b.errorsCount > b.errors.size();
    return b;
}

bool recordBuild(const QString &canonProject, ParsedBuild &build) {
    const QString dir = cacheDirLocal(canonProject);
    if (dir.isEmpty()) return false;
    if (!QFileInfo(dir).isDir()) {
        if (!QDir().mkpath(dir)) return false;
    }

    build.recordedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (build.startedAtMs > 0 && build.finishedAtMs > 0 &&
        build.durationMs < 0) {
        build.durationMs = build.finishedAtMs - build.startedAtMs;
    }

    const QJsonObject obj = toJson(build);
    const QByteArray  body = QJsonDocument(obj).toJson(QJsonDocument::Indented);

    QSaveFile sf(cachePath(canonProject));
    if (!sf.open(QIODevice::WriteOnly)) return false;
    setOwnerOnlyPerms(sf);
    if (sf.write(body) != body.size()) return false;
    if (!sf.commit()) return false;
    setOwnerOnlyPerms(cachePath(canonProject));
    return true;
}

std::optional<ParsedBuild> loadBuild(const QString &canonProject) {
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
    ParsedBuild b = fromJson(doc.object(), &ok);
    if (!ok) return std::nullopt;
    return b;
}

StaleCheck checkStale(const QString &canonProject, qint64 recordedAtMs) {
    StaleCheck out;
    if (canonProject.isEmpty() || recordedAtMs <= 0) {
        out.staleKnown = true;
        out.stale      = false;
        return out;
    }
    int probed = 0;

    auto probeFile = [&](const QString &path) -> bool {
        const QFileInfo fi(path);
        if (!fi.exists()) return false;
        if (fi.lastModified().toMSecsSinceEpoch() > recordedAtMs) {
            out.stale = true;
            return true;
        }
        return false;
    };

    // Single-file probe: top-level CMakeLists.txt
    if (probeFile(canonProject + QStringLiteral("/CMakeLists.txt"))) {
        return out;
    }

    // Directory probes: src/, tests/, cmake/, include/ (each if present)
    const QStringList lanes = {
        QStringLiteral("src"),
        QStringLiteral("tests"),
        QStringLiteral("cmake"),
        QStringLiteral("include"),
    };
    for (const QString &lane : lanes) {
        const QString laneDir = canonProject + QLatin1Char('/') + lane;
        if (!QFileInfo(laneDir).isDir()) continue;
        QDirIterator it(laneDir,
                        QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            ++probed;
            if (probed > kStaleWalkCap) {
                out.staleKnown = false;
                return out;
            }
            const QFileInfo fi(p);
            if (fi.lastModified().toMSecsSinceEpoch() > recordedAtMs) {
                out.stale = true;
                return out;
            }
        }
    }
    return out;
}

QJsonObject toJson(const ParsedBuild &build) {
    QJsonObject obj;
    obj["schema_version"]  = kSchemaVersion;
    obj["recorded_at_ms"]  = build.recordedAtMs;
    obj["exit_code"]       = build.exitCode;
    if (build.startedAtMs  > 0) obj["started_at_ms"]  = build.startedAtMs;
    if (build.finishedAtMs > 0) obj["finished_at_ms"] = build.finishedAtMs;
    if (build.durationMs   >= 0) obj["duration_ms"]   = build.durationMs;
    QJsonArray errs;
    for (const ParsedError &e : build.errors) {
        QJsonObject eo;
        eo["file"]     = e.file;
        eo["line"]     = e.line;
        eo["severity"] = e.severity;
        eo["message"]  = e.message;
        errs.append(eo);
    }
    obj["errors"]            = errs;
    obj["errors_count"]      = build.errorsCount;
    obj["errors_truncated"]  = build.errorsTruncated;
    obj["warnings_count"]    = build.warningsCount;
    obj["output_byte_size"]  = build.outputByteSize;
    return obj;
}

ParsedBuild fromJson(const QJsonObject &obj, bool *okOut) {
    ParsedBuild b;
    const int v = obj.value(QStringLiteral("schema_version")).toInt(0);
    if (v != kSchemaVersion) {
        if (okOut) *okOut = false;
        return b;
    }
    b.recordedAtMs   = static_cast<qint64>(
        obj.value(QStringLiteral("recorded_at_ms")).toDouble(0));
    b.exitCode       = obj.value(QStringLiteral("exit_code")).toInt(0);
    if (obj.contains(QStringLiteral("started_at_ms"))) {
        b.startedAtMs = static_cast<qint64>(
            obj.value(QStringLiteral("started_at_ms")).toDouble(0));
    }
    if (obj.contains(QStringLiteral("finished_at_ms"))) {
        b.finishedAtMs = static_cast<qint64>(
            obj.value(QStringLiteral("finished_at_ms")).toDouble(0));
    }
    if (obj.contains(QStringLiteral("duration_ms"))) {
        b.durationMs = static_cast<qint64>(
            obj.value(QStringLiteral("duration_ms")).toDouble(-1));
    }
    const QJsonArray errs =
        obj.value(QStringLiteral("errors")).toArray();
    for (const QJsonValue &v2 : errs) {
        const QJsonObject eo = v2.toObject();
        ParsedError e;
        e.file     = eo.value(QStringLiteral("file")).toString();
        e.line     = eo.value(QStringLiteral("line")).toInt(0);
        e.severity = eo.value(QStringLiteral("severity")).toString();
        e.message  = eo.value(QStringLiteral("message")).toString();
        b.errors.append(e);
    }
    b.errorsCount    = obj.value(QStringLiteral("errors_count")).toInt(0);
    b.errorsTruncated =
        obj.value(QStringLiteral("errors_truncated")).toBool(false);
    b.warningsCount  = obj.value(QStringLiteral("warnings_count")).toInt(0);
    b.outputByteSize = static_cast<qint64>(
        obj.value(QStringLiteral("output_byte_size")).toDouble(0));
    if (okOut) *okOut = true;
    return b;
}

}  // namespace BuildCache
