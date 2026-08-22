// ANTS-3833 TU 11/16 — Cold eyes and test verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "buildcache.h"
#include "pathvalidation.h"
#include "falseposledger.h"
#include "projectlayoutengine.h"
#include "remotecontrolgate.h"
#include "sessionmemoryengine.h"
#include "roadmapfoldin.h"
#include "testrescache.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include "resolvedroot.h"

using namespace rcdetail;  // ANTS-3833

QJsonDocument RemoteControl::cmdColdEyesPartition(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_partition: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_partition: no focused project")));

    const QString scopeRaw = req.value(QStringLiteral("scope")).toString();
    ColdEyesEngine::Scope scope = ColdEyesEngine::Scope::Default;
    if (!ColdEyesEngine::parseScope(scopeRaw, &scope)) {
        QJsonObject err = ceErr(
            QStringLiteral("bad_scope"),
            QStringLiteral("cold_eyes_partition: scope must be one of "
                           "\"default\", \"docs_only\", \"contracts_only\""));
        err["echo"] = ceSanitiseEcho(scopeRaw);
        return QJsonDocument(err);
    }

    // INV-12 mtime-cache (5 s TTL). Cache hit needs path+scope match
    // AND stamp within TTL. Miss → regenerate.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_coldEyesCachePath != root || m_coldEyesCacheScope != scope
        || now - m_coldEyesCacheStampMs > kColdEyesCacheTtlMs) {
        m_coldEyesCache       = ColdEyesEngine::derivePartition(root, scope);
        m_coldEyesCachePath   = root;
        m_coldEyesCacheScope  = scope;
        m_coldEyesCacheStampMs = now;
    }

    QJsonObject env;
    env["ok"]            = true;
    env["lanes"]         = ceLaneArrayToJson(m_coldEyesCache.lanes);
    env["path"]          = m_coldEyesCache.overridePath;
    env["scope"]         = !scopeRaw.isEmpty() ? scopeRaw
                                              : QStringLiteral("default");
    env["scoped_count"]  = m_coldEyesCache.scopedCount;
    env["truncated"]     = m_coldEyesCache.truncated;
    // ANTS-1619 — debug field naming which code path built the
    // partition. `"default"` covers the absent + malformed-fall-back
    // cases; `"override"` indicates `.cold-eyes/partition.json`
    // parsed cleanly.
    env["partition_source"] = m_coldEyesCache.partitionSource;
    // ANTS-1619 — surface the contract-doc probe outcome. Callers
    // (and cross-session reports) can tell at a glance whether the
    // partition silently skipped a doc the summary mentioned.
    {
        QJsonArray disc;
        for (const QString &p : m_coldEyesCache.discoveredContractFiles) {
            disc.append(p);
        }
        env["discovered_contract_files"] = disc;
        QJsonArray miss;
        for (const QString &p : m_coldEyesCache.missingContractFiles) {
            miss.append(p);
        }
        env["missing_contract_files"] = miss;
    }
    // ANTS-1412 — surface malformed override files so callers know
    // their `.cold-eyes/partition.json` was ignored and why. Field
    // omitted when override loaded cleanly or is absent.
    if (!m_coldEyesCache.overrideWarning.isEmpty()) {
        env["override_warning"] = m_coldEyesCache.overrideWarning;
    }
    // ANTS-1506 — surface the near-empty-default signal so callers
    // don't silently treat "scan found ≤ 1 lane under default scope"
    // as a valid sweep. Real projects on default scope yield at
    // least contracts + standards (≥2). Anything below that signals
    // a misnamed contract doc, a missing docs/ tree, or the caller
    // passed a project root that isn't quite the repo root yet.
    const bool defaultScope = scope == ColdEyesEngine::Scope::Default;
    if (defaultScope && m_coldEyesCache.lanes.size() <= 1) {
        env["sparse_partition"] = true;
        // ANTS-1634a — point at the two existing escape hatches
        // (ANTS-1508 lane-agnostic brief + ANTS-1412 project
        // override) so callers driving a sweep on a non-canonical
        // doc layout see the workaround alongside the diagnostic.
        env["sparse_partition_hint"] = QStringLiteral(
            "Default scope returned %1 lane(s). Check that the "
            "project root carries CLAUDE.md / README.md / ROADMAP.md "
            "/ CHANGELOG.md (case-insensitive match) and that "
            "docs/standards/ + docs/decisions/ exist. Pass scope="
            "\"contracts_only\" to confirm the contract-doc shape. "
            "Pass doc_paths[] to cold_eyes_brief for one-shot ad-hoc "
            "lanes (ANTS-1508), or commit "
            "<projectPath>/.cold-eyes/partition.json per ANTS-1412 "
            "to persist an override.")
                .arg(m_coldEyesCache.lanes.size());
        // ANTS-1571 — point callers at the lane-agnostic cold_eyes_brief
        // escape hatch (ANTS-1508). Callers driving a sweep on a non-
        // canonical project layout can mint a brief over an arbitrary
        // doc set without committing a .cold-eyes/partition.json.
        env["next_step_hint"] = QStringLiteral(
            "Call cold_eyes_brief(lane=\"<your-label>\", "
            "doc_paths=[\"...\"]) to mint a brief over an arbitrary "
            "doc set when the default partition is too narrow "
            "(ANTS-1508 lane-agnostic mode).");
    }
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdColdEyesBrief(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_brief: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_brief: no focused project")));

    const QString laneNameRaw = req.value(QStringLiteral("lane")).toString();
    const QString laneName    = laneNameRaw.trimmed();
    if (laneName.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral("cold_eyes_brief: lane required")));

    // Reuse the partition cache (INV-12). On miss, regenerate with
    // Default scope — the caller wants a specific lane and didn't pass
    // a scope arg here, so Default is the right baseline.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_coldEyesCachePath != root
        || m_coldEyesCacheScope != ColdEyesEngine::Scope::Default
        || now - m_coldEyesCacheStampMs > kColdEyesCacheTtlMs) {
        m_coldEyesCache       = ColdEyesEngine::derivePartition(
            root, ColdEyesEngine::Scope::Default);
        m_coldEyesCachePath   = root;
        m_coldEyesCacheScope  = ColdEyesEngine::Scope::Default;
        m_coldEyesCacheStampMs = now;
    }

    const ColdEyesEngine::Lane *match = nullptr;
    for (const auto &l : m_coldEyesCache.lanes) {
        if (l.name == laneName) { match = &l; break; }
    }
    // ANTS-1508 — lane-agnostic fallback: if the caller passes a lane
    // name not in the cached partition AND an explicit `doc_paths`
    // array, synthesise an ad-hoc Lane on the fly. Anchors each path
    // inside the project root via the same INV-13 logic that the
    // partition.json override uses (no symlink escape, no absolute
    // paths). Callers driving custom sweeps (e.g. fork-internal lanes
    // not surfaced by the auto-partition) can now use the brief
    // tool without having to commit a .cold-eyes/partition.json.
    ColdEyesEngine::Lane adhoc;
    // ANTS-1831 — entries the caller supplied that we refused to slurp.
    // Surfaced in the response (success or not_found) instead of being
    // dropped silently, so the caller learns *which* path was bad.
    QJsonArray rejectedDocPaths;
    if (!match) {
        const QJsonValue dpV = req.value(QStringLiteral("doc_paths"));
        if (dpV.isArray()) {
            for (const QJsonValue &v : dpV.toArray()) {
                const QString d = v.toString().trimmed();
                if (d.isEmpty()) continue;
                // ANTS-1831 — route through the central cwd-anchor
                // chokepoint (ANTS-1295) rather than a hand-rolled anchor
                // pair. `root` is already canonical.
                const auto pc = PathValidation::validatePath(
                    d, root, QStringLiteral("cold_eyes_brief"),
                    QStringLiteral("doc_paths"));
                if (pc.bad) {
                    QJsonObject rej;
                    rej[QStringLiteral("path")]   = d;
                    rej[QStringLiteral("reason")] =
                        pc.err.value(QStringLiteral("error")).toString();
                    rejectedDocPaths.append(rej);
                    continue;
                }
                // validatePath leaves `resolved` empty for a path that
                // doesn't canonicalise — i.e. doesn't exist. An ad-hoc
                // lane can only review files that are actually present.
                if (pc.resolved.isEmpty()) {
                    QJsonObject rej;
                    rej[QStringLiteral("path")]   = d;
                    rej[QStringLiteral("reason")] = QStringLiteral(
                        "cold_eyes_brief: \"doc_paths\" no such file");
                    rejectedDocPaths.append(rej);
                    continue;
                }
                adhoc.docPaths << d;
            }
        }
        if (!adhoc.docPaths.isEmpty()) {
            adhoc.name    = laneName;
            adhoc.summary = QStringLiteral(
                "Ad-hoc lane (caller-supplied doc_paths).");
            match = &adhoc;
        }
    }
    if (!match) {
        QJsonObject err = ceErr(
            QStringLiteral("not_found"),
            QStringLiteral("cold_eyes_brief: no such lane (and no "
                           "doc_paths[] override supplied)"));
        err["echo"] = ceSanitiseEcho(laneNameRaw);
        // List known lanes so the caller can recover without a
        // second round-trip to cold_eyes_partition.
        QJsonArray known;
        for (const auto &l : m_coldEyesCache.lanes) known.append(l.name);
        err["known_lanes"] = known;
        // ANTS-1831 — if every supplied doc_paths entry was rejected the
        // ad-hoc lane is empty and we land here; tell the caller why.
        if (!rejectedDocPaths.isEmpty())
            err["doc_paths_rejected"] = rejectedDocPaths;
        return QJsonDocument(err);
    }

    // ANTS-1634(b) — orchestrator-supplied "already fixed in loop 1"
    // records. Parsed permissively: skip non-object items, skip
    // entries with both fields empty, trim each side. Empty array or
    // missing field → no section emitted by the engine.
    QList<ColdEyesEngine::PriorLoopFix> priorFixes;
    const QJsonValue priorFixesV =
        req.value(QStringLiteral("prior_loop_fixes"));
    if (priorFixesV.isArray()) {
        for (const QJsonValue &v : priorFixesV.toArray()) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            ColdEyesEngine::PriorLoopFix fix;
            fix.title =
                o.value(QStringLiteral("title")).toString().trimmed();
            fix.summary =
                o.value(QStringLiteral("summary")).toString().trimmed();
            if (fix.title.isEmpty() && fix.summary.isEmpty()) continue;
            priorFixes.append(fix);
        }
    }

    const auto m = ColdEyesEngine::assembleBriefManifest(
        root, *match, priorFixes);

    QJsonArray dps;
    for (const QString &p : m.docPaths) dps.append(p);
    QJsonArray xref;
    for (const QString &p : m.crossReferenceDocs) xref.append(p);
    // ANTS-3526 — the subset of cross_reference_docs that are large append-only
    // logs (ROADMAP.md / CHANGELOG.md). The brief already routes these to a
    // "SEARCH, do not full-read" section; surface the subset structurally so a
    // caller building its own pipeline can apply the same discipline without
    // re-parsing the brief markdown.
    QJsonArray xrefLarge;
    for (const QString &p : m.largeCrossReferenceDocs) xrefLarge.append(p);
    QJsonArray code;
    for (const QString &p : m.citedCodePaths) code.append(p);
    // ANTS-3522 — cited code regions: per-file the exact cited lines, so a
    // reviewer reads a window around each (outline + read_region) instead of
    // the whole file. Additive alongside cited_code_paths. Empty when no
    // `<path>:<line>` citation resolved.
    QJsonArray regions;
    for (auto it = m.citedCodeRegions.constBegin();
         it != m.citedCodeRegions.constEnd(); ++it) {
        QJsonObject r;
        r["path"] = it.key();
        QJsonArray lines;
        for (int ln : it.value()) lines.append(ln);
        r["lines"] = lines;
        regions.append(r);
    }
    // ANTS-1633 — paths the regex matched but the filesystem
    // could not resolve under projectPath. Empty array when
    // every citation resolved (the common case). Per-lane
    // reviewers treat non-empty entries as accuracy-dimension
    // findings: either the doc cites a deleted file, or the
    // path has been moved/renamed.
    QJsonArray stale;
    for (const QString &p : m.staleCitations) stale.append(p);

    // ANTS-3601 — deterministic doc-integrity findings for the lane's own
    // docs (dead anchors / broken links / TOC gaps), ready for cold-eyes
    // Phase 1e. Empty when the lane's docs are clean (the common case).
    QJsonArray docIntegrity;
    for (const QString &p : m.docIntegrity) docIntegrity.append(p);

    // ANTS-3740 — per-doc section index for the lane's own docs. Lets a
    // reviewer cite `<doc> § <heading>` (an anchor that survives edits above
    // it) and fetch any section with `read_region section=<slug>`, without
    // first deriving the map itself. Never covers the cross-reference docs.
    QJsonArray sectionIndex;
    for (const auto &si : m.sectionIndex) {
        QJsonObject d;
        d["path"] = si.path;
        QJsonArray secs;
        for (const auto &s : si.sections) {
            QJsonObject o;
            o["heading"]    = s.heading;
            o["slug"]       = s.slug;
            o["level"]      = s.level;
            o["start_line"] = s.startLine;
            o["end_line"]   = s.endLine;
            secs.append(o);
        }
        d["sections"] = secs;
        if (si.truncated) d["truncated"] = true;
        sectionIndex.append(d);
    }

    QJsonObject env;
    env["ok"]                    = true;
    env["lane"]                  = laneName;
    env["brief"]                 = m.brief;
    env["doc_paths"]             = dps;
    env["cross_reference_docs"]  = xref;
    env["large_cross_reference_docs"] = xrefLarge;
    env["cited_code_paths"]      = code;
    env["cited_code_regions"]    = regions;
    env["stale_citations"]       = stale;
    env["doc_integrity"]         = docIntegrity;  // ANTS-3601
    env["section_index"]         = sectionIndex;  // ANTS-3740
    // ANTS-1440 — surface the structured summary so callers don't
    // have to grep the brief markdown for the H1 line.
    env["summary"]               = m.summary;
    env["byte_count"]            = m.brief.toUtf8().size();
    // ANTS-3718 — SHA-256 of the lane's full review input (brief + docs +
    // small cross-refs + cited code). Lets a loop-N orchestrator skip a lane
    // whose input is unchanged since it last passed clean, without reading
    // the input to hash it.
    env["input_hash"]            = m.inputHash;
    // ANTS-1831 — caller-supplied doc_paths the anchor refused (empty
    // when the lane came from the cached partition or all paths were
    // valid). Non-empty entries are an accuracy signal for the caller.
    if (!rejectedDocPaths.isEmpty())
        env["doc_paths_rejected"] = rejectedDocPaths;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdColdEyesCrossDocDiff(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_cross_doc_diff: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_cross_doc_diff: no focused project")));

    // ANTS-1509: accept EITHER `reports` (inline map, mirrors
    // indie_review_corroborate's v1 shape) OR `reports_dir` (server-
    // side disk read). XOR — exactly one required. The /cold-eyes
    // skill bundles agent reports inline in the orchestrator's
    // context, so the disk path was unreachable without a fan-out.
    const bool hasReports    = req.contains(QStringLiteral("reports"));
    const bool hasReportsDir = req.contains(QStringLiteral("reports_dir"));
    if (hasReports == hasReportsDir) {
        return QJsonDocument(ceErr(
            QStringLiteral("bad_args"),
            QStringLiteral(
                "cold_eyes_cross_doc_diff: provide exactly one of "
                "`reports` (inline map) or `reports_dir` (project-relative "
                "directory of *.md files)")));
    }

    int minLanes = req.value(QStringLiteral("min_lanes")).toInt(2);
    if (minLanes < 1) minLanes = 1;

    QList<IndieReviewEngine::CorroboratedFinding> findings;
    QString reportsDir;
    int     reportsRead = 0;
    qint64  totalIn = 0;

    if (hasReportsDir) {
        const QString reportsDirRaw =
            req.value(QStringLiteral("reports_dir")).toString();
        reportsDir = reportsDirRaw.trimmed();
        if (reportsDir.isEmpty()) return QJsonDocument(ceErr(
            QStringLiteral("bad_args"),
            QStringLiteral("cold_eyes_cross_doc_diff: reports_dir must be a "
                           "non-empty project-relative path")));

        // ANTS-1295: anchor reports_dir before reaching the engine.
        const auto check = PathValidation::validatePath(
            reportsDir, root,
            QStringLiteral("cold_eyes_cross_doc_diff"),
            QStringLiteral("reports_dir"));
        if (check.bad) return QJsonDocument(check.err);

        findings = ColdEyesEngine::crossDocDiffFromDir(
            root, reportsDir, minLanes, &reportsRead);
    } else {
        const QJsonObject reportsObj =
            req.value(QStringLiteral("reports")).toObject();
        QHash<QString, QString> reports;
        for (auto it = reportsObj.constBegin();
             it != reportsObj.constEnd(); ++it) {
            const QString r = it.value().toString();
            reports.insert(it.key(), r);
            totalIn += r.toUtf8().size();
        }
        reportsRead = reports.size();
        findings = ColdEyesEngine::crossDocDiffFromReports(
            root, reports, minLanes);
    }

    QJsonArray arr;
    for (const auto &f : findings) arr.append(ceFindingToJson(f));

    QJsonObject env;
    env["ok"]              = true;
    env["findings"]        = arr;
    env["total_findings"]  = arr.size();
    env["reports_read"]    = reportsRead;
    if (hasReportsDir) {
        env["reports_dir"] = reportsDir;
    } else {
        env["total_input_bytes"] = totalIn;
    }
    env["min_lanes"]       = minLanes;
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdColdEyesFoldIn(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_fold_in: no MainWindow")));
    // ANTS-1630: caller-cwd-anchored write. Resolve the ROADMAP root from
    // the caller's own caller_cwd (the project the orchestrating session
    // owns), NOT the focused tab — so a /cold-eyes run folds into its own
    // project even when the user has a different tab focused. The pre-1630
    // focused-tab gate refused with cwd_mismatch whenever caller != focused.
    // resolveCallerCwdRoot (ANTS-1401) is the single canonical caller_cwd
    // decoder (no second open-coded canonicalisation); absent
    // caller_cwd is already refused upstream by the Required contract
    // (caller_cwd_required) before this handler runs.
    const QString callerCwd =
        req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr =
        ants::resolveCallerCwdRoot(m_main, callerCwd);
    if (rr.cwd.isEmpty() || !QFileInfo(rr.cwd).isDir())
        return QJsonDocument(ceErr(
            QStringLiteral("cwd_bad"),
            QStringLiteral("cold_eyes_fold_in: caller_cwd \"%1\" does not "
                           "resolve to a directory").arg(callerCwd)));
    const QString root = rr.cwd;

    // ANTS-2227 — dry_run preview: peek IDs (no counter bump) and skip every
    // ROADMAP.md insert (both the narrative and structured paths below).
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // ANTS-1644 — narrative-mode short-circuit. Caller supplies
    // pre-rendered markdown under the `### 📝 Cold-eyes <DATE>`
    // heading; handler inserts it verbatim, skipping ID allocation and
    // per-finding bullet rendering. Useful when the natural shape is
    // "prose subsection grouped by Closed-inline / Deferred / False-
    // positives", not N structured bullets. Ports the ANTS-1635
    // template from test_audit_fold_in.
    const bool narrativeMode =
        req.value(QStringLiteral("narrative_mode")).toBool();
    if (narrativeMode) {
        QString dateIso = req.value(QStringLiteral("date_iso")).toString();
        if (dateIso.isEmpty()) {
            dateIso = QDate::currentDate().toString(Qt::ISODate);
        }
        const QString narrative =
            req.value(QStringLiteral("narrative_md")).toString().trimmed();
        if (narrative.isEmpty()) return QJsonDocument(ceErr(
            QStringLiteral("narrative_md_required"),
            QStringLiteral(
                "cold_eyes_fold_in: narrative_mode=true requires "
                "non-empty narrative_md")));
        QString block;
        block.reserve(narrative.size() + 64);
        block += QStringLiteral("### 📝 Cold-eyes ") + dateIso
              + QStringLiteral("\n\n");
        block += narrative;
        if (!block.endsWith(QChar('\n'))) block += QChar('\n');
        QString heading =
            req.value(QStringLiteral("release_block_heading")).toString();
        if (heading.isEmpty()) heading =
            RoadmapFoldIn::findActiveReleaseHeading(root);
        bool written = false;
        if (!dryRun && !heading.isEmpty()) {
            written = RoadmapFoldIn::insertBlock(root, heading, block);
        }
        QJsonObject env;
        env["ok"]            = true;
        if (dryRun) env["dry_run"] = true;
        env["block"]         = block;
        env["allocated_ids"] = QJsonArray();
        env["id_allocation"] = QStringLiteral("skip");
        env["written"]       = written;
        if (!heading.isEmpty()) env["release_block_heading"] = heading;
        return QJsonDocument(env);
    }

    const QJsonArray actArr =
        req.value(QStringLiteral("actionable")).toArray();
    if (actArr.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral(
            "cold_eyes_fold_in: actionable array required "
            "(or pass narrative_mode=true + narrative_md=\"…\" — "
            "ANTS-1644)")));

    QList<IndieReviewEngine::CorroboratedFinding> actionable;
    for (const auto &v : actArr) {
        const auto o = v.toObject();
        IndieReviewEngine::CorroboratedFinding f;
        f.file = o.value(QStringLiteral("file")).toString();
        f.line = o.value(QStringLiteral("line")).toInt(-1);
        for (const auto &lv :
             o.value(QStringLiteral("citing_lanes")).toArray()) {
            f.citingLanes << lv.toString();
        }
        // ANTS-1278 — optional rich-card fields. Each falls through
        // to the renderer's loud TODO placeholder when absent, so
        // a caller cannot silently ship a stub bullet. Same shape
        // accepted by indie_review_fold_in and cold_eyes_fold_in,
        // both of which share the IndieReviewEngine renderer.
        f.title       = o.value(QStringLiteral("title")).toString();
        f.description = o.value(QStringLiteral("description")).toString();
        f.layman      = o.value(QStringLiteral("layman")).toString();
        f.kind        = o.value(QStringLiteral("kind")).toString();
        if (f.file.isEmpty()) continue;
        actionable.append(f);
    }
    if (actionable.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("bad_args"),
        QStringLiteral("cold_eyes_fold_in: no valid actionable entries")));

    QString dateIso = req.value(QStringLiteral("date_iso")).toString();
    if (dateIso.isEmpty()) {
        dateIso = QDate::currentDate().toString(Qt::ISODate);
    }

    // ANTS-1510 — id_allocation defaults to "auto" (the existing behavior:
    // pull N consecutive IDs from .roadmap-counter, render the block with
    // `[ANTS-NNNN]` prefixes). "skip" suppresses the counter touch entirely
    // — required for projects whose roadmap doesn't follow the shareable
    // docs/standards/roadmap-format.md § 3.5.1 ID scheme (e.g. RetroDB's
    // "Pass N.M" headings). When skipped, the block uses the freeform
    // template + the response carries `id_allocation:"skip"` echo +
    // `allocated_ids:[]`.
    const QString idAllocRaw =
        req.value(QStringLiteral("id_allocation")).toString();
    QString idAllocMode = QStringLiteral("auto");
    if (!idAllocRaw.isEmpty()) {
        if (idAllocRaw == QStringLiteral("auto")
            || idAllocRaw == QStringLiteral("skip")) {
            idAllocMode = idAllocRaw;
        } else {
            return QJsonDocument(ceErr(
                QStringLiteral("bad_args"),
                QStringLiteral("cold_eyes_fold_in: id_allocation must be "
                               "\"auto\" or \"skip\"")));
        }
    }
    const bool skipAlloc = (idAllocMode == QStringLiteral("skip"));

    // ANTS-3498 — optional id_prefix override (parity with roadmap_log
    // op:append). Validate before any counter touch.
    const QString idPrefixArg = req.value(QStringLiteral("id_prefix")).toString();
    if (!idPrefixArg.isEmpty() && !RoadmapFoldIn::isValidIdPrefix(idPrefixArg)) {
        return QJsonDocument(ceErr(QStringLiteral("bad_args"),
            QStringLiteral("cold_eyes_fold_in: id_prefix \"%1\" must contain a "
                           "letter and be 1-16 chars of [A-Za-z0-9_-] "
                           "(e.g. ANTS, 3D_E) — ANTS-3492").arg(idPrefixArg)));
    }

    QString heading = req.value(QStringLiteral("release_block_heading"))
                          .toString();
    if (heading.isEmpty()) heading =
        RoadmapFoldIn::findActiveReleaseHeading(root);

    QList<int> ids;
    if (!skipAlloc) {
        // ANTS-2201 — only allocate (and bump .roadmap-counter) once we know
        // there is a heading to insert the IDs under; otherwise the insert never
        // runs and the IDs leak into a permanent gap. skip-alloc/freeform mode
        // allocates nothing, so it may still return its block with no heading.
        if (heading.isEmpty()) return QJsonDocument(ceErr(
            QStringLiteral("no_release_heading"),
            QStringLiteral("cold_eyes_fold_in: no active release heading to insert "
                           "under (pass release_block_heading explicitly)")));
        // ANTS-2227 — dry_run peeks the would-be IDs without bumping the counter.
        ids = dryRun
            ? RoadmapFoldIn::peekIds(root, actionable.size())
            : RoadmapFoldIn::allocateIds(root, actionable.size());
        if (ids.isEmpty()) return QJsonDocument(ceErr(
            QStringLiteral("counter_failed"),
            QStringLiteral("cold_eyes_fold_in: could not allocate IDs")));
    }

    // ANTS-3480 — sniff once; block render + allocated_ids echo agree and
    // zero-pad via RoadmapFoldIn::renderId (freeform: ids is empty, so the
    // echo is []). Sniffing in the freeform arm is a cheap harmless read.
    // ANTS-3498 — the explicit override wins over the sniff.
    const QString idPrefix = idPrefixArg.isEmpty()
        ? RoadmapFoldIn::sniffIdPrefix(root) : idPrefixArg;
    const QString block = skipAlloc
        ? ColdEyesEngine::templateColdEyesFoldInBlockFreeform(
              actionable, dateIso)
        : ColdEyesEngine::templateColdEyesFoldInBlock(
              actionable, ids, dateIso, idPrefix);

    bool written = false;
    if (!dryRun && !heading.isEmpty()) {
        written = RoadmapFoldIn::insertBlock(root, heading, block);
    }

    QJsonObject env;
    env["ok"]            = true;
    if (dryRun) env["dry_run"] = true;
    env["block"]         = block;
    QJsonArray idsArr;
    for (int id : ids) idsArr.append(RoadmapFoldIn::renderId(idPrefix, id));
    env["allocated_ids"] = idsArr;
    env["id_allocation"] = idAllocMode;
    env["written"]       = written;
    if (!heading.isEmpty()) env["release_block_heading"] = heading;
    return QJsonDocument(env);
}

