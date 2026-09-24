// ANTS-4932 § 2.3 — the project-scoped MCP verb registrations, moved here
// from MainWindow::setupClaudeMcpProviders(). See mcptoolregistry.h.
//
// Every handler here reads no tab or terminal state. One that needs to has
// to be registered by the terminal instead and named in
// terminalScopedVerbNames() — INV-2 and INV-4 check both halves.

#include "mcptoolregistry.h"

#include "auditrunner.h"      // ANTS-1351 — server-side audit runner
#include "claudeintegration.h"
#include "guithread.h"
#include "remotecontrol.h"
#include "rootprovider.h"
#include "testauditengine.h"  // ANTS-1397 — test_audit_* trio engine
#ifdef ANTS_LUA_PLUGINS
#include "luaengine.h"        // ANTS-2093 — project_query
#endif

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QThread>

namespace mcp {

namespace {

const char *const kRcUnavailable = ClaudeIntegration::kMcpRcUnavailable;

// ANTS-1833 — resolve+validate the caller_cwd that the inline audit_run /
// indie_review_dispatch handlers use as their in-flight-gate key. A
// non-existent root canonicalises to "" and would collapse every such
// call onto one shared key (one bogus caller blocks real sweeps); the
// dispatcher only enforces non-empty, not is-a-directory. On reject the
// `errOut` is a ready-to-return bad_cwd envelope. Returns true (and
// fills canonOut) only for an existing directory.
bool resolveInflightCallerCwd(const QString &callerCwd, const char *tool,
                              QString *canonOut, QString *errOut) {
    const QString canon = QFileInfo(callerCwd).canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        // `bad_cwd` per docs/standards/mcp-error-codes.md — the precise
        // code for "caller_cwd does not exist or isn't a directory".
        env[QStringLiteral("code")]  = QStringLiteral("bad_cwd");
        env[QStringLiteral("error")] =
            QStringLiteral("%1: \"caller_cwd\" is not an existing directory")
                .arg(QLatin1String(tool));
        *errOut = QString::fromUtf8(
            QJsonDocument(env).toJson(QJsonDocument::Compact));
        return false;
    }
    *canonOut = canon;
    return true;
}

}  // namespace

RcHandler rcDelegate(RemoteControlGetter rc,
                     QJsonDocument (RemoteControl::*fn)(const QJsonObject &),
                     DispatchLane lane) {
    return RcHandler{[rc = std::move(rc), fn](const QJsonObject &args) -> QString {
        RemoteControl *r = rc ? rc() : nullptr;
        if (!r) return QString::fromUtf8(kRcUnavailable);
        return QString::fromUtf8((r->*fn)(args).toJson(QJsonDocument::Compact));
    }, true, lane};
}

QString sourceToString(ants::ResolvedRoot::Source s) {
    using S = ants::ResolvedRoot::Source;
    switch (s) {
        case S::ExplicitMatch: return QStringLiteral("ExplicitMatch");
        case S::EmptyFallback: return QStringLiteral("EmptyFallback");
        case S::NoMatch:       return QStringLiteral("NoMatch");
        case S::Unresolvable:  return QStringLiteral("Unresolvable");
        case S::ServerCwd:     return QStringLiteral("ServerCwd");
    }
    return QStringLiteral("Unresolvable");  // -Wreturn-type
}

const QVector<QString> &terminalScopedVerbNames() {
    // § 2.3 — every handler that reads tab or terminal state. The terminal
    // registers all but get_session_info, which its pipeline answers inline.
    static const QVector<QString> names{
        QStringLiteral("get_cwd"),
        QStringLiteral("get_environment"),
        QStringLiteral("get_git_status"),
        QStringLiteral("get_last_command"),
        QStringLiteral("get_scrollback"),
        QStringLiteral("get_session_info"),
        QStringLiteral("get_text"),
        QStringLiteral("last_selection"),
        QStringLiteral("recent_errors"),
        QStringLiteral("tab_list"),
        QStringLiteral("token_usage"),
    };
    return names;
}

