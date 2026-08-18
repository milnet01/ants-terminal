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
//   v4 = ANTS-1880 widened to docs/{,private/,internal/,fork/}phases (phasesDir field) — per-phase design docs (Vestige and similar projects keep phase_<NN>_<topic>_design.md outside docs/specs/)
//   v5 = ANTS-1903 — body-scan fallback resolves the false-unknown case (a long preamble + first-bullet beyond the 4 KB sniff budget); invalidates pre-1903 caches that returned format:"unknown" + bullet_count_estimate:0 on a clearly-structured file
//   v6 = ANTS-2138 — standardsFiles now enumerates the *.md INSIDE a resolved canonical standards/ dir (previously populated only by the no-canonical-dir name-glob fallback, so a project with docs/standards/ reported standards_files:[]); invalidates pre-2138 caches that stored the empty list
//   v7 = ANTS-4439 — the AppStream probe was single-level, so a project packaging for more than one distro (packaging/obs/, packaging/flatpak/) reported appstream_metainfo:"", indistinguishable from shipping no AppStream metadata at all. Widened to one level under packaging/ and pkg/, and to the legacy *.appdata.xml spelling (probed after every *.metainfo.xml candidate, so the current name still wins). Same widening-needs-a-bump case ANTS-1620 already paid for once on the ANTS-1493 set: without it, cached envelopes keep the empty answer and the narrow probed_paths[] echo until 7-day TTL expiry
constexpr int kProbeSetVersion  = 7;

// ANTS-1903 — per-branch trace of the format sniffer's decision.
// Surfaces which branches scored a hit vs miss on each pass so a
// failing project ("format:unknown" on a clearly-structured file)
// can be diagnosed from the envelope alone without round-tripping
// to instrumented binaries. headBytesScanned / fullScan flags echo
// the budget so the caller can tell whether the head was too small
// (pre-1903 4 KB cap missed a long preamble + first bullet) or the
// file genuinely doesn't carry any of the recognised shapes.
struct RoadmapSnifferTrace {
    bool   markerHit         = false;  // <!-- ants-roadmap-format: 1 -->
    bool   antsV1EmojiHit    = false;  // - ✅/📋/🚧/💭
    bool   gfmTaskListHit    = false;  // - [ ]/[x]/[X]
    bool   fullScan          = false;  // true iff sniffer fell back to body
    qint64 headBytesScanned  = 0;
};

struct RoadmapInfo {
    QString  path;
    QString  format;                       // "ants-v1" | "github-task-list" | "mixed" | "unknown" | ""
    bool     formatMarkerPresent = false;
    int      bulletCountEstimate = 0;
    qint64   sizeBytes           = 0;
    qint64   mtimeMs             = 0;
    // ANTS-1903 — populated by detectFormat; emitted as
    // sniffer_branches_tried in the JSON envelope.
    RoadmapSnifferTrace snifferTrace;
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
    // ANTS-1880 — per-phase design docs (`docs/phases/` or one of
    // its private/internal/fork variants). Empty when no phases
    // dir is present. Same probe-order convention as specsDir.
    QString        phasesDir;
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
    // Project-relative paths of standards documents. Two mutually
    // exclusive sources (canonical dir wins):
    //  - ANTS-2138 — when a canonical standards/ dir resolves
    //    (`standardsDir` non-empty), every *.md directly inside it is
    //    enumerated here (sorted, no name/min-lines filter — every file
    //    in a dedicated standards dir IS a standard). These are NOT
    //    re-added to `discovered[]` (the dir itself already is).
    //  - ANTS-1574 — when docs/standards/ is ABSENT, top-level docs/*.md
    //    files whose names match STANDARD|DESIGN|STYLE|GUIDE (case-
    //    insensitive, >= 100 lines) populate this list as a fallback;
    //    those fallback hits ARE folded into `discovered[]` too.
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