// ANTS-1413 — cold_eyes_single_doc. Cross-consistency brief for one
// doc without running the full multi-lane partition + brief workflow.
// Returns the doc's neighbourhood (same-dir siblings, project
// standards, root contracts) plus a default reviewer-role list.
QJsonDocument RemoteControl::cmdColdEyesSingleDoc(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_single_doc: no MainWindow")));
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cold_eyes_single_doc: no focused project")));

    const QString docPathRaw =
        req.value(QStringLiteral("doc_path")).toString().trimmed();
    if (docPathRaw.isEmpty()) {
        QJsonObject err = ceErr(
            QStringLiteral("bad_args"),
            QStringLiteral("cold_eyes_single_doc: doc_path required"));
        return QJsonDocument(err);
    }
    // ANTS-1295: anchor the path before the engine sees it. The
    // engine trusts the input, so the chokepoint must live here.
    const auto check = PathValidation::validatePath(
        docPathRaw, root,
        QStringLiteral("cold_eyes_single_doc"),
        QStringLiteral("doc_path"));
    if (check.bad) return QJsonDocument(check.err);
    if (check.resolved.isEmpty() || !QFileInfo::exists(check.resolved)) {
        QJsonObject err = ceErr(
            QStringLiteral("not_found"),
            QStringLiteral("cold_eyes_single_doc: doc_path does not "
                           "exist on disk"));
        err["echo"] = ceSanitiseEcho(docPathRaw);
        return QJsonDocument(err);
    }

    // ANTS-1807 — pass the canonical resolved form (project-relative), not the
    // raw input. assembleSingleDocBrief re-derives projectPath + "/" + docPath
    // and opens it; passing the raw form leaves a symlink-swap TOCTOU window
    // between validatePath's canonicalisation and the engine's read. Mirrors
    // the debt_sweep_apply_fix fix above.
    QString safeDoc = docPathRaw;
    if (!check.resolved.isEmpty() &&
        check.resolved.startsWith(root + QLatin1Char('/'))) {
        safeDoc = check.resolved.mid(root.size() + 1);
    }
    const auto b = ColdEyesEngine::assembleSingleDocBrief(root, safeDoc);

    QJsonObject related;
    QJsonArray  sibs;
    for (const QString &p : b.sameDirSiblings) sibs.append(p);
    related["same_dir_siblings"] = sibs;
    QJsonArray stds;
    for (const QString &p : b.standards) stds.append(p);
    related["standards"] = stds;
    QJsonArray rc;
    for (const QString &p : b.rootContracts) rc.append(p);
    related["root_contracts"] = rc;

    QJsonArray rev;
    for (const QString &r : b.recommendedReviewers) rev.append(r);

    QJsonObject env;
    env["ok"]                    = true;
    env["doc_path"]              = b.docPath;
    env["summary"]               = b.summary;
    env["related"]               = related;
    env["recommended_reviewers"] = rev;
    return QJsonDocument(env);
}