void registerProjectScopedVerbs(ToolSink &sink, RemoteControlGetter rc,
                                RegistryHost host) {

    // ANTS-1636 — find_sources. Project-scoped topic-to-files
    // discovery; reads under <caller_cwd>/src + <caller_cwd>/tests.
    // Required contract — refuses without caller_cwd at the dispatcher.
    sink.registerToolProvider("find_sources",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFindSources));

    // ANTS-3368 — co_change_family: every edit site of one settings field.
    sink.registerToolProvider("co_change_family",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdCoChangeFamily));

    // ANTS-1244 surface — the next 7 tools delegate to RemoteControl
    // cmd handlers so the IPC and MCP transports share verb logic.
    // ANTS-3422 — roadmap_query forwards `args` VERBATIM via the
    // shared rcDelegate factory (which passes the whole args object
    // straight to the cmd handler). This retires the hand-maintained
    // per-arg forward allowlist that silently dropped any new
    // verb-specific arg at the MCP boundary — the exact bug that
    // recurred five times (ANTS-1856 id / ANTS-1398
    // include_section_headers / ANTS-1437 mode / ANTS-1586
    // include_body / ANTS-3420 query + max_body_bytes /
    // include_section_etags / section_etag_match). cmdRoadmapQuery
    // already owns every arg's validation and reads each key
    // defensively (empty status→"all", empty section→full-file path,
    // empty/absent id/ids→list path, non-numeric offset/limit→bad_args),
    // so a verbatim forward is behaviour-preserving and no future
    // schema arg can be dropped here again.
    sink.registerToolProvider("roadmap_query",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdRoadmapQuery));
    // ANTS-1424 — roadmap_log: append a new bullet to ROADMAP.md.
    // Required-contract gated at the dispatcher (ANTS-1404), so
    // absent caller_cwd refuses upstream before this lambda runs.
    sink.registerToolProvider("roadmap_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdRoadmapLog));
    // ANTS-3855 — roadmap_migrate: the only production entry point into the
    // migration engine. Write op → Required contract (refuses absent
    // caller_cwd upstream, before the handler runs).
    sink.registerToolProvider("roadmap_migrate",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdRoadmapMigrate, ClaudeIntegration::DispatchLane::Bulk));
    // ANTS-4622 — session_message: the cross-session mailbox. Required
    // contract on every op, read included: `inbox` and `ack` resolve the
    // CALLING project from caller_cwd, so an absent one has no mailbox to
    // read rather than a default one.
    sink.registerToolProvider("session_message",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSessionMessage));
    // ANTS-5299 — run_trace: a review run records its trace-index row.
    // Required on every op, get included: the index lives under the
    // caller's project, and there is no default project to read.
    sink.registerToolProvider("run_trace",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdRunTrace));
    // ANTS-1548 — changelog_log: token-frugal Keep-a-Changelog writer.
    // Write op → Required contract (refuses absent caller_cwd upstream).
    sink.registerToolProvider("changelog_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdChangelogLog));
    // ANTS-3533 — changelog_query: read-only structured CHANGELOG reader,
    // the symmetric read side of changelog_log. Required contract (read
    // verb, ANTS-1520). Opts into fields=/compact/etag/offload allowlists.
    sink.registerToolProvider("changelog_query",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdChangelogQuery));
    // ANTS-1583 — roadmap_branch_drift: compare ROADMAP ✅ entries'
    // cited commit SHAs against HEAD's reachable history. caller_cwd
    // is Required (ANTS-1404 contract registered below).
    sink.registerToolProvider("roadmap_branch_drift",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdRoadmapBranchDrift));
    // ANTS-1351 — audit_run server-side runner. Inline in-flight gate
    // via ClaudeIntegration::verbInFlight* (§ 2.4 of v4 spec) — no
    // class abstraction; two consumers (this verb + indie_review_dispatch,
    // ANTS-1352) don't justify the helper class. ANTS-1397's
    // test_audit_partition was a designed third consumer but was never
    // wired — see that spec's INV-12 (corrected 2026-07-25).
    // caller_cwd is Required (ANTS-1404 contract registered in
    // callerCwdContractFor); dispatcher refuses upstream when absent.
    // ANTS-4682 — STAYS inline, deliberately (ANTS-2132 § 5). Its job registry
    // is GUI-thread state and it builds its own worker, and the sweep touches
    // the tree: the § 5 hazard is NOT closed for this verb.
    // ANTS-2132 § 2.8 — it registers as a DeferredToolHandler, so the
    // synchronous branch replies from a completion slot instead of joining the
    // sweep on the GUI thread (ANTS-5035).
    sink.registerToolProvider("audit_run",
        ClaudeIntegration::CallerCwdContract::Required,
        [=](const QJsonObject &args, std::function<void(QString)> reply) {
            const QString callerCwd = args.value(
                QStringLiteral("caller_cwd")).toString();
            // ANTS-1833 — reject a non-existent root before it can
            // collapse the in-flight key (canon would be empty).
            QString canon, badEnv;
            if (!resolveInflightCallerCwd(callerCwd, "audit_run",
                                          &canon, &badEnv)) {
                reply(badEnv);
                return;
            }
            // In-flight gate (INV-11).
            const qint64 existing =
                host.ci->verbInFlightTryAcquire(
                    QStringLiteral("audit_run"), canon);
            if (existing >= 0) {
                QJsonObject env;
                env["ok"]               = false;
                env["code"]             = QStringLiteral("already_running");
                env["error"]            = QStringLiteral(
                    "audit_run: a sweep is already in flight for this "
                    "project root; retry after it completes");
                env["running_since_ms"] =
                    QDateTime::currentMSecsSinceEpoch() - existing;
                env["retry_after_ms"]   = 5000;  // INV-9 hint
                reply(QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact)));
                return;
            }
            // RAII: release the in-flight slot on EVERY exit path (including
            // an exception or a future early return), honouring the header's
            // documented guard contract. indie-review-2026-05-21.
            // Non-const: the ANTS-3396 async branch calls dismiss() to hand
            // the slot release to the completion slot.
            auto inFlightGuard = qScopeGuard([&, ci = host.ci] {
                ci->verbInFlightRelease(
                    QStringLiteral("audit_run"), canon);
            });
            // Build the engine request.
            AuditRunner::RunRequest req;
            req.projectRoot = callerCwd;
            const QJsonArray toolsArr = args.value(
                QStringLiteral("tools")).toArray();
            for (const QJsonValue &v : toolsArr)
                req.tools.append(v.toString());
            req.scope = args.value(QStringLiteral("scope")).toString();
            if (args.value(QStringLiteral("cap_per_tool_seconds"))
                    .isDouble()) {
                req.capPerToolSeconds = args.value(
                    QStringLiteral("cap_per_tool_seconds")).toInt();
            }
            req.suppressionsMode =
                args.value(QStringLiteral("suppressions")).toString();
            const QJsonArray formatsArr = args.value(
                QStringLiteral("formats")).toArray();
            for (const QJsonValue &v : formatsArr)
                req.formats.append(v.toString());
            if (args.value(QStringLiteral("top_findings_count"))
                    .isDouble()) {
                req.topFindingsCount = args.value(
                    QStringLiteral("top_findings_count")).toInt();
            }
            // ANTS-1512 — scoped-check mode: narrow the tool's scope
            // to specific paths and/or a specific check set.
            const QJsonArray pathsArr = args.value(
                QStringLiteral("paths")).toArray();
            for (const QJsonValue &v : pathsArr)
                req.paths.append(v.toString());
            const QJsonArray checksArr = args.value(
                QStringLiteral("checks")).toArray();
            for (const QJsonValue &v : checksArr)
                req.checks.append(v.toString());
            // ANTS-3710 — the negative counterpart to `paths`: drop code
            // that is present but not ours (a vendored dependency tree)
            // without also dropping the repo-global tools the way a
            // narrowing `paths` does.
            const QJsonArray exclArr = args.value(
                QStringLiteral("exclude_paths")).toArray();
            for (const QJsonValue &v : exclArr)
                req.excludePaths.append(v.toString());
            // ANTS-3396 — opt-in async mode: spawn the sweep detached,
            // register a job, and return a handle immediately so the caller
            // never blocks on the ~60 s MCP transport cap. Default false →
            // the synchronous branch below, which replies with the full
            // result once the sweep ends (ANTS-2132 § 2.8).
            if (args.value(QStringLiteral("async")).toBool()) {
                const qint64 startedMs =
                    QDateTime::currentMSecsSinceEpoch();
                const QString jobId =
                    host.ci->auditJobRegister(canon, startedMs);
                if (jobId.isEmpty()) {
                    // Registry saturated (all entries running, none
                    // evictable). The in-flight guard releases the slot on
                    // return; the sync path stays available (INV-8).
                    QJsonObject env;
                    env["ok"]             = false;
                    env["code"]           = QStringLiteral("too_many_jobs");
                    env["error"]          = QStringLiteral(
                        "audit_run: too many audit jobs in flight; retry "
                        "shortly or run synchronously (async:false)");
                    env["retry_after_ms"] = 5000;
                    reply(QString::fromUtf8(
                        QJsonDocument(env).toJson(QJsonDocument::Compact)));
                    return;
                }
                // The completion slot now owns the in-flight release
                // (§2.4 / INV-5) — dismiss the sync scope-guard so the slot
                // is NOT freed the moment this handler returns the handle.
                inFlightGuard.dismiss();
                auto result =
                    std::make_shared<AuditRunner::RunResult>();
                // Worker created on the main/dispatch thread → its thread
                // affinity is the main thread, so its own deleteLater() runs
                // there. `req` copied by value
                // (the handler's stack frame unwinds before the sweep ends).
                QThread *worker = QThread::create(
                    [req, result]() { *result = AuditRunner::runAudit(req); });
                ClaudeIntegration *ci = host.ci;
                // ANTS-5080 — the worker frees itself. The completion below
                // has ci as its context and is severed if ci is destroyed
                // mid-sweep, so freeing the worker there could leave it never
                // freed. The completion no longer touches the worker, so the
                // two queued events may run in either order.
                QObject::connect(worker, &QThread::finished, worker,
                                 &QObject::deleteLater);
                // Queued completion on the main thread. Context object = ci
                // so the connection auto-severs if ci is destroyed at
                // teardown (the QPointer-equivalent shutdown guard, §2.4):
                // a still-running worker then fires into nothing rather than
                // touching a freed owner. The worker touches only its own
                // RunResult; the registry flip + slot release happen here on
                // the main thread.
                QObject::connect(worker, &QThread::finished, ci,
                    [ci, result, jobId, canon]() {
                        ClaudeIntegration::AuditJob term;
                        const AuditRunner::RunResult &r = *result;
                        if (!r.ok) {
                            term.status = QStringLiteral("error");
                            term.code   = r.code;
                            term.error  = r.error;
                        } else {
                            term.status = QStringLiteral("done");
                            // cache-write failure → fall back to the /tmp
                            // SARIF so the recovery path never breaks.
                            term.cachePath = r.cachePath.isEmpty()
                                ? r.sarifPath : r.cachePath;
                            term.totalRaw        = r.totalRaw;
                            term.totalActionable = r.totalActionable;
                            term.partial         = r.partial;
                            term.noChanges       = r.noChanges;
                            term.incompleteTools = r.incompleteTools;
                            // ANTS-3585 — carry the richer surfaces so the
                            // async-poll done-branch emits them too.
                            term.incompleteToolsDetail = r.incompleteToolsDetail;
                            term.parseFailures         = r.parseFailures;
                            // ANTS-3706 — and the parse-failure reasons.
                            term.parseFailuresDetail   = r.parseFailuresDetail;
                        }
                        ci->auditJobComplete(jobId, term);
                        ci->verbInFlightRelease(
                            QStringLiteral("audit_run"), canon);
                    }, Qt::QueuedConnection);
                worker->start();
                QJsonObject env;
                env["ok"]            = true;
                env["async"]         = true;
                env["job_id"]        = jobId;
                env["status"]        = QStringLiteral("running");
                env["started_at_ms"] = startedMs;
                env["poll_with"]     = QStringLiteral("audit_poll");
                env["note"]          = QStringLiteral(
                    "Sweep running server-side; poll audit_poll {job_id} or "
                    "read last_audit_summary when done. Results are written "
                    "to .audit_cache regardless of poll.");
                reply(QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact)));
                return;
            }
            // ANTS-2103 — run the audit on a worker thread so its internal
            // QEventLoop (auditrunner.cpp), which multiplexes the per-tool
            // QProcesses, lives OFF the main thread. Running it synchronously
            // here spun that nested QEventLoop on the GUI/MCP thread, which
            // reentrantly delivered QLocalSocket read-notifications and freed
            // the live MCP socket mid-dispatch -> use-after-free SIGSEGV (the
            // ANTS-2101 write-path guard was necessary but not sufficient; the
            // deeper hazard is pumping the main event loop at all). This
            // realises the INV-9 worker-thread isolation auditrunner.h already
            // documents.
            // ANTS-2132 § 2.8 — and the GUI thread does not join it. The
            // completion slot builds the envelope and replies, so the window
            // keeps painting and MCP traffic keeps flowing for the whole sweep.
            // A second synchronous call for this root now arrives while the
            // sweep runs and is refused already_running by the gate above.
            inFlightGuard.dismiss();
            auto result = std::make_shared<AuditRunner::RunResult>();
            QThread *worker = QThread::create(
                [req, result]() { *result = AuditRunner::runAudit(req); });
            ClaudeIntegration *ci = host.ci;
            // Context object = ci, so a sweep still running at teardown
            // replies into nothing, as the async branch's slot does.
            QObject::connect(worker, &QThread::finished, ci,
                [ci, worker, result, canon, reply]() {
                    ci->verbInFlightRelease(QStringLiteral("audit_run"), canon);
                    worker->deleteLater();
                    const AuditRunner::RunResult &r = *result;
                    // Serialise envelope.
                    QJsonObject env;
                    if (!r.ok) {
                        env["ok"]    = false;
                        env["code"]  = r.code;
                        env["error"] = r.error;
                        // ANTS-3612 — the aggregate concurrency cap is transient,
                        // so give the caller the same backoff hint the other
                        // busy-style refusals carry (already_running,
                        // too_many_jobs). Every other engine refusal is a hard
                        // input error and gets no retry hint.
                        if (r.code == QLatin1String("server_busy"))
                            env["retry_after_ms"] = 5000;
                        reply(QString::fromUtf8(
                            QJsonDocument(env).toJson(QJsonDocument::Compact)));
                        return;
                    }
                    env["ok"] = true;
                    QJsonObject byTool;
                    for (auto it = r.byTool.constBegin();
                         it != r.byTool.constEnd(); ++it) {
                        QJsonObject t;
                        t["status"]              = it->status;
                        t["elapsed_ms"]          = it->elapsedMs;
                        t["raw_count"]           = it->rawCount;
                        t["after_filter_count"]  = it->afterFilterCount;
                        t["samples"]             = it->samples;
                        // ANTS-4371 — evidence the tool was handed work. A zero-finding
                        // audit is the most consequential result this verb returns (it
                        // is what lets a phase close), and "ran across the tree and
                        // found nothing" was byte-identical to "ran against an empty
                        // file list". `paths_given` is the explicit positional count;
                        // `scanned_whole_project` says the tool was pointed at the root
                        // instead, which under scope:"full" is the normal shape and
                        // makes paths_given legitimately 0 — so the count alone would
                        // read as "scanned nothing" for the very case this reassures
                        // about. `no_files` is the one that matters: a NARROWED scope
                        // that matched nothing, which scope:"files"/"since-last-run"
                        // produce legitimately.
                        t["paths_given"]           = it->pathsGiven;
                        t["scanned_whole_project"] = it->wholeProject;
                        const bool noFiles = !it->wholeProject && it->pathsGiven == 0;
                        if (noFiles) t["no_files"] = true;
                        byTool[it.key()]         = t;
                    }
                    env["by_tool"]          = byTool;
                    // ANTS-4371 — the top-level roll-up, so a caller reading only the
                    // summary can tell a real sweep from an empty one without walking
                    // by_tool. Deliberately NOT folded into `partial` /
                    // `incomplete_tools`: a narrowed scope matching no files is a
                    // legitimate outcome, and marking it partial would make every
                    // narrow scan report a failure it did not have.
                    {
                        QJsonArray noFilesTools;
                        int pathsTotal = 0;
                        bool anyWholeProject = false;
                        for (auto it = r.byTool.constBegin();
                             it != r.byTool.constEnd(); ++it) {
                            pathsTotal += it->pathsGiven;
                            if (it->wholeProject) anyWholeProject = true;
                            else if (it->pathsGiven == 0) noFilesTools.append(it.key());
                        }
                        env["paths_given_total"]     = pathsTotal;
                        env["scanned_whole_project"] = anyWholeProject;
                        if (!noFilesTools.isEmpty())
                            env["tools_with_no_files"] = noFilesTools;
                    }
                    env["total_raw"]        = r.totalRaw;
                    env["total_actionable"] = r.totalActionable;
                    env["noise_rate_pct"]   = r.noiseRatePct;
                    // ANTS-2032 — explicit partiality signal: true when a tool
                    // timed out / crashed but the rest of the run still produced
                    // results (and the SARIF artifact below). `incomplete_tools`
                    // lists the offenders so the caller need not scan by_tool[].
                    env["partial"]          = r.partial;
                    if (!r.incompleteTools.isEmpty()) {
                        QJsonArray inc;
                        for (const QString &t : r.incompleteTools) inc.append(t);
                        env["incomplete_tools"] = inc;
                    }
                    // ANTS-3585 — richer partiality (why each tool is incomplete:
                    // truncated vs crashed + elapsed_ms) and the zero-coverage list
                    // (source files a tool could not parse). Both omitted when empty.
                    if (!r.incompleteToolsDetail.isEmpty())
                        env["incomplete_tools_detail"] = r.incompleteToolsDetail;
                    if (!r.parseFailures.isEmpty()) {
                        QJsonArray pf;
                        for (const QString &f : r.parseFailures) pf.append(f);
                        env["parse_failures"] = pf;
                        // ANTS-3706 — why each file failed, so a missing include path
                        // (fixable) is distinguishable from a frontend limitation
                        // (route around) without re-running the tool by hand.
                        if (!r.parseFailuresDetail.isEmpty())
                            env["parse_failures_detail"] = r.parseFailuresDetail;
                    }
                    if (!r.sarifPath.isEmpty())
                        env["sarif_path"] = r.sarifPath;
                    if (!r.htmlPath.isEmpty())
                        env["html_path"] = r.htmlPath;
                    QJsonArray skipped;
                    for (const auto &ts : r.toolsSkipped) {
                        QJsonObject s;
                        s["tool"]   = ts.tool;
                        s["reason"] = ts.reason;
                        skipped.append(s);
                    }
                    env["tools_skipped"]    = skipped;
                    env["elapsed_total_ms"] = r.elapsedTotalMs;
                    env["samples_truncated"]= r.samplesTruncated;
                    if (!r.topFindings.isEmpty())
                        env["top_findings"] = r.topFindings;
                    // ANTS-1555 — per-project `.audit_cache/` surface.
                    // `cache_path` is set only when the SARIF landed in
                    // `<root>/.audit_cache/`; `prior_run` carries the
                    // pre-existing manifest's last_run snapshot (empty
                    // object on a project's first sweep). ANTS-1504 reads
                    // `prior_run.commit` as the since-last-run diff anchor
                    // (precise findings delta deferred — ANTS-1504 § 5).
                    if (!r.cachePath.isEmpty())
                        env["cache_path"] = r.cachePath;
                    if (!r.priorRun.isEmpty())
                        env["prior_run"] = r.priorRun;
                    // ANTS-1504 — narrowing-scope surface.
                    if (!r.scopeResolved.isEmpty())
                        env["scope_resolved"] = r.scopeResolved;
                    if (!r.scopeAnchorCommit.isEmpty())
                        env["scope_anchor_commit"] = r.scopeAnchorCommit;
                    if (!r.scopeResolved.isEmpty())
                        env["changed_files_count"] = r.changedFilesCount;
                    if (!r.scopeDemoted.isEmpty()) {
                        env["scope_demoted"] = r.scopeDemoted;
                        env["scope_demoted_reason"] = r.scopeDemotedReason;
                    }
                    if (r.noChanges)
                        env["no_changes"] = true;
                    // ANTS-3710 — echo the applied exclusions, and name the tools in
                    // this run that could not honour them. A silent partial exclusion
                    // would read as a complete one, which is worse than the noise.
                    if (!r.excludePathsApplied.isEmpty()) {
                        QJsonArray xp;
                        for (const QString &p : r.excludePathsApplied) xp.append(p);
                        env["exclude_paths_applied"] = xp;
                        QJsonArray ig;
                        for (const QString &t : r.excludePathsIgnoredBy) ig.append(t);
                        if (!ig.isEmpty()) env["exclude_paths_ignored_by"] = ig;
                    }
                    // ANTS-1870 — since-last-run findings delta. `delta` and
                    // `delta_unavailable_reason` are mutually exclusive; exactly one
                    // appears under a narrowed since-last-run, neither otherwise.
                    // `findings_truncated` flags a run that hit the per-tool finding
                    // ceiling (the delta is then suppressed in favour of the reason).
                    if (!r.delta.isEmpty())
                        env["delta"] = r.delta;
                    if (!r.deltaUnavailableReason.isEmpty())
                        env["delta_unavailable_reason"] = r.deltaUnavailableReason;
                    if (r.findingsTruncated)
                        env["findings_truncated"] = true;
                    reply(QString::fromUtf8(
                        QJsonDocument(env).toJson(QJsonDocument::Compact)));
                }, Qt::QueuedConnection);
            worker->start();
        });
    // ANTS-3396 — audit_poll: read the in-memory async-audit job
    // registry. Read-only, cheap, never blocks. Required caller_cwd for
    // parity with audit_run (dispatcher refuses caller_cwd_required
    // upstream); under the claude.mcp_enabled master gate.
    // ANTS-4682 — STAYS. Same GUI-thread job registry as audit_run.
    sink.registerToolProvider("audit_poll",
        ClaudeIntegration::CallerCwdContract::Required,
        [=](const QJsonObject &args) -> QString {
            const QString jobId =
                args.value(QStringLiteral("job_id")).toString();
            if (jobId.isEmpty()) {
                QJsonObject env;
                env["ok"]    = false;
                env["code"]  = QStringLiteral("bad_args");
                env["error"] = QStringLiteral(
                    "audit_poll: job_id (non-empty string) is required");
                return QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact));
            }
            // Resolve caller_cwd to the same canonical root the async
            // registration keyed on, so a poll only sees its own project's
            // jobs (bad_cwd on an unresolvable root).
            const QString callerCwd = args.value(
                QStringLiteral("caller_cwd")).toString();
            QString canon, badEnv;
            if (!resolveInflightCallerCwd(callerCwd, "audit_poll",
                                          &canon, &badEnv))
                return badEnv;
            const QJsonObject env =
                host.ci->auditJobPollEnvelope(jobId, canon);
            return QString::fromUtf8(
                QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    // ANTS-1397 — test_audit verb family. All five register the
    // Required caller_cwd contract (the spec's original "Optional"
    // design never shipped; ANTS-1397.md INV-5 was corrected to match
    // this code, 2026-07-25). fold_in delegates to RoadmapFoldIn::*
    // engine entries directly (NOT MCP re-entry — INV-3).
    sink.registerToolProvider("test_audit_partition",
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[](const QJsonObject &args) -> QString {
            TestAuditEngine::PartitionRequest req;
            req.callerCwd   = args.value(QStringLiteral("caller_cwd")).toString();
            req.scope       = args.value(QStringLiteral("scope")).toString();
            req.dimensions  = args.value(QStringLiteral("dimensions")).toString();
            if (args.value(QStringLiteral("chunk_size")).isDouble())
                req.chunkSize = args.value(QStringLiteral("chunk_size")).toInt();
            if (args.value(QStringLiteral("offset")).isDouble())
                req.offset = args.value(QStringLiteral("offset")).toInt();
            if (args.value(QStringLiteral("limit")).isDouble())
                req.limit = args.value(QStringLiteral("limit")).toInt();
            const auto r = TestAuditEngine::partition(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"] = true;
            env["framework"]    = r.framework;
            // ANTS-1623 — polyglot signal. Only emitted when non-empty
            // so single-framework projects (the common case) carry
            // zero overhead in the envelope.
            if (!r.additionalFrameworks.isEmpty())
                env["additional_frameworks"] = r.additionalFrameworks;
            env["test_globs"]   = QJsonArray::fromStringList(r.testGlobs);
            env["total_files"]  = r.totalFiles;
            env["chunks_count"] = r.chunksCount;
            QJsonArray chunks;
            for (const auto &c : r.chunks) {
                QJsonObject co;
                co["id"]                  = c.id;
                co["paths"]               = QJsonArray::fromStringList(c.paths);
                // ANTS-1487: renamed from `dimension_hints` so callers can't
                // mistake "dimensions the pre-pass grep hit" for "dimensions
                // worth auditing". Full lane list is `dimensions_active` at
                // envelope level.
                co["pre_pass_dimensions"] = QJsonArray::fromStringList(c.prePassDimensions);
                chunks.append(co);
            }
            env["chunks"] = chunks;
            if (r.chunkByteBudget > 0)   // ANTS-4113
                env["chunk_byte_budget"] = static_cast<double>(r.chunkByteBudget);
            env["dimensions_active"] = QJsonArray::fromStringList(r.dimensionsActive);
            // ANTS-4111 — a dimension left out of "auto" is reported, not
            // silently absent: a caller seeding from dimensions_active[] can see
            // that the taxonomy is wider than the default and why.
            if (!r.dimensionsSkipped.isEmpty()) {
                env["dimensions_skipped"] =
                    QJsonArray::fromStringList(r.dimensionsSkipped);
                QJsonObject why;
                for (auto it = r.skipReasonPerDimension.constBegin();
                     it != r.skipReasonPerDimension.constEnd(); ++it)
                    why[it.key()] = it.value();
                env["skip_reason_per_dimension"] = why;
            }
            QJsonObject prePass;
            for (auto it = r.prePassFindingsByChunk.constBegin();
                 it != r.prePassFindingsByChunk.constEnd(); ++it) {
                prePass[it.key()] = it.value();
            }
            // ANTS-2070 — the inlined pre-pass map is the envelope's bulk
            // (each chunk caps at 20 findings, but a 35-chunk suite still
            // overflowed the MCP tool-result token cap with 547 findings).
            // When the map would be large, omit it from the wire and flag
            // pre_pass_cached so the caller fetches per-chunk via
            // test_audit_brief — the full map stays in the partition cache
            // for that lookup, and pre_pass_chunk_ids below still advertises
            // which chunks carry findings.
            const QByteArray prePassJson =
                QJsonDocument(prePass).toJson(QJsonDocument::Compact);
            constexpr int kPrePassInlineCapBytes = 24 * 1024;
            const bool prePassOmittedBySize =
                prePassJson.size() > kPrePassInlineCapBytes;
            // ANTS-2096 — a paginated (page 2+) result keeps its pre-pass
            // map in the partition cache for test_audit_brief, but must NOT
            // inline it here: prePassCached signals "fetch per-chunk via
            // brief", so omit the wire map when cached, not only on size.
            if (!prePassOmittedBySize && !r.prePassCached)
                env["pre_pass_findings_by_chunk"] = prePass;
            // ANTS-1489 — echo the chunk-ID keyset at envelope level so
            // callers can decide which per-chunk briefs are worth
            // fetching without descending into the nested map.
            QJsonArray prePassChunkIds;
            for (auto it = r.prePassFindingsByChunk.constBegin();
                 it != r.prePassFindingsByChunk.constEnd(); ++it) {
                if (!it.value().isEmpty()) prePassChunkIds.append(it.key());
            }
            // Stable order — callers may iterate the array directly.
            QStringList idsSorted;
            for (const auto &v : prePassChunkIds) idsSorted.append(v.toString());
            std::sort(idsSorted.begin(), idsSorted.end());
            env["pre_pass_chunk_ids"] = QJsonArray::fromStringList(idsSorted);
            env["pre_pass_cached"] = r.prePassCached || prePassOmittedBySize;
            if (prePassOmittedBySize) {
                // ANTS-2070 — tell the caller why the map is absent and how
                // big it was, so it knows to fetch per-chunk via brief.
                env["pre_pass_omitted"] = true;
                env["pre_pass_omitted_bytes"] = prePassJson.size();
            }
            env["partition_token"] = r.partitionToken;
            env["offset"]    = r.offset;
            env["limit"]     = r.limit;
            env["total"]     = r.total;
            env["truncated"] = r.truncated;
            if (r.nextOffset >= 0) env["next_offset"] = r.nextOffset;
            env["byte_count"] = r.byteCount;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        }});
    sink.registerToolProvider("test_audit_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[](const QJsonObject &args) -> QString {
            TestAuditEngine::BriefRequest req;
            req.callerCwd       = args.value(QStringLiteral("caller_cwd")).toString();
            req.chunkId         = args.value(QStringLiteral("chunk_id")).toString();
            req.partitionToken  = args.value(QStringLiteral("partition_token")).toString();
            const auto r = TestAuditEngine::brief(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]                = true;
            env["chunk_id"]          = r.chunkId;
            env["source_paths"]      = QJsonArray::fromStringList(r.sourcePaths);
            env["dimensions"]        = QJsonArray::fromStringList(r.dimensions);
            env["framework_context"] = r.frameworkContext;
            env["pre_pass_findings"] = r.prePassFindings;
            // ANTS-1457 — surface the prior false-positive ledger
            // entries as a structured field for the reviewer LLM.
            env["prior_false_positives"] = r.priorFalsePositives;
            env["byte_count"]        = r.byteCount;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        }});
    sink.registerToolProvider("test_audit_synthesis_prompt",
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[](const QJsonObject &args) -> QString {
            TestAuditEngine::SynthRequest req;
            req.callerCwd          = args.value(QStringLiteral("caller_cwd")).toString();
            req.partitionToken     = args.value(QStringLiteral("partition_token")).toString();
            req.reportsDir         = args.value(QStringLiteral("reports_dir")).toString();
            req.calibrationAnchor  = args.value(QStringLiteral("calibration_anchor")).toObject();
            // ANTS-1455 — opt-in escape hatch + mode + pagination.
            req.allowOutsideProject = args.value(QStringLiteral("allow_outside_project")).toBool(false);
            req.mode               = args.value(QStringLiteral("mode")).toString();
            req.offset             = args.value(QStringLiteral("offset")).toInt(0);
            // limit defaulting: if caller omitted, leave at -1 sentinel
            // so engine picks mode-appropriate default (5 for "full",
            // ignored for "summary"). 0 is a valid "use default" too.
            if (args.contains(QStringLiteral("limit"))) {
                req.limit = args.value(QStringLiteral("limit")).toInt(-1);
            }
            const auto r = TestAuditEngine::synthesize(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]                  = true;
            env["mode"]                = r.mode;
            env["prompt"]              = r.prompt;
            env["dimension_summaries"] = r.dimensionSummaries;
            env["top_dimensions"]      = r.topDimensions;
            env["file_index"]          = r.fileIndex;
            // ANTS-1488 — per-dimension severity histograms so callers
            // can decide whether to drop into mode:"full" or mode:"hybrid"
            // based on whether any dimension surfaced a CRIT/HIGH.
            env["severity_histograms"] = r.severityHistograms;
            env["truncated"]           = r.truncated;
            env["reports_read"]        = r.reportsRead;
            env["chunks_total"]        = r.chunksTotal;
            env["chunks_returned"]     = r.chunksReturned;
            env["next_offset"]         = r.nextOffset;
            env["truncated_by_limit"]  = r.truncatedByLimit;
            env["byte_count"]          = r.byteCount;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        }});
    sink.registerToolProvider("test_audit_fold_in",
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[](const QJsonObject &args) -> QString {
            // ANTS-5086 — busy guard shared hold (ANTS-2132 § 2.10).
            const RemoteControl::RoadmapWriteHold writeHold(
                args.value(QStringLiteral("caller_cwd")).toString());
            if (!writeHold.held())
                return QString::fromUtf8(QJsonDocument(RemoteControl::roadmapBusyRefusal(
                    QStringLiteral("test_audit_fold_in"))).toJson(QJsonDocument::Compact));
            TestAuditEngine::FoldInRequest req;
            req.callerCwd    = args.value(QStringLiteral("caller_cwd")).toString();
            req.actionable   = args.value(QStringLiteral("actionable")).toArray();
            req.framework    = args.value(QStringLiteral("framework")).toString();
            req.filesScanned = args.value(QStringLiteral("files_scanned")).toInt();
            const QJsonArray dimsArr = args.value(QStringLiteral("dimensions")).toArray();
            for (const QJsonValue &v : dimsArr) req.dimensions.append(v.toString());
            req.rawFindings  = args.value(QStringLiteral("raw_findings")).toInt();
            // ANTS-1635 — narrative-mode opt-in. Forward both fields so
            // the engine's short-circuit gate is reachable.
            req.narrativeMode = args.value(QStringLiteral("narrative_mode")).toBool();
            req.narrativeMd   = args.value(QStringLiteral("narrative_md")).toString();
            // ANTS-2227 — dry_run preview (no counter bump, no ROADMAP write).
            req.dryRun        = args.value(QStringLiteral("dry_run")).toBool();
            // ANTS-3498 — optional id_prefix override (validated in the engine).
            req.idPrefix      = args.value(QStringLiteral("id_prefix")).toString();
            const auto r = TestAuditEngine::foldIn(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                env["written_count"]=r.writtenCount; env["failed_count"]=r.failedCount;
                env["partial"]=r.partial;
                // ANTS-1527 — surface counter_path as a programmatic
                // field on id_counter_failed so the caller can clear
                // a stale `.lock` sibling without parsing the prose
                // error. Only emitted when the engine populated it
                // (id_counter_failed path); other failure modes leave
                // it empty.
                if (!r.counterPath.isEmpty()) env["counter_path"] = r.counterPath;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]                    = true;
            if (req.dryRun) env["dry_run"] = true;   // ANTS-2227
            env["block"]                 = r.block;
            env["allocated_ids"]         = QJsonArray::fromStringList(r.allocatedIds);
            env["written"]               = r.written;
            env["release_block_heading"] = r.releaseBlockHeading;
            env["bytes_written"]         = r.bytesWritten;
            env["written_count"]         = r.writtenCount;
            env["failed_count"]          = r.failedCount;
            env["partial"]               = r.partial;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        }});
    // ANTS-1513 — test_audit_recheck: verify a deferred finding's cite
    // is still live before resuming the work. Read-only project query.
    sink.registerToolProvider("test_audit_recheck",
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[](const QJsonObject &args) -> QString {
            TestAuditEngine::RecheckRequest req;
            req.callerCwd  = args.value(QStringLiteral("caller_cwd")).toString();
            req.findingId  = args.value(QStringLiteral("finding_id")).toString();
            const auto r = TestAuditEngine::recheck(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]    = true;
            env["found"] = r.found;
            if (r.found) {
                env["cited_file"]  = r.citedFile;
                env["cited_line"]  = r.citedLine;
                env["file_exists"] = r.fileExists;
                env["line_exists"] = r.lineExists;
                if (r.lineExists) {
                    env["current_line_text"]         = r.currentLineText;
                    env["line_still_matches_pattern"] = r.lineStillMatchesPattern;
                    if (r.lineStillMatchesPattern) {
                        env["matched_pattern_id"] = r.matchedPatternId;
                        env["matched_dimension"]  = r.matchedDimension;
                    }
                }
                // ANTS-1513 — best-effort git rename hint for a gone file.
                // Lives here (not the engine) so testauditengine.cpp stays
                // QProcess-free (test_audit trio INV-1). Only for a
                // relative cite under an existing project root.
                if (!r.fileExists && !r.citedFile.isEmpty() &&
                    !r.citedFile.startsWith(QLatin1Char('/'))) {
                    const QString canon = QFileInfo(
                        args.value(QStringLiteral("caller_cwd")).toString())
                        .canonicalFilePath();
                    if (!canon.isEmpty()) {
                        QProcess git;
                        git.setWorkingDirectory(canon);
                        git.start(QStringLiteral("git"), {
                            QStringLiteral("log"), QStringLiteral("--all"),
                            QStringLiteral("--diff-filter=R"),
                            QStringLiteral("--name-status"),
                            QStringLiteral("--format="), QStringLiteral("--"),
                            r.citedFile });
                        if (git.waitForFinished(3000) &&
                            git.exitStatus() == QProcess::NormalExit) {
                            const QStringList outLines = QString::fromUtf8(
                                git.readAllStandardOutput())
                                .split(QChar('\n'), Qt::SkipEmptyParts);
                            for (const QString &ol : outLines) {
                                if (!ol.startsWith(QChar('R'))) continue;
                                const QStringList parts = ol.split(QChar('\t'));
                                if (parts.size() >= 3) {
                                    env["drift_hint"] = QStringLiteral(
                                        "file likely moved to %1")
                                        .arg(parts.at(2));
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        }});
    // ANTS-2144 — off the socket thread: cmdWorkspaceSearch blocks on
    // rg.waitForFinished(), which starved the QLocalSocket notifier and
    // tripped concurrent verbs into a -32000 transport timeout. caller_cwd
    // is Required here, so the off-thread path never reaches the
    // focused-tab fallback (RootProvider, main-thread-only state).
    sink.registerToolProvider("workspace_search",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdWorkspaceSearch));
    // ANTS-3716 — cited_by. Off the socket thread for the same reason
    // workspace_search is, and more so: it runs ONE rg per anchor, up to 64 of
    // them, each blocking on waitForFinished(). caller_cwd is Required, so the
    // off-thread path never reaches the focused-tab fallback.
    sink.registerToolProvider("cited_by",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdCitedBy));
    sink.registerToolProvider("file_outline",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFileOutline));
    // ANTS-4398 — mutation_probe. Each mutation runs a full test command, so
    // a batch is seconds-to-minutes; ANTS-2132 dispatches it off the GUI
    // thread, so the window keeps painting for the duration.
    sink.registerToolProvider("mutation_probe",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdMutationProbe));
    // ANTS-1855 — read_log: filter a log file (debug log or caller_cwd path).
    sink.registerToolProvider("read_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdReadLog));
    // ANTS-2021 — read_region: line-range / symbol-body slice of a project file.
    sink.registerToolProvider("read_region",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdReadRegion));
    // ANTS-2219 — read_regions: batched multi-selector read (read-side mirror
    // of apply_edits). Per-item etag → individual 304; shared max_bytes budget.
    sink.registerToolProvider("read_regions",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdReadRegions));
    // ANTS-2094 — read_spill: re-read an offloaded result by its handle.
    // caller_cwd Optional — the spill store is global/content-addressed,
    // not project-scoped.
    sink.registerToolProvider("read_spill",
        ClaudeIntegration::CallerCwdContract::Optional,
        rcDelegate(rc, &RemoteControl::cmdReadSpill));
    // ANTS-2022 — apply_edits: atomic-per-file batch of {path, old, new} edits.
    sink.registerToolProvider("apply_edits",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdApplyEdits));
    // ANTS-1637 — codebase_index: pre-computed project structural map.
    sink.registerToolProvider("codebase_index",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdCodebaseIndex));
    // ANTS-2139 — docs_index: pre-computed project documentation map.
    sink.registerToolProvider("docs_index",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDocsIndex));
    // ANTS-3601 — doc_integrity: deterministic dead-anchor / broken-link / TOC checks.
    sink.registerToolProvider("doc_integrity",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDocIntegrity));
    // ANTS-3636 — doc_citations: resolve a doc's path:line citations, return the text.
    sink.registerToolProvider("doc_citations",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDocCitations));
    // ANTS-3661 — doc_symbols: resolve the identifiers a doc asserts exist.
    sink.registerToolProvider("doc_symbols",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDocSymbols));
    // ANTS-3662 — spec_lint: the greppable half of the spec-format contract.
    sink.registerToolProvider("spec_lint",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSpecLint));
    // ANTS-4108 — spec_conformance: run a spec's own patterns against the
    // examples beside them (spec_lint's executable sibling).
    sink.registerToolProvider("spec_conformance",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSpecConformance));
    // ANTS-3660 — doc_dedup: the same passage written twice.
    sink.registerToolProvider("doc_dedup",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDocDedup));
    // ANTS-3663 — doc_lint: the five deterministic doc checkers in one call.
    sink.registerToolProvider("doc_lint",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDocLint));
    // ANTS-2161 — project_settings: detect layout + create/update .ants/project.json.
    sink.registerToolProvider("project_settings",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdProjectSettings));
