// ANTS-6221 — the one builder for audit_run's finished-run fields. The
// synchronous reply (mcptoolregistry.cpp) and the async job the audit_poll
// reply reads (ClaudeIntegration::auditJobPollEnvelope) both take their fields
// from here, so the two cannot drift apart again.

#include "auditrunner.h"

#include <QJsonArray>
#include <QJsonObject>

namespace AuditRunner {

namespace {

QJsonArray toArray(const QStringList &list) {
    QJsonArray out;
    for (const QString &s : list) out.append(s);
    return out;
}

}  // namespace

namespace internal {

QJsonObject byToolObject(const QHash<QString, ToolResult> &byTool) {
    QJsonObject out;
    for (auto it = byTool.constBegin(); it != byTool.constEnd(); ++it) {
        QJsonObject t;
        t["status"]             = it->status;
        t["elapsed_ms"]         = it->elapsedMs;
        t["raw_count"]          = it->rawCount;
        t["after_filter_count"] = it->afterFilterCount;
        t["samples"]            = it->samples;
        // ANTS-4371 — evidence the tool was handed work. A zero-finding audit
        // is what lets a phase close, and "ran across the tree and found
        // nothing" was byte-identical to "ran against an empty file list".
        // Under scope:"full" a tool is pointed at the root, so paths_given is
        // legitimately 0 and scanned_whole_project says why. `no_files` is a
        // NARROWED scope that matched nothing.
        t["paths_given"]           = it->pathsGiven;
        t["scanned_whole_project"] = it->wholeProject;
        if (!it->wholeProject && it->pathsGiven == 0) t["no_files"] = true;
        out[it.key()] = t;
    }
    return out;
}

}  // namespace internal

QJsonObject doneEnvelope(const RunResult &r) {
    QJsonObject env;
    env["by_tool"] = internal::byToolObject(r.byTool);
    // ANTS-4371 — the top-level roll-up, so a caller reading only the summary
    // can tell a real sweep from an empty one without walking by_tool.
    // Deliberately NOT folded into `partial` / `incomplete_tools`: a narrowed
    // scope matching no files is a legitimate outcome, and marking it partial
    // would make every narrow scan report a failure it did not have.
    {
        QJsonArray noFilesTools;
        int pathsTotal = 0;
        bool anyWholeProject = false;
        for (auto it = r.byTool.constBegin(); it != r.byTool.constEnd(); ++it) {
            pathsTotal += it->pathsGiven;
            if (it->wholeProject) anyWholeProject = true;
            else if (it->pathsGiven == 0) noFilesTools.append(it.key());
        }
        env["paths_given_total"]     = pathsTotal;
        env["scanned_whole_project"] = anyWholeProject;
        if (!noFilesTools.isEmpty()) env["tools_with_no_files"] = noFilesTools;
    }
    env["total_raw"]        = r.totalRaw;
    env["total_actionable"] = r.totalActionable;
    env["noise_rate_pct"]   = r.noiseRatePct;
    // ANTS-2032 — true when a tool timed out or crashed but the rest of the
    // run still produced results; `incomplete_tools` names the offenders.
    env["partial"] = r.partial;
    if (!r.incompleteTools.isEmpty())
        env["incomplete_tools"] = toArray(r.incompleteTools);
    // ANTS-3585 — why each tool is incomplete, and the source files a tool
    // could not parse (zero coverage). Both omitted when empty.
    if (!r.incompleteToolsDetail.isEmpty())
        env["incomplete_tools_detail"] = r.incompleteToolsDetail;
    if (!r.parseFailures.isEmpty()) {
        env["parse_failures"] = toArray(r.parseFailures);
        // ANTS-3706 — why each file failed, so a missing include path
        // (fixable) is distinguishable from a frontend limitation.
        if (!r.parseFailuresDetail.isEmpty())
            env["parse_failures_detail"] = r.parseFailuresDetail;
    }
    if (!r.sarifPath.isEmpty()) env["sarif_path"] = r.sarifPath;
    // ANTS-6981 — why no sarif_path came back; omitted when a write landed.
    if (!r.sarifError.isEmpty()) env["sarif_error"] = r.sarifError;
    if (!r.htmlPath.isEmpty())  env["html_path"]  = r.htmlPath;
    QJsonArray skipped;
    for (const auto &ts : r.toolsSkipped) {
        QJsonObject s;
        s["tool"]   = ts.tool;
        s["reason"] = ts.reason;
        skipped.append(s);
    }
    env["tools_skipped"]     = skipped;
    env["elapsed_total_ms"]  = r.elapsedTotalMs;
    env["samples_truncated"] = r.samplesTruncated;
    if (!r.topFindings.isEmpty()) env["top_findings"] = r.topFindings;
    // ANTS-1555 — `cache_path` is set only when the SARIF landed in
    // `<root>/.audit_cache/`; `prior_run` is the manifest's previous last_run
    // snapshot (absent on a project's first sweep).
    if (!r.cachePath.isEmpty()) env["cache_path"] = r.cachePath;
    if (!r.priorRun.isEmpty())  env["prior_run"]  = r.priorRun;
    // ANTS-1504 — narrowing-scope surface.
    if (!r.scopeResolved.isEmpty()) {
        env["scope_resolved"]      = r.scopeResolved;
        env["changed_files_count"] = r.changedFilesCount;
    }
    if (!r.scopeAnchorCommit.isEmpty())
        env["scope_anchor_commit"] = r.scopeAnchorCommit;
    if (!r.scopeDemoted.isEmpty()) {
        env["scope_demoted"]        = r.scopeDemoted;
        env["scope_demoted_reason"] = r.scopeDemotedReason;
    }
    if (r.noChanges) env["no_changes"] = true;
    // ANTS-3710 — echo the applied exclusions, and name the tools that could
    // not honour them: a silent partial exclusion would read as a complete one.
    if (!r.excludePathsApplied.isEmpty()) {
        env["exclude_paths_applied"] = toArray(r.excludePathsApplied);
        if (!r.excludePathsIgnoredBy.isEmpty())
            env["exclude_paths_ignored_by"] = toArray(r.excludePathsIgnoredBy);
    }
    // ANTS-1870 — `delta` and `delta_unavailable_reason` are mutually
    // exclusive; `findings_truncated` flags a run that hit the per-tool
    // finding ceiling.
    if (!r.delta.isEmpty()) env["delta"] = r.delta;
    if (!r.deltaUnavailableReason.isEmpty())
        env["delta_unavailable_reason"] = r.deltaUnavailableReason;
    if (r.findingsTruncated) env["findings_truncated"] = true;
    // ANTS-1706 — findings new since the pinned baseline, and the baseline
    // this run saved. Each absent when empty.
    if (!r.sinceBaseline.isEmpty()) env["since_baseline"] = r.sinceBaseline;
    if (!r.baselineSaved.isEmpty()) env["baseline_saved"] = r.baselineSaved;
    if (r.minConfidence > 0)
        env["below_confidence_floor_count"] = r.belowConfidenceFloor;
    return env;
}

}  // namespace AuditRunner