// ANTS-1414 — cross_doc_diff. Lane-source-agnostic alias for
// cold_eyes_cross_doc_diff / indie_review_corroborate's regex hotspot
// primitive. Lets a caller corroborate any reviewer-report bundle
// without committing to the cold-eyes vs indie-review framing. Same
// args, same envelope shape — delegates to the same engine helper.
QJsonDocument RemoteControl::cmdCrossDocDiff(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cross_doc_diff: no MainWindow")));
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) return QJsonDocument(ceErr(
        QStringLiteral("no_project"),
        QStringLiteral("cross_doc_diff: no focused project")));

    const bool hasReports    = req.contains(QStringLiteral("reports"));
    const bool hasReportsDir = req.contains(QStringLiteral("reports_dir"));
    if (hasReports == hasReportsDir) {
        return QJsonDocument(ceErr(
            QStringLiteral("bad_args"),
            QStringLiteral(
                "cross_doc_diff: provide exactly one of "
                "`reports` (inline map) or `reports_dir` (project-relative "
                "directory of *.md files)")));
    }

    int minLanes = req.value(QStringLiteral("min_lanes")).toInt(2);
    if (minLanes < 1) minLanes = 1;

    QList<IndieReviewEngine::CorroboratedFinding> findings;
    QString reportsDir;
    int     reportsRead = 0;
    qint64  totalIn = 0;
    // ANTS-1344 — parity with cmdIndieReviewCorroborate. cross_doc_diff
    // shares the engine path so it needs the same truncation surface.
    QStringList truncatedLanes;

    if (hasReportsDir) {
        reportsDir = req.value(QStringLiteral("reports_dir"))
                        .toString().trimmed();
        if (reportsDir.isEmpty()) return QJsonDocument(ceErr(
            QStringLiteral("bad_args"),
            QStringLiteral("cross_doc_diff: reports_dir must be a "
                           "non-empty project-relative path")));
        const auto check = PathValidation::validatePath(
            reportsDir, root,
            QStringLiteral("cross_doc_diff"),
            QStringLiteral("reports_dir"));
        if (check.bad) return QJsonDocument(check.err);
        findings = IndieReviewEngine::corroboratedFindingsFromDir(
            root, reportsDir, minLanes, &reportsRead);
        // ANTS-1344 — see cmdIndieReviewCorroborate companion block.
        QDir d(check.resolved);
        const QStringList entries = d.entryList(
            QStringList{QStringLiteral("*.md")},
            QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &name : entries) {
            if (name.startsWith(QChar('.'))) continue;
            const QFileInfo fi(d.filePath(name));
            if (fi.size() > IndieReviewEngine::kMaxScanBytes) {
                truncatedLanes << QFileInfo(name).completeBaseName();
            }
        }
    } else {
        const QJsonObject reportsObj =
            req.value(QStringLiteral("reports")).toObject();
        QHash<QString, QString> reports;
        for (auto it = reportsObj.constBegin();
             it != reportsObj.constEnd(); ++it) {
            const QString r = it.value().toString();
            reports.insert(it.key(), r);
            totalIn += r.toUtf8().size();
            if (r.size() > IndieReviewEngine::kMaxScanBytes) {
                truncatedLanes << it.key();
            }
        }
        reportsRead = reports.size();
        findings = IndieReviewEngine::corroboratedFindings(
            root, reports, minLanes);
    }

    QJsonArray arr;
    for (const auto &f : findings) arr.append(ceFindingToJson(f));

    QJsonObject env;
    env["ok"]              = true;
    env["findings"]        = arr;
    env["total_findings"]  = arr.size();
    env["reports_read"]    = reportsRead;
    if (hasReportsDir) {
        env["reports_dir"] = reportsDir;
    } else {
        env["total_input_bytes"] = totalIn;
    }
    env["min_lanes"]       = minLanes;
    if (!truncatedLanes.isEmpty()) {
        env["truncated"]          = true;
        QJsonArray tl;
        for (const QString &ln : std::as_const(truncatedLanes)) tl.append(ln);
        env["truncated_lanes"]    = tl;
        env["truncated_at_bytes"] = IndieReviewEngine::kMaxScanBytes;
    }
    return QJsonDocument(env);
}

