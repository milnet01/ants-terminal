// ANTS-1555 — per-project `.audit_cache/` infrastructure.

#include "auditcache.h"

#include "secureio.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>

namespace AuditCache {

namespace {

constexpr int  kHistoryMax = 10;
constexpr int  kGitTimeoutMs = 1'500;
const char    *kManifestName = "index.json";

QString cacheDirImpl(const QString &canonProject) {
    if (canonProject.isEmpty()) return {};
    return canonProject + QLatin1String("/.audit_cache");
}

bool ensureCacheDir(const QString &canonProject) {
    const QString d = cacheDirImpl(canonProject);
    if (d.isEmpty()) return false;
    if (QFileInfo(d).isDir()) return true;
    return QDir().mkpath(d);
}

QString runGit(const QString &canonProject, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(canonProject);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(kGitTimeoutMs)) return {};
    if (!p.waitForFinished(kGitTimeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return {};
    }
    if (p.exitCode() != 0) return {};
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

}  // namespace

QString cacheDir(const QString &canonProject) {
    return cacheDirImpl(canonProject);
}

QString sarifPathFor(const QString &canonProject,
                     const QString &isoBasename,
                     const QString &gitShaShort) {
    const QString d = cacheDirImpl(canonProject);
    if (d.isEmpty()) return {};
    return QStringLiteral("%1/audit-%2-%3.sarif")
        .arg(d, isoBasename, gitShaShort);
}

QString htmlPathFor(const QString &canonProject,
                    const QString &isoBasename,
                    const QString &gitShaShort) {
    const QString d = cacheDirImpl(canonProject);
    if (d.isEmpty()) return {};
    return QStringLiteral("%1/audit-%2-%3.html")
        .arg(d, isoBasename, gitShaShort);
}

IsoNow isoNow() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    IsoNow v;
    v.forManifest = now.toString(Qt::ISODate);
    // Replace colons with hyphens for filesystem-safe filename:
    //   "2026-05-18T18:32:00Z" → "2026-05-18T18-32-00Z"
    v.forFilename = v.forManifest;
    v.forFilename.replace(QLatin1Char(':'), QLatin1Char('-'));
    // Qt's toString(Qt::ISODate) on UTC times appends "Z" — keep it
    // verbatim. If a future Qt drops the trailing Z, the test in
    // `audit_run_cache` will catch the regression.
    return v;
}

GitInfo gitInfo(const QString &canonProject) {
    GitInfo g;
    g.shortSha = runGit(canonProject,
        {QStringLiteral("rev-parse"), QStringLiteral("--short"),
         QStringLiteral("HEAD")});
    if (g.shortSha.isEmpty()) g.shortSha = QStringLiteral("nogit");
    g.branch = runGit(canonProject,
        {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
         QStringLiteral("HEAD")});
    return g;
}

Manifest loadManifest(const QString &canonProject) {
    Manifest m;
    const QString d = cacheDirImpl(canonProject);
    if (d.isEmpty()) return m;
    const QString p = d + QLatin1Char('/') + QLatin1String(kManifestName);
    QFile f(p);
    if (!f.exists()) return m;
    if (!f.open(QIODevice::ReadOnly)) return m;
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return m;
    const QJsonObject obj = doc.object();
    const int version = obj.value(QStringLiteral("version")).toInt(0);
    if (version != 1) return m;  // ANTS-1555 INV-7: unknown version → treat as empty
    m.version = version;
    m.raw     = obj;
    if (obj.contains(QStringLiteral("last_run"))) {
        const QJsonValue lr = obj.value(QStringLiteral("last_run"));
        if (lr.isObject()) m.lastRun = lr.toObject();
    }
    return m;
}

RecordedRun recordRun(const QString &canonProject,
                      const QJsonObject &lastRunJson,
                      QJsonObject *priorRunOut) {
    RecordedRun out;
    if (!ensureCacheDir(canonProject)) {
        if (priorRunOut) *priorRunOut = {};
        return out;
    }

    // Load prior manifest to (a) surface priorRun, (b) migrate
    // last_run into history.
    const Manifest prev = loadManifest(canonProject);
    if (priorRunOut) *priorRunOut = prev.lastRun;

    // Pull iso/sha out of the caller's new last_run; the caller built
    // it using AuditCache::isoNow + AuditCache::gitInfo so they
    // already match the sarif filename.
    out.iso       = lastRunJson.value(QStringLiteral("iso_timestamp"))
                                .toString();
    out.sha       = lastRunJson.value(QStringLiteral("commit"))
                                .toString();
    out.sarifPath = lastRunJson.value(QStringLiteral("sarif"))
                                .toString();
    if (lastRunJson.contains(QStringLiteral("html"))) {
        out.htmlPath = lastRunJson.value(QStringLiteral("html"))
                                   .toString();
    }
    // Resolve sarif/html basenames into absolute paths for the caller
    // contract (RecordedRun.sarifPath is absolute in spec). The
    // caller passed basenames in `sarif`/`html`.
    const QString dir = cacheDirImpl(canonProject);
    if (!out.sarifPath.isEmpty() &&
        !out.sarifPath.startsWith(QLatin1Char('/'))) {
        out.sarifPath = dir + QLatin1Char('/') + out.sarifPath;
    }
    if (!out.htmlPath.isEmpty() &&
        !out.htmlPath.startsWith(QLatin1Char('/'))) {
        out.htmlPath = dir + QLatin1Char('/') + out.htmlPath;
    }

    // Build the new manifest: version + last_run + history[].
    QJsonObject newManifest;
    newManifest[QStringLiteral("version")]  = 1;
    newManifest[QStringLiteral("last_run")] = lastRunJson;

    QJsonArray history;
    if (!prev.lastRun.isEmpty()) {
        // Distil prior last_run into a compact history entry.
        QJsonObject entry;
        entry[QStringLiteral("iso_timestamp")] =
            prev.lastRun.value(QStringLiteral("iso_timestamp"));
        entry[QStringLiteral("commit")] =
            prev.lastRun.value(QStringLiteral("commit"));
        if (prev.lastRun.contains(QStringLiteral("sarif"))) {
            entry[QStringLiteral("sarif")] =
                prev.lastRun.value(QStringLiteral("sarif"));
        }
        if (prev.lastRun.contains(QStringLiteral("html"))) {
            entry[QStringLiteral("html")] =
                prev.lastRun.value(QStringLiteral("html"));
        }
        history.append(entry);
    }
    // Append the old history[] after the migrated last_run.
    if (prev.raw.contains(QStringLiteral("history"))) {
        const QJsonArray oldHistory =
            prev.raw.value(QStringLiteral("history")).toArray();
        for (const QJsonValue &v : oldHistory) {
            history.append(v);
        }
    }
    // ANTS-1555 INV-3 — cap history[] at 10 entries. Anything beyond
    // gets reaped (INV-4) — but only the files we wrote. AuditDialog
    // GUI artefacts never appear in our history[] in the first place.
    QJsonArray reaped;
    while (history.size() > kHistoryMax) {
        reaped.append(history.last());
        history.removeLast();
    }
    newManifest[QStringLiteral("history")] = history;

    // Reaper (INV-4): delete sarif/html named in dropped entries.
    // Each dropped entry is a JSON object we ourselves wrote, so the
    // filenames came from sarifPathFor/htmlPathFor — they live in
    // <root>/.audit_cache/ by construction. Resolve basename-only
    // entries to absolute paths.
    for (const QJsonValue &v : reaped) {
        const QJsonObject e = v.toObject();
        const QString sarif = e.value(QStringLiteral("sarif")).toString();
        const QString html  = e.value(QStringLiteral("html")).toString();
        auto reapOne = [&](const QString &basename) {
            if (basename.isEmpty()) return;
            QString abs = basename;
            if (!abs.startsWith(QLatin1Char('/'))) {
                abs = dir + QLatin1Char('/') + abs;
            }
            // Defence in depth: never delete files outside our cache dir.
            const QString canonAbs = QFileInfo(abs).absoluteFilePath();
            if (!canonAbs.startsWith(dir + QLatin1Char('/'))) return;
            QFile::remove(canonAbs);
        };
        reapOne(sarif);
        reapOne(html);
    }

    // Atomic write of index.json via QSaveFile.
    const QString manifestPath =
        dir + QLatin1Char('/') + QLatin1String(kManifestName);
    QSaveFile sf(manifestPath);
    if (!sf.open(QIODevice::WriteOnly)) {
        qWarning() << "AuditCache: failed to open index.json for write:"
                   << sf.errorString();
        return out;
    }
    setOwnerOnlyPerms(sf);
    const QByteArray body =
        QJsonDocument(newManifest).toJson(QJsonDocument::Indented);
    if (sf.write(body) != body.size()) {
        qWarning() << "AuditCache: short write on index.json";
        return out;
    }
    if (!sf.commit()) {
        qWarning() << "AuditCache: commit failed on index.json:"
                   << sf.errorString();
        return out;
    }
    setOwnerOnlyPerms(manifestPath);

    out.ok = true;
    return out;
}

}  // namespace AuditCache