#ifdef ANTS_LUA_PLUGINS
    // ANTS-2093 — project_query: run an agent-supplied read-only Lua snippet
    // server-side and return only its result (the code-execution token-saver).
    // Lives entirely in ants_lua_lib (LuaEngine::projectQueryVerb) because
    // ants_core_lib's RemoteControl cannot see LuaEngine; the provider lambda
    // (chrome_lib, which links lua_lib) reads the gate + tuning from Config and
    // delegates. Registered only in ANTS_LUA_PLUGINS builds (verb absent
    // otherwise — clean drop-out, no dead refusal path). See docs/specs/ANTS-2093.md.
    sink.registerToolProvider("project_query",
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[=](const QJsonObject &args) -> QString {
            // ANTS-4682 — the three Config reads are GUI-thread-owned; the
            // snippet run they parameterise is not, and it walks the project
            // tree. Marshal the reads, run the verb on the worker: that is
            // what re-serialises this verb against the off-thread ones, which
            // is the hazard ANTS-2132 § 5 named and left open.
            const std::optional<ProjectQueryConfig> cfg =
                host.projectQueryConfig ? host.projectQueryConfig()
                                        : std::nullopt;
            // § 2.5 — refuse, never default. A default-constructed Cfg reads
            // the feature's own off switch as false, so the verb would report
            // "project_query is disabled" to a caller who had enabled it.
            if (!cfg) {
                QJsonObject env;
                env[QStringLiteral("ok")]    = false;
                env[QStringLiteral("code")]  = QStringLiteral("gui_read_refused");
                env[QStringLiteral("error")] = QStringLiteral(
                    "project_query: the settings read was refused because the "
                    "dispatcher is shutting down");
                return QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact));
            }
            return QString::fromUtf8(QJsonDocument(LuaEngine::projectQueryVerb(
                    args, cfg->enabled, cfg->timeoutMs, cfg->capBytes))
                .toJson(QJsonDocument::Compact));
        }});