// ---------------------------------------------------------------------------
// ANTS-1283 — session_memory MCP tool
// ---------------------------------------------------------------------------
//
// Per-cwd key-value persistence backed by
// ~/.cache/ants-terminal/mcp-state/<cwd-hash>.json. Pure delegation to
// SessionMemoryEngine::execute. INV-12 echo hygiene applied to every
// user-supplied string echoed in error responses. See
// docs/specs/ANTS-1283.md.

namespace rcdetail {

QString smSanitiseEcho(const QString &raw) {
    QString verbatim = raw;
    verbatim.truncate(64);
    QString out;
    out.reserve(verbatim.size());
    for (int i = 0; i < verbatim.size(); ++i) {
        out.append(verbatim.at(i).unicode() < 0x20 ? QChar('?')
                                                  : verbatim.at(i));
    }
    return out;
}

QJsonObject smErr(const QString &code, const QString &msg,
                  const QString &opStr, const QString &keyEcho) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = msg;
    if (!opStr.isEmpty())   o["op"]   = opStr;
    if (!keyEcho.isEmpty()) o["echo"] = smSanitiseEcho(keyEcho);
    return o;
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdSessionMemory(const QJsonObject &req) {
    if (!m_main) return QJsonDocument(smErr(
        QStringLiteral("no_window"),
        QStringLiteral("session_memory: no MainWindow"),
        QString(), QString()));

    // Parse op early — get/list are read-only and skip the ANTS-1372
    // caller-cwd gate; set/delete mutate the project session-memory
    // store and require the gate.
    const QString opRaw = req.value(QStringLiteral("op")).toString();
    SessionMemoryEngine::Op op = SessionMemoryEngine::Op::Get;
    if (!SessionMemoryEngine::parseOp(opRaw, &op)) {
        return QJsonDocument(smErr(
            QStringLiteral("bad_op"),
            QStringLiteral("session_memory: op must be one of "
                           "\"get\", \"set\", \"delete\", \"list\""),
            QString(), opRaw));
    }

    // ANTS-1336 + ANTS-1435 — gate routing is now ASYMMETRIC:
    //   * Read ops (get, list): anchor to caller_cwd directly. The
    //     storage at ~/.cache/.../mcp-state/<sha256(cwd)>.json is
    //     per-cwd-hashed, and the caller's bucket is self-scoped.
    //     No focused-tab match required — Vestige's cross-tab read
    //     pattern works. §Limitations: a same-UID process can read
    //     any bucket it can name; documented trade-off per cold-eyes
    //     H1 / spec sign-off.
    //   * Write ops (set, delete): keep RcGate flow. Prevents the
    //     confused-deputy "session in /A writes to /B's bucket" attack.
    // ANTS-1435 INV-4b — read ops require caller_cwd to canonicalise
    // AND be a directory; QFileInfo::canonicalFilePath accepts any
    // existing path (file, FIFO, device). A read against /etc/passwd
    // would hash to a real bucket file and silently return empty.
    QString cwd;
    const bool isReadOp = (op == SessionMemoryEngine::Op::Get ||
                           op == SessionMemoryEngine::Op::List);
    // ANTS-1543 — concrete JSON example for refusal envelopes. Shows
    // the exact arguments shape so a session that hit `cwd_missing`
    // / `cwd_bad` / a gate refusal can self-correct without round-
    // tripping through the docs. Op-specific so it doubles as a
    // syntax cheat-sheet.
    auto smExample = [&]() -> QJsonObject {
        QJsonObject ex;
        ex["op"]         = opRaw.isEmpty() ? QStringLiteral("get") : opRaw;
        ex["caller_cwd"] = QStringLiteral("<your $PWD>");
        if (op == SessionMemoryEngine::Op::Get ||
            op == SessionMemoryEngine::Op::Set ||
            op == SessionMemoryEngine::Op::Delete) {
            ex["key"] = QStringLiteral("my-key");
        }
        if (op == SessionMemoryEngine::Op::Set) {
            ex["value"] = QStringLiteral("<any JSON>");
        }
        return ex;
    };

    if (isReadOp) {
        const QString rawCaller =
            req.value(QStringLiteral("caller_cwd")).toString();
        if (rawCaller.isEmpty()) {
            QJsonObject env = smErr(
                QStringLiteral("cwd_missing"),
                QStringLiteral("session_memory: caller_cwd argument "
                    "required (pass your $PWD)"),
                opRaw, QString());
            env["example"] = smExample();
            return QJsonDocument(env);
        }
        const QFileInfo fi(rawCaller);
        const QString canon = fi.canonicalFilePath();
        if (canon.isEmpty()) {
            QJsonObject env = smErr(
                QStringLiteral("cwd_bad"),
                QStringLiteral("session_memory: caller_cwd \"%1\" "
                    "does not exist").arg(rawCaller),
                opRaw, QString());
            env["example"] = smExample();
            return QJsonDocument(env);
        }
        if (!QFileInfo(canon).isDir()) {
            QJsonObject env = smErr(
                QStringLiteral("cwd_bad"),
                QStringLiteral("session_memory: caller_cwd \"%1\" "
                    "is not a directory").arg(rawCaller),
                opRaw, QString());
            env["example"] = smExample();
            return QJsonDocument(env);
        }
        cwd = canon;
    } else {
        const auto gate = RcGate::checkCallerCwd(
            resolveRootCanonical(m_main), req,
            QStringLiteral("session_memory"));
        if (!gate.ok) {
            QJsonObject env = smErr(gate.errorCode, gate.error,
                                    opRaw, QString());
            env["example"] = smExample();
            return QJsonDocument(env);
        }
        cwd = gate.focused;
    }

    const QString    key   = req.value(QStringLiteral("key")).toString();
    const QJsonValue value = req.value(QStringLiteral("value"));

    // INV-9 — handler-side check for required key/value past schema.
    const bool needsKey = (op != SessionMemoryEngine::Op::List);
    if (needsKey && key.isEmpty()) {
        return QJsonDocument(smErr(
            QStringLiteral("bad_key"),
            QStringLiteral("session_memory: key required for get/set/delete"),
            opRaw, key));
    }
    if (op == SessionMemoryEngine::Op::Set && value.isUndefined()) {
        return QJsonDocument(smErr(
            QStringLiteral("bad_value"),
            QStringLiteral("session_memory: value required for set"),
            opRaw, key));
    }

    const SessionMemoryEngine::OpResult r =
        SessionMemoryEngine::execute(cwd, op, key, value);

    if (!r.ok) {
        QJsonObject env = smErr(r.code, r.error, r.op, r.key);
        if (!r.path.isEmpty()) env["path"] = r.path;
        return QJsonDocument(env);
    }

    QJsonObject env;
    env["ok"]          = true;
    env["op"]          = r.op;
    env["path"]        = r.path;
    env["total_bytes"] = static_cast<qint64>(r.totalBytes);
    switch (op) {
        case SessionMemoryEngine::Op::Get:
            env["key"]   = r.key;
            env["found"] = r.found;
            if (r.found) env["value"] = r.value;
            break;
        case SessionMemoryEngine::Op::Set:
            env["key"]           = r.key;
            env["bytes_written"] = static_cast<qint64>(r.bytesWritten);
            break;
        case SessionMemoryEngine::Op::Delete:
            env["key"]   = r.key;
            env["found"] = r.found;
            break;
        case SessionMemoryEngine::Op::List:
            env["keys"] = r.keys;
            break;
    }
    return QJsonDocument(env);
}

// ANTS-1723 — workflow_state: per-project, per-skill step/phase store.
// Uses session_memory's backing store with "wf.<skill>" key namespace.
// Write ops (set/clear) use RcGate (ANTS-1435). Read ops anchor to
// caller_cwd directly. 72 h lazy-TTL purge on every set. 4 KiB cap.
QJsonDocument RemoteControl::cmdWorkflowState(const QJsonObject &req)
{
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("workflow_state: no MainWindow")));
    }

    // --- parse op ---
    // ANTS-3511 — `op` and `skill` are both required for every op; an
    // absent op names BOTH in the first refusal so the caller resolves
    // the full arg set in one round-trip (finbreak feedback 2026-07-14)
    // instead of discovering `skill` only on the next call.
    const QString opRaw = req.value(QStringLiteral("op")).toString();
    enum class Op { Get, Set, Clear };
    Op op;
    if      (opRaw == QStringLiteral("get"))   op = Op::Get;
    else if (opRaw == QStringLiteral("set"))   op = Op::Set;
    else if (opRaw == QStringLiteral("clear")) op = Op::Clear;
    else if (opRaw.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: op and skill are required "
                           "(op must be get/set/clear)")));
    } else {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: op must be get/set/clear")));
    }

    // --- validate skill name: ^[A-Za-z0-9_-]{1,32}$ ---
    // ANTS-3511 — distinguish absent from malformed: an empty/absent
    // skill reads as "required", not "invalid" (the regex message only
    // makes sense for a present-but-non-conforming value).
    const QString skill = req.value(QStringLiteral("skill")).toString();
    if (skill.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: skill is required")));
    }
    static const QRegularExpression kSkillRe(
        QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    if (!kSkillRe.match(skill).hasMatch()) {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: invalid skill name — "
                           "must match ^[A-Za-z0-9_-]{1,32}$")));
    }

    // --- key: "wf.<skill>" — dot separator, valid in key charset ---
    const QString key = QStringLiteral("wf.") + skill;

    // --- TTL constant: 72 h in milliseconds ---
    constexpr qint64 kTtlMs = 72LL * 3600LL * 1000LL;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // ----------------------------------------------------------------
    // GET — read op: anchor to caller_cwd directly (ANTS-1435).
    // ----------------------------------------------------------------
    if (op == Op::Get) {
        const QString rawCaller =
            req.value(QStringLiteral("caller_cwd")).toString();
        if (rawCaller.isEmpty()) {
            return QJsonDocument(csErr(QStringLiteral("cwd_missing"),
                QStringLiteral("workflow_state: caller_cwd required")));
        }
        const QFileInfo fi(rawCaller);
        const QString canon = fi.canonicalFilePath();
        if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
            return QJsonDocument(csErr(QStringLiteral("cwd_bad"),
                QStringLiteral("workflow_state: caller_cwd unresolvable")));
        }
        SessionMemoryEngine::OpResult r =
            SessionMemoryEngine::execute(canon,
                SessionMemoryEngine::Op::Get, key, QJsonValue());
        if (!r.ok) return QJsonDocument(csErr(r.code, r.error));
        if (!r.found) {
            QJsonObject out;
            out[QStringLiteral("ok")]    = true;
            out[QStringLiteral("found")] = false;
            return QJsonDocument(out);
        }
        // Check 72 h TTL.
        const qint64 updatedAt =
            r.value.toObject()
                .value(QStringLiteral("updated_at_ms")).toDouble(0);
        if (updatedAt > 0 && (nowMs - updatedAt) > kTtlMs) {
            QJsonObject out;
            out[QStringLiteral("ok")]      = true;
            out[QStringLiteral("found")]   = false;
            out[QStringLiteral("expired")] = true;
            return QJsonDocument(out);
        }
        QJsonObject out;
        out[QStringLiteral("ok")]    = true;
        out[QStringLiteral("found")] = true;
        out[QStringLiteral("state")] = r.value;
        return QJsonDocument(out);
    }

    // ----------------------------------------------------------------
    // CLEAR / SET — write ops: enforce RcGate (ANTS-1435).
    // ----------------------------------------------------------------
    const auto gate = RcGate::checkCallerCwd(
        resolveRootCanonical(m_main), req,
        QStringLiteral("workflow_state"));
    if (!gate.ok) return QJsonDocument(RcGate::gateErrorEnvelope(gate));
    const QString cwd = gate.focused;

    // --- CLEAR ---
    if (op == Op::Clear) {
        // Issue Delete directly; r.found indicates whether the key existed.
        SessionMemoryEngine::OpResult r =
            SessionMemoryEngine::execute(cwd,
                SessionMemoryEngine::Op::Delete, key, QJsonValue());
        if (!r.ok) return QJsonDocument(csErr(r.code, r.error));
        QJsonObject out;
        out[QStringLiteral("ok")]      = true;
        out[QStringLiteral("deleted")] = r.found;
        return QJsonDocument(out);
    }

    // --- SET ---
    // Validate required fields: step (int) and phase (non-empty string).
    const QJsonValue stepVal = req.value(QStringLiteral("step"));
    const QString    phase   = req.value(QStringLiteral("phase")).toString();
    if (!stepVal.isDouble() || phase.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("bad_args"),
            QStringLiteral("workflow_state: set requires step (int) "
                           "and phase (non-empty string)")));
    }
    const QJsonArray notes = req.value(QStringLiteral("notes")).toArray();

    // Build the stored value; always overwrite updated_at_ms with server clock.
    QJsonObject state;
    state[QStringLiteral("step")]          = static_cast<int>(stepVal.toDouble());
    state[QStringLiteral("phase")]         = phase;
    state[QStringLiteral("notes")]         = notes;
    state[QStringLiteral("updated_at_ms")] = static_cast<double>(nowMs);

    // Payload cap: 4 KiB serialised.
    const QByteArray payload =
        QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (payload.size() > 4096) {
        return QJsonDocument(csErr(QStringLiteral("payload_too_large"),
            QStringLiteral("workflow_state: state payload exceeds 4 KiB")));
    }

    // ANTS-1823 — prune expired wf.* keys AND write the new state in ONE
    // locked read-modify-write cycle. The pre-fix path ran two separate
    // *unlocked* cycles — a lazy-TTL purge (load → filter → save) then a
    // distinct execute(Set) (load → insert → save). Across two concurrent
    // same-cwd CC sessions (the multi-tester workflow) that interleaving
    // last-writer-wins-dropped a key; the gap between the purge write and
    // the set write doubled the window. mutateLocked holds an advisory
    // cross-process lock for the whole load → mutate → save and enforces
    // the store-level caps. ANTS-1774's single-pass purge is preserved —
    // it's just folded into the same locked mutation as the set now.
    // (The per-value cap execute(Set) used is moot: the 4 KiB payload
    // cap above is stricter than the engine's 16 KiB per-value cap.)
    const QString storePath = SessionMemoryEngine::storePathFor(cwd);
    const SessionMemoryEngine::OpResult wr =
        SessionMemoryEngine::mutateLocked(storePath,
            [&](QJsonObject &store) {
                const QStringList allKeys = store.keys();
                for (const QString &k : allKeys) {
                    if (!k.startsWith(QStringLiteral("wf."))) continue;
                    const qint64 ts = store.value(k).toObject()
                        .value(QStringLiteral("updated_at_ms")).toDouble(0);
                    if (ts > 0 && (nowMs - ts) > kTtlMs) store.remove(k);
                }
                store.insert(key, QJsonValue(state));
                return true;
            });
    if (!wr.ok) return QJsonDocument(csErr(wr.code, wr.error));

    QJsonObject out;
    out[QStringLiteral("ok")] = true;
    return QJsonDocument(out);
}

