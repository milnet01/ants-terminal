// ANTS-1430 — per-project layout scan helper. Pre-caches the
// shape of a project tree (ROADMAP.md, CHANGELOG.md, docs/specs,
// docs/standards, docs/decisions, packaging/*.metainfo.xml,
// .roadmap-counter) so MCP tools don't re-derive the same set of
// stat() probes on every call. Persists via SessionMemoryEngine
// under the well-known key `project_layout`; TTL + mtime
// invalidation.
//
// Pure Qt6::Core. Lives in ants_core_lib so non-GUI consumers
// (CI runners, ants-helper, future MCP) link it directly.
// See docs/specs/ANTS-1430.md.

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ProjectLayoutEngine {

constexpr int kDefaultTtlDays   = 7;

// Maximum bytes read from the head of ROADMAP.md when sniffing
// the format marker. See spec § Scan logic for the 4 KB
// justification. Files smaller than this are read whole.
constexpr int kFormatSniffBytes = 4096;

struct RoadmapInfo {
    QString  path;
    QString  format;                       // "ants-v1" | "github-task-list" | "unknown" | ""
    bool     formatMarkerPresent = false;
    int      bulletCountEstimate = 0;
    qint64   sizeBytes           = 0;
    qint64   mtimeMs             = 0;
};

struct ChangelogInfo {
    QString  path;
    qint64   sizeBytes = 0;
    qint64   mtimeMs   = 0;
};

struct LayoutEnvelope {
    qint64         scannedAtMs   = 0;
    int            ttlDays       = kDefaultTtlDays;
    QString        rootCwd;
    RoadmapInfo    roadmap;
    ChangelogInfo  changelog;
    QString        specsDir;
    QString        standardsDir;
    QString        adrDir;
    QString        appstreamMetainfo;
    QString        counterFile;
    // Probed paths (relative to rootCwd). isStale() re-stats
    // these to detect post-scan changes.
    QStringList    probedPaths;
    // ANTS-1507 — every probe that actually matched (file or dir).
    // Lets callers tell "scan succeeded with nothing" from "scan
    // succeeded and here's what I found" without inspecting every
    // nested field. Entries are project-relative paths.
    QStringList    discovered;
};

// Walks the well-known path set under `absoluteCwd`, populates
// the envelope. Read-only operation; allocates O(file size) for
// the ROADMAP bullet-count pass. See spec § Scan logic for the
// probe ordering and behaviour-when-absent rules.
LayoutEnvelope scanLayout(const QString &absoluteCwd);

// Mechanical case-mapping: camelCase struct fields ↔
// snake_case JSON keys, 1:1, no renames or reshaping.
QJsonObject    toJson(const LayoutEnvelope &env);
LayoutEnvelope fromJson(const QJsonObject &obj);

// True when the cache is missing, has expired (`nowMs -
// scannedAtMs > ttlDays * 86_400_000`), or any probed path's
// mtime is newer than `cached.scannedAtMs`. Directory probes
// use the directory's own mtime (entry-level change), not a
// recursive scan — see spec § Scan logic.
bool isStale(const LayoutEnvelope &cached, qint64 nowMs);

}  // namespace ProjectLayoutEngine
