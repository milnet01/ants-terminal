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

// ANTS-1620 — probe-set schema version. Bump whenever the
// candidate path list (`kRoadmapCandidates`, `kChangelogCandidates`,
// `kSpecsCandidates`, etc.) widens — old envelopes whose
// `probedPaths[]` echo doesn't include the new candidates
// otherwise stay valid until TTL (7 days), surfacing a stale
// `probed_paths[]` contract back to callers. `isStale` invalidates
// any cache whose stored `probeSetVersion` is below this constant.
//
// History:
//   v1 = initial ANTS-1430 set (root-level ROADMAP/CHANGELOG/docs/{specs,standards,decisions}, packaging .metainfo.xml, .roadmap-counter)
//   v2 = ANTS-1493 widened to docs/{private,internal,fork}/ + data/changelog.yaml + standards name-glob fallback
//   v3 = ANTS-1632 format-sniffer recognises "mixed" (GFM task-list + ants-v1 emoji bullets in the same file) and `bullet_count_estimate` counts the union — invalidates pre-1632 caches that returned `format:"unknown"` + `bullet_count_estimate:0` on the same on-disk file
constexpr int kProbeSetVersion  = 3;

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
    // ANTS-1574 — when docs/standards/ is absent, top-level docs/*.md
    // files whose names match STANDARD|DESIGN|STYLE|GUIDE (case-
    // insensitive, >= 100 lines) populate this list. standards_dir
    // intentionally stays empty in that case — its contract is "the
    // directory that holds standards", not "a single standards file".
    // The fallback files are also folded into `discovered[]` so
    // callers that scan that list pick them up uniformly.
    QStringList    standardsFiles;
    // ANTS-1620 — schema version of the probe set that produced
    // `probedPaths[]`. `isStale` returns true when this is less
    // than `kProbeSetVersion` so cached envelopes from before a
    // probe-set widening don't surface a stale echo. Defaults to
    // 0 on fromJson when the field is missing (pre-ANTS-1620
    // caches) — also < kProbeSetVersion, so they invalidate.
    int            probeSetVersion = kProbeSetVersion;
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