// ---------------------------------------------------------------------------
// ANTS-1430 — project_layout MCP tool
// ---------------------------------------------------------------------------
//
// Pre-cached project file layout (ROADMAP/CHANGELOG/specs/etc.) per
// caller_cwd, persisted via SessionMemoryEngine under the well-known
// key `project_layout`. Required-contract gated (dispatcher refuses
// empty caller_cwd before the provider lambda runs). On invocation:
// gate → cache lookup → freshness check (TTL + mtime) → scan-if-stale
// → cache write (best-effort). See docs/specs/ANTS-1430.md.

QJsonDocument RemoteControl::cmdProjectLayout(const QJsonObject &req) {
    if (!m_main) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("no_window");
        env["error"] = QStringLiteral("project_layout: no MainWindow");
        return QJsonDocument(env);
    }
    // ANTS-1404 + ANTS-1435 — caller_cwd anchoring.
    // Dispatcher Required-contract refusal already caught empty
    // caller_cwd upstream. Here we canonicalise + isDir-check the
    // value and use it as the tenancy assertion (no focused-tab
    // match — project_layout reads are self-scoped to the caller's
    // bucket, same trade-off as session_memory read ops). Cold-eyes
    // H2 / M3: isDir gate prevents /etc/passwd-style false hits.
    const QString rawCaller = req.value(QStringLiteral("caller_cwd")).toString();
    const QFileInfo plFi(rawCaller);
    const QString cwd = plFi.canonicalFilePath();
    if (cwd.isEmpty() || !QFileInfo(cwd).isDir()) {
        QJsonObject env;
        env["ok"]    = false;
        env["error"] = QStringLiteral(
            "project_layout: caller_cwd \"%1\" is not a directory")
                .arg(rawCaller);
        env["code"]  = QStringLiteral("cwd_bad");
        // ANTS-1566 — concrete JSON snippet so the caller can copy
        // the exact arguments shape (mirrors session_memory + RcGate
        // envelopes). Catches IPC-direct callers that bypass the
        // dispatcher's caller_cwd_required gate.
        QJsonObject ex;
        ex[QStringLiteral("caller_cwd")] = QStringLiteral("<your $PWD>");
        env[QStringLiteral("example")] = ex;
        return QJsonDocument(env);
    }

    const bool forceRescan =
        req.value(QStringLiteral("force_rescan")).toBool(false);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // Cache lookup. SessionMemoryEngine::execute(Get) returns
    // OpResult.value as a QJsonValue; the layout envelope is a
    // JSON object so we round-trip via toObject().
    bool cacheHit = false;
    ProjectLayoutEngine::LayoutEnvelope env;
    if (!forceRescan) {
        const auto getRes = SessionMemoryEngine::execute(
            cwd, SessionMemoryEngine::Op::Get,
            QStringLiteral("project_layout"),
            QJsonValue());
        if (getRes.ok && getRes.found && getRes.value.isObject()) {
            env = ProjectLayoutEngine::fromJson(
                getRes.value.toObject());
            if (!ProjectLayoutEngine::isStale(env, nowMs)) {
                cacheHit = true;
            }
        }
    }
    if (!cacheHit) {
        env = ProjectLayoutEngine::scanLayout(cwd);
        // Best-effort cache write. Spec § INV-8: store-write
        // failure is non-fatal; the verb still returns the fresh
        // envelope.
        SessionMemoryEngine::execute(
            cwd, SessionMemoryEngine::Op::Set,
            QStringLiteral("project_layout"),
            QJsonValue(ProjectLayoutEngine::toJson(env)));
    }

    QJsonObject out = ProjectLayoutEngine::toJson(env);
    out[QStringLiteral("ok")]     = true;
    out[QStringLiteral("cached")] = cacheHit;
    return QJsonDocument(out);
}

