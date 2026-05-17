// ANTS-1430 — project_layout scan helper. See header.

#include "projectlayoutengine.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace ProjectLayoutEngine {

namespace {

// Detect Ants-format marker / GFM task-list shape from the head
// of a ROADMAP.md. See spec § Scan logic for the rule list.
QString detectFormat(const QByteArray &head, bool &markerPresent) {
    markerPresent = false;
    if (head.contains("<!-- ants-roadmap-format: 1 -->")) {
        markerPresent = true;
        return QStringLiteral("ants-v1");
    }
    // GFM task-list bullets: "- [ ]" or "- [x]" at line start.
    const auto lines = head.split('\n');
    for (const auto &ln : lines) {
        if (ln.startsWith("- [ ]") || ln.startsWith("- [x]") ||
            ln.startsWith("- [X]")) {
            return QStringLiteral("github-task-list");
        }
    }
    return QStringLiteral("unknown");
}

// One streaming pass over the ROADMAP body counting bullets that
// match the detected format. Pre-1430 native parsers already do
// this; we duplicate the count here because callers want it
// without paying the full parser tax.
int countBullets(const QByteArray &body, const QString &format) {
    const auto lines = body.split('\n');
    int n = 0;
    if (format == QStringLiteral("ants-v1") ||
        format == QStringLiteral("unknown") ||
        format.isEmpty()) {
        // Native shape: `- <emoji>` at line start. The four status
        // emoji glyphs in UTF-8.
        static const QByteArray kPrefixes[4] = {
            QByteArrayLiteral("- \xE2\x9C\x85"),  // ✅
            QByteArrayLiteral("- \xF0\x9F\x93\x8B"),  // 📋
            QByteArrayLiteral("- \xF0\x9F\x9A\xA7"),  // 🚧
            QByteArrayLiteral("- \xF0\x9F\x92\xAD"),  // 💭
        };
        for (const auto &ln : lines) {
            for (const auto &p : kPrefixes) {
                if (ln.startsWith(p)) { ++n; break; }
            }
        }
    }
    if (format == QStringLiteral("github-task-list")) {
        for (const auto &ln : lines) {
            if (ln.startsWith("- [ ]") || ln.startsWith("- [x]") ||
                ln.startsWith("- [X]")) {
                ++n;
            }
        }
    }
    return n;
}

// ANTS-1493 — fork-only doc tree candidates. Some projects keep
// fork-internal docs under docs/private/ or docs/internal/ or
// docs/fork/ to avoid leaking to upstream. Cheap to probe; almost
// always null on a normal project.
namespace {
const QStringList kRoadmapCandidates = {
    QStringLiteral("ROADMAP.md"),
    QStringLiteral("docs/private/ROADMAP.md"),
    QStringLiteral("docs/internal/ROADMAP.md"),
    QStringLiteral("docs/fork/ROADMAP.md"),
};
const QStringList kChangelogCandidates = {
    QStringLiteral("CHANGELOG.md"),
    QStringLiteral("docs/private/CHANGELOG.md"),
    QStringLiteral("docs/internal/CHANGELOG.md"),
    QStringLiteral("docs/fork/CHANGELOG.md"),
};
const QStringList kSpecsCandidates = {
    QStringLiteral("docs/specs"),
    QStringLiteral("docs/private/specs"),
    QStringLiteral("docs/internal/specs"),
    QStringLiteral("docs/fork/specs"),
};
const QStringList kStandardsCandidates = {
    QStringLiteral("docs/standards"),
    QStringLiteral("docs/private/standards"),
    QStringLiteral("docs/internal/standards"),
};
const QStringList kAdrCandidates = {
    QStringLiteral("docs/decisions"),
    QStringLiteral("docs/private/decisions"),
    QStringLiteral("docs/internal/decisions"),
};
}  // namespace

void scanRoadmap(const QString &cwd, RoadmapInfo &out,
                 QStringList &probed) {
    QString rel;
    QString full;
    for (const QString &cand : kRoadmapCandidates) {
        probed.append(cand);
        const QString f = cwd + QLatin1Char('/') + cand;
        if (QFileInfo(f).exists()) {
            rel = cand; full = f;
            break;
        }
    }
    if (rel.isEmpty()) {
        // Absent: every field stays empty/zero per spec § Scan
        // logic ("ROADMAP.md is absent" rule).
        return;
    }
    QFileInfo fi(full);
    out.path      = rel;
    out.sizeBytes = fi.size();
    out.mtimeMs   = fi.lastModified().toMSecsSinceEpoch();

    QFile f(full);
    if (!f.open(QIODevice::ReadOnly)) {
        // Stat succeeded but read failed (perm/race). Leave
        // format empty so callers can detect the degraded state.
        return;
    }
    const qint64 toRead =
        qMin(static_cast<qint64>(kFormatSniffBytes), out.sizeBytes);
    const QByteArray head = f.read(toRead);
    out.format = detectFormat(head, out.formatMarkerPresent);

    // Bullet count needs the whole file. Seek to start, read all.
    f.seek(0);
    const QByteArray body = f.readAll();
    out.bulletCountEstimate = countBullets(body, out.format);
}

void scanChangelog(const QString &cwd, ChangelogInfo &out,
                   QStringList &probed) {
    for (const QString &cand : kChangelogCandidates) {
        probed.append(cand);
        const QString full = cwd + QLatin1Char('/') + cand;
        QFileInfo fi(full);
        if (!fi.exists()) continue;
        out.path      = cand;
        out.sizeBytes = fi.size();
        out.mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
        return;
    }
}

void scanDir(const QString &cwd, const QString &rel,
             QString &out, QStringList &probed) {
    probed.append(rel);
    const QFileInfo fi(cwd + QLatin1Char('/') + rel);
    if (fi.exists() && fi.isDir()) out = rel;
}

void scanFile(const QString &cwd, const QString &rel,
              QString &out, QStringList &probed) {
    probed.append(rel);
    const QFileInfo fi(cwd + QLatin1Char('/') + rel);
    if (fi.exists() && fi.isFile()) out = rel;
}

void scanAppStream(const QString &cwd, QString &out,
                   QStringList &probed) {
    // ANTS-1493 — probe at repo root + the common packaging dirs.
    // Reverse-DNS metainfo names vary wildly so we glob *.metainfo.xml
    // rather than hard-coding a prefix.
    static const QStringList kDirs = {
        QStringLiteral("."),
        QStringLiteral("packaging"),
        QStringLiteral("pkg"),
        QStringLiteral("data"),
        QStringLiteral("share/applications"),
    };
    for (const QString &rel : kDirs) {
        probed.append(rel == QStringLiteral(".")
                      ? QStringLiteral("(root)/*.metainfo.xml")
                      : (rel + QStringLiteral("/*.metainfo.xml")));
        const QString full = cwd + QLatin1Char('/') + rel;
        QDirIterator it(full, QStringList{QStringLiteral("*.metainfo.xml")},
                        QDir::Files);
        if (it.hasNext()) {
            it.next();
            out = (rel == QStringLiteral("."))
                ? it.fileName()
                : (rel + QLatin1Char('/') + it.fileName());
            return;
        }
    }
}

}  // namespace

