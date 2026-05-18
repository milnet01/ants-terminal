// ANTS-1555 — per-project `.audit_cache/` infrastructure for
// `audit_run`. Routes the MCP runner's SARIF + manifest into the
// same per-project directory the AuditDialog GUI already uses, so
// `last_audit_summary` discovers MCP sweeps, and ANTS-1504
// (`since-last-run` mode) has a stable prior-run anchor.
//
// Layout / invariants documented in `docs/specs/ANTS-1555.md`.

#pragma once

#include <QJsonObject>
#include <QString>

namespace AuditCache {

// Compute `<canonProject>/.audit_cache`. Does not create the dir;
// callers that intend to write through `recordRun` will mkpath as
// part of the recording flow.
QString cacheDir(const QString &canonProject);

// Compute the absolute SARIF path for a new run. `isoBasename` and
// `gitShaShort` are the manifest fields (§ 2.1 of spec). Returns
// `<cacheDir>/audit-<iso>-<sha>.sarif`. Caller checks for empty
// project root before invoking.
QString sarifPathFor(const QString &canonProject,
                     const QString &isoBasename,
                     const QString &gitShaShort);
QString htmlPathFor(const QString &canonProject,
                    const QString &isoBasename,
                    const QString &gitShaShort);

// ISO-UTC timestamp in filesystem-safe form (`yyyy-MM-ddTHH-mm-ssZ`).
// Two distinct fields are returned for the manifest:
//   - `forFilename` — colons replaced with hyphens, used in path.
//   - `forManifest` — canonical ISO-8601 with colons, used in JSON.
struct IsoNow {
    QString forFilename;
    QString forManifest;
};
IsoNow isoNow();

// Resolve the short git SHA (`rev-parse --short HEAD`) for the
// project, or the literal `"nogit"` when not a git repo / git
// missing. Branch returned as a side channel; empty when not
// resolvable.
struct GitInfo {
    QString shortSha;
    QString branch;
};
GitInfo gitInfo(const QString &canonProject);

// Manifest schema (`.audit_cache/index.json`).
//
// `lastRun` and `history` mirror the JSON shape verbatim; loaders
// treat any unrecognised top-level `version` (≠ 1) as a fresh
// manifest (returns an empty `Manifest`).
struct Manifest {
    int          version = 1;
    QJsonObject  lastRun;     // empty when none
    // history is stored as a JSON array on disk (newest-first).
    // Returned here as a QJsonArray-shaped value inside the object
    // for convenience (callers iterate via QJsonArray::at).
    QJsonObject  raw;         // entire parsed top-level object
};

// Read `<root>/.audit_cache/index.json`. Missing / unparseable /
// wrong-version files all yield an empty Manifest{} with the
// `raw` object empty.
Manifest loadManifest(const QString &canonProject);

// Record a completed run. Writes the SARIF + manifest atomically:
//   1. Compute iso + git short-sha.
//   2. Build the new `last_run` JSON object from `byTool` + scope.
//   3. Migrate prior `last_run` into the head of `history[]`.
//   4. Trim `history[]` to 10 entries (reaper deletes dropped
//      files matching its own naming convention — never AuditDialog
//      GUI files).
//   5. QSaveFile + commit on `index.json` with 0600 perms.
//
// Returns true on success. On any failure (mkdir denied, save
// failure), returns false; the caller's RunResult should keep
// `cachePath` empty + retain its `/tmp` fallback for `sarifPath`.
//
// `priorRunOut` is filled with the manifest's pre-existing
// `last_run` (so the caller can surface it as `RunResult.priorRun`).
struct RecordedRun {
    QString iso;          // canonical, with colons
    QString sha;          // 7-char short OR "nogit"
    QString sarifPath;    // absolute
    QString htmlPath;     // empty when no html
    bool    ok = false;
};
RecordedRun recordRun(const QString &canonProject,
                      const QJsonObject &lastRunJson,
                      QJsonObject *priorRunOut);

}  // namespace AuditCache