#endif
    // ANTS-1961 / ANTS-1962 — cross-session feedback-file read + write.
    sink.registerToolProvider("feedback_query",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFeedbackQuery));
    sink.registerToolProvider("feedback_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFeedbackLog));
    // ANTS-2129 — audit_falsepos_log: write side of the false-positive ledger.
    sink.registerToolProvider("audit_falsepos_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdAuditFalseposLog));
    // ANTS-1713 — audit_dismiss: write side of the fingerprint-keyed
    // learned-FP ledger (ANTS-1708 shipped the ledger + the GUI recording
    // path; this lets a CC session record the verdict it just reasoned to).
    sink.registerToolProvider("audit_dismiss",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdAuditDismiss));
    sink.registerToolProvider("git_state",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdGitState));
    sink.registerToolProvider("subsystem",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSubsystem));
    sink.registerToolProvider("last_audit_summary",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdLastAuditSummary));
    // ANTS-1569 — current_state aggregator. MCP-only (mirrors
    // last_audit_summary; no IPC dispatch branch). Pure composer over
    // cmdRoadmapQuery + cmdGitState + cmdLastAuditSummary.
    sink.registerToolProvider("current_state",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdCurrentState));

    // ANTS-1735 — model_switch_stats. Read-only aggregation of the model-switch
    // effectiveness ledger, scoped to caller_cwd's project. MCP-only (mirrors
    // current_state; no IPC dispatch branch). See docs/specs/ANTS-1735.md §2.5.
    sink.registerToolProvider("model_switch_stats",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdModelSwitchStats));

    // ANTS-1309 + ANTS-1308 — spec-aware token-savers. spec_query
    // returns one spec's parsed {title, status, kind, invariants[]};
    // invariant_check scans docs/specs/*.md for specs that mention
    // any path in `files[]` and returns their invariant lists.
    // Both MCP-only.
    sink.registerToolProvider("spec_query",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSpecQuery));
    // ANTS-1963 — spec_log: write the three recurring spec mutations.
    sink.registerToolProvider("spec_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSpecLog));
    sink.registerToolProvider("invariant_check",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdInvariantCheck));

    // ANTS-1306 + ANTS-1307 — task-start context composers.
    // task_priors bundles matching specs + ROADMAP cards + recent
    // commits + ADRs for a free-text task description; project_conventions
    // returns the task_type-scoped convention subset. Both MCP-only.
    // See docs/specs/ANTS-1306.md and docs/specs/ANTS-1307.md.
    sink.registerToolProvider("task_priors",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdTaskPriors));
    sink.registerToolProvider("project_conventions",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdProjectConventions));

    // ANTS-1299 + ANTS-1300 — build/test cache MCP tools. Both
    // are op-dispatched (op=read | op=record) and write to
    // <project>/.audit_cache/. MCP-only. See docs/specs/ANTS-1299.md
    // and docs/specs/ANTS-1300.md.
    sink.registerToolProvider("build_status",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdBuildStatus));
    sink.registerToolProvider("test_results",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdTestResults));

    // ANTS-1302 — focused_test. Runs only the ctest subset touching the
    // changed files (via tests/coverage-map.json), returns the
    // test_results envelope. Expensive (shells out to ctest), MCP-only.
    // See docs/specs/ANTS-1302.md.
    sink.registerToolProvider("focused_test",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFocusedTest));

    // ANTS-3745 — build_target_for. Which target owns a source, read from
    // CMakeLists.txt, plus the build and ctest lines that follow. Read-only
    // and static — the opposite cost profile to focused_test above, which is
    // why it is its own verb rather than an op on it.
    sink.registerToolProvider("build_target_for",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdBuildTargetFor));

    // ANTS-1303 — find_definition + find_caller. Tree-wide regex symbol
    // scanner (no LSP). Both take {symbol, caller_cwd, lang?,
    // max_results?} and delegate to SymbolQuery via RemoteControl.
    // MCP-only conceptually; also reachable via the IPC dispatch verbs
    // find-definition / find-caller. See docs/specs/ANTS-1303.md.
    sink.registerToolProvider("find_definition",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFindDefinition));
    sink.registerToolProvider("find_caller",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdFindCaller));

    // ANTS-1305 — similar_code. Tree-wide shape matcher: reuses the
    // FileOutline extractor + ranks signatures by token-set Jaccard
    // similarity to a free-text {shape, caller_cwd, lang?, max_results?}
    // query. Delegates to SimilarCode via RemoteControl. MCP-only
    // conceptually; also reachable via the IPC dispatch verb
    // similar-code. See docs/specs/ANTS-1305.md.
    sink.registerToolProvider("similar_code",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSimilarCode));

    // ANTS-1112 — five `indie_review_*` tools. Each handler resolves
    // the active project via the focused TerminalWidget's shellCwd
    // (matches the convention used by git_state / subsystem /
    // last_audit_summary). All five delegate to RemoteControl's
    // cmdIndieReview* methods, mirroring the ANTS-1253 registry shape.
    sink.registerToolProvider("indie_review_partition",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdIndieReviewPartition));
    sink.registerToolProvider("indie_review_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdIndieReviewBrief));
    sink.registerToolProvider("indie_review_corroborate",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdIndieReviewCorroborate));
    sink.registerToolProvider("indie_review_synthesis_prompt",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdIndieReviewSynthesisPrompt));
    sink.registerToolProvider("indie_review_fold_in",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdIndieReviewFoldIn));
    // ANTS-1279 — indie_review_orchestrate. Pure read (partition + brief
    // manifests); no subprocess, so no in-flight gate. caller_cwd Required.
    sink.registerToolProvider("indie_review_orchestrate",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdIndieReviewOrchestrate));

    // ANTS-1352 — indie_review_dispatch. Inline in-flight gate via
    // verbInFlight* (same pattern as audit_run); caller_cwd Required
    // per callerCwdContractFor; rate-limit tier Expensive.
    // ANTS-4682 — STAYS, deliberately (ANTS-2132 § 5), same shape as
    // audit_run: GUI-owned job registry, own worker, still freezes.
    sink.registerToolProvider("indie_review_dispatch",
        ClaudeIntegration::CallerCwdContract::Required,
        [=](const QJsonObject &args) -> QString {
            if (!rc()) return QString::fromUtf8(kRcUnavailable);
            const QString callerCwd = args.value(
                QStringLiteral("caller_cwd")).toString();
            // ANTS-1833 — reject a non-existent root before it can
            // collapse the in-flight key (canon would be empty).
            QString canon, badEnv;
            if (!resolveInflightCallerCwd(callerCwd, "indie_review_dispatch",
                                          &canon, &badEnv))
                return badEnv;
            const qint64 existing =
                host.ci->verbInFlightTryAcquire(
                    QStringLiteral("indie_review_dispatch"), canon);
            if (existing >= 0) {
                QJsonObject env;
                env["ok"]               = false;
                env["code"]             = QStringLiteral("already_running");
                env["error"]            = QStringLiteral(
                    "indie_review_dispatch: a sweep is already in flight "
                    "for this project root; retry after it completes");
                env["running_since_ms"] =
                    QDateTime::currentMSecsSinceEpoch() - existing;
                env["retry_after_ms"]   = 30000;
                return QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact));
            }
            // RAII: release on every exit path (incl. exception). indie-review-2026-05-21.
            const auto inFlightGuard = qScopeGuard([&, ci = host.ci] {
                ci->verbInFlightRelease(
                    QStringLiteral("indie_review_dispatch"), canon);
            });
            // ANTS-2104 — run the dispatch on a worker thread, exactly like
            // audit_run (ANTS-2103). cmdIndieReviewDispatch -> dispatchLanes
            // spins a local QNetworkAccessManager + QEventLoop (indiereview
            // dispatcher.cpp:223-224); on the main thread that nested loop
            // reentrantly delivers MCP QLocalSocket read-notifications during
            // the multi-minute LLM sweep -> the same use-after-free SIGSEGV
            // class as audit_run. The nam/loop are locals, so they construct
            // on the worker; QThread::wait() is a join (no event pump), so no
            // foreign socket notification fires during the dispatch.
            // ANTS-5024 — the worker gets `canon`, resolved on this thread.
            // wait() parks the GUI thread, so a GUI-thread marshal from the
            // worker is never served and Ants hangs for good.
            QJsonDocument doc;
            {
                QThread *worker = QThread::create(
                    [&]() {
                        doc = rc()->cmdIndieReviewDispatch(args, canon);
                    });
                worker->start();
                worker->wait();
                delete worker;
            }
            QString out = QString::fromUtf8(
                doc.toJson(QJsonDocument::Compact));
            return out;
        });

    // ANTS-1113 — debt_sweep_* (4 tools). _scan shells out to git and
    // _apply_fix to a packaging script (debtsweepengine.cpp
    // waitForFinished); ANTS-2132 dispatches the whole rc-delegate family off
    // the GUI thread, so neither blocks the window.
    sink.registerToolProvider("debt_sweep_scan",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDebtSweepScan));
    sink.registerToolProvider("debt_sweep_apply_fix",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDebtSweepApplyFix));
    sink.registerToolProvider("debt_sweep_defer",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDebtSweepDefer));
    sink.registerToolProvider("debt_sweep_triage_prompt",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdDebtSweepTriagePrompt));

    // ANTS-1289 — verify_changes. It shells out to per-gate build/test
    // commands (verifyengine.cpp waitForFinished), which would freeze the GUI
    // for the gate timeout; ANTS-2132 dispatches it off the GUI thread.
    sink.registerToolProvider("verify_changes",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdVerifyChanges));

    // ANTS-1290 — plan_template.
    sink.registerToolProvider("plan_template",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdPlanTemplate));

    // ANTS-1360 — mcp_trace: read a slice of ClaudeIntegration's
    // ring buffer of last tool/call dispatches. Does NOT delegate
    // to RemoteControl — the ring lives inside ClaudeIntegration.
    // ANTS-4682 — STAYS. Reads ClaudeIntegration's trace ring; no project
    // file, so it is outside the § 5 hazard.
    sink.registerToolProvider("mcp_trace",
        ClaudeIntegration::CallerCwdContract::ProcessGlobal,
        [ci = host.ci](const QJsonObject &args) -> QString {
            const quint64 since = static_cast<quint64>(
                args.value("since").toVariant().toLongLong());
            const int limit = args.value("limit").toInt(50);
            return QString::fromUtf8(
                QJsonDocument(ci->queryMcpTrace(since, limit))
                    .toJson(QJsonDocument::Compact));
        });

    // ANTS-1319 — cold_eyes_* (4 tools). Mirror to indie_review fold-in
    // pattern; each handler delegates to RemoteControl::cmdColdEyes*.
    sink.registerToolProvider("cold_eyes_partition",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdColdEyesPartition));
    sink.registerToolProvider("cold_eyes_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdColdEyesBrief));
    sink.registerToolProvider("cold_eyes_cross_doc_diff",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdColdEyesCrossDocDiff));
    sink.registerToolProvider("cold_eyes_fold_in",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdColdEyesFoldIn));
    // ANTS-1413 — cold_eyes_single_doc. Single-spec cross-consistency
    // brief without the partition+brief multi-step.
    sink.registerToolProvider("cold_eyes_single_doc",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdColdEyesSingleDoc));
    // ANTS-1414 — cross_doc_diff. Lane-source-agnostic alias for the
    // regex hotspot primitive shared by cold-eyes + indie-review.
    sink.registerToolProvider("cross_doc_diff",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdCrossDocDiff));

    // ANTS-1283 — session_memory KV.
    sink.registerToolProvider("session_memory",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSessionMemory));

    // ANTS-1723 — workflow_state: superpowers skill step/phase store.
    sink.registerToolProvider("workflow_state",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdWorkflowState));

    // ANTS-1724 — session_brief: compact session-state envelope.
    sink.registerToolProvider("session_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSessionBrief));

    // ANTS-1883 — session_orient: bundle of current_state +
    // project_layout + roadmap_query (section_index, active).
    sink.registerToolProvider("session_orient",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdSessionOrient));

    // ANTS-1430 — project_layout pre-cache.
    sink.registerToolProvider("project_layout",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(rc, &RemoteControl::cmdProjectLayout));

    // ANTS-1400 — caller_cwd_info diagnostic verb. Pure delegation to
    // ants::resolveCallerCwdRoot. No filesystem operations beyond the
    // canonicalisations the helper performs; no shell, no process.
    // The verb is intentionally classified Optional in
    // ClaudeIntegration::callerCwdContractFor so empty caller_cwd is
    // accepted (EmptyFallback is the legitimate "what would happen
    // without it?" question). See docs/specs/ANTS-1400.md.
    sink.registerToolProvider("caller_cwd_info",
        ClaudeIntegration::CallerCwdContract::Optional,
        ClaudeIntegration::RcHandler{[=](const QJsonObject &args) -> QString {
            const QString callerCwd =
                args.value(QStringLiteral("caller_cwd")).toString();
            const ants::ResolvedRoot rr =
                ants::resolveCallerCwdRoot(host.roots, callerCwd);
            QJsonObject env;
            env["ok"]           = true;
            env["source"]       = sourceToString(rr.source);
            env["resolved_cwd"] = rr.cwd;
            if (rr.tabIndex) env["tab_index"] = *rr.tabIndex;
            return QString::fromUtf8(
                QJsonDocument(env).toJson(QJsonDocument::Compact));
        }});
}

}  // namespace mcp