LayoutEnvelope scanLayout(const QString &absoluteCwd) {
    LayoutEnvelope env;
    env.rootCwd     = absoluteCwd;
    env.scannedAtMs = QDateTime::currentMSecsSinceEpoch();
    env.ttlDays     = kDefaultTtlDays;
    scanRoadmap(absoluteCwd, env.roadmap,   env.probedPaths);
    scanChangelog(absoluteCwd, env.changelog, env.probedPaths);
    // ANTS-1493 — iterate candidate dirs; first-hit wins per field.
    for (const QString &cand : kSpecsCandidates) {
        if (!env.specsDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.specsDir, env.probedPaths);
    }
    for (const QString &cand : kStandardsCandidates) {
        if (!env.standardsDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.standardsDir, env.probedPaths);
    }
    for (const QString &cand : kAdrCandidates) {
        if (!env.adrDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.adrDir, env.probedPaths);
    }
    scanAppStream(absoluteCwd, env.appstreamMetainfo,
                  env.probedPaths);
    scanFile(absoluteCwd, QStringLiteral(".roadmap-counter"),
             env.counterFile, env.probedPaths);
    return env;
}

QJsonObject toJson(const LayoutEnvelope &env) {
    QJsonObject root;
    root[QStringLiteral("scanned_at_ms")] = env.scannedAtMs;
    root[QStringLiteral("ttl_days")]      = env.ttlDays;
    root[QStringLiteral("root_cwd")]      = env.rootCwd;
    QJsonObject rm;
    rm[QStringLiteral("path")]                  = env.roadmap.path;
    rm[QStringLiteral("format")]                = env.roadmap.format;
    rm[QStringLiteral("format_marker_present")] = env.roadmap.formatMarkerPresent;
    rm[QStringLiteral("bullet_count_estimate")] = env.roadmap.bulletCountEstimate;
    rm[QStringLiteral("size_bytes")]            = env.roadmap.sizeBytes;
    rm[QStringLiteral("mtime_ms")]              = env.roadmap.mtimeMs;
    root[QStringLiteral("roadmap")] = rm;
    QJsonObject cl;
    cl[QStringLiteral("path")]       = env.changelog.path;
    cl[QStringLiteral("size_bytes")] = env.changelog.sizeBytes;
    cl[QStringLiteral("mtime_ms")]   = env.changelog.mtimeMs;
    root[QStringLiteral("changelog")] = cl;
    root[QStringLiteral("specs_dir")]          = env.specsDir;
    root[QStringLiteral("standards_dir")]      = env.standardsDir;
    root[QStringLiteral("adr_dir")]            = env.adrDir;
    root[QStringLiteral("appstream_metainfo")] = env.appstreamMetainfo;
    root[QStringLiteral("counter_file")]       = env.counterFile;
    QJsonArray probed;
    for (const auto &p : env.probedPaths) probed.append(p);
    root[QStringLiteral("probed_paths")] = probed;
    return root;
}

