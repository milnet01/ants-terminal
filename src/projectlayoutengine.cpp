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
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace ProjectLayoutEngine {

namespace {

// Detect Ants-format marker / GFM task-list / ants-v1 emoji shape
// from the head of a ROADMAP.md. See spec § Scan logic for the rule
// list. ANTS-1632 — when multiple shapes hit (mixed-format roadmaps
// accumulated over time: newer sections in GFM task-list shape,
// older slices in ants-v1 emoji shape, no explicit format marker),
// return "mixed" instead of dropping out to "unknown". `countBullets`
// honours the same disjunction so `bullet_count_estimate` doesn't
// drop to zero on the mixed case. ANTS-1903 — every call fills the
// trace struct so an "unknown" outcome is diagnosable from the
// envelope (was it the 4 KB sniff budget? the marker? the bullets?).
QString detectFormat(const QByteArray &slice,
                     bool &markerPresent,
                     RoadmapSnifferTrace &trace) {
    markerPresent = false;
    trace.headBytesScanned = slice.size();
    if (slice.contains("<!-- ants-roadmap-format: 1 -->")) {
        markerPresent = true;
        trace.markerHit = true;
        return QStringLiteral("ants-v1");
    }
    // ANTS-1632 — scan once, score both shapes.
    bool gfmHit  = false;
    bool antsHit = false;
    // ants-v1 emoji status prefixes (UTF-8). Mirror countBullets'
    // kPrefixes set so detection and count agree on what counts
    // as an ants-v1 bullet.
    static const QByteArray kAntsPrefixes[4] = {
        QByteArrayLiteral("- \xE2\x9C\x85"),         // ✅
        QByteArrayLiteral("- \xF0\x9F\x93\x8B"),     // 📋
        QByteArrayLiteral("- \xF0\x9F\x9A\xA7"),     // 🚧
        QByteArrayLiteral("- \xF0\x9F\x92\xAD"),     // 💭
    };
    const auto lines = slice.split('\n');
    for (const auto &ln : lines) {
        if (!gfmHit &&
            (ln.startsWith("- [ ]") || ln.startsWith("- [x]") ||
             ln.startsWith("- [X]"))) {
            gfmHit = true;
        }
        if (!antsHit) {
            for (const auto &p : kAntsPrefixes) {
                if (ln.startsWith(p)) { antsHit = true; break; }
            }
        }
        if (gfmHit && antsHit) break;
    }
    trace.gfmTaskListHit = gfmHit;
    trace.antsV1EmojiHit = antsHit;
    if (gfmHit && antsHit) return QStringLiteral("mixed");
    if (gfmHit)            return QStringLiteral("github-task-list");
    if (antsHit)           return QStringLiteral("ants-v1");
    return QStringLiteral("unknown");
}

// One streaming pass over the ROADMAP body counting bullets that
// match the detected format. Pre-1430 native parsers already do
// this; we duplicate the count here because callers want it
// without paying the full parser tax.
// ANTS-1632 — "mixed" counts the union (a line scoring as either
// shape ticks the counter once); "unknown" stays on the ants-v1
// pass so back-compat callers see the same number they always have.
int countBullets(const QByteArray &body, const QString &format) {
    const auto lines = body.split('\n');
    int n = 0;
    // Native ants-v1 shape: `- <status-emoji>` at line start. The
    // four status emoji glyphs in UTF-8. Used by the ants-v1,
    // mixed, unknown, and empty-format paths.
    static const QByteArray kAntsPrefixes[4] = {
        QByteArrayLiteral("- \xE2\x9C\x85"),         // ✅
        QByteArrayLiteral("- \xF0\x9F\x93\x8B"),     // 📋
        QByteArrayLiteral("- \xF0\x9F\x9A\xA7"),     // 🚧
        QByteArrayLiteral("- \xF0\x9F\x92\xAD"),     // 💭
    };
    auto matchesAnts = [&](const QByteArray &ln) {
        for (const auto &p : kAntsPrefixes) {
            if (ln.startsWith(p)) return true;
        }
        return false;
    };
    auto matchesGfm = [](const QByteArray &ln) {
        return ln.startsWith("- [ ]") || ln.startsWith("- [x]") ||
               ln.startsWith("- [X]");
    };
    const bool wantAnts = (format == QStringLiteral("ants-v1") ||
                           format == QStringLiteral("mixed") ||
                           format == QStringLiteral("unknown") ||
                           format.isEmpty());
    const bool wantGfm  = (format == QStringLiteral("github-task-list") ||
                           format == QStringLiteral("mixed"));
    for (const auto &ln : lines) {
        if (wantAnts && matchesAnts(ln)) { ++n; continue; }
        if (wantGfm  && matchesGfm(ln))  { ++n; continue; }
    }
    return n;
}

// ANTS-1493 — fork-only doc tree candidates. Some projects keep
// fork-internal docs under docs/private/ or docs/internal/ or
// docs/fork/ to avoid leaking to upstream. Cheap to probe; almost
// always null on a normal project.
//
// ANTS-1507 — case-insensitive resolver. We pre-declare candidates
// in canonical case (UPPER on the basename) but resolve via
// `caseInsensitiveResolve` so projects that ship `Readme.md` /
// `Changelog.md` / `roadmap.md` aren't told their docs are missing.
namespace {
// Resolve `relPath` under `cwd` case-insensitively. Returns the
// actual on-disk relative path that matches, or QString() on miss.
// Walks the path component-by-component so each segment can match
// independently of the others' case. Bounded to the existing
// candidate set — the directory listings remain O(entries-per-dir).
QString caseInsensitiveResolve(const QString &cwd, const QString &relPath) {
    // Fast path: exact match.
    if (QFileInfo::exists(cwd + QLatin1Char('/') + relPath)) {
        return relPath;
    }
    const QStringList parts = relPath.split(QLatin1Char('/'),
                                            Qt::SkipEmptyParts);
    QString cur;
    for (const QString &p : parts) {
        const QString parent = cur.isEmpty() ? cwd
                                             : (cwd + QLatin1Char('/') + cur);
        QDir d(parent);
        if (!d.exists()) return {};
        // Match the part case-insensitively against the directory's
        // entries; first hit wins (stable ordering by Qt::Name).
        const QStringList entries =
            d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                        QDir::Name);
        QString hit;
        for (const QString &e : entries) {
            if (e.compare(p, Qt::CaseInsensitive) == 0) { hit = e; break; }
        }
        if (hit.isEmpty()) return {};
        if (!cur.isEmpty()) cur += QLatin1Char('/');
        cur += hit;
    }
    return cur;
}

const QStringList kRoadmapCandidates = {
    QStringLiteral("ROADMAP.md"),
    QStringLiteral("docs/private/ROADMAP.md"),
    QStringLiteral("docs/internal/ROADMAP.md"),
    QStringLiteral("docs/fork/ROADMAP.md"),
};
const QStringList kChangelogCandidates = {
    QStringLiteral("CHANGELOG.md"),
    QStringLiteral("CHANGELOG.yaml"),
    QStringLiteral("CHANGELOG.yml"),
    // ANTS-1574 — RetroDB and similar projects keep a runtime-readable
    // structured changelog at data/changelog.yaml. Probed independently
    // so it doesn't collide with the root-level CHANGELOG.* set, and so
    // case-insensitive resolution keeps the lowercase `changelog.yaml`
    // basename intact.
    QStringLiteral("data/changelog.yaml"),
    QStringLiteral("data/changelog.yml"),
    QStringLiteral("data/CHANGELOG.yaml"),
    QStringLiteral("data/CHANGELOG.yml"),
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
// ANTS-1880 — per-phase design docs at `docs/phases/`. Probe order
// mirrors kSpecsCandidates so the JSON wire shape stays uniform.
const QStringList kPhasesCandidates = {
    QStringLiteral("docs/phases"),
    QStringLiteral("docs/private/phases"),
    QStringLiteral("docs/internal/phases"),
    QStringLiteral("docs/fork/phases"),
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
                 QStringList &probed, QStringList &discovered) {
    QString rel;
    QString full;
    for (const QString &cand : kRoadmapCandidates) {
        probed.append(cand);
        // ANTS-1507 — case-insensitive resolution. `Readme.md`,
        // `roadmap.md`, etc. resolve to the same probe key.
        const QString resolved = caseInsensitiveResolve(cwd, cand);
        if (!resolved.isEmpty()) {
            rel = resolved; full = cwd + QLatin1Char('/') + resolved;
            discovered.append(resolved);
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
    out.format = detectFormat(head, out.formatMarkerPresent,
                              out.snifferTrace);

    // Bullet count needs the whole file. Seek to start, read all.
    f.seek(0);
    const QByteArray body = f.readAll();

    // ANTS-1903 — when the 4 KB head sniff returns "unknown" but the
    // file is larger than the head budget, retry the sniff against
    // the full body. Vestige's 484 KB mixed-format ROADMAP had a
    // long preamble + heading block before the first bullet, which
    // exceeded the head budget on every probe. Re-sniffing the body
    // is cheap (we just read it for countBullets anyway) and
    // resolves the false-unknown case without changing the common
    // back-compat path (when the head sniff hit, we skip this).
    if (out.format == QStringLiteral("unknown") &&
        out.sizeBytes > kFormatSniffBytes) {
        const QString headFormat = out.format;
        RoadmapSnifferTrace bodyTrace;
        const QString bodyFormat =
            detectFormat(body, out.formatMarkerPresent, bodyTrace);
        if (bodyFormat != headFormat) {
            out.format = bodyFormat;
            out.snifferTrace = bodyTrace;
            out.snifferTrace.fullScan = true;
        }
    }

    out.bulletCountEstimate = countBullets(body, out.format);
}

void scanChangelog(const QString &cwd, ChangelogInfo &out,
                   QStringList &probed, QStringList &discovered) {
    for (const QString &cand : kChangelogCandidates) {
        probed.append(cand);
        // ANTS-1507 — case-insensitive resolution + YAML extensions.
        const QString resolved = caseInsensitiveResolve(cwd, cand);
        if (resolved.isEmpty()) continue;
        QFileInfo fi(cwd + QLatin1Char('/') + resolved);
        if (!fi.exists()) continue;
        out.path      = resolved;
        out.sizeBytes = fi.size();
        out.mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
        discovered.append(resolved);
        return;
    }
}

void scanDir(const QString &cwd, const QString &rel,
             QString &out, QStringList &probed,
             QStringList &discovered) {
    probed.append(rel);
    const QString resolved = caseInsensitiveResolve(cwd, rel);
    if (resolved.isEmpty()) return;
    const QFileInfo fi(cwd + QLatin1Char('/') + resolved);
    if (fi.exists() && fi.isDir()) {
        out = resolved;
        discovered.append(resolved);
    }
}

void scanFile(const QString &cwd, const QString &rel,
              QString &out, QStringList &probed,
              QStringList &discovered) {
    probed.append(rel);
    const QString resolved = caseInsensitiveResolve(cwd, rel);
    if (resolved.isEmpty()) return;
    const QFileInfo fi(cwd + QLatin1Char('/') + resolved);
    if (fi.exists() && fi.isFile()) {
        out = resolved;
        discovered.append(resolved);
    }
}

// ANTS-1574 — name-based standards fallback. When docs/standards/ doesn't
// exist, scan docs/*.md for filenames matching STANDARD|DESIGN|STYLE|GUIDE
// (case-insensitive). Filter to ≥ kStandardsFallbackMinLines so stub
// helper docs (e.g. a 3-line CONTRIBUTING-style placeholder named
// `STYLE.md`) don't get promoted to standards-class. Matches go into
// `standardsFiles[]` AND `discovered[]` so callers that scan either
// surface pick them up uniformly. probedPaths records the docs/
// directory we tried so isStale() can detect later additions.
constexpr int kStandardsFallbackMinLines = 100;

int countLinesProbe(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    int n = 0;
    while (!f.atEnd()) {
        f.readLine();
        ++n;
        if (n >= kStandardsFallbackMinLines) return n;
    }
    return n;
}

void scanStandardsFallback(const QString &cwd, QStringList &out,
                           QStringList &probed, QStringList &discovered) {
    // Marker so isStale picks up future additions / standards-name renames.
    probed.append(QStringLiteral("docs/*STANDARD*|*DESIGN*|*STYLE*|*GUIDE*.md"));
    const QString docsDir =
        caseInsensitiveResolve(cwd, QStringLiteral("docs"));
    if (docsDir.isEmpty()) return;
    QDir d(cwd + QLatin1Char('/') + docsDir);
    if (!d.exists()) return;
    static const QRegularExpression rx(
        QStringLiteral(".*(STANDARD|DESIGN|STYLE|GUIDE).*\\.md$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto entries =
        d.entryList(QStringList{QStringLiteral("*.md")},
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDir::Name);
    for (const QString &e : entries) {
        if (!rx.match(e).hasMatch()) continue;
        const QString rel = docsDir + QLatin1Char('/') + e;
        if (countLinesProbe(cwd + QLatin1Char('/') + rel)
            < kStandardsFallbackMinLines) continue;
        out.append(rel);
        discovered.append(rel);
    }
}

void scanAppStream(const QString &cwd, QString &out,
                   QStringList &probed, QStringList &discovered) {
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
            discovered.append(out);
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
    scanRoadmap(absoluteCwd, env.roadmap, env.probedPaths,
                env.discovered);
    scanChangelog(absoluteCwd, env.changelog, env.probedPaths,
                  env.discovered);
    // ANTS-1493 — iterate candidate dirs; first-hit wins per field.
    for (const QString &cand : kSpecsCandidates) {
        if (!env.specsDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.specsDir, env.probedPaths,
                env.discovered);
    }
    // ANTS-1880 — phases probe mirrors the specs probe; field stays
    // empty when no phases dir exists (back-compat for ants-terminal
    // itself, which uses only docs/specs/).
    for (const QString &cand : kPhasesCandidates) {
        if (!env.phasesDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.phasesDir, env.probedPaths,
                env.discovered);
    }
    for (const QString &cand : kStandardsCandidates) {
        if (!env.standardsDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.standardsDir, env.probedPaths,
                env.discovered);
    }
    // ANTS-1574 — only fall back to name-glob when no canonical
    // standards/ dir resolved. Projects that ship both
    // docs/standards/ AND name-glob hits keep `standards_dir` as the
    // primary signal; the name-glob is the "no canonical dir at all"
    // safety net.
    if (env.standardsDir.isEmpty()) {
        scanStandardsFallback(absoluteCwd, env.standardsFiles,
                              env.probedPaths, env.discovered);
    }
    for (const QString &cand : kAdrCandidates) {
        if (!env.adrDir.isEmpty()) break;
        scanDir(absoluteCwd, cand, env.adrDir, env.probedPaths,
                env.discovered);
    }
    scanAppStream(absoluteCwd, env.appstreamMetainfo,
                  env.probedPaths, env.discovered);
    scanFile(absoluteCwd, QStringLiteral(".roadmap-counter"),
             env.counterFile, env.probedPaths, env.discovered);
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
    // ANTS-1903 — per-branch sniffer trace. Emitted only when the
    // roadmap was found (path non-empty); back-compat callers reading
    // the existing fields ignore the additive object.
    if (!env.roadmap.path.isEmpty()) {
        QJsonObject st;
        st[QStringLiteral("marker_hit")] =
            env.roadmap.snifferTrace.markerHit;
        st[QStringLiteral("ants_v1_emoji_hit")] =
            env.roadmap.snifferTrace.antsV1EmojiHit;
        st[QStringLiteral("gfm_task_list_hit")] =
            env.roadmap.snifferTrace.gfmTaskListHit;
        st[QStringLiteral("full_scan")] =
            env.roadmap.snifferTrace.fullScan;
        st[QStringLiteral("head_bytes_scanned")] =
            env.roadmap.snifferTrace.headBytesScanned;
        rm[QStringLiteral("sniffer_branches_tried")] = st;
    }
    root[QStringLiteral("roadmap")] = rm;
    QJsonObject cl;
    cl[QStringLiteral("path")]       = env.changelog.path;
    cl[QStringLiteral("size_bytes")] = env.changelog.sizeBytes;
    cl[QStringLiteral("mtime_ms")]   = env.changelog.mtimeMs;
    root[QStringLiteral("changelog")] = cl;
    root[QStringLiteral("specs_dir")]          = env.specsDir;
    // ANTS-1880 — per-phase design docs dir; empty string when
    // absent. Pre-1880 cached envelopes deserialize cleanly via
    // fromJson's missing-key default below.
    root[QStringLiteral("phases_dir")]         = env.phasesDir;
    root[QStringLiteral("standards_dir")]      = env.standardsDir;
    root[QStringLiteral("adr_dir")]            = env.adrDir;
    root[QStringLiteral("appstream_metainfo")] = env.appstreamMetainfo;
    root[QStringLiteral("counter_file")]       = env.counterFile;
    QJsonArray probed;
    for (const auto &p : env.probedPaths) probed.append(p);
    root[QStringLiteral("probed_paths")] = probed;
    // ANTS-1620 — schema version so `isStale` can invalidate
    // pre-widening caches before TTL expiry.
    root[QStringLiteral("probe_set_version")] = env.probeSetVersion;
    // ANTS-1507 — discovered[] mirror of probedPaths filtered to
    // paths that actually matched. Cheap disambiguation for callers
    // ("scan succeeded with nothing" vs "scan found these things").
    QJsonArray discovered;
    for (const auto &p : env.discovered) discovered.append(p);
    root[QStringLiteral("discovered")] = discovered;
    // ANTS-1511 — explicit top-level booleans so callers don't have
    // to inspect nested objects. `roadmap_found` is true iff the
    // roadmap probe matched a file.
    root[QStringLiteral("roadmap_found")]   = !env.roadmap.path.isEmpty();
    root[QStringLiteral("changelog_found")] = !env.changelog.path.isEmpty();
    // ANTS-1574 — name-glob standards fallback. Empty when
    // standards_dir was populated OR when no docs/*STANDARD*.md
    // matched. Callers reading `discovered[]` already see these
    // paths; this surface lets a caller filter directly without
    // re-applying the name regex.
    QJsonArray sfiles;
    for (const auto &p : env.standardsFiles) sfiles.append(p);
    root[QStringLiteral("standards_files")] = sfiles;
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
    // ANTS-1903 — round-trip the sniffer trace. Missing object (pre-
    // 1903 caches) leaves the struct at default-init (all false / 0)
    // — harmless, and the probeSetVersion bump invalidates them
    // anyway via isStale().
    const QJsonObject st =
        rm.value(QStringLiteral("sniffer_branches_tried")).toObject();
    env.roadmap.snifferTrace.markerHit =
        st.value(QStringLiteral("marker_hit")).toBool(false);
    env.roadmap.snifferTrace.antsV1EmojiHit =
        st.value(QStringLiteral("ants_v1_emoji_hit")).toBool(false);
    env.roadmap.snifferTrace.gfmTaskListHit =
        st.value(QStringLiteral("gfm_task_list_hit")).toBool(false);
    env.roadmap.snifferTrace.fullScan =
        st.value(QStringLiteral("full_scan")).toBool(false);
    env.roadmap.snifferTrace.headBytesScanned = static_cast<qint64>(
        st.value(QStringLiteral("head_bytes_scanned")).toDouble(0));
    const QJsonObject cl = obj.value(QStringLiteral("changelog")).toObject();
    env.changelog.path      = cl.value(QStringLiteral("path")).toString();
    env.changelog.sizeBytes = static_cast<qint64>(cl.value(QStringLiteral("size_bytes")).toDouble(0));
    env.changelog.mtimeMs   = static_cast<qint64>(cl.value(QStringLiteral("mtime_ms")).toDouble(0));
    env.specsDir          = obj.value(QStringLiteral("specs_dir")).toString();
    // ANTS-1880 — missing key → empty string (pre-1880 caches).
    env.phasesDir         = obj.value(QStringLiteral("phases_dir")).toString();
    env.standardsDir      = obj.value(QStringLiteral("standards_dir")).toString();
    env.adrDir            = obj.value(QStringLiteral("adr_dir")).toString();
    env.appstreamMetainfo = obj.value(QStringLiteral("appstream_metainfo")).toString();
    env.counterFile       = obj.value(QStringLiteral("counter_file")).toString();
    const QJsonArray probed = obj.value(QStringLiteral("probed_paths")).toArray();
    for (const auto &v : probed) env.probedPaths.append(v.toString());
    // ANTS-1620 — default 0 when the field is absent (pre-1620
    // cached envelopes); 0 < kProbeSetVersion ⇒ isStale invalidates.
    env.probeSetVersion =
        obj.value(QStringLiteral("probe_set_version")).toInt(0);
    const QJsonArray disc = obj.value(QStringLiteral("discovered")).toArray();
    for (const auto &v : disc) env.discovered.append(v.toString());
    const QJsonArray sfiles =
        obj.value(QStringLiteral("standards_files")).toArray();
    for (const auto &v : sfiles) env.standardsFiles.append(v.toString());
    return env;
}

bool isStale(const LayoutEnvelope &cached, qint64 nowMs) {
    if (cached.scannedAtMs <= 0) return true;
    // ANTS-1620 — probe-set version mismatch invalidates the cache.
    // A cache that was scanned with a narrower candidate set would
    // otherwise return a stale `probed_paths[]` echo until TTL
    // expiry (7 days), violating the documented contract that the
    // echo lists every path actually probed by the current build.
    if (cached.probeSetVersion < kProbeSetVersion) return true;
    const qint64 ttlMs =
        static_cast<qint64>(cached.ttlDays) * 24 * 3600 * 1000;
    if (nowMs - cached.scannedAtMs > ttlMs) return true;
    for (const auto &p : cached.probedPaths) {
        // ANTS-1507 — probed paths are stored in canonical case but
        // the real on-disk file may have shifted case (or appeared
        // post-scan in a new case). Resolve case-insensitively when
        // the literal form misses, so we don't claim freshness when
        // an alt-case rename has happened.
        QString rel = p;
        QFileInfo fi(cached.rootCwd + QLatin1Char('/') + rel);
        if (!fi.exists()) {
            const QString alt = caseInsensitiveResolve(cached.rootCwd, p);
            if (!alt.isEmpty()) {
                rel = alt;
                fi = QFileInfo(cached.rootCwd + QLatin1Char('/') + rel);
            }
        }
        if (fi.exists() &&
            fi.lastModified().toMSecsSinceEpoch() >
                cached.scannedAtMs) {
            return true;
        }
    }
    return false;
}

}  // namespace ProjectLayoutEngine
