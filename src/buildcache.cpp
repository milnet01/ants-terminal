// ANTS-1299 — `build_status` MCP cache.

#include "buildcache.h"

#include "privatejsoncache.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

namespace BuildCache {

namespace {

constexpr int  kSchemaVersion    = 1;
const char    *kFileName         = "build.json";

// path:line:col: [fatal ](error|warning|note): message
// Column is optional (some clang/make/ld lines omit it). Path matches
// up to the first colon followed by a digit. ANTS-5110 — `[ \t]`, not `\s`:
// `\s*$` ate the newline and a blank line after it, so a note after a blank
// line read as adjacent and folded into the previous error. GCC's
// `fatal error:` is an error, as in scrollbackerrors.cpp (ANTS-6596).
const QRegularExpression &gccLineRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(^([^:\n]+):(\d+)(?::(\d+))?:[ \t]+(?:fatal[ \t]+)?(error|warning|note):[ \t]*(.+?)[ \t]*$)"),
        QRegularExpression::MultilineOption);
    return re;
}

// Cut `m` to kMessageCharCap UTF-16 units, one fewer when the cut would
// split a surrogate pair.
void clipMessage(QString &m) {
    if (m.size() <= kMessageCharCap) return;
    m.truncate(kMessageCharCap);
    if (m.back().isHighSurrogate()) m.chop(1);
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
        clipMessage(m);
        // lastSeverity remains "error" so a subsequent note also folds
        return;
    }
    // ANTS-5110 — the notes of an error past kMaxErrors belong to that
    // uncounted error, not to the last one kept.
    if (candidate.severity == QLatin1String("note") &&
        lastSeverity == QLatin1String("dropped-error")) {
        return;
    }

    if (candidate.severity == QLatin1String("error")) {
        ++errorsCount;
        if (errors.size() < kMaxErrors) {
            ParsedError clipped = candidate;
            clipMessage(clipped.message);
            errors.append(clipped);
            lastSeverity = QStringLiteral("error");
        } else {
            lastSeverity = QStringLiteral("dropped-error");
        }
    } else {
        ++warningsCount;
        lastSeverity = QStringLiteral("warning");
    }
}

}  // namespace

QString cachePath(const QString &canonProject) {
    const QString dir = PrivateJsonCache::dir(canonProject);
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
    const QString path = cachePath(canonProject);
    if (path.isEmpty()) return false;

    build.recordedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (build.startedAtMs > 0 && build.finishedAtMs > 0 &&
        build.durationMs < 0) {
        build.durationMs = build.finishedAtMs - build.startedAtMs;
    }

    return PrivateJsonCache::write(path, toJson(build), "build cache");
}

std::optional<ParsedBuild> loadBuild(const QString &canonProject) {
    // ANTS-6528 — the writer keeps the file small; a bigger one is not ours
    // to trust.
    const auto obj = PrivateJsonCache::read(cachePath(canonProject),
                                            kMaxCacheFileBytes);
    if (!obj) return std::nullopt;
    bool ok = false;
    ParsedBuild b = fromJson(*obj, &ok);
    if (!ok) return std::nullopt;
    return b;
}

qint64 staleSinceMs(const ParsedBuild &build) {
    return build.startedAtMs > 0 ? build.startedAtMs : build.recordedAtMs;
}

StaleCheck checkStale(const QString &canonProject, qint64 sinceMs) {
    StaleCheck out;
    // No time to compare against: whether the build is stale is unknown.
    if (canonProject.isEmpty() || sinceMs <= 0) {
        out.staleKnown = false;
        out.noTime = true;
        return out;
    }
    int probed = 0;

    const QFileInfo cmakeLists(canonProject + QStringLiteral("/CMakeLists.txt"));
    if (cmakeLists.exists() &&
        cmakeLists.lastModified().toMSecsSinceEpoch() > sinceMs) {
        out.stale = true;
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
            it.next();
            ++probed;
            if (probed > kStaleWalkCap) {
                out.staleKnown = false;
                return out;
            }
            // The iterator's own entry: a fresh QFileInfo(path) stats again.
            const QFileInfo fi = it.fileInfo();
            if (fi.lastModified().toMSecsSinceEpoch() > sinceMs) {
                out.stale = true;
                return out;
            }
        }
    }
    return out;
}

void addStaleness(QJsonObject &env, const StaleCheck &stale) {
    if (!stale.staleKnown) {
        env[stale.noTime ? QStringLiteral("stale_unknown")
                         : QStringLiteral("stale_walk_capped")] = true;
    } else if (stale.stale) {
        env[QStringLiteral("stale")] = true;
    }
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
    for (const auto &v2 : errs) {
        if (b.errors.size() == kMaxErrors) break;   // ANTS-6528 — the writer's caps
        const QJsonObject eo = v2.toObject();
        ParsedError e;
        e.file     = eo.value(QStringLiteral("file")).toString();
        e.line     = eo.value(QStringLiteral("line")).toInt(0);
        e.severity = eo.value(QStringLiteral("severity")).toString();
        e.message  = eo.value(QStringLiteral("message")).toString();
        clipMessage(e.message);
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