LayoutEnvelope fromJson(const QJsonObject &obj) {
    LayoutEnvelope env;
    env.scannedAtMs = static_cast<qint64>(
        obj.value(QStringLiteral("scanned_at_ms")).toDouble(0));
    env.ttlDays     = obj.value(QStringLiteral("ttl_days"))
                          .toInt(kDefaultTtlDays);
    env.rootCwd     = obj.value(QStringLiteral("root_cwd")).toString();
    const QJsonObject rm = obj.value(QStringLiteral("roadmap")).toObject();
    env.roadmap.path                = rm.value(QStringLiteral("path")).toString();
    env.roadmap.format              = rm.value(QStringLiteral("format")).toString();
    env.roadmap.formatMarkerPresent = rm.value(QStringLiteral("format_marker_present")).toBool(false);
    env.roadmap.bulletCountEstimate = rm.value(QStringLiteral("bullet_count_estimate")).toInt(0);
    env.roadmap.sizeBytes           = static_cast<qint64>(rm.value(QStringLiteral("size_bytes")).toDouble(0));
    env.roadmap.mtimeMs             = static_cast<qint64>(rm.value(QStringLiteral("mtime_ms")).toDouble(0));
    const QJsonObject cl = obj.value(QStringLiteral("changelog")).toObject();
    env.changelog.path      = cl.value(QStringLiteral("path")).toString();
    env.changelog.sizeBytes = static_cast<qint64>(cl.value(QStringLiteral("size_bytes")).toDouble(0));
    env.changelog.mtimeMs   = static_cast<qint64>(cl.value(QStringLiteral("mtime_ms")).toDouble(0));
    env.specsDir          = obj.value(QStringLiteral("specs_dir")).toString();
    env.standardsDir      = obj.value(QStringLiteral("standards_dir")).toString();
    env.adrDir            = obj.value(QStringLiteral("adr_dir")).toString();
    env.appstreamMetainfo = obj.value(QStringLiteral("appstream_metainfo")).toString();
    env.counterFile       = obj.value(QStringLiteral("counter_file")).toString();
    const QJsonArray probed = obj.value(QStringLiteral("probed_paths")).toArray();
    for (const auto &v : probed) env.probedPaths.append(v.toString());
    return env;
}

bool isStale(const LayoutEnvelope &cached, qint64 nowMs) {
    if (cached.scannedAtMs <= 0) return true;
    const qint64 ttlMs =
        static_cast<qint64>(cached.ttlDays) * 24 * 3600 * 1000;
    if (nowMs - cached.scannedAtMs > ttlMs) return true;
    for (const auto &p : cached.probedPaths) {
        const QFileInfo fi(cached.rootCwd + QLatin1Char('/') + p);
        if (fi.exists() &&
            fi.lastModified().toMSecsSinceEpoch() >
                cached.scannedAtMs) {
            return true;
        }
    }
    return false;
}

}  // namespace ProjectLayoutEngine