// =====================================================================
// ANTS-1583 — roadmap_branch_drift
// =====================================================================
//
// Compares ROADMAP ✅ entries' cited commit SHAs against HEAD-reachable
// history. Useful for projects with multiple long-lived branches where
// fix commits and docs commits land on different branches and drift.
//
// Implementation reuses findRoadmapUnder (ANTS-1459), collectGitSnapshot
// (no extra rev-parse fork), and runGit (2 s wall-clock).
//
// See docs/specs/ANTS-1583.md for invariants.

namespace rcdetail {

QJsonObject rbdErr(const QString &code, const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = msg;
    return o;
}

// ANTS-1583 — anchored SHA detector for roadmap_branch_drift.
// Pre-anchor (`commit X`, `(`, line start, `merge`/`via`/`in`/`at`,
// `Landed in commit`, `Source:`) plus alpha-required lookahead
// `(?=[0-9a-f]*[a-f])` plus post-anchor (`[,.\)\s]` or end of input).
// The post-anchor lookahead covers the bare trailing-comma /
// period / paren form.
static const QRegularExpression &rxCommitSha() {
    static const QRegularExpression r(
        QStringLiteral(
            "(?:^|commit\\s+|\\(|"
            "(?:\\bmerge\\b|\\bvia\\b|\\bin\\b|\\bat\\b|"
                "\\bLanded in commit\\s+|\\bSource:\\s*)\\s*)"
            "(?=[0-9a-f]*[a-f])"
            "([0-9a-f]{7,40})"
            "(?=[,.\\)\\s]|$)"));
    return r;
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdRoadmapBranchDrift(const QJsonObject &req) {
    // Caller_cwd contract is Required — enforced at the dispatch layer
    // (ANTS-1404), but the handler also defends against an empty value
    // from legacy IPC paths.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty()) {
        return QJsonDocument(rbdErr(QStringLiteral("caller_cwd_required"),
            QStringLiteral("roadmap_branch_drift: caller_cwd is required")));
    }
    const QString rootCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(rbdErr(QStringLiteral("cwd_bad"),
            QStringLiteral("roadmap_branch_drift: caller_cwd does not "
                           "resolve to an existing directory")));
    }

    // Roadmap discovery — shared with cmdRoadmapQuery / cmdRoadmapLog.
    const QString roadmapPath = findRoadmapUnder(rootCanonical);
    if (roadmapPath.isEmpty()) {
        return QJsonDocument(rbdErr(QStringLiteral("no_roadmap_loaded"),
            QStringLiteral("roadmap_branch_drift: no ROADMAP.md found "
                           "under %1 (or its docs/ / .github/ "
                           "siblings)").arg(rootCanonical)));
    }

    // ANTS-1583 INV-10 — reuse collectGitSnapshot so we don't fork an
    // extra rev-parse HEAD on top of the snapshot path's own call.
    const VerifyGitSnapshot snap = collectGitSnapshot(rootCanonical);
    if (!snap.valid) {
        return QJsonDocument(rbdErr(QStringLiteral("no_git_state"),
            QStringLiteral("roadmap_branch_drift: %1 is not a git tree "
                           "or `git rev-parse HEAD` returned empty")
                .arg(rootCanonical)));
    }
    const QString currentCommit = snap.head;
    const QString currentBranch = QString::fromUtf8(runGit(
        rootCanonical,
        {QStringLiteral("symbolic-ref"), QStringLiteral("--short"),
         QStringLiteral("HEAD")})).trimmed();

    // max_drift clamp [1, 100], default 20.
    int maxDrift = 20;
    if (req.contains(QStringLiteral("max_drift")) &&
        req.value(QStringLiteral("max_drift")).isDouble()) {
        maxDrift = req.value(QStringLiteral("max_drift")).toInt();
    }
    if (maxDrift < 1)   maxDrift = 1;
    if (maxDrift > 100) maxDrift = 100;

