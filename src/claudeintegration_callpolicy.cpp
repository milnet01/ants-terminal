// ANTS-1677 claudeintegration piece 9/9 — reply wrapping, in-flight caps, audit jobs, ETags, the read cache and rate limits
#include "claudeintegration.h"
#include "claudeintegration_internal.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>

#include <limits>

using namespace claudeintegrationdetail;

// --- ANTS-1294 — MCP output sanitisation ---

QString ClaudeIntegration::wrapMcpData(const QString &toolName,
                                       const QString &payload) {
    // Neutralise close-tag breakout: a hostile commit message / file
    // line / scrollback chunk that contains the literal substring
    // </ants_mcp_data> would otherwise close the outer wrap and let
    // anything that follows be interpreted by the consuming assistant
    // as outside-the-wrap prose. Replace with a self-closing sentinel
    // of an unused tag name — visibly modified (a reviewer can tell
    // sanitisation happened) but inert.
    QString sanitised = payload;
    sanitised.replace(QStringLiteral("</ants_mcp_data>"),
                      QStringLiteral("<ants_mcp_data_escaped/>"));
    // Indie-review-2026-05-14 lane-5 ME-2: case/whitespace variants
    // (e.g. `</ANTS_MCP_DATA>`, `</ ants_mcp_data >`) — some assistant
    // tokenisers normalise tag casing and whitespace before pattern
    // matching, so the strict-case sentinel from above isn't enough.
    // QRegularExpression i-flag + tolerant whitespace.
    // ANTS-4457 — and tolerant TRAILING CONTENT, matching the open-tag
    // scrub below. This pattern required `>` right after the whitespace,
    // so a close tag carrying any other token passed through untouched
    // and could forge the frame for exactly the lenient consumer both
    // scrubs are written for. A strict XML parser is what would reject a
    // close tag with attributes, and a strict parser is not the threat
    // model. The `\b` is what makes the widening safe: the replacement
    // sentinel continues `ants_mcp_data_`, and `data` followed by `_` is
    // no word boundary, so the scrub cannot consume its own output.
    // src/doccitations.cpp carries a copy of both patterns to disclose
    // what will be rewritten; it was widened in the same change and
    // tests/features/envelope_close_tag_scrub checks they still match.
    static const QRegularExpression closeTagVariantRe(
        QStringLiteral(R"(</\s*ants_mcp_data\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    sanitised.replace(closeTagVariantRe,
                      QStringLiteral("<ants_mcp_data_escaped/>"));
    // ANTS-1670 M2 — symmetric open-tag scrub. The close-tag breakout
    // above is only half the wrapper: a payload carrying a literal
    // `<ants_mcp_data …>` open tag (or a case/whitespace variant) could
    // spoof a nested wrapper-open for a consuming assistant that matches
    // the open tag tolerantly, desyncing the real frame. Neutralise the
    // open form too. (The `\b` after the tag name leaves the
    // `<ants_mcp_data_escaped/>` sentinel emitted just above untouched:
    // `data` is followed by `_`, itself a word char, so no word boundary
    // exists there and the sentinel is never re-matched.)
    static const QRegularExpression openTagVariantRe(
        QStringLiteral(R"(<\s*ants_mcp_data\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    sanitised.replace(openTagVariantRe,
                      QStringLiteral("<ants_mcp_data_escaped/>"));
    // ANTS-1996 — neutralise XML/HTML comment markers. The close-tag
    // scrub above is defeated by a comment desync: an unterminated
    // `<!--` in the payload swallows the real `</ants_mcp_data>` from a
    // consuming assistant that treats markup comments structurally, so
    // the literal close tag is never seen and the wrap silently runs on
    // into following content; a stray `-->` can re-open surrounding
    // context the same way. Replace both markers with a visibly-modified
    // but inert form (a space breaks the token) so a hostile payload
    // can't smuggle structural ambiguity past the close-tag scrub.
    sanitised.replace(QStringLiteral("<!--"), QStringLiteral("<!- -"));
    sanitised.replace(QStringLiteral("-->"),  QStringLiteral("- ->"));
    // Indie-review-2026-05-14 lane-3 M1: defensive escaping of the
    // tool-name attribute. The current registry guarantees the name
    // matches `^[a-z][a-z0-9_]+$` so this is paranoia today, but the
    // helper is public-static and reachable from any future caller;
    // a `"` / `>` / `<` / `&` slipping through would break the wrap.
    QString safeTool = toolName;
    safeTool.replace(QLatin1Char('&'),  QStringLiteral("&amp;"));
    safeTool.replace(QLatin1Char('<'),  QStringLiteral("&lt;"));
    safeTool.replace(QLatin1Char('>'),  QStringLiteral("&gt;"));
    safeTool.replace(QLatin1Char('"'),  QStringLiteral("&quot;"));
    return QStringLiteral("<ants_mcp_data tool=\"%1\">%2</ants_mcp_data>")
        .arg(safeTool, sanitised);
}

// --- ANTS-2218 — opt-in raw (verbatim) framing for content reads ---
//
// wrapMcpData (above) neutralises any literal </ants_mcp_data> (plus open-tag
// and `<!--`/`-->` variants) in the payload. That scrub is DELIBERATELY lossy
// and irreversible: a hostile file/scrollback must not be able to forge the
// frame-close and have following bytes read as trusted prose, and any escaping
// a good agent could invert a hostile normalising tokeniser could invert too —
// reopening the ANTS-1294/1670/1996 breakout. The cost is that an agent reading
// frame-sensitive SOURCE (e.g. this file, a spec, HTML/markdown with comments)
// sees doctored bytes and would corrupt the file if it built an Edit from them
// (ANTS-2218).
//
// Raw mode is the opt-in escape hatch: emit the payload VERBATIM so the agent
// gets true bytes, made safe by an UNFORGEABLE delimiter instead of a scrub.
// The frame tag carries a per-call nonce — a truncated SHA-256 of the payload,
// verified absent from the payload — so </ants_mcp_data_raw__<nonce>> cannot
// occur in the content and there is nothing reversible for a hostile consumer
// to recover. Falls back to the safe scrub in the (infeasible) case that no
// collision-free nonce is found. Honoured only for mcp::isRawEligible read
// verbs at the dispatch site. See tests/features/mcp_raw_read.
QString ClaudeIntegration::wrapMcpDataRaw(const QString &toolName,
                                          const QString &payload) {
    // Derive the nonce from the payload's own hash, then widen until it is
    // absent from the payload. A payload that contains its own SHA-256 is a
    // preimage problem (infeasible), but verify-and-widen keeps unforgeability
    // a proof rather than an assumption.
    const QByteArray digest =
        QCryptographicHash::hash(payload.toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    QString nonce;
    for (int len = 16; len <= digest.size(); len += 8) {
        nonce = QString::fromLatin1(digest.left(len));
        if (!payload.contains(nonce)) break;
    }
    if (payload.contains(nonce)) {
        // Unreachable in practice. Rather than emit a forgeable frame, fall
        // back to the safe (lossy) scrub.
        return wrapMcpData(toolName, payload);
    }
    // Tool-name attribute hardening (mirrors wrapMcpData).
    QString safeTool = toolName;
    safeTool.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    safeTool.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    safeTool.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    safeTool.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return QStringLiteral(
               "<ants_mcp_data_raw__%1 tool=\"%2\">%3</ants_mcp_data_raw__%1>")
        .arg(nonce, safeTool, payload);
}

// --- ANTS-1404 — per-tool caller_cwd contract ---

// ANTS-1351 + ANTS-1397 § 2.4 — inline verb-in-flight gate
// implementations. Single QMutex around a QHash keyed by
// (verb, canonicalProjectRoot). Stale-slot reaper sweeps entries
// older than kVerbInFlightReapMs to recover from worker-death
// orphans (SIGKILL, segfault, panic-no-RAII).
qint64 ClaudeIntegration::verbInFlightTryAcquire(
        const QString &verb, const QString &projectRoot) {
    QMutexLocker lk(&m_verbInFlightMutex);
    const qint64 now = monotonicNowMs();
    // Stale-slot reap: drop any entries older than the reap window.
    for (auto it = m_verbInFlight.begin(); it != m_verbInFlight.end(); ) {
        if (now - it.value().startedMonoMs > kVerbInFlightReapMs) {
            it = m_verbInFlight.erase(it);
        } else {
            ++it;
        }
    }
    const auto key = qMakePair(verb, projectRoot);
    auto it = m_verbInFlight.find(key);
    if (it != m_verbInFlight.end())
        return it.value().startedEpochMs;  // already running
    m_verbInFlight.insert(
        key, InFlightSlot{QDateTime::currentMSecsSinceEpoch(), now});
    return -1;
}

void ClaudeIntegration::verbInFlightRelease(
        const QString &verb, const QString &projectRoot) {
    QMutexLocker lk(&m_verbInFlightMutex);
    m_verbInFlight.remove(qMakePair(verb, projectRoot));
}

// ANTS-3396 — async audit_run job registry. Bounded/reaped/evicted so a
// runaway or process-lifetime accumulation can never grow unbounded.
QString ClaudeIntegration::auditJobRegister(
        const QString &root, qint64 startedMs) {
    QMutexLocker lk(&m_auditJobsMutex);
    const qint64 now = monotonicNowMs();
    // (1) Reap entries older than the reap window (same as the in-flight
    // gate). A stale running entry ages out too — its result stays durable
    // on disk and the poll then reads `expired`.
    for (auto it = m_auditJobs.begin(); it != m_auditJobs.end(); ) {
        if (now - it.value().startedMonoMs > kAuditJobReapMs)
            it = m_auditJobs.erase(it);
        else
            ++it;
    }
    // (2) Still at the cap → evict the oldest TERMINAL entry (by startedMs
    // — the only timestamp stored; a deliberate simplification, harmless on
    // the 16-entry single-project cap). A running entry is never evicted.
    if (m_auditJobs.size() >= kAuditJobsMax) {
        QString victim;
        qint64  oldest = 0;
        bool    have = false;
        for (auto it = m_auditJobs.constBegin();
             it != m_auditJobs.constEnd(); ++it) {
            const AuditJob &j = it.value();
            if (j.status != QLatin1String("running")
                && (!have || j.startedMs < oldest)) {
                oldest = j.startedMs;
                victim = it.key();
                have   = true;
            }
        }
        // (3) All entries running → no room; caller emits `too_many_jobs`.
        if (!have) return QString();
        m_auditJobs.remove(victim);
    }
    const QString jobId =
        QStringLiteral("audit-") + QString::number(m_auditJobNextId++);
    AuditJob j;
    j.status    = QStringLiteral("running");
    j.root      = root;
    j.startedMs = startedMs;
    j.startedMonoMs = now;
    m_auditJobs.insert(jobId, j);
    return jobId;
}

void ClaudeIntegration::auditJobComplete(
        const QString &jobId, const AuditJob &result) {
    QMutexLocker lk(&m_auditJobsMutex);
    auto it = m_auditJobs.find(jobId);
    if (it == m_auditJobs.end())
        return;  // reaped/evicted — the result is durable on disk.
    // Overlay the terminal fields; keep the original start time + root.
    AuditJob j     = result;
    j.startedMs    = it.value().startedMs;
    j.startedMonoMs = it.value().startedMonoMs;
    j.root         = it.value().root;
    it.value()     = j;
}

QJsonObject ClaudeIntegration::auditJobPollEnvelope(
        const QString &jobId, const QString &callerRoot) const {
    QMutexLocker lk(&m_auditJobsMutex);
    QJsonObject env;
    env["ok"]     = true;
    env["job_id"] = jobId;
    auto it = m_auditJobs.constFind(jobId);
    // A miss OR a job owned by a different project root both read as
    // `expired` — the caller can only see its own project's jobs, and a
    // cross-root probe is indistinguishable from a genuine miss.
    if (it == m_auditJobs.constEnd() || it.value().root != callerRoot) {
        // Registry miss — normal terminal poll status, NOT a refusal.
        env["status"] = QStringLiteral("expired");
        env["hint"]   = QStringLiteral(
            "job not in registry (completed long ago or evicted); read the "
            "latest result via last_audit_summary.");
        return env;
    }
    const AuditJob &j = it.value();
    env["status"] = j.status;
    if (j.status == QLatin1String("running")) {
        env["started_at_ms"] = j.startedMs;
        env["elapsed_ms"]    =
            monotonicNowMs() - j.startedMonoMs;
    } else if (j.status == QLatin1String("error")) {
        // Branch on `status`, not `ok` — the poll itself succeeded.
        // `code` is job-scoped; omit when runAudit left it empty (error
        // carries the detail). NOT a poll-level refusal code.
        if (!j.code.isEmpty()) env["code"] = j.code;
        env["error"] = j.error;
    } else {  // "done"
        if (!j.cachePath.isEmpty()) env["cache_path"] = j.cachePath;
        env["total_raw"]        = j.totalRaw;
        env["total_actionable"] = j.totalActionable;
        env["partial"]          = j.partial;
        QJsonArray inc;
        for (const QString &t : j.incompleteTools) inc.append(t);
        env["incomplete_tools"] = inc;
        // ANTS-3585 — same richer surfaces as the sync provider (mcptoolregistry.cpp):
        // per-tool truncated/crashed detail + the zero-coverage parse-failure
        // list. Omitted when empty.
        if (!j.incompleteToolsDetail.isEmpty())
            env["incomplete_tools_detail"] = j.incompleteToolsDetail;
        if (!j.parseFailures.isEmpty()) {
            QJsonArray pf;
            for (const QString &f : j.parseFailures) pf.append(f);
            env["parse_failures"] = pf;
            // ANTS-3706 — same detail sibling as the sync provider.
            if (!j.parseFailuresDetail.isEmpty())
                env["parse_failures_detail"] = j.parseFailuresDetail;
        }
        if (j.noChanges) env["no_changes"] = true;
        env["read_full_with"]   = QStringLiteral("last_audit_summary");
    }
    return env;
}

ClaudeIntegration::CallerCwdContract
ClaudeIntegration::callerCwdContractFor(const QString &toolName) {
    using C = CallerCwdContract;
    // Required — refuse with caller_cwd_required when absent.
    if (toolName == QStringLiteral("last_audit_summary")) return C::Required;
    if (toolName == QStringLiteral("git_state"))          return C::Required;
    if (toolName == QStringLiteral("verify_changes"))     return C::Required;
    // ANTS-1352: indie_review_dispatch — server-side reviewer fan-out.
    // Required: assembles per-lane prompts from caller_cwd's CLAUDE.md.
    if (toolName == QStringLiteral("indie_review_dispatch"))     return C::Required;
    // ANTS-1351 — audit_run mutates per-call SARIF files under /tmp
    // and consumes a worker-pool slot; Required gate refuses early
    // before any pool dispatch.
    if (toolName == QStringLiteral("audit_run"))          return C::Required;
    // ANTS-3396 — audit_poll reads the async-audit job registry.
    // Required for parity with audit_run (the dispatcher refuses
    // caller_cwd_required before the read-only poll runs).
    if (toolName == QStringLiteral("audit_poll"))         return C::Required;
    // ANTS-1430 — project_layout reads from the tenant-hashed
    // session_memory store. Joins session_memory in the gated
    // Required set (see ANTS-1336 INV-7 amendment).
    if (toolName == QStringLiteral("project_layout"))     return C::Required;
    // ANTS-1855 — read_log resolves project-relative paths + anchors
    // tenancy; Required even for the debug-log default so the tool can't
    // be used as an unscoped file reader.
    if (toolName == QStringLiteral("read_log"))            return C::Required;
    // ANTS-2021 — read_region resolves a project-relative path + anchors
    // tenancy; Required.
    if (toolName == QStringLiteral("read_region"))         return C::Required;
    // ANTS-2219 — read_regions: batched sibling; each item resolves under
    // the same root, so caller_cwd is Required.
    if (toolName == QStringLiteral("read_regions"))        return C::Required;
    // ANTS-2094 — read_spill resolves a GLOBAL content-addressed handle
    // under ~/.cache (not project-scoped), so caller_cwd is neither an
    // anchor nor required; Optional accepts the absent case.
    if (toolName == QStringLiteral("read_spill"))          return C::Optional;
    // ANTS-2022 — apply_edits writes project files anchored by caller_cwd +
    // PathValidation (roadmap_log/changelog_log posture, no RcGate); Required.
    if (toolName == QStringLiteral("apply_edits"))         return C::Required;
    // ANTS-1637 — codebase_index is a project-scoped structural-map reader
    // keyed on the resolved root; Required.
    if (toolName == QStringLiteral("codebase_index"))      return C::Required;
    // ANTS-2139 — docs_index is a project-scoped documentation-map reader
    // keyed on the resolved root; Required (sibling of codebase_index).
    if (toolName == QStringLiteral("docs_index"))          return C::Required;
    // ANTS-3601 — doc_integrity is a project-scoped doc-consistency reader
    // keyed on the resolved root; Required.
    if (toolName == QStringLiteral("doc_integrity"))       return C::Required;
    // ANTS-3636 — doc_citations resolves citations against the focused
    // project's tree; Required.
    if (toolName == QStringLiteral("doc_citations"))       return C::Required;
    // ANTS-3661 — doc_symbols walks the focused project's docs AND resolves
    // against its source tree; both need the caller's anchor.
    if (toolName == QStringLiteral("doc_symbols"))         return C::Required;
    // ANTS-3662 — spec_lint walks the focused project's specs_dir and reads its
    // format standard from the same root; Required.
    if (toolName == QStringLiteral("spec_lint"))           return C::Required;
    // ANTS-4108 — spec_conformance resolves a required `path` under the focused
    // project's root and reads only that document; Required.
    if (toolName == QStringLiteral("spec_conformance"))    return C::Required;
    // ANTS-3660 — doc_dedup walks the focused project's docs_dir; Required.
    if (toolName == QStringLiteral("doc_dedup"))           return C::Required;
    // ANTS-3663 — doc_lint walks the focused project's docs_dir; Required.
    if (toolName == QStringLiteral("doc_lint"))            return C::Required;
    // ANTS-2161 — project_settings reads/writes <root>/.ants/project.json
    // anchored on the resolved root; Required.
    if (toolName == QStringLiteral("project_settings"))    return C::Required;
    // ANTS-2093 — project_query: caller_cwd is both the project anchor and
    // the FS-confinement root for the snippet's project.* reads.
    if (toolName == QStringLiteral("project_query"))       return C::Required;
    // ANTS-1961 / ANTS-1962 — feedback verbs resolve a relative `path`
    // off caller_cwd and anchor tenancy; Required even though the
    // canonical case is an absolute shared-root path.
    if (toolName == QStringLiteral("feedback_query"))      return C::Required;
    if (toolName == QStringLiteral("feedback_log"))        return C::Required;
    // ANTS-2129 — audit_falsepos_log resolves <root>/.ants_review_falsepos.jsonl
    // off caller_cwd; Required (matches the other root-resolving write verbs).
    if (toolName == QStringLiteral("audit_falsepos_log")) return C::Required;
    // ANTS-1713 — audit_dismiss resolves <root>/.audit_cache/learned-fp.jsonl
    // off caller_cwd; Required (same shape as audit_falsepos_log).
    if (toolName == QStringLiteral("audit_dismiss"))      return C::Required;
    // ANTS-1963 — spec_log resolves docs/specs|phases/<id>.md under the
    // caller's project root; Required matches spec_query.
    if (toolName == QStringLiteral("spec_log"))            return C::Required;
    // ANTS-1435 — session_memory: dispatcher refuses empty
    // caller_cwd upstream (Required). The handler still has a
    // body-level cwd_missing for the IPC path which bypasses the
    // contract — kept for diagnostic parity. Asymmetric internal
    // routing: reads anchor to caller_cwd, writes match focused tab.
    if (toolName == QStringLiteral("session_memory"))     return C::Required;
    if (toolName == QStringLiteral("session_orient"))     return C::Required;  // ANTS-1883
    if (toolName == QStringLiteral("workflow_state"))     return C::Required;
    // TabSpecific — ENFORCED since ANTS-1415 Phase 3b: no caller_cwd and
    // no usable `tab` index refuses `tab_or_cwd_required`. This comment
    // read "classified but not enforced in Phase 3a" long after 3b
    // shipped, which is the same stale-promise defect ANTS-4457 found in
    // the shared schema description.
    if (toolName == QStringLiteral("get_scrollback"))     return C::TabSpecific;
    if (toolName == QStringLiteral("get_text"))           return C::TabSpecific;
    // ANTS-1301 — reads the focused terminal's scrollback.
    if (toolName == QStringLiteral("recent_errors"))      return C::TabSpecific;
    // ANTS-1312 — reads the focused terminal's current selection.
    if (toolName == QStringLiteral("last_selection"))    return C::TabSpecific;
    if (toolName == QStringLiteral("get_last_command"))   return C::TabSpecific;
    if (toolName == QStringLiteral("get_environment"))    return C::TabSpecific;
    if (toolName == QStringLiteral("get_cwd"))            return C::TabSpecific;
    // ProcessGlobal — caller_cwd accepted and ignored.
    if (toolName == QStringLiteral("mcp_trace"))          return C::ProcessGlobal;
    if (toolName == QStringLiteral("token_usage"))        return C::ProcessGlobal;
    if (toolName == QStringLiteral("tab_list"))           return C::ProcessGlobal;
    if (toolName == QStringLiteral("get_session_info"))   return C::ProcessGlobal;
    // ANTS-1399 — tool_info reads the process-wide descriptor cache.
    if (toolName == QStringLiteral("tool_info"))          return C::ProcessGlobal;

    // ANTS-1424 — roadmap_log mutates ROADMAP.md + .roadmap-counter
    // under the caller's project root. Required-contract gated so
    // an absent caller_cwd refuses at the dispatcher rather than
    // falling back to the focused tab's roadmap.
    if (toolName == QStringLiteral("roadmap_log"))        return C::Required;
    // ANTS-3855 — roadmap_migrate loads ONE project into the roadmap store,
    // resolved from caller_cwd. A migration is a whole-project operation and
    // has nothing to fall back to when no project is named.
    if (toolName == QStringLiteral("roadmap_migrate"))    return C::Required;
    // ANTS-4622 — session_message resolves WHICH project's mailbox this
    // is from caller_cwd, on `inbox` and `ack` as much as on `send`. An
    // absent caller_cwd has no mailbox rather than a default one, so the
    // read ops are Required too.
    if (toolName == QStringLiteral("session_message"))    return C::Required;
    // ANTS-6310 — every op, as ANTS-6310 § 4.2 says: a write resolves its author
    // from caller_cwd, and a read refuses a cwd that does not resolve.
    if (toolName == QStringLiteral("know_how"))           return C::Required;
    // ANTS-5299 — run_trace writes and reads the caller's project trace
    // index; no project, nothing to act on.
    if (toolName == QStringLiteral("run_trace"))          return C::Required;
    // ANTS-5515 — dependency_inventory reads the caller's tracked files.
    if (toolName == QStringLiteral("dependency_inventory")) return C::Required;
    // ANTS-6399 — ci_report reads the caller's workflows and saved report.
    if (toolName == QStringLiteral("ci_report")) return C::Required;
    // ANTS-5510 — version_check reads the bump recipe and what it lists.
    if (toolName == QStringLiteral("version_check"))      return C::Required;
    // ANTS-5502 — quotation_check reads files under the caller's project root.
    if (toolName == QStringLiteral("quotation_check"))    return C::Required;
    // ANTS-5787 — function_sizes walks paths under the caller's project root.
    if (toolName == QStringLiteral("function_sizes"))     return C::Required;
    // ANTS-5602 — source_scrapes reads the caller's CMakeLists.txt and tests.
    if (toolName == QStringLiteral("source_scrapes"))     return C::Required;
    // ANTS-5786 — comment_claims walks paths under the caller's project root.
    if (toolName == QStringLiteral("comment_claims"))     return C::Required;
    // ANTS-5785 — duplicate_blocks walks paths under the caller's project root.
    if (toolName == QStringLiteral("duplicate_blocks"))   return C::Required;
    // ANTS-5698 — session_cost reads the transcript folder named by the cwd.
    if (toolName == QStringLiteral("session_cost"))       return C::Required;
    // ANTS-5734 — trace walks and parses the caller's project.
    if (toolName == QStringLiteral("trace"))              return C::Required;
    // ANTS-1548 — changelog_log mutates CHANGELOG.md under the caller's
    // project root. Required for the same reason as roadmap_log.
    if (toolName == QStringLiteral("changelog_log"))      return C::Required;
    // ANTS-1400 — caller_cwd_info is the diagnostic verb that
    // surfaces the resolution Source enum. caller_cwd is its
    // INPUT (not an anchor), so neither Required nor ProcessGlobal
    // is the right fit — Optional accepts the empty case (which
    // is the "what would happen without it?" question the verb
    // is built to answer).
    if (toolName == QStringLiteral("caller_cwd_info"))    return C::Optional;

    // ANTS-1520 — promoted from Optional to Required across the
    // project-scoped read and review surfaces. The mixed regime
    // (some tools Required, others Optional with focused-tab
    // fallback) added a per-call mental tax of "did I need it
    // this time?" and, worse, optional `caller_cwd` quietly
    // returned the wrong project's data when the focused Ants
    // tab differed from the caller's project. Uniform refusal
    // closes that silent-miss failure mode. The shared
    // `caller_cwd_required` refusal envelope is emitted by the
    // tools/call dispatcher at the same site as ANTS-1404
    // (no per-tool wiring needed).
    if (toolName == QStringLiteral("roadmap_query"))      return C::Required;
    if (toolName == QStringLiteral("changelog_query"))    return C::Required;  // ANTS-3533
    if (toolName == QStringLiteral("subsystem"))          return C::Required;
    if (toolName == QStringLiteral("workspace_search"))   return C::Required;
    // ANTS-1636 — find_sources: project-scoped topic-to-files scan.
    if (toolName == QStringLiteral("find_sources"))       return C::Required;
    // ANTS-3368 — co_change_family: project-scoped repo-wide field scan.
    if (toolName == QStringLiteral("co_change_family"))   return C::Required;
    if (toolName == QStringLiteral("file_outline"))       return C::Required;
    if (toolName == QStringLiteral("mutation_probe"))     return C::Required;  // ANTS-4398
    // ANTS-1735 — model_switch_stats: read-only ledger aggregation scoped to the
    // caller's project. Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("model_switch_stats"))  return C::Required;
    // ANTS-1309 + ANTS-1308 — spec-aware MCP tools. Read under
    // docs/specs/ which lives strictly inside the project root, so
    // Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("spec_query"))         return C::Required;
    if (toolName == QStringLiteral("invariant_check"))    return C::Required;
    // ANTS-1299 + ANTS-1300 — build/test cache MCP tools. Both ops
    // (record + read) scope to <root>/.audit_cache/; Required matches
    // sibling project-scoped tools.
    if (toolName == QStringLiteral("build_status"))       return C::Required;
    if (toolName == QStringLiteral("test_results"))       return C::Required;
    // ANTS-1302 — focused_test reads/runs under the caller's project root.
    if (toolName == QStringLiteral("focused_test"))       return C::Required;
    // ANTS-3745 — build_target_for parses the caller's own CMakeLists.txt.
    if (toolName == QStringLiteral("build_target_for"))   return C::Required;
    // ANTS-1303 — symbol queries scan the project tree under the
    // caller's root; Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("find_definition"))    return C::Required;
    if (toolName == QStringLiteral("find_caller"))        return C::Required;
    // ANTS-1305 — similar_code scans the project tree under the
    // caller's root; Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("similar_code"))        return C::Required;
    // ANTS-3716 — cited_by searches the caller's doc tree; Required, same as
    // workspace_search, whose rg runner it shares.
    if (toolName == QStringLiteral("cited_by"))            return C::Required;
    // ANTS-1306 + ANTS-1307 — task-start context composers read under
    // the caller's project root; Required matches sibling readers.
    if (toolName == QStringLiteral("task_priors"))        return C::Required;
    if (toolName == QStringLiteral("project_conventions")) return C::Required;
    // Cold-eyes verb cluster (ANTS-1313).
    if (toolName == QStringLiteral("cold_eyes_brief"))         return C::Required;
    if (toolName == QStringLiteral("cold_eyes_partition"))     return C::Required;
    // Indie-review verb cluster (ANTS-1311).
    if (toolName == QStringLiteral("indie_review_corroborate"))      return C::Required;
    if (toolName == QStringLiteral("indie_review_partition"))        return C::Required;
    // Test-audit verb cluster (ANTS-1397).
    if (toolName == QStringLiteral("test_audit_brief"))            return C::Required;
    if (toolName == QStringLiteral("test_audit_partition"))        return C::Required;

    // Unclassified — fall through. Future tools should be added
    // above; the ANTS-1417 coverage test fails the build if a
    // new registerToolProvider call has no matching branch here.
    // ANTS-1520 also flips the fall-through default to Required
    // so new tools fail-closed: any new registration that didn't
    // get an explicit classification refuses without caller_cwd
    // (loud, contributor-visible) instead of silently anchoring
    // to the focused tab.
    return C::Required;
}

// --- ANTS-1357 — MCP idempotent-read cache helpers ---

bool ClaudeIntegration::isIdempotentReadTool(const QString &toolName) {
    // Allowlist enforced at lookup AND insert (INV-4). Hardcoded; new
    // tools must opt in explicitly to make caching-eligibility a
    // deliberate decision per tool.
    return toolName == QStringLiteral("get_cwd")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("last_audit_summary");
}

bool ClaudeIntegration::isEtagSupportedTool(const QString &toolName) {
    // ANTS-1499 — read tools that return JSON envelopes large enough
    // for the etag short-circuit to be worth it. Order mirrors the
    // priority list in the roadmap entry.
    return toolName == QStringLiteral("project_layout")
        || toolName == QStringLiteral("roadmap_query")
        || toolName == QStringLiteral("changelog_query")   // ANTS-3533
        || toolName == QStringLiteral("file_outline")
        // ANTS-2021 — read_region: a re-read of an unchanged slice 304s,
        // the core "free re-read" win.
        || toolName == QStringLiteral("read_region")
        // ANTS-1637 — codebase_index: an unchanged warm query 304s.
        || toolName == QStringLiteral("codebase_index")
        // ANTS-2139 — docs_index: an unchanged warm query 304s.
        || toolName == QStringLiteral("docs_index")
        // ANTS-3601 — doc_integrity: unchanged docs → identical findings → 304.
        || toolName == QStringLiteral("doc_integrity")
        // ANTS-3636 — doc_citations: unchanged doc + unchanged targets → an
        // identical answer → 304.
        || toolName == QStringLiteral("doc_citations")
        // ANTS-3716 — cited_by: a pure function of (tree, request), and its
        // arrays all carry a stated order precisely so the hash is stable.
        || toolName == QStringLiteral("cited_by")
        // ANTS-3368 — co_change_family: a pure function of (tree, stems), and
        // INV-6 pins a total order on files and sites, so the hash is stable.
        || toolName == QStringLiteral("co_change_family")
        || toolName == QStringLiteral("doc_symbols")  // ANTS-3661
        || toolName == QStringLiteral("spec_lint")    // ANTS-3662
        || toolName == QStringLiteral("doc_dedup")    // ANTS-3660
        || toolName == QStringLiteral("last_audit_summary")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("subsystem")
        || toolName == QStringLiteral("git_state")
        // ANTS-1735 — model_switch_stats: the aggregate is stable between
        // switches, so a session re-polling its scorecard short-circuits.
        || toolName == QStringLiteral("model_switch_stats")
        // ANTS-1883 — session_orient: composer over three ETag-
        // eligible verbs, naturally ETag-eligible too.
        || toolName == QStringLiteral("session_orient")
        // ANTS-1299 + ANTS-1300 — build/test caches. The envelope on
        // op=read is stable between record calls, so re-reads from
        // peer sessions / tabs short-circuit. Etag injection is
        // op-agnostic at the dispatcher (record responses also carry
        // an etag); the field is only semantically useful on op=read.
        || toolName == QStringLiteral("build_status")
        || toolName == QStringLiteral("test_results")
        // ANTS-1961 — feedback_query: the delta is stable between
        // contributor appends, so a session re-querying the same file in
        // one review short-circuits. The etag is the sha256 of the
        // envelope (which carries the file-derived delta), so it changes
        // iff the file content changes. (feedback_log / spec_log are
        // writers — deliberately NOT etag-enabled.)
        || toolName == QStringLiteral("feedback_query");
}

QString ClaudeIntegration::etagFor(const QString &payload) {
    // Same hex16-of-sha256 shape as idempotentReadCacheKey for
    // operational sanity (one hash format across MCP internals).
    return QString::fromUtf8(QCryptographicHash::hash(
        payload.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString ClaudeIntegration::applyEtagPattern(const QString &toolName,
                                            const QJsonObject &args,
                                            const QString &responseText,
                                            bool *unchanged) {
    if (unchanged) *unchanged = false;
    if (!isEtagSupportedTool(toolName)) return responseText;

    // ANTS-4446 — parse BEFORE the 304 arm, which otherwise speaks for a body
    // it has not read. `mcp-tools.md` § ETag-304: "a refusal envelope is
    // never short-circuited."
    //
    // Two defects, one root, both closed by this ordering. The short-circuit
    // built {ok:true, unchanged:true, etag} without looking at responseText,
    // so a caller whose cached etag matched was told a just-refused call had
    // succeeded. The tail below then attached an etag to any parseable object
    // regardless of `ok` — which is what put a replayable etag in that
    // caller's hands, since every etag-supported verb tells it to cache the
    // value and send it back.
    //
    // Withholding the etag is the half that matters: a caller can only replay
    // what it was given. Both are kept anyway — the sibling read cache
    // (maybeInsertIdempotentReadCache, INV-5(a)/(b)) guards its own store
    // rather than trusting its callers.
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &err);
    const bool isJsonObject =
        (err.error == QJsonParseError::NoError && doc.isObject());

    // The refusal test is mcp::projectFields' verbatim (ANTS-2112): `ok`
    // present and false. An envelope with no `ok` key is NOT a refusal —
    // if these two ever disagree, one is narrowing an error envelope the
    // other is protecting.
    if (isJsonObject) {
        const QJsonObject probe = doc.object();
        if (probe.contains(QStringLiteral("ok")) &&
            !probe.value(QStringLiteral("ok")).toBool()) {
            return responseText;
        }
    }

    const QString etag  = etagFor(responseText);
    const QString match = args.value(QStringLiteral("etag_match"))
        .toString();
    if (!match.isEmpty() && match == etag) {
        QJsonObject env;
        env[QStringLiteral("ok")]        = true;
        env[QStringLiteral("unchanged")] = true;
        env[QStringLiteral("etag")]      = etag;
        if (unchanged) *unchanged = true;
        return QString::fromUtf8(
            QJsonDocument(env).toJson(QJsonDocument::Compact));
    }
    if (!isJsonObject) {
        // Provider returned a non-JSON payload — return as-is.
        // ETag injection only applies to JSON-envelope tools.
        return responseText;
    }
    QJsonObject obj = doc.object();
    obj[QStringLiteral("etag")] = etag;
    return QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString ClaudeIntegration::idempotentReadCacheKey(
    const QString &toolName, const QJsonObject &args) {
    // INV-9 — args are part of the key. QJsonObject::toJson(Compact)
    // emits keys lexicographically (Qt6 internal), so the bytes are
    // stable across runs for the same arg set.
    QByteArray buf = toolName.toUtf8();
    buf.append('\0');
    buf.append(QJsonDocument(args).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QCryptographicHash::hash(
        buf, QCryptographicHash::Sha256).toHex().left(16));
}

QString ClaudeIntegration::tryGetIdempotentReadCache(
    const QString &toolName, const QJsonObject &args) const {
    if (!isIdempotentReadTool(toolName)) return QString();
    const QString key = idempotentReadCacheKey(toolName, args);
    auto it = m_idempotentReadCache.find(key);
    if (it == m_idempotentReadCache.end()) return QString();
    const qint64 nowMs = monotonicNowMs();
    if (nowMs - it->stampMs > kIdempotentReadTtlMs) {
        // INV-2 — entries past TTL are treated as miss (the next
        // insert will overwrite).
        return QString();
    }
    // INV-3 LRU bump.
    m_idempotentReadLru.removeOne(key);
    m_idempotentReadLru.prepend(key);
    return it->response;
}

void ClaudeIntegration::maybeInsertIdempotentReadCache(
    const QString &toolName, const QJsonObject &args,
    const QString &response) {
    // INV-4 — allowlist check at insert.
    if (!isIdempotentReadTool(toolName)) return;
    // INV-5(a) — empty response = "couldn't answer". Don't cache.
    if (response.isEmpty()) return;
    // INV-5(b) — kRcUnavailable = transient startup window. Don't cache.
    if (response == QString::fromUtf8(kMcpRcUnavailable)) return;
    const QString key = idempotentReadCacheKey(toolName, args);
    m_idempotentReadCache.insert(key,
        IdempotentReadEntry{monotonicNowMs(), response});
    m_idempotentReadLru.removeOne(key);
    m_idempotentReadLru.prepend(key);
    while (m_idempotentReadLru.size() > kIdempotentReadCacheCap) {
        const QString evicted = m_idempotentReadLru.takeLast();
        m_idempotentReadCache.remove(evicted);
    }
}

// --- ANTS-1356 — MCP per-tool rate-limit / quota ---

namespace {
// Test-only cap overrides. Defaulted to -1 = "use compile-time
// constants" (kRateLimitCheapCap / kRateLimitBriefAssemblyCap /
// kRateLimitExpensiveCap). The `*ForTest` setters write here;
// production callers never touch it.
int g_rateLimitCheapCapOverride         = -1;
int g_rateLimitBriefAssemblyCapOverride = -1;
int g_rateLimitExpensiveCapOverride     = -1;
}  // namespace

ClaudeIntegration::RateLimitClass
ClaudeIntegration::rateLimitClassFor(const QString &toolName) {
    using R = RateLimitClass;
    // ControlPlane — uncapped. Pure reads of server-owned state +
    // diagnostic verbs. Spec INV-13.
    if (toolName == QStringLiteral("get_session_info"))   return R::ControlPlane;
    if (toolName == QStringLiteral("token_usage"))        return R::ControlPlane;
    if (toolName == QStringLiteral("tool_info"))          return R::ControlPlane;
    if (toolName == QStringLiteral("mcp_trace"))          return R::ControlPlane;
    if (toolName == QStringLiteral("caller_cwd_info"))    return R::ControlPlane;
    // ANTS-1629 + ANTS-1643 — BriefAssembly tier (30/min). Brief-
    // generation verbs that fan out one-per-lane during a review sweep,
    // which the 10/min Expensive cap was blocking. Cost shape is
    // read-and-template-assembly, not shell-out — between Cheap and
    // Expensive. ANTS-1643 added test_audit_brief.
    if (toolName == QStringLiteral("cold_eyes_brief"))          return R::BriefAssembly;
    if (toolName == QStringLiteral("test_audit_brief"))         return R::BriefAssembly;
    // ANTS-5112 — review lanes fan workspace_search out the same way, and
    // every lane of one project shares the bucket, so at 10/min most were
    // refused and fell back to raw Grep. Since ANTS-4932 it runs in
    // ants-mcpd, not on the terminal's GUI thread.
    if (toolName == QStringLiteral("workspace_search"))         return R::BriefAssembly;
    // Expensive — 10/min. Heavy verbs (shell-out, subagent dispatch,
    // cmake/ctest, full-corpus scan).
    if (toolName == QStringLiteral("audit_run"))                return R::Expensive;
    if (toolName == QStringLiteral("verify_changes"))           return R::Expensive;
    // ANTS-1302 — focused_test shells out to ctest.
    if (toolName == QStringLiteral("focused_test"))             return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_partition"))      return R::Expensive;
    // ANTS-1643: test_audit_brief moved to BriefAssembly above. Its
    // sibling partition / corroborate / dispatch verbs stay Expensive
    // (heavier cost shapes — subagent dispatch, multi-doc scan).
    if (toolName == QStringLiteral("indie_review_partition"))   return R::Expensive;
    if (toolName == QStringLiteral("indie_review_corroborate")) return R::Expensive;
    // ANTS-1352: indie_review_dispatch
    if (toolName == QStringLiteral("indie_review_dispatch"))    return R::Expensive;
    if (toolName == QStringLiteral("test_audit_partition"))     return R::Expensive;
    // ANTS-6399 — ci_report runs gh, up to 300 calls a live run.
    if (toolName == QStringLiteral("ci_report"))                return R::Expensive;
    // Default — Cheap (60/min). All read verbs not classified above.
    return R::Cheap;
}

ClaudeIntegration::RateLimitTier
ClaudeIntegration::rateLimitTierFor(RateLimitClass cls) {
    switch (cls) {
        case RateLimitClass::ControlPlane:
            return RateLimitTier{INT_MAX, kRateLimitWindowMs};
        case RateLimitClass::Expensive: {
            const int cap = (g_rateLimitExpensiveCapOverride >= 0)
                ? g_rateLimitExpensiveCapOverride
                : kRateLimitExpensiveCap;
            return RateLimitTier{cap, kRateLimitWindowMs};
        }
        case RateLimitClass::BriefAssembly: {
            const int cap = (g_rateLimitBriefAssemblyCapOverride >= 0)
                ? g_rateLimitBriefAssemblyCapOverride
                : kRateLimitBriefAssemblyCap;
            return RateLimitTier{cap, kRateLimitWindowMs};
        }
        case RateLimitClass::Cheap:
        default: {
            const int cap = (g_rateLimitCheapCapOverride >= 0)
                ? g_rateLimitCheapCapOverride
                : kRateLimitCheapCap;
            return RateLimitTier{cap, kRateLimitWindowMs};
        }
    }
}

// ANTS-1771 — canonicalise a caller_cwd into a stable rate-limit bucket
// key. Existing paths resolve via canonicalFilePath (symlink + `.`/`..`
// + case on case-insensitive mounts); non-existent paths fall back to
// QDir::cleanPath (lexical-only) so synonyms still collapse. Empty in →
// empty out (the "absent caller_cwd" bucket).
QString ClaudeIntegration::canonicaliseCallerKey(const QString &callerCwd) {
    if (callerCwd.isEmpty()) return QString();
    const QString resolved = QFileInfo(callerCwd).canonicalFilePath();
    if (!resolved.isEmpty()) return resolved;
    return QDir::cleanPath(callerCwd);
}

qint64 ClaudeIntegration::rateLimitCheck(
    const QString &toolName, const QString &callerCwd, qint64 nowMs) {
    const RateLimitClass cls = rateLimitClassFor(toolName);
    if (cls == RateLimitClass::ControlPlane) return 0;
    const RateLimitTier tier = rateLimitTierFor(cls);

    // indie-review-2026-05-19 claudeintegration H1 + ANTS-1771 —
    // canonicalise the bucket key. Raw callerCwd values (`/foo`,
    // `/foo/`, `/foo/./`, `/foo//bar/..`, case-variants on
    // case-insensitive mounts) would otherwise produce distinct map
    // entries and let a same-UID peer multiply their effective budget
    // by routing through synonyms.
    //
    // ANTS-1771 — QFileInfo::canonicalFilePath returns "" for a
    // non-existent path; the pre-fix code fell back to the RAW string
    // there, so two non-existent synonyms (`/nope` vs `/nope/.`) still
    // landed in distinct buckets. We now fall back to QDir::cleanPath
    // (lexical normalisation: collapses `.`/`..`/`//`/trailing slash)
    // so the budget-multiplication hole closes for non-existent callers
    // too. The empty-string ("absent caller_cwd") bucket stays empty.
    //
    // Note: the tab-ROUTING path (ants::resolveCallerCwdRoot) is
    // deliberately NOT unified with this helper — routing must REJECT a
    // non-existent path (no tab can match it) rather than lexically
    // coerce it, so it keeps the existence-only canonicalFilePath rule.
    //
    // ANTS-5090 — memoise the canonical key per raw caller_cwd for
    // kCallerKeyMemoTtlMs, so a repeat call skips the stat on the GUI
    // thread. A stalled mount still blocks the first call. Each raw value
    // still maps to exactly one key, so the synonym collapse above holds.
    QString canon;
    const auto memo = m_callerKeyMemo.constFind(callerCwd);
    if (memo != m_callerKeyMemo.cend() &&
        nowMs - memo->stampMs < kCallerKeyMemoTtlMs) {
        canon = memo->canon;
    } else {
        canon = canonicaliseCallerKey(callerCwd);
        if (m_callerKeyMemo.size() >= kCallerKeyMemoCap)
            m_callerKeyMemo.clear();
        m_callerKeyMemo.insert(callerCwd, CallerKeyMemo{canon, nowMs});
    }
    const QPair<QString, QString> key{toolName, canon};
    auto it = m_rateLimitBuckets.find(key);

    // Map-cap LRU eviction (INV-7). Only when inserting a NEW key
    // at the cap; existing-key updates don't grow the map.
    if (it == m_rateLimitBuckets.end() &&
        m_rateLimitBuckets.size() >= kRateLimitMapCap) {
        // Linear scan: evict bucket with the oldest most-recent
        // timestamp (LRU-by-last-call). Bounded cost (256 ≤ 256).
        QPair<QString, QString> evictKey;
        qint64 evictMostRecent = std::numeric_limits<qint64>::max();
        for (auto cit = m_rateLimitBuckets.constBegin();
             cit != m_rateLimitBuckets.constEnd(); ++cit) {
            const qint64 mostRecent =
                cit->tsMs.isEmpty() ? 0 : cit->tsMs.back();
            if (mostRecent < evictMostRecent) {
                evictMostRecent = mostRecent;
                evictKey = cit.key();
            }
        }
        m_rateLimitBuckets.remove(evictKey);
    }

    RateLimitBucket &bucket = m_rateLimitBuckets[key];

    // Prune front: drop entries that fell out of the sliding window.
    while (!bucket.tsMs.isEmpty() &&
           bucket.tsMs.front() < nowMs - tier.windowMs) {
        bucket.tsMs.dequeue();
    }

    // INV-6 — empty bucket auto-evicts. Insert nowMs into a fresh
    // entry and return accept. (We re-fetch the bucket reference
    // after the remove + re-insert because QHash may rehash.)
    if (bucket.tsMs.isEmpty()) {
        m_rateLimitBuckets.remove(key);
        RateLimitBucket &fresh = m_rateLimitBuckets[key];
        fresh.tsMs.enqueue(nowMs);
        return 0;
    }

    // Under cap — accept and append.
    if (bucket.tsMs.size() < tier.capPerWindow) {
        bucket.tsMs.enqueue(nowMs);
        return 0;
    }

    // At cap — refuse. retry_after_ms = (oldest + window) - now,
    // floored at 1 (callers expect a positive sentinel).
    const qint64 retryAfter = (bucket.tsMs.front() + tier.windowMs) - nowMs;
    return retryAfter > 0 ? retryAfter : 1;
}

void ClaudeIntegration::setRateLimitCapsForTest(int cheap, int expensive) {
    g_rateLimitCheapCapOverride     = cheap;
    g_rateLimitExpensiveCapOverride = expensive;
}

// ANTS-1629 — additive overload so tests can drive the new
// BriefAssembly tier without disturbing existing two-arg callers.
void ClaudeIntegration::setRateLimitCapsForTest(
    int cheap, int briefAssembly, int expensive) {
    g_rateLimitCheapCapOverride         = cheap;
    g_rateLimitBriefAssemblyCapOverride = briefAssembly;
    g_rateLimitExpensiveCapOverride     = expensive;
}

void ClaudeIntegration::resetRateLimitCapsForTest() {
    g_rateLimitCheapCapOverride         = -1;
    g_rateLimitBriefAssemblyCapOverride = -1;
    g_rateLimitExpensiveCapOverride     = -1;
}
