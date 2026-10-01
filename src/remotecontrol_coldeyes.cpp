// ANTS-3833 TU 13/20 — Cold eyes and test verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "buildfixhint.h"   // ANTS-3374 — enrichLikelyFixes
#include "buildcache.h"
#include "pathvalidation.h"
#include "falseposledger.h"
#include "projectlayoutengine.h"
#include "remotecontrolgate.h"
#include "sessionmemoryengine.h"
#include "testrescache.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include "resolvedroot.h"

using namespace rcdetail;  // ANTS-3833

QJsonDocument RemoteControl::cmdColdEyesPartition(const QJsonObject &req) {
    if (!m_roots) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_partition: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_roots, req);
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
    if (!m_roots) return QJsonDocument(ceErr(
        QStringLiteral("no_window"),
        QStringLiteral("cold_eyes_brief: no MainWindow")));
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString root = resolveRootCanonical(m_roots, req);
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
    if (!m_roots) return QJsonDocument(smErr(
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
            resolveRootCanonical(m_roots), req,
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
    if (!m_roots) {
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
        resolveRootCanonical(m_roots), req,
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
    if (!m_roots) {
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

// ANTS-4932 § 2.2 — moved from TU 2, which went GUI-side: cmdBuildStatus
// below and TU 2's cmdRecentErrors both call it, and it reads no window.
void RemoteControl::enrichLikelyFixes(QJsonArray &errors,
                                      const QString &root) const {
    // ANTS-3374 — stitch the diagnose→fix loop: on an undeclared-symbol
    // diagnostic, resolve the declaring header and attach a `likely_fix`
    // add_include hint. Dedups by symbol (cascades name the same symbol
    // repeatedly) and caps distinct header lookups so a wall of errors
    // can't fan out into an unbounded SymbolQuery tree-walk.
    if (root.isEmpty() || errors.isEmpty()) return;
    constexpr int kMaxLookups = 25;
    // ANTS-5053 — gather the distinct symbols first (up to the cap), then
    // resolve them in ONE tree walk: a walk per symbol cost seconds on the
    // GUI thread right after a failed build.
    QStringList wanted;
    for (const QJsonValue &v : std::as_const(errors)) {
        const QString sym = BuildFixHint::undeclaredSymbol(
            v.toObject().value("message").toString());
        if (!sym.isEmpty() && !wanted.contains(sym) && wanted.size() < kMaxLookups)
            wanted << sym;
    }
    if (wanted.isEmpty()) return;
    const QHash<QString, QString> headerBySym =
        BuildFixHint::resolveHeaders(root, wanted);  // symbol → header ("" = miss)
    for (int i = 0; i < errors.size(); ++i) {
        QJsonObject e = errors.at(i).toObject();
        const QString sym =
            BuildFixHint::undeclaredSymbol(e.value("message").toString());
        // Empty for a miss, and for a symbol past the lookup cap.
        const QString header = headerBySym.value(sym);
        if (header.isEmpty()) continue;
        QJsonObject lf;
        lf["add_include"] = header;
        lf["defines"]     = sym;
        const QString at = e.value("file").toString();
        if (!at.isEmpty()) lf["at"] = at;
        e["likely_fix"] = lf;
        errors.replace(i, e);
    }
}

QJsonDocument RemoteControl::cmdBuildStatus(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_roots, req);
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
    const QString rootCanonical = resolveRootCanonical(m_roots, req);
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