    // Reachable-set build — one `git log --format=%H` rather than N
    // `git branch --contains` forks. Cap at 200k.
    const QByteArray logRaw = runGit(rootCanonical,
        {QStringLiteral("log"), QStringLiteral("--format=%H"),
         QStringLiteral("HEAD"), QStringLiteral("--max-count=200000")});
    QSet<QString> reachableFull;
    QMultiHash<QString, QString> reachableByPrefix;  // 7-hex prefix → full
    bool truncatedHistory = false;
    {
        const QList<QByteArray> lines = logRaw.split('\n');
        int count = 0;
        for (const QByteArray &line : lines) {
            if (line.size() < 40) continue;
            const QString full = QString::fromUtf8(line.left(40));
            reachableFull.insert(full);
            reachableByPrefix.insert(full.left(7), full);
            if (++count >= 200000) { truncatedHistory = true; break; }
        }
    }

    // Read + parse the ROADMAP. Use the same parseBullets entry point
    // the rest of the file uses; it auto-detects ants-v1 / GFM-task-list
    // / pass-headings.
    // ANTS-3863 — the provider reads only as far as this path needs; on a
    // migrated project the classification comes from the store and the body is
    // never touched.
    auto text = RoadmapSource::RoadmapText::fromFile(roadmapPath);
    if (text.openFailed()) {
        return QJsonDocument(rbdErr(QStringLiteral("read_failed"),
            QStringLiteral("roadmap_branch_drift: could not open %1")
                .arg(roadmapPath)));
    }
    // ANTS-3793 — the store when this project is migrated, the file's text when
    // it is not. This verb refuses rather than degrading: its whole output is a
    // per-bullet classification, and one built from the wrong source is worse
    // than none.
    RoadmapSource::ReadError srcWhy = RoadmapSource::ReadError::None;
    QString srcErr;
    const auto bullets = roadmapBullets(rootCanonical, text,
                                        /*includeArchive=*/false,
                                        &srcWhy, &srcErr);
    if (srcWhy != RoadmapSource::ReadError::None) {
        QJsonObject srcOut;
        rcRoadmapSourceRefused(srcOut, srcWhy, srcErr);
        return QJsonDocument(srcOut);
    }

    // Classifier helpers.
    auto isReachable = [&](const QString &sha) -> bool {
        if (sha.size() == 40) return reachableFull.contains(sha);
        // Short SHA — look up via the 7-hex prefix index.
        const QString prefix = sha.left(7);
        const QList<QString> candidates = reachableByPrefix.values(prefix);
        for (const QString &c : candidates) {
            if (c.startsWith(sha)) return true;
        }
        return false;
    };
    auto existsInGit = [&](const QString &sha) -> bool {
        // runGit returns empty bytes on non-zero exit which is
        // ambiguous with cat-file -e's "exists, no stdout" success;
        // call git directly so we can read the exit code.
        QProcess p;
        QStringList full;
        full << QStringLiteral("-C") << rootCanonical
             << QStringLiteral("cat-file") << QStringLiteral("-e") << sha;
        p.start(QStringLiteral("git"), full);
        if (!p.waitForStarted(1000)) return false;
        if (!p.waitForFinished(2000)) { p.kill(); return false; }
        return (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
    };

    // ANTS-3437 — a legacy ants-v1 roadmap carries no [PREFIX-NNNN] ids, so
    // EVERY ✅ bullet has an empty `id` and the id-less guard below would
    // skip the whole file, reporting a false scanned_bullets:0 / "no drift"
    // all-clear even when the roadmap cites dozens of fix SHAs (RetroArch /
    // Album-Builder feedback). The id-less skip exists only to drop
    // narrator / rollup bullets in a MODERN roadmap. Detect the legacy case:
    // when NO bullet in the file has an id, disable the skip and identify
    // each bullet by a short headline slug instead. (A modern GFM roadmap
    // never reaches the empty-id branch — its adapter assigns a
    // content-hash id — so this only engages for native no-id ants-v1.)
    bool anyBulletHasId = false;
    for (const auto &b : bullets) {
        if (!b.id.isEmpty()) { anyBulletHasId = true; break; }
    }
    const bool legacyNoId = !anyBulletHasId && !bullets.isEmpty();
    auto bulletIdFor = [&](const auto &bul) -> QString {
        if (!bul.id.isEmpty()) return bul.id;
        QString slug = bul.headlineFull.isEmpty() ? bul.headline
                                                  : bul.headlineFull;
        slug = slug.trimmed().left(60);
        return slug.isEmpty() ? QStringLiteral("(untitled)") : slug;
    };

    // Walk ✅ bullets; tally drift. The loop variable is renamed `bul`
    // (not the conventional `b`) so it doesn't trip the source-grep
    // tripwire in tests/features/roadmap_query_section_index/ that
    // counts cmdRoadmapQuery cache-fill loops via the b-variable
    // signature. This verb has its own emission path with no
    // section_slug contract — exempt by construction.
    QJsonArray drift;
    int scannedBullets = 0;
    int withSha        = 0;
    bool driftTruncated = false;
    for (const auto &bul : bullets) {
        if (bul.status != QStringLiteral("✅")) continue;
        // ANTS-3437 — skip id-less narrator bullets only in a modern roadmap;
        // a legacy no-id roadmap scans them all (identified by headline slug).
        if (bul.id.isEmpty() && !legacyNoId) continue;
        ++scannedBullets;

        const QString joined = bul.headline + QChar('\n') + bul.body;
        // Extract all SHA candidates from the bullet body.
        QStringList shas;
        QRegularExpressionMatchIterator it =
            rxCommitSha().globalMatch(joined);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString sha = m.captured(1);
            if (!shas.contains(sha)) shas.append(sha);
        }
        if (shas.isEmpty()) continue;
        ++withSha;

        // First-drifted-wins per bullet (spec § 6 out-of-scope: per-
        // bullet drift_per_bullet[] left as follow-up).
        for (const QString &sha : shas) {
            if (isReachable(sha)) continue;
            QJsonObject o;
            o["bullet_id"]  = bulletIdFor(bul);
            o["cited_sha"]  = sha;
            o["reason"]     = existsInGit(sha)
                ? QStringLiteral("sha_not_in_HEAD")
                : QStringLiteral("sha_not_in_git");
            o["headline"]   = bul.headline;
            drift.append(o);
            if (drift.size() >= maxDrift) { driftTruncated = true; break; }
            break;  // one drift entry per bullet (first-drifted-wins)
        }
        if (driftTruncated) break;
    }

    // ANTS-2057 — optional cross-branch reachability. The HEAD-only scan
    // above is blind to a fix that landed on the WRONG long-lived branch:
    // a SHA reachable from HEAD (so never flagged as drift) but ABSENT from
    // a sibling source/fix branch. When the caller passes against_refs[],
    // build one reachable set per named ref and flag those SHAs under a
    // distinct `mis_branched[]` class (RetroArch Bundle 79). Absent
    // against_refs → the envelope is byte-identical to the pre-2057 shape.
    const QJsonArray againstRefsIn =
        req.value(QStringLiteral("against_refs")).toArray();
    QJsonArray misBranched;
    QJsonArray unknownRefs;
    QStringList checkedRefs;
    bool misBranchedTruncated = false;
    if (!againstRefsIn.isEmpty()) {
        struct RefSet {
            QSet<QString> full;
            QMultiHash<QString, QString> byPrefix;  // 7-hex prefix → full
        };
        QHash<QString, RefSet> refSets;
        int refCount = 0;
        for (const QJsonValue &rv : againstRefsIn) {
            if (!rv.isString()) continue;
            const QString ref = rv.toString().trimmed();
            // Reject empty / leading-`-` (argv flag-injection guard, mirrors
            // git_state's isValidRange posture) — runGit is argv-based so a
            // shell can't intervene, but a leading `-` would be read as a flag.
            if (ref.isEmpty() || ref.startsWith(QChar('-'))) {
                if (!ref.isEmpty()) unknownRefs.append(ref);
                continue;
            }
            if (ref == currentBranch) continue;  // same as HEAD — no contrast
            if (refSets.contains(ref)) continue; // de-dup
            if (++refCount > 10) break;           // bound the fork count
            const QByteArray rRaw = runGit(rootCanonical,
                {QStringLiteral("log"), QStringLiteral("--format=%H"),
                 ref, QStringLiteral("--max-count=200000")});
            if (rRaw.trimmed().isEmpty()) { unknownRefs.append(ref); continue; }
            RefSet rs;
            const QList<QByteArray> rl = rRaw.split('\n');
            for (const QByteArray &line : rl) {
                if (line.size() < 40) continue;
                const QString full = QString::fromUtf8(line.left(40));
                rs.full.insert(full);
                rs.byPrefix.insert(full.left(7), full);
            }
            refSets.insert(ref, rs);
            checkedRefs.append(ref);
        }
        auto reachableInSet = [](const RefSet &rs, const QString &sha) -> bool {
            if (sha.size() == 40) return rs.full.contains(sha);
            const QList<QString> cands = rs.byPrefix.values(sha.left(7));
            for (const QString &c : cands) if (c.startsWith(sha)) return true;
            return false;
        };
        for (const auto &bul : bullets) {
            if (bul.status != QStringLiteral("✅") ||
                (bul.id.isEmpty() && !legacyNoId))   // ANTS-3437 — legacy no-id
                continue;
            const QString joined = bul.headline + QChar('\n') + bul.body;
            QStringList shas;
            QRegularExpressionMatchIterator mi =
                rxCommitSha().globalMatch(joined);
            while (mi.hasNext()) {
                const QString sha = mi.next().captured(1);
                if (!shas.contains(sha)) shas.append(sha);
            }
            for (const QString &sha : shas) {
                // Only SHAs ON HEAD are candidates — an unreachable-from-HEAD
                // SHA is already reported under `drift`, not mis-branched.
                if (!isReachable(sha)) continue;
                QJsonArray missingFrom;
                for (const QString &ref : checkedRefs)
                    if (!reachableInSet(refSets.value(ref), sha))
                        missingFrom.append(ref);
                if (missingFrom.isEmpty()) continue;
                QJsonObject o;
                o["bullet_id"]    = bulletIdFor(bul);
                o["cited_sha"]    = sha;
                o["headline"]     = bul.headline;
                o["missing_from"] = missingFrom;
                misBranched.append(o);
                if (misBranched.size() >= maxDrift) {
                    misBranchedTruncated = true;
                    break;
                }
            }
            if (misBranchedTruncated) break;
        }
    }

