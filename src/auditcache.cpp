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
    // ANTS-1988 — ensurePrivateDir creates `.audit_cache` at 0700 with no
    // world-readable window AND tightens a pre-existing 0755 dir (the old
    // QFileInfo::isDir early-return left stale-permissive perms in place).
    return ensurePrivateDir(d);
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
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // ANTS-1745 — a corrupt manifest is dropped (start fresh), but
        // warn: the next recordRun sees empty history, so prior SARIF
        // files named in the lost manifest are never reaped (leak) and
        // the audit trail is silently discarded.
        qWarning() << "AuditCache: corrupt index.json (" << err.errorString()
                   << ") at" << p << "— starting with empty history;"
                   << "prior SARIF files may be orphaned.";
        return m;
    }
    const QJsonObject obj = doc.object();
    const int version = obj.value(QStringLiteral("version")).toInt(0);
    if (version != 1) {  // ANTS-1555 INV-7: unknown version → treat as empty
        // ANTS-1745 — surface the version mismatch (a newer Ants wrote a
        // schema this build can't read) rather than dropping it silently.
        qWarning() << "AuditCache: index.json version" << version
                   << "!= 1 at" << p << "— treating as empty history.";
        return m;
    }
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
                      QJsonObject *priorRunOut,
                      const QJsonArray &mergedFindings) {
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

    // ANTS-1870 — when the caller supplies a merged whole-tree findings
    // set, write a sibling findings sidecar `findings-<iso>-<sha>.json` and
    // stamp its basename onto last_run.findings_file. The iso here is the
    // colon form (forManifest); the filename takes the colons→hyphens form
    // sarifPathFor uses, so the stem matches the run's sarif/html siblings.
    QJsonObject lastRun = lastRunJson;
    if (!mergedFindings.isEmpty()) {
        QString fnIso = out.iso;
        fnIso.replace(QLatin1Char(':'), QLatin1Char('-'));
        const QString findingsBasename =
            QStringLiteral("findings-%1-%2.json").arg(fnIso, out.sha);
        QJsonObject sidecar;
        sidecar[QStringLiteral("version")]       = 1;
        sidecar[QStringLiteral("iso_timestamp")] = out.iso;
        sidecar[QStringLiteral("commit")]        = out.sha;
        sidecar[QStringLiteral("scope")] =
            lastRunJson.value(QStringLiteral("scope"));
        sidecar[QStringLiteral("truncated")] =
            lastRunJson.value(QStringLiteral("findings_truncated")).toBool(false);
        sidecar[QStringLiteral("findings")]      = mergedFindings;
        const QString sidecarPath =
            dir + QLatin1Char('/') + findingsBasename;
        QSaveFile sc(sidecarPath);
        if (sc.open(QIODevice::WriteOnly)) {
            setOwnerOnlyPerms(sc);
            const QByteArray scBody =
                QJsonDocument(sidecar).toJson(QJsonDocument::Compact);
            if (sc.write(scBody) == scBody.size() && sc.commit()) {
                setOwnerOnlyPerms(sidecarPath);
                fsyncParentDir(sidecarPath);
                lastRun[QStringLiteral("findings_file")] = findingsBasename;
            } else {
                qWarning() << "AuditCache: failed to write findings sidecar"
                           << sidecarPath;
            }
        }
    }

    // Build the new manifest: version + last_run + history[].
    QJsonObject newManifest;
    newManifest[QStringLiteral("version")]  = 1;
    newManifest[QStringLiteral("last_run")] = lastRun;

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
        // ANTS-1870 — carry the findings sidecar basename so the reaper
        // can delete it alongside the sarif/html when this run ages out.
        if (prev.lastRun.contains(QStringLiteral("findings_file"))) {
            entry[QStringLiteral("findings_file")] =
                prev.lastRun.value(QStringLiteral("findings_file"));
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
    // ANTS-1761 — QSaveFile::commit() fsyncs the file + atomically
    // renames, but the directory entry for the rename isn't durable until
    // the parent dir is fsynced. Without this, power loss between the
    // rename and the journal flush can lose the manifest update and
    // orphan the SARIF it pointed at.
    fsyncParentDir(manifestPath);

    // Reaper (INV-4): delete sarif/html named in dropped entries — only
    // AFTER the new manifest is durably committed. ANTS-2004 — deleting
    // before commit() meant a disk-full / permission failure at commit
    // left files removed but the *old* manifest still on disk, so its
    // history[] referenced files that no longer exist (permanent orphan
    // refs). Reaping post-commit makes the manifest and the filesystem
    // consistent in either outcome: on commit failure we returned above
    // with the old files intact; here the manifest no longer names them.
    // Each dropped entry is a JSON object we ourselves wrote, so the
    // filenames came from sarifPathFor/htmlPathFor — they live in
    // <root>/.audit_cache/ by construction. Resolve basename-only
    // entries to absolute paths.
    for (const QJsonValue &v : reaped) {
        const QJsonObject e = v.toObject();
        const QString sarif = e.value(QStringLiteral("sarif")).toString();
        const QString html  = e.value(QStringLiteral("html")).toString();
        const QString findingsFile =
            e.value(QStringLiteral("findings_file")).toString();
        auto reapOne = [&](const QString &basename) {
            if (basename.isEmpty()) return;
            QString abs = basename;
            if (!abs.startsWith(QLatin1Char('/'))) {
                abs = dir + QLatin1Char('/') + abs;
            }
            // Defence in depth: never delete files outside our cache dir.
            // canonicalFilePath resolves symlinks + `..`, so a tampered
            // manifest can't escape via a symlink planted inside
            // .audit_cache. indie-review-2026-05-21.
            const QString canonAbs = QFileInfo(abs).canonicalFilePath();
            const QString canonDir = QFileInfo(dir).canonicalFilePath();
            if (canonAbs.isEmpty() || canonDir.isEmpty()) return;
            if (!canonAbs.startsWith(canonDir + QLatin1Char('/'))) return;
            // ANTS-2004 — surface remove failures (spec §2.4); a left-over
            // file is now a harmless orphan (manifest no longer names it),
            // but the warning flags a permission/FS problem in the cache.
            if (QFile::exists(canonAbs) && !QFile::remove(canonAbs)) {
                qWarning() << "AuditCache: failed to reap dropped artefact"
                           << canonAbs;
            }
        };
        reapOne(sarif);
        reapOne(html);
        reapOne(findingsFile);  // ANTS-1870
    }

    out.ok = true;
    return out;
}

AuditCache::SidecarLoad readFindingsSidecar(const QString &canonProject,
                                            const QString &basename) {
    SidecarLoad s;
    if (basename.isEmpty()) return s;
    const QString d = cacheDirImpl(canonProject);
    if (d.isEmpty()) return s;
    QString abs = basename;
    if (!abs.startsWith(QLatin1Char('/')))
        abs = d + QLatin1Char('/') + abs;
    QFile f(abs);
    if (!f.exists()) return s;
    s.present = true;
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return s;
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("version")).toInt(0) != 1) return s;
    s.valid     = true;
    s.truncated = o.value(QStringLiteral("truncated")).toBool(false);
    s.findings  = o.value(QStringLiteral("findings")).toArray();
    return s;
}

QJsonArray loadFindingsSidecar(const QString &canonProject,
                               const QString &basename) {
    return readFindingsSidecar(canonProject, basename).findings;
}

}  // namespace AuditCache
