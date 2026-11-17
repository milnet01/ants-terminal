// ANTS-1555 — per-project `.audit_cache/` infrastructure for
// `audit_run`. Routes the MCP runner's SARIF + manifest into the
// same per-project directory the AuditDialog GUI already uses, so
// `last_audit_summary` discovers MCP sweeps, and ANTS-1504
// (`since-last-run` mode) has a stable prior-run anchor.
//
// Layout / invariants documented in `docs/specs/ANTS-1555-per-project-audit-cache-infrastructure.md`.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

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
// `raw` object empty. A wrong-version file's `version` is kept, so
// recordRun can leave a newer manifest alone (ANTS-6481).
Manifest loadManifest(const QString &canonProject);

// Record a completed run into the manifest. The caller builds `lastRunJson`
// (AuditCache::isoNow, AuditCache::gitInfo) and writes the SARIF itself.
// This function:
//   1. Writes the findings sidecar (below) when there are findings.
//   2. Migrates the prior `last_run` into the head of `history[]`.
//   3. Trims `history[]` to 10 entries (reaper deletes dropped
//      files matching its own naming convention — never AuditDialog
//      GUI files; covers sarif/html AND the ANTS-1870 findings sidecar).
//   4. QSaveFile + commit on `index.json` with 0600 perms.
//
// Returns a RecordedRun with `ok==true` on success. On any failure
// (mkdir denied, save failure), `ok` is false; the caller's RunResult
// should keep `cachePath` empty + retain its `/tmp` fallback for
// `sarifPath`.
//
// `priorRunOut` is filled with the manifest's pre-existing
// `last_run` (so the caller can surface it as `RunResult.priorRun`).
//
// ANTS-1870 — when `mergedFindings` is non-empty, a sibling findings
// sidecar `findings-<iso>-<sha>.json` (version 1) is written in the same
// atomic flow and its basename recorded as `last_run.findings_file`; the
// sidecar's `truncated` flag mirrors `last_run.findings_truncated`. The
// default (empty) preserves the no-findings back-compat path. The param
// trails `priorRunOut` because a defaulted parameter cannot precede a
// non-defaulted one.
struct RecordedRun {
    QString iso;          // canonical, with colons
    QString sha;          // 7-char short OR "nogit"
    QString sarifPath;    // absolute
    QString htmlPath;     // empty when no html
    bool    ok = false;
};
RecordedRun recordRun(const QString &canonProject,
                      const QJsonObject &lastRunJson,
                      QJsonObject *priorRunOut,
                      const QJsonArray &mergedFindings = {});

// ANTS-1870 — read a findings sidecar by basename (resolved under
// `<canonProject>/.audit_cache/`). `present` is set when the referenced
// file exists; `valid` when it parsed as a version-1 sidecar; `truncated`
// mirrors the sidecar's flag. A missing / unparseable / wrong-version
// sidecar yields `{present, valid:false, findings:{}}` (mirrors the
// manifest forward-compat rule). The caller distinguishes
// `no_prior_findings` (basename empty) from `prior_findings_unreadable`
// (referenced but `!valid`).
struct SidecarLoad {
    bool       present = false;
    bool       valid = false;
    bool       truncated = false;
    QJsonArray findings;
    QString    commit;         // as recorded; empty when absent
    QString    isoTimestamp;
    QStringList tools;         // ANTS-1706 — the baseline's; a run sidecar has none
};
SidecarLoad readFindingsSidecar(const QString &canonProject,
                                const QString &basename);
// Convenience: the `findings[]` array only (empty on any failure).
QJsonArray loadFindingsSidecar(const QString &canonProject,
                               const QString &basename);

// ANTS-1706 — the pinned baseline `audit_run` reports new findings against:
// `<cacheDir>/baseline-findings.json`, a version-1 findings sidecar plus a
// `tools` list naming the tools whose findings it holds. Read it with
// readFindingsSidecar(canonProject, kBaselineBasename). It is not the audit
// dialog's `baseline.json`, which holds line-sensitive dedup keys.
inline constexpr const char *kBaselineBasename = "baseline-findings.json";

// Write the baseline owner-only, replacing any earlier one. Returns its
// absolute path, or an empty string when the write failed.
QString writeBaseline(const QString &canonProject, const QJsonArray &findings,
                      const QStringList &tools, bool truncated);

}  // namespace AuditCache