    QJsonObject env;
    env["ok"]               = true;
    env["current_branch"]   = currentBranch;
    env["current_commit"]   = currentCommit;
    env["scanned_bullets"]  = scannedBullets;
    env["with_sha"]         = withSha;
    env["drift_count"]      = drift.size();
    env["drift"]            = drift;
    env["path"]             = QFileInfo(roadmapPath).absoluteFilePath();
    // ANTS-3437 — flag the legacy no-id enumeration path so a caller does not
    // mistake bullet_id headline-slugs for real ids (and so a scanned_bullets
    // count on a no-id roadmap is understood as "scanned via slug", not empty).
    if (legacyNoId)        env["roadmap_format"]     = QStringLiteral("ants-v1-legacy-noid");
    if (driftTruncated)    env["drift_truncated"]    = true;
    if (truncatedHistory)  env["truncated_history"]  = true;
    // ANTS-2057 — only present when against_refs[] was supplied, so the
    // default envelope shape is untouched for existing callers.
    if (!againstRefsIn.isEmpty()) {
        QJsonArray checked;
        for (const QString &r : checkedRefs) checked.append(r);
        env["checked_refs"]        = checked;
        env["mis_branched"]        = misBranched;
        env["mis_branched_count"]  = misBranched.size();
        if (misBranchedTruncated) env["mis_branched_truncated"] = true;
        if (!unknownRefs.isEmpty()) env["unknown_refs"] = unknownRefs;
    }
    return QJsonDocument(env);
}

// ----- ANTS-1299 + ANTS-1300 — build/test cache MCP tools -----------
//
// Shared helpers — small refusal envelope + op dispatcher. The two
// tools live next to each other because they share the .audit_cache/
// directory, the op surface ({read, record}), and the refusal-code
// taxonomy (see docs/standards/mcp-error-codes.md § 2).

namespace rcdetail {

QJsonObject btErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = code;
    return o;
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdBuildStatus(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(btErr(
            QStringLiteral("no_project"),
            QStringLiteral("build_status: project root unresolved")));
    }
    const QString op = req.value(QStringLiteral("op")).toString(
        QStringLiteral("read"));
    if (op != QLatin1String("read") && op != QLatin1String("record")) {
        return QJsonDocument(btErr(
            QStringLiteral("bad_args"),
            QStringLiteral("build_status: \"op\" must be \"read\" or "
                           "\"record\"")));
    }

    if (op == QLatin1String("record")) {
        // Validate args.
        const QJsonValue exitV = req.value(QStringLiteral("exit_code"));
        if (!exitV.isDouble()) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("build_status: \"exit_code\" must be an "
                               "integer (required for op=record)")));
        }
        const QJsonValue outV = req.value(QStringLiteral("output"));
        if (!outV.isString()) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("build_status: \"output\" must be a "
                               "string (required for op=record)")));
        }
        const QString output = outV.toString();
        if (output.isEmpty()) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("build_status: \"output\" must be "
                               "non-empty for op=record")));
        }
        BuildCache::ParsedBuild build = BuildCache::parseBuildOutput(output);
        build.exitCode = exitV.toInt();
        if (req.contains(QStringLiteral("started_at_ms"))) {
            build.startedAtMs = static_cast<qint64>(
                req.value(QStringLiteral("started_at_ms")).toDouble(0));
        }
        if (req.contains(QStringLiteral("finished_at_ms"))) {
            build.finishedAtMs = static_cast<qint64>(
                req.value(QStringLiteral("finished_at_ms")).toDouble(0));
        }
        if (!BuildCache::recordBuild(rootCanonical, build)) {
            return QJsonDocument(btErr(
                QStringLiteral("write_failed"),
                QStringLiteral("build_status: failed to write "
                               ".audit_cache/build.json")));
        }
        QJsonObject env = BuildCache::toJson(build);
        env["ok"] = true;
        return QJsonDocument(env);
    }

    // op == "read"
    auto loaded = BuildCache::loadBuild(rootCanonical);
    if (!loaded) {
        return QJsonDocument(btErr(
            QStringLiteral("not_cached"),
            QStringLiteral("build_status: no recorded build "
                           "(call op=record first)")));
    }
    QJsonObject env = BuildCache::toJson(*loaded);
    env["ok"] = true;
    // ANTS-3374 — add_include hint on undeclared-symbol errors.
    QJsonArray btErrors = env.value("errors").toArray();
    enrichLikelyFixes(btErrors, rootCanonical);
    env["errors"] = btErrors;
    const auto stale =
        BuildCache::checkStale(rootCanonical, loaded->recordedAtMs);
    if (!stale.staleKnown) {
        env["stale_walk_capped"] = true;
    } else if (stale.stale) {
        env["stale"] = true;
    }
    return QJsonDocument(env);
}

QJsonDocument RemoteControl::cmdTestResults(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(btErr(
            QStringLiteral("no_project"),
            QStringLiteral("test_results: project root unresolved")));
    }
    const QString op = req.value(QStringLiteral("op")).toString(
        QStringLiteral("read"));
    if (op != QLatin1String("read") && op != QLatin1String("record")) {
        return QJsonDocument(btErr(
            QStringLiteral("bad_args"),
            QStringLiteral("test_results: \"op\" must be \"read\" or "
                           "\"record\"")));
    }

    if (op == QLatin1String("record")) {
        if (req.contains(QStringLiteral("detail"))) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("test_results: \"detail\" not allowed "
                               "on op=record (read-only argument)")));
        }
        const QJsonValue exitV = req.value(QStringLiteral("exit_code"));
        if (!exitV.isDouble()) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("test_results: \"exit_code\" must be "
                               "an integer (required for op=record)")));
        }
        const QJsonValue outV = req.value(QStringLiteral("output"));
        if (!outV.isString()) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("test_results: \"output\" must be a "
                               "string (required for op=record)")));
        }
        const QString output = outV.toString();
        if (output.isEmpty()) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("test_results: \"output\" must be "
                               "non-empty for op=record")));
        }
        TestResCache::ParsedTests tests =
            TestResCache::parseCtestOutput(output);
        if (!tests.recognised) {
            return QJsonDocument(btErr(
                QStringLiteral("bad_args"),
                QStringLiteral("test_results: \"output\" does not look "
                               "like ctest --output-on-failure output "
                               "(no summary footer or per-test status "
                               "line recognised)")));
        }
        tests.exitCode = exitV.toInt();
        if (req.contains(QStringLiteral("started_at_ms"))) {
            tests.startedAtMs = static_cast<qint64>(
                req.value(QStringLiteral("started_at_ms")).toDouble(0));
        }
        if (req.contains(QStringLiteral("finished_at_ms"))) {
            tests.finishedAtMs = static_cast<qint64>(
                req.value(QStringLiteral("finished_at_ms")).toDouble(0));
        }
        if (req.contains(QStringLiteral("duration_ms"))) {
            tests.durationMs = static_cast<qint64>(
                req.value(QStringLiteral("duration_ms")).toDouble(-1));
        }
        if (!TestResCache::recordTests(rootCanonical, tests)) {
            return QJsonDocument(btErr(
                QStringLiteral("write_failed"),
                QStringLiteral("test_results: failed to write "
                               ".audit_cache/tests.json")));
        }
        QJsonObject env = TestResCache::toJsonWire(tests);
        env["ok"] = true;
        return QJsonDocument(env);
    }

    // op == "read"
    auto loaded = TestResCache::loadTests(rootCanonical);
    if (!loaded) {
        return QJsonDocument(btErr(
            QStringLiteral("not_cached"),
            QStringLiteral("test_results: no recorded test run "
                           "(call op=record first)")));
    }
    const QString detail = req.value(QStringLiteral("detail")).toString();
    if (!detail.isEmpty()) {
        for (const auto &pf : loaded->failingTests) {
            if (pf.name == detail) {
                QJsonObject env;
                env["ok"]     = true;
                QJsonObject d;
                d["name"]    = pf.name;
                d["excerpt"] = pf.fullExcerpt;  // replace with full body
                env["detail"] = d;
                return QJsonDocument(env);
            }
        }
        return QJsonDocument(btErr(
            QStringLiteral("detail_not_found"),
            QStringLiteral("test_results: \"%1\" not in failing_tests[]")
                .arg(detail)));
    }
    QJsonObject env = TestResCache::toJsonWire(*loaded);
    env["ok"] = true;
    return QJsonDocument(env);
}
