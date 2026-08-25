// ANTS-3833 TU 10/17 — Session and state verbs.
#include "remotecontrol.h"
#include "gitwrap.h"   // ANTS-4352 — gate_drift
#include "remotecontrol_internal.h"
#include "buildcache.h"
#include "feedbackfile.h"        // ANTS-1961 / ANTS-1962
#include "findsources.h"
#include "readregion.h"
#include "specparse.h"           // ANTS-3665 — hoisted spec-body parser
#include "modelswitchledger.h"   // ANTS-1735 — model_switch_stats aggregation
#include "modelnearmissledger.h" // ANTS-1894 — model_switch_stats near-miss arm
#include "focusedtest.h"
#include "config.h"
#include "pathvalidation.h"
#include "projectsettings.h"    // ANTS-2160 — .ants/project.json overrides
#include "roadmapstore.h"       // ANTS-4622 — session_orient's mail_pending
#include "similarcode.h"
#include "subsystemmap.h"
#include "testrescache.h"
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <sys/socket.h>
#include "build_info.h"
#include <unistd.h>

using namespace rcdetail;  // ANTS-3833

QJsonDocument RemoteControl::cmdGitState(const QJsonObject &req) {
    // ANTS-1250-INV-1: dispatch on op ∈ {status, log, diff}.
    // ANTS-3365: the verb is advertised as a one-call "status + branch +
    // ahead/behind" read, so an omitted op defaults to "status" (a bare
    // git_state{caller_cwd} just works). A non-empty unknown op still
    // refuses with bad_op below.
    QString op = req.value("op").toString();
    if (op.isEmpty()) op = QStringLiteral("status");
    if (op == QLatin1String("status")) {
        // ANTS-1391: thread req through so caller_cwd anchors the root.
        return QJsonDocument(runStatusOp(m_main, req));
    }
    if (op == QLatin1String("log")) {
        return QJsonDocument(runLogOp(m_main, req));
    }
    if (op == QLatin1String("diff")) {
        return QJsonDocument(runDiffOp(m_main, req));
    }
    return QJsonDocument(gitErr("bad_op",
        QStringLiteral("git_state: \"op\" must be one of "
                       "{status, log, diff}, got \"%1\"").arg(op)));
    // ANTS-1250-INV-2: parseStatusHeader handles `## branch...upstream
    //                  [ahead N, behind M]`.
    // ANTS-1250-INV-7: stderr cap enforced inside GitWrap::run.
    // ANTS-1250-INV-9: gitwrap.started=false → git_missing.
    // ANTS-1250-INV-10: non-zero exit → git_failed with stderr.
    // ANTS-1250-INV-13: reachability gate inherits from UDS + MCP socket
    //                  (SO_PEERCRED UID + 0700 + S_ISSOCK).
}

// Client — runs in the --remote invocation of the binary. No Qt
// event loop; synchronous connect → write → readLine → exit.
int RemoteControl::runClient(const QString &command,
                             const QJsonObject &args,
                             const QString &socketPath) {
    QJsonObject env = args;
    env["cmd"] = command;
    const QByteArray payload = QJsonDocument(env).toJson(
        QJsonDocument::Compact) + '\n';

    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(2000)) {
        fprintf(stderr,
            "ants-terminal --remote: cannot connect to %s (%s)\n"
            "  Is Ants Terminal running with remote-control enabled?\n"
            "  Override the path via ANTS_REMOTE_SOCKET=...\n",
            qUtf8Printable(socketPath),
            qUtf8Printable(socket.errorString()));
        return 1;
    }
    // ANTS-1995 — verify the SERVER's peer UID before any payload leaves
    // this process. The server already checks the client's UID via
    // SO_PEERCRED (line ~1296), but the client never checked the server's:
    // a process that wins the socket path ($ANTS_REMOTE_SOCKET override,
    // or a predictable default in a shared dir) could accept the
    // connection and harvest the request body (caller_cwd, file paths,
    // notes) we are about to write. Mirror the server's fail-closed check
    // and abort on any mismatch before write().
    {
        const qintptr fd = socket.socketDescriptor();
        struct ucred cred{};
        socklen_t len = sizeof(cred);
        if (fd < 0 ||
            ::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_PEERCRED,
                         &cred, &len) != 0 ||
            len != sizeof(cred) || cred.uid != ::getuid()) {
            fprintf(stderr,
                "ants-terminal --remote: server peer-credential check "
                "failed — refusing to send request (suspect socket "
                "hijack via ANTS_REMOTE_SOCKET)\n");
            return 1;
        }
    }
    socket.write(payload);
    if (!socket.waitForBytesWritten(2000)) {
        fprintf(stderr, "ants-terminal --remote: write timeout\n");
        return 1;
    }
    // Read until newline or disconnect. ANTS-1169: cap the receive
    // buffer at 1 MiB to mirror the server's frame cap. Without this
    // a same-UID malicious local process could answer the client
    // (set $ANTS_REMOTE_SOCKET to its own listener) and reply with a
    // multi-hundred-MB body that saturates this client process.
    constexpr qint64 kMaxResponseBytes = 1 * 1024 * 1024;
    // ANTS-1671 M5 — overall read deadline. The per-iteration 2 s timeout
    // resets every loop, so a hostile peer (one that won $ANTS_REMOTE_SOCKET
    // and answers in this client's place) could dribble one byte just under
    // every 2 s and tie this process up indefinitely while staying under the
    // 1 MiB byte cap. The byte cap bounds total data, not total time — so
    // bound the cumulative wait as well.
    constexpr qint64 kOverallTimeoutMs = 10 * 1000;
    const qint64 deadline =
        QDateTime::currentMSecsSinceEpoch() + kOverallTimeoutMs;
    QByteArray resp;
    while (socket.waitForReadyRead(2000)) {
        resp += socket.readAll();
        if (resp.contains('\n')) break;
        if (resp.size() > kMaxResponseBytes) {
            fprintf(stderr,
                    "ants-terminal --remote: response exceeds %lld bytes; "
                    "aborting (suspect socket hijack)\n",
                    static_cast<long long>(kMaxResponseBytes));
            return 1;
        }
        if (QDateTime::currentMSecsSinceEpoch() > deadline) {
            fprintf(stderr,
                    "ants-terminal --remote: response timed out after %lld ms; "
                    "aborting (suspect slow-drip peer)\n",
                    static_cast<long long>(kOverallTimeoutMs));
            return 1;
        }
    }
    if (resp.isEmpty()) {
        fprintf(stderr, "ants-terminal --remote: no response\n");
        return 1;
    }
    // Strip trailing newline for tidier stdout.
    while (!resp.isEmpty() && (resp.endsWith('\n') || resp.endsWith('\r'))) {
        resp.chop(1);
    }
    fwrite(resp.constData(), 1, resp.size(), stdout);
    fputc('\n', stdout);

    // Exit-code shaping: parse the "ok" field so callers can
    // `if ants-terminal --remote ls; then ...` without piping
    // through jq.
    QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (doc.isObject() && doc.object().value("ok").toBool()) return 0;
    return 2;
}

// =====================================================================
// ANTS-1251 — subsystem (consolidated; map / files / recent_changes)
// =====================================================================

namespace rcdetail {

QJsonObject subsystemErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]      = false;
    o["code"]    = code;
    o["error"]   = message;
    return o;
}

// Locate `CLAUDE.md` for the focused tab's project: walk up from the
// shellCwd looking for a file named CLAUDE.md; stop at filesystem root.
// Returns absolute path or empty string.
QString findClaudeMdForRoot(const QString &startDir) {
    if (startDir.isEmpty()) return {};
    QDir d(startDir);
    while (true) {
        const QString candidate = d.filePath(QStringLiteral("CLAUDE.md"));
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).canonicalFilePath();
        }
        if (!d.cdUp()) break;
    }
    return {};
}

QJsonObject lanesAsJson(const QVector<SubsystemMap::Lane> &lanes) {
    QJsonObject root;
    QJsonArray arr;
    for (const auto &l : lanes) {
        QJsonObject o;
        o["name"]    = l.name;
        o["summary"] = l.summary;
        arr.append(o);
    }
    root["lanes"] = arr;
    return root;
}

QJsonArray laneNamesArray(const QVector<SubsystemMap::Lane> &lanes) {
    QJsonArray arr;
    for (const auto &l : lanes) arr.append(l.name);
    return arr;
}

bool laneIsKnown(const QString &name, const QVector<SubsystemMap::Lane> &lanes) {
    for (const auto &l : lanes) {
        if (l.name == name) return true;
    }
    return false;
}

// ANTS-1251-INV-4: enumerate `src/<lane>*` files; canonical-startswith
// re-check each result against project root before yielding it.
QStringList resolveLaneFiles(const QString &lane, const QString &rootCanonical) {
    QStringList out;
    if (rootCanonical.isEmpty()) return out;
    QDir srcDir(QDir(rootCanonical).filePath(QStringLiteral("src")));
    if (!srcDir.exists()) return out;
    const QStringList filters{ lane + QStringLiteral("*") };
    const QFileInfoList entries = srcDir.entryInfoList(
        filters, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &fi : entries) {
        // ANTS-1295: route the defence-in-depth anchor (malicious
        // symlink inside src/ that points outside root) through the
        // central validator. We treat any failure here as "skip this
        // entry" rather than emitting the validator's envelope, since
        // resolveLaneFiles is a filter and the lane itself is already
        // a parsed-CLAUDE.md member.
        const auto check = PathValidation::validatePath(
            fi.absoluteFilePath(), rootCanonical,
            QStringLiteral("subsystem"), QStringLiteral("file"));
        if (check.bad || check.resolved.isEmpty()) continue;
        // Emit repo-relative paths.
        QString rel = check.resolved;
        rel.remove(0, rootCanonical.size());
        if (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}


}  // namespace rcdetail

QJsonDocument RemoteControl::cmdSubsystem(const QJsonObject &req) {
    const QString op = req.value("op").toString();
    if (op != QLatin1String("map") &&
        op != QLatin1String("files") &&
        op != QLatin1String("recent_changes")) {
        return QJsonDocument(subsystemErr("bad_op",
            QStringLiteral("subsystem: \"op\" must be one of "
                           "{map, files, recent_changes}, got \"%1\"").arg(op)));
    }

    // ANTS-1391: caller_cwd anchors the root when present.
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    const QString claudeMdPath  = findClaudeMdForRoot(rootCanonical);
    // ANTS-1292: the module map lives in docs/subsystems.md when present,
    // else falls back to CLAUDE.md (un-migrated projects).
    const QString sourcePath = SubsystemMap::resolveSource(claudeMdPath);
    // Note: cachedLanes returns empty when the file is missing; that
    // collapses to an empty `lanes[]` for op:"map" (INV-7) and a
    // unknown_lane error for the other ops.
    const QVector<SubsystemMap::Lane> lanes =
        SubsystemMap::cachedLanes(sourcePath);

    if (op == QLatin1String("map")) {
        QJsonObject ok;
        ok["ok"]     = true;
        ok["op"]     = "map";
        // Report the source relative to root so callers can see whether
        // it came from docs/subsystems.md (ANTS-1292) or legacy CLAUDE.md.
        QString srcLabel = sourcePath;
        if (!rootCanonical.isEmpty() && srcLabel.startsWith(rootCanonical)) {
            srcLabel.remove(0, rootCanonical.size());
            if (srcLabel.startsWith(QLatin1Char('/'))) srcLabel.remove(0, 1);
        }
        ok["source"] = srcLabel.isEmpty() ? QStringLiteral("CLAUDE.md")
                                          : srcLabel;
        // ANTS-3414 — optional `name` substring filter: narrow the lane
        // list to lanes whose name contains `name` (case-insensitive), so a
        // caller after one subsystem gets just its slice instead of the whole
        // map. Empty/missing → the full list (back-compat). Echoed back so
        // the caller sees the filter applied; a needle matching nothing
        // yields an empty lanes[] (not an error — op:map lists, it doesn't
        // validate a lane the way files / recent_changes do).
        QJsonArray lanesJson = lanesAsJson(lanes).value("lanes").toArray();
        const QString nameFilter = req.value("name").toString().trimmed();
        if (!nameFilter.isEmpty()) {
            QJsonArray filtered;
            for (const QJsonValue &v : std::as_const(lanesJson)) {
                if (v.toObject().value("name").toString()
                        .contains(nameFilter, Qt::CaseInsensitive))
                    filtered.append(v);
            }
            lanesJson  = filtered;
            ok["name"] = nameFilter;
        }
        ok["lanes"]  = lanesJson;
        return QJsonDocument(ok);
    }

    // ANTS-1251-INV-1: lane validation precedes any filesystem call.
    const QString lane = req.value("lane").toString();
    if (lane.isEmpty() || !laneIsKnown(lane, lanes)) {
        QJsonObject err = subsystemErr("unknown_lane",
            QStringLiteral("subsystem: \"lane\" \"%1\" is not in the "
                           "parsed Module map").arg(lane));
        err["lanes"] = laneNamesArray(lanes);
        return QJsonDocument(err);
    }

    if (op == QLatin1String("files")) {
        // ANTS-1251-INV-4 inside resolveLaneFiles.
        const QStringList files = resolveLaneFiles(lane, rootCanonical);
        QJsonObject ok;
        ok["ok"]   = true;
        ok["op"]   = "files";
        ok["lane"] = lane;
        QJsonArray arr;
        for (const QString &f : files) arr.append(f);
        ok["files"] = arr;
        return QJsonDocument(ok);
    }

    // op == "recent_changes"
    // ANTS-1251-INV-5: compose cmdGitState({op:"log", ...}) per file
    // resolved for the lane; merge by sha; keep top n by date.
    int n = 10;
    if (req.contains("n") && req.value("n").isDouble()) {
        n = req.value("n").toInt();
    }
    if (n < 1)   n = 1;
    if (n > 100) n = 100;

    const QStringList files = resolveLaneFiles(lane, rootCanonical);
    if (files.isEmpty()) {
        QJsonObject ok;
        ok["ok"]      = true;
        ok["op"]      = "recent_changes";
        ok["lane"]    = lane;
        ok["commits"] = QJsonArray{};
        return QJsonDocument(ok);
    }

    // sha → commit object; preserve insertion order for tie-breaks.
    QHash<QString, QJsonObject>           bySha;
    QVector<QString>                      shaOrder;
    // ANTS-1340 (was ANTS-1251-INV-5: per-file compose) — ONE batched
    // `git log -- <all lane files>` instead of forking git once per file
    // (worst case 20 files × 5 s = 100 s blocking the GUI on a wedged repo).
    // git returns the union of commits touching any lane file, already deduped
    // + date-sorted; the merge/sort/truncate below is kept as a defensive
    // normaliser (a near-no-op on the batched result, and it still bounds the
    // output to n).
    {
        QJsonObject sub;
        sub["op"] = "log";
        sub["n"]  = n;
        QJsonArray pathsArr;
        for (const QString &f : files) pathsArr.append(f);
        sub["paths"] = pathsArr;
        // Thread caller_cwd so git resolves against the caller's project,
        // not the focused tab — matches every sibling composer
        // (current_state / task_priors / build_status). indie-review-2026-05-21.
        sub["caller_cwd"] = req.value("caller_cwd");
        const QJsonObject r = cmdGitState(sub).object();
        if (r.value("ok").toBool()) {
            const QJsonArray commits = r.value("commits").toArray();
            for (const QJsonValue &v : commits) {
                const QJsonObject c = v.toObject();
                const QString sha = c.value("sha").toString();
                if (sha.isEmpty() || bySha.contains(sha)) continue;
                bySha.insert(sha, c);
                shaOrder.push_back(sha);
            }
        }
    }

    // Sort merged commits by date desc, fall back to insertion order
    // when dates tie.
    std::sort(shaOrder.begin(), shaOrder.end(),
              [&](const QString &a, const QString &b) {
                  return bySha.value(a).value("date").toString() >
                         bySha.value(b).value("date").toString();
              });
    if (shaOrder.size() > n) shaOrder.resize(n);

    QJsonArray commits;
    for (const QString &sha : shaOrder) commits.append(bySha.value(sha));

    QJsonObject ok;
    ok["ok"]      = true;
    ok["op"]      = "recent_changes";
    ok["lane"]    = lane;
    ok["commits"] = commits;
    return QJsonDocument(ok);
    // ANTS-1251-INV-6: reachability gate inherits from UDS + MCP socket.
}

// ============================================================
// ANTS-1254 — last_audit_summary
// ============================================================
//
// Reads the lex-max audit-*.sarif under {projectRoot}/.audit_cache,
// returns counts + top_findings. Single-entry mtime-keyed cache.
// Reachability gate inherits from UDS + MCP socket (INV-5).

namespace rcdetail {

// ANTS-1576 — forward declaration of the runGit helper defined further
// down in this file (used by the live-git fallback in cmdLastAuditSummary).
// Definition lives near collectGitSnapshot at the bottom of the file.
QByteArray runGit(const QString &root, const QStringList &argv);

QJsonObject lasErr(const QString &code, const QString &msg) {
    QJsonObject o;
    o["ok"]    = false;
    o["code"]  = code;
    o["error"] = msg;
    return o;
}

QJsonObject auditSummaryFindingAsJson(
    const AuditEngine::AuditSummaryFinding &f) {
    QJsonObject o;
    o["level"]          = f.level;
    o["severity"]       = f.severity;
    o["ruleId"]         = f.ruleId;
    o["file"]           = f.file;
    o["line"]           = f.line;
    o["message"]        = f.message;
    o["confidence"]     = f.confidence;
    o["highConfidence"] = f.highConfidence;
    return o;
}

QJsonObject buildLasEnvelope(const AuditEngine::AuditSummary &s) {
    QJsonObject ok;
    ok["ok"]         = true;
    // ANTS-1576 — null-or-omit normalisation. Always emit the
    // load-bearing fields (sarif_path / source_format / counts /
    // top_findings); omit run_at / html_path when blank.
    if (!s.runAtIso.isEmpty())  ok["run_at"]    = s.runAtIso;
    ok["sarif_path"] = s.sarifPath;
    if (!s.htmlPath.isEmpty())  ok["html_path"] = s.htmlPath;
    // ANTS-1459 — name the source format on every response so the
    // caller doesn't have to guess from sarif_path's extension.
    ok["source_format"] = s.sourceFormat.isEmpty()
        ? QStringLiteral("sarif")
        : s.sourceFormat;

    QJsonObject counts;
    counts["error"]      = s.countError;
    counts["warning"]    = s.countWarning;
    counts["note"]       = s.countNote;
    counts["suppressed"] = s.countSuppressed;
    ok["counts"] = counts;

    QJsonArray top;
    for (const auto &f : s.topFindings) top.append(auditSummaryFindingAsJson(f));
    ok["top_findings"] = top;

    // ANTS-1539 + ANTS-1576 — surface capture-time git provenance.
    // Fields omitted when empty (no probe succeeded). ANTS-1576 adds
    // `branch_source` ("file_provenance" | "read_time") so the caller
    // can distinguish the SARIF-carried record from the live read-time
    // fallback.
    if (!s.branch.isEmpty())        ok["branch"]         = s.branch;
    if (!s.commit.isEmpty())        ok["commit"]         = s.commit;
    if (!s.repositoryUri.isEmpty()) ok["repository_uri"] = s.repositoryUri;
    if (!s.branchSource.isEmpty())  ok["branch_source"]  = s.branchSource;

    return ok;
}

// ANTS-1576 — scope classifier. Inspects the parsed top-findings and
// the report basename; tags the response as single_file / narrow /
// broad. Counts distinct files in topFindings[] (server-clamped to
// 50 by the caller, so O(50) max). Pure function.
struct ScopeClassification {
    QString     tag;            // "single_file" | "narrow" | "broad"
    QStringList distinctFiles;  // up to 5 entries (preview list)
};

ScopeClassification classifyAuditScope(
    const AuditEngine::AuditSummary &s,
    const QString &reportPath) {
    QSet<QString> seen;
    QStringList preview;
    for (const auto &f : s.topFindings) {
        if (!seen.contains(f.file)) {
            seen.insert(f.file);
            if (preview.size() < 5) preview.append(f.file);
        }
    }
    const int distinct = seen.size();

    const QString base = QFileInfo(reportPath).baseName();
    const bool narrowHint =
        base.contains(QStringLiteral("-postfix")) ||
        base.contains(QStringLiteral("-single"))  ||
        base.contains(QStringLiteral("-narrow"));

    ScopeClassification c;
    c.distinctFiles = preview;
    if (distinct == 1 && !narrowHint) {
        c.tag = QStringLiteral("single_file");
    } else if (distinct >= 1 && distinct <= 5) {
        c.tag = QStringLiteral("narrow");
    } else {
        c.tag = QStringLiteral("broad");
    }
    return c;
}

// ANTS-1625 — foreign-format picker preference. The lex-max picker that
// audit-cache SARIF naming relies on is wrong for user-named foreign-
// scanner outputs (cppcheck-b68-ozone-postfix.xml sorts above
// cppcheck-broad.xml even though the latter is the actually-broad sweep).
// Among candidates within a 24-hour window of the newest file, prefer
// non-narrow-name (no `-postfix` / `-single` / `-narrow` suffix) and
// larger size. Returns `{basename, basis}` where basis ∈
// {"sole","newest","broadest_in_recency_window"}.
struct ForeignPick {
    QString name;
    QString basis;
};

ForeignPick pickForeignReport(const QDir &cacheDir, const QString &glob) {
    ForeignPick out;
    const QStringList ns = cacheDir.entryList(
        QStringList{glob}, QDir::Files, QDir::Name | QDir::Reversed);
    if (ns.isEmpty()) return out;
    if (ns.size() == 1) {
        out.name  = ns.first();
        out.basis = QStringLiteral("sole");
        return out;
    }

    struct Cand {
        QString name;
        qint64  mtimeMs = 0;
        qint64  size    = 0;
        bool    narrow  = false;
    };
    QList<Cand> cands;
    qint64 newestMs = 0;
    for (const QString &n : ns) {
        const QFileInfo fi(cacheDir.absoluteFilePath(n));
        Cand c;
        c.name    = n;
        c.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
        c.size    = fi.size();
        const QString lower = n.toLower();
        // Narrow-suffix set mirrors ANTS-1576's classifyAuditScope
        // verbatim — keep the two surfaces consistent so a caller
        // seeing `pick_basis == "broadest_in_recency_window"` and
        // `scope == "narrow"` reads as the picker preferred broader
        // already and the chosen file still looks narrow.
        c.narrow = lower.contains(QStringLiteral("-postfix"))
                || lower.contains(QStringLiteral("-single"))
                || lower.contains(QStringLiteral("-narrow"));
        cands.append(c);
        if (c.mtimeMs > newestMs) newestMs = c.mtimeMs;
    }

    constexpr qint64 kRecencyWindowMs = 24LL * 60LL * 60LL * 1000LL;
    const qint64 minMs = newestMs - kRecencyWindowMs;

    // Locate the lex-max-name newest file (matches the legacy picker
    // behaviour: when no broader candidate is in-window, we surface
    // this entry with basis == "newest").
    QString newestName;
    for (const Cand &c : cands) {
        if (c.mtimeMs != newestMs) continue;
        if (newestName.isEmpty() || c.name > newestName) newestName = c.name;
    }

    // Among in-window candidates, prefer non-narrow then larger size;
    // tiebreak lex-max name.
    const Cand *best = nullptr;
    for (const Cand &c : cands) {
        if (c.mtimeMs < minMs) continue;
        if (!best) { best = &c; continue; }
        // non-narrow beats narrow
        if (c.narrow != best->narrow) {
            if (!c.narrow) best = &c;
            continue;
        }
        // same narrowness: larger size wins
        if (c.size != best->size) {
            if (c.size > best->size) best = &c;
            continue;
        }
        // size tie: lex-max name wins (matches legacy ordering)
        if (c.name > best->name) best = &c;
    }
    if (!best) {
        // No candidate inside the window (only possible if the file
        // mtimes span > 24h AND only one file is the newest). Fall
        // back to the legacy newest.
        out.name  = newestName;
        out.basis = QStringLiteral("newest");
        return out;
    }
    out.name  = best->name;
    out.basis = (best->name == newestName)
        ? QStringLiteral("newest")
        : QStringLiteral("broadest_in_recency_window");
    return out;
}

// ANTS-1540 — post-cap rule_ids filter. Operates on a snapshot of
// AuditSummary; restricts topFindings[] to entries whose ruleId is in
// the filter set, then caps to `cap`. Pure function.
AuditEngine::AuditSummary applyRuleIdsFilter(
    AuditEngine::AuditSummary s,
    const QSet<QString> &ruleIds,
    int cap) {
    QList<AuditEngine::AuditSummaryFinding> kept;
    kept.reserve(s.topFindings.size());
    for (const auto &f : s.topFindings) {
        if (ruleIds.contains(f.ruleId)) kept.append(f);
        if (kept.size() >= cap) break;
    }
    s.topFindings = std::move(kept);
    return s;
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdLastAuditSummary(const QJsonObject &req) {
    // INV-8: severity_floor validation runs before disk scanning.
    QString floor = QStringLiteral("warning");
    if (req.contains(QStringLiteral("severity_floor"))) {
        floor = req.value(QStringLiteral("severity_floor")).toString();
    }
    if (floor != QLatin1String("error") &&
        floor != QLatin1String("warning") &&
        floor != QLatin1String("note")) {
        return QJsonDocument(lasErr(QStringLiteral("bad_severity_floor"),
            QStringLiteral("last_audit_summary: \"severity_floor\" "
                           "must be one of {error, warning, note}")));
    }

    // top_n default 5, server-clamp [0, 50].
    int topN = 5;
    if (req.contains(QStringLiteral("top_n")) &&
        req.value(QStringLiteral("top_n")).isDouble()) {
        topN = req.value(QStringLiteral("top_n")).toInt();
    }
    if (topN < 0)  topN = 0;
    if (topN > 50) topN = 50;

    // ANTS-1540 — optional `rule_ids` filter. When set, the internal
    // parser pass uses a generous topN=50 so a rare rule that didn't
    // make the default top-N still surfaces, then we post-filter to
    // ruleId ∈ set and re-cap to the caller's topN.
    QSet<QString> ruleIdsFilter;
    bool ruleIdsRequested = false;
    if (req.contains(QStringLiteral("rule_ids")) &&
        req.value(QStringLiteral("rule_ids")).isArray()) {
        const QJsonArray arr = req.value(QStringLiteral("rule_ids")).toArray();
        for (const QJsonValue &v : arr) {
            if (!v.isString()) continue;
            const QString s = v.toString().trimmed();
            if (!s.isEmpty()) ruleIdsFilter.insert(s);
        }
        // Empty array ⇒ filter absent (per schema). Non-empty ⇒ active.
        ruleIdsRequested = !ruleIdsFilter.isEmpty();
    }
    const int parserTopN = ruleIdsRequested ? 50 : topN;

    // Discover latest SARIF in {projectRoot}/.audit_cache.
    // ANTS-1391: caller_cwd anchors the root when present.
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(lasErr(QStringLiteral("not_audited"),
            QStringLiteral("last_audit_summary: project root unresolved")));
    }
    QDir cacheDir(rootCanonical + QStringLiteral("/.audit_cache"));
    if (!cacheDir.exists()) {
        return QJsonDocument(lasErr(QStringLiteral("not_audited"),
            QStringLiteral("last_audit_summary: no .audit_cache directory")));
    }
    const QStringList sarifNames = cacheDir.entryList(
        QStringList{QStringLiteral("audit-*.sarif")},
        QDir::Files, QDir::Name | QDir::Reversed);
    // ANTS-1459 + ANTS-1494 — when no SARIF is present, fall back to a
    // raw-scanner-output discovery order:
    //   cppcheck-*.xml   (cppcheck --xml --xml-version=2)
    //   clang-tidy-*.txt (clang-tidy native text)
    //   semgrep-*.json   (semgrep --json)
    // First non-empty match wins; mtime preserved as the discovery
    // pivot via QDir::Name|Reversed lexicographic ordering on the
    // pattern-matched names. Returning the same envelope shape
    // regardless of input format is the discoverability fix.
    QString reportPath;
    QString sourceFormat;  // "sarif" | "cppcheck-xml" | "clang-tidy-text" | "semgrep-json"
    // ANTS-1625 — pick_basis records how the picker landed on `reportPath`.
    //   "sole"                       — one match for the chosen glob
    //   "newest"                     — multi-match; chosen entry is the newest
    //   "broadest_in_recency_window" — multi-match; picker preferred a
    //                                  broader non-narrow-name file within
    //                                  24 h of the newest entry
    QString pickBasis;
    auto pickForeign = [&](const QString &glob, const QString &tag) {
        if (!reportPath.isEmpty()) return;
        const auto fp = pickForeignReport(cacheDir, glob);
        if (fp.name.isEmpty()) return;
        reportPath   = cacheDir.absoluteFilePath(fp.name);
        sourceFormat = tag;
        pickBasis    = fp.basis;
    };
    if (!sarifNames.isEmpty()) {
        reportPath   = cacheDir.absoluteFilePath(sarifNames.first());
        sourceFormat = QStringLiteral("sarif");
        // SARIF naming (audit-<iso-utc>-<sha>.sarif) sorts
        // lex-max == newest, so the existing lex-max behaviour is
        // already "newest". Tag accordingly so every {ok:true}
        // envelope carries pick_basis (INV-2).
        pickBasis = (sarifNames.size() == 1)
            ? QStringLiteral("sole")
            : QStringLiteral("newest");
    } else {
        pickForeign(QStringLiteral("cppcheck-*.xml"),
                    QStringLiteral("cppcheck-xml"));
        pickForeign(QStringLiteral("clang-tidy-*.txt"),
                    QStringLiteral("clang-tidy-text"));
        pickForeign(QStringLiteral("semgrep-*.json"),
                    QStringLiteral("semgrep-json"));
    }
    if (reportPath.isEmpty()) {
        return QJsonDocument(lasErr(QStringLiteral("not_audited"),
            QStringLiteral("last_audit_summary: no audit-*.sarif, "
                           "cppcheck-*.xml, clang-tidy-*.txt, or "
                           "semgrep-*.json found under .audit_cache "
                           "(ANTS-1459 + ANTS-1494). Run audit_run or "
                           "one of the supported scanners first.")));
    }

    // Cache key: (path, mtime, parserTopN, floor). Keyed on resolved
    // path so a cppcheck-xml read can't collide with a sarif read.
    // ANTS-1540 — when rule_ids is set, the parser ran with the
    // expanded budget (50), so the cache slot is keyed off that
    // budget; subsequent calls without rule_ids re-parse only if the
    // earlier slot used a smaller cap.
    const qint64 mtimeMs =
        QFileInfo(reportPath).lastModified().toMSecsSinceEpoch();
    const bool hit = (reportPath == m_auditSummaryPath
                      && mtimeMs == m_auditSummaryMtimeMs
                      && parserTopN == m_auditSummaryCachedTopN
                      && floor   == m_auditSummaryCachedFloor);
    AuditEngine::AuditSummary summary;
    if (hit) {
        summary = m_auditSummaryCache;
    } else {
        // Cache miss — parse with the format-appropriate engine helper.
        auto parsed = [&]() -> std::optional<AuditEngine::AuditSummary> {
            if (sourceFormat == QLatin1String("cppcheck-xml"))
                return AuditEngine::summariseCppcheckXml(reportPath, parserTopN, floor);
            if (sourceFormat == QLatin1String("clang-tidy-text"))
                return AuditEngine::summariseClangTidyText(reportPath, parserTopN, floor);
            if (sourceFormat == QLatin1String("semgrep-json"))
                return AuditEngine::summariseSemgrepJson(reportPath, parserTopN, floor);
            return AuditEngine::summariseSarif(reportPath, parserTopN, floor);
        }();
        if (!parsed) {
            QFile f(reportPath);
            if (!f.open(QIODevice::ReadOnly)) {
                return QJsonDocument(lasErr(QStringLiteral("read_failed"),
                    QStringLiteral("last_audit_summary: cannot read "
                                   "report")));
            }
            f.close();
            // INV-10: empty results / no runs[] → not_audited.
            return QJsonDocument(lasErr(QStringLiteral("parse_failed"),
                QStringLiteral("last_audit_summary: report malformed or "
                               "missing findings (%1)").arg(sourceFormat)));
        }

        // ANTS-1576 — read-time provenance fallback. When the parser
        // didn't surface branch/commit (every non-SARIF format today,
        // plus pre-1576 SARIFs without versionControlProvenance), back
        // -fill from a live git probe before storing into the cache so
        // subsequent cache hits inherit the populated data for free.
        if (parsed->branch.isEmpty() && parsed->commit.isEmpty()) {
            const QString headRaw = QString::fromUtf8(runGit(
                rootCanonical,
                {QStringLiteral("rev-parse"), QStringLiteral("HEAD")})).trimmed();
            const QString branchRaw = QString::fromUtf8(runGit(
                rootCanonical,
                {QStringLiteral("symbolic-ref"), QStringLiteral("--short"),
                 QStringLiteral("HEAD")})).trimmed();
            if (!headRaw.isEmpty()) {
                parsed->commit = headRaw;
                if (!branchRaw.isEmpty()) parsed->branch = branchRaw;
                parsed->branchSource = QStringLiteral("read_time");
            }
        } else {
            parsed->branchSource = QStringLiteral("file_provenance");
        }

        m_auditSummaryPath        = reportPath;
        m_auditSummaryMtimeMs     = mtimeMs;
        m_auditSummaryCachedTopN  = parserTopN;
        m_auditSummaryCachedFloor = floor;
        m_auditSummaryCache       = std::move(*parsed);
        // ANTS-1459 — sourceFormat lives on AuditSummary itself so the
        // cache hit path naturally carries the tag.
        summary = m_auditSummaryCache;
    }

    // ANTS-1406 — `since_commit` short-circuit. Caller (typically
    // /close-phase) asks "is there an audit-clean snapshot already
    // cached at this commit?" before dispatching /audit. Two
    // independent gates: (a) commit-equality (exact prefix match,
    // ≥ 7 hex chars treated as a prefix; full SHAs match exactly),
    // (b) mtime within a 5-minute freshness window.
    constexpr qint64 kSinceCommitFreshnessMs = 5 * 60 * 1000;
    const QString sinceCommitRaw =
        req.value(QStringLiteral("since_commit")).toString().trimmed();
    if (!sinceCommitRaw.isEmpty()) {
        const qint64 nowMs =
            QDateTime::currentMSecsSinceEpoch();
        const qint64 ageMs = nowMs - mtimeMs;
        const QString lastCommit = summary.commit.trimmed();
        QString reason;
        bool fresh = false;
        if (lastCommit.isEmpty()) {
            reason = QStringLiteral("no_provenance");
        } else if (ageMs > kSinceCommitFreshnessMs) {
            reason = QStringLiteral("stale_mtime");
        } else {
            // Case-insensitive hex compare. Treat the shorter of the
            // two strings as the prefix so a full-SHA caller can
            // match an abbreviated last_run_commit, and vice versa.
            const QString a = sinceCommitRaw.toLower();
            const QString b = lastCommit.toLower();
            const int n = std::min(a.size(), b.size());
            const bool commitOk = n >= 7
                && a.left(n) == b.left(n);
            if (!commitOk) {
                reason = QStringLiteral("commit_drift");
            } else {
                fresh = true;
            }
        }
        if (!fresh) {
            QJsonObject env;
            env[QStringLiteral("ok")]               = true;
            env[QStringLiteral("fresh")]            = false;
            env[QStringLiteral("since_commit")]     = sinceCommitRaw;
            if (!lastCommit.isEmpty()) {
                env[QStringLiteral("last_run_commit")] = lastCommit;
            }
            env[QStringLiteral("last_run_age_ms")]  = ageMs;
            env[QStringLiteral("reason")]           = reason;
            return QJsonDocument(env);
        }
        // Fall through; the full envelope below gets the `fresh:true`
        // flag so callers can confirm the short-circuit landed.
    }

    // ANTS-1540 — post-cap rule_ids filter. Applied on a copy so the
    // cache stays globally-shaped. Echo the requested filter so the
    // caller can confirm what got applied.
    QJsonObject env;
    if (ruleIdsRequested) {
        summary = applyRuleIdsFilter(summary, ruleIdsFilter, topN);
        env = buildLasEnvelope(summary);
        QJsonArray echoed;
        for (const QString &r : ruleIdsFilter) echoed.append(r);
        env["rule_ids_filter"] = echoed;
    } else {
        env = buildLasEnvelope(summary);
    }
    // ANTS-1406 — confirm the short-circuit gate landed on a fresh
    // cache hit. Callers (/close-phase, future automation) read
    // `fresh:true` as "skip the /audit dispatch".
    if (!sinceCommitRaw.isEmpty()) {
        env[QStringLiteral("fresh")] = true;
    }

    // ANTS-1576 — scope classifier. Always tag the response so the
    // caller can distinguish a project-wide sweep from a single-file
    // rerun. The classifier walks the post-rule_ids topFindings (so
    // a narrow rule_ids filter doesn't masquerade as a single_file
    // rerun: classifying after applyRuleIdsFilter would lie — apply
    // the classifier to the original summary instead).
    {
        AuditEngine::AuditSummary preFilter = m_auditSummaryCache;
        const ScopeClassification sc = classifyAuditScope(preFilter, reportPath);
        env["scope"] = sc.tag;

        // ANTS-3512 — the derived tag above is a proxy: it infers scope
        // from the distinct-file count in top findings, so a genuine
        // whole-tree sweep that surfaces findings in one file reads as
        // `single_file`. The *requested* scope is authoritative and is
        // already persisted by audit_run into the sibling findings
        // sidecar (`findings-<iso>-<sha>.json`, key "scope", ANTS-1870).
        // Read it back and emit it as `requested_scope`; when the run was
        // a confirmed full-tree request, suppress the "a broader recent
        // file may exist" warning — it is definitionally false there.
        // Native `.audit_cache/audit-*.sarif` picks have a sidecar;
        // foreign-format picks (cppcheck-*.xml …) don't, so the heuristic
        // stays as the fallback (finbreak feedback 2026-07-14).
        QString requestedScope;
        {
            const QFileInfo rfi(reportPath);
            const QString base = rfi.fileName();
            if (base.startsWith(QLatin1String("audit-")) &&
                base.endsWith(QLatin1String(".sarif"))) {
                QString sidecarName = base;
                sidecarName.replace(0, 6, QStringLiteral("findings-"));
                sidecarName.chop(6);              // ".sarif"
                sidecarName += QStringLiteral(".json");
                QFile scf(rfi.absolutePath() + QLatin1Char('/') + sidecarName);
                if (scf.open(QIODevice::ReadOnly)) {
                    const QJsonObject sidecar =
                        QJsonDocument::fromJson(scf.readAll()).object();
                    requestedScope =
                        sidecar.value(QStringLiteral("scope")).toString().trimmed();
                }
            }
        }
        // ANTS-3517 — extend the ANTS-3512 `full`-only gate to every explicit
        // multi-file scope selector. `since-tag:*`, `since-commit:*`,
        // `since-last-run`, `branch-diff` and `files` are deliberate changeset
        // sweeps (auditscope resolveChangedFiles) and `full`/`auto` are
        // whole-tree; surfacing findings in one file is normal for all of them,
        // so none is a mis-picked single_file rerun. The distinct-file
        // heuristic stays only for the foreign-format fallback (no sidecar) or
        // an unrecognised scope. (finbreak feedback 2026-07-14: a genuine
        // since-tag sweep still tripped the warning under the full-only gate.)
        const bool confirmedBroad =
            requestedScope == QLatin1String("full")
            || requestedScope == QLatin1String("auto")
            || requestedScope == QLatin1String("since-last-run")
            || requestedScope == QLatin1String("branch-diff")
            || requestedScope == QLatin1String("files")
            || requestedScope.startsWith(QLatin1String("since-tag:"))
            || requestedScope.startsWith(QLatin1String("since-commit:"));
        if (!requestedScope.isEmpty())
            env["requested_scope"] = requestedScope;

        if (sc.tag != QLatin1String("broad") && !confirmedBroad) {
            env["narrow_run_warning"] =
                QStringLiteral("%1 looks like a %2 rerun "
                               "(%3 distinct files in top findings). "
                               "A broader recent file may exist in "
                               ".audit_cache/.")
                    .arg(QFileInfo(reportPath).fileName(),
                         sc.tag,
                         QString::number(sc.distinctFiles.size()));
            QJsonArray files;
            for (const QString &f : sc.distinctFiles) files.append(f);
            env["narrow_run_files"] = files;
        }
    }
    // ANTS-1625 — always emit `pick_basis` so callers can tell whether
    // the picker preferred a broader file or just took the newest.
    if (!pickBasis.isEmpty()) env["pick_basis"] = pickBasis;

    // ANTS-2056 — always-on staleness signal. The cached artifact's
    // commit/branch provenance is read-time HEAD for every non-SARIF
    // format (branch_source:"read_time" above), so a snapshot generated
    // long ago reads as "HEAD's current findings". Compare the artifact
    // mtime against the current HEAD commit's author-date: an artifact
    // older than HEAD describes a PAST tree state, where already-fixed
    // findings masquerade as live (the RetroArch two-branch fork case).
    // Not gated behind `since_commit` — that gate is opt-in and can't
    // evict a stale-but-recent cache. Best-effort: the flag is omitted
    // (not guessed) when the HEAD date can't be read.
    {
        const QString headDateRaw = QString::fromUtf8(runGit(
            rootCanonical,
            {QStringLiteral("log"), QStringLiteral("-1"),
             QStringLiteral("--format=%ct"),
             QStringLiteral("HEAD")})).trimmed();
        bool headDateOk = false;
        const qint64 headDateMs =
            headDateRaw.toLongLong(&headDateOk) * 1000;
        if (headDateOk && headDateMs > 0) {
            const bool stale = mtimeMs < headDateMs;
            env["stale"] = stale;
            if (stale) {
                env["stale_reason"] = QStringLiteral(
                    "cached audit artifact \"%1\" predates the current "
                    "HEAD commit — findings may already be fixed; re-run "
                    "audit_run for current results.")
                        .arg(QFileInfo(reportPath).fileName());
            }
        }
    }

    // ANTS-2056 — pinned-snapshot hint. A filename like `*-bNN-fixes.*`
    // or `*-pre-*` is a deliberately-pinned artifact, not a dated run,
    // and routinely predates HEAD. Surface it so a caller doesn't read
    // it as HEAD's current findings even when the mtime check can't run.
    {
        static const QRegularExpression kPinnedSnapshot(
            QStringLiteral("-b[0-9]+-fixes|-pre-"),
            QRegularExpression::CaseInsensitiveOption);
        const QString fname = QFileInfo(reportPath).fileName();
        if (kPinnedSnapshot.match(fname).hasMatch()) {
            env["pinned_snapshot_hint"] = QStringLiteral(
                "report \"%1\" looks like a pinned snapshot (not a dated "
                "run); it may predate HEAD — re-run audit_run for current "
                "findings.").arg(fname);
        }
    }
    return QJsonDocument(env);
}

// ----- ANTS-1569 — current_state aggregator ------------------------

namespace rcdetail {

QJsonObject csErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = code;
    return o;
}

// Best-effort parse of .claude/workflow.md: locate first `## Status`
// heading, return the first non-blank line below it. Empty string
// when the file exists but the block is missing or empty.
// `fileExists` reports whether the file is on disk so the caller can
// decide whether to omit or emit-as-empty.
QString readWorkflowStatusLine(const QString &rootCanonical,
                               bool *fileExists) {
    *fileExists = false;
    const QString path =
        rootCanonical + QStringLiteral("/.claude/workflow.md");
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return QString();
    *fileExists = true;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    QTextStream in(&f);
    bool inStatus = false;
    while (!in.atEnd()) {
        const QString line    = in.readLine();
        const QString trimmed = line.trimmed();
        if (!inStatus) {
            if (trimmed.startsWith(QStringLiteral("## ")) &&
                trimmed.mid(3).trimmed().compare(
                    QStringLiteral("Status"),
                    Qt::CaseInsensitive) == 0) {
                inStatus = true;
            }
            continue;
        }
        // Inside the `## Status` block. Hitting another heading ends
        // the block; blank lines are skipped; first non-blank line
        // wins.
        if (trimmed.startsWith(QLatin1Char('#'))) break;
        if (trimmed.isEmpty()) continue;
        return trimmed;
    }
    return QString();  // file exists but block missing or empty
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdCurrentState(const QJsonObject &req) {
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("current_state: no MainWindow")));
    }

    // ANTS-1569 INV-13: anchor on caller_cwd via the same chokepoint
    // every Required tool uses. Empty result → `no_project` refusal.
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("no_project"),
            QStringLiteral("current_state: project root unresolved")));
    }

    // ANTS-1569: response envelope named `result` (not `env`) so the
    // first occurrence of the cmdTokenUsage success-path anchor in
    // this file stays inside cmdTokenUsage — its INV-4 source-scrape
    // test in tests/features/token_usage_no_ci_diagnostic relies on
    // that anchor.
    QJsonObject result;
    result["ok"] = true;

    // (1) active_bullet — first 🚧 in document order, else first 📋.
    // INV-7 delegation: pure composer over cmdRoadmapQuery; INV-8
    // omit-on-empty / non-ok.
    {
        QJsonObject rqReq;
        rqReq[QStringLiteral("caller_cwd")] = rootCanonical;
        rqReq[QStringLiteral("status")]     = QStringLiteral("active");
        const QJsonDocument rqDoc = cmdRoadmapQuery(rqReq);
        const QJsonObject     rqObj = rqDoc.object();
        if (rqObj.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonArray bullets =
                rqObj.value(QStringLiteral("bullets")).toArray();
            QJsonObject pick;
            for (const QJsonValue &v : bullets) {
                const QJsonObject b = v.toObject();
                if (b.value(QStringLiteral("status")).toString() ==
                    QStringLiteral("🚧")) {
                    pick = b;
                    break;
                }
            }
            if (pick.isEmpty() && !bullets.isEmpty()) {
                pick = bullets.first().toObject();
            }
            if (!pick.isEmpty()) {
                QJsonObject ab;
                ab[QStringLiteral("id")] =
                    pick.value(QStringLiteral("id")).toString();
                ab[QStringLiteral("headline")] =
                    pick.value(QStringLiteral("headline_oneline")).toString();
                ab[QStringLiteral("section_slug")] =
                    pick.value(QStringLiteral("section_slug")).toString();
                ab[QStringLiteral("kind")] =
                    pick.value(QStringLiteral("kind")).toString();
                ab[QStringLiteral("lanes")] =
                    pick.value(QStringLiteral("lanes")).toArray();
                ab[QStringLiteral("status")] =
                    pick.value(QStringLiteral("status")).toString();
                result[QStringLiteral("active_bullet")] = ab;
            }
        }
    }

    // (2) workflow_status_line — INV-9: omit when file absent; emit
    // empty string when file exists but the block is empty.
    {
        bool fileExists = false;
        const QString status =
            readWorkflowStatusLine(rootCanonical, &fileExists);
        if (fileExists) {
            result[QStringLiteral("workflow_status_line")] = status;
        }
    }

    // (3) git_branch_state — always-present (INV-14). Upstream
    // non-ok collapses to empty/zero fallback so callers iterating
    // the envelope can rely on the shape.
    {
        QJsonObject gsReq;
        gsReq[QStringLiteral("caller_cwd")] = rootCanonical;
        gsReq[QStringLiteral("op")]         = QStringLiteral("status");
        const QJsonDocument gsDoc = cmdGitState(gsReq);
        const QJsonObject     gs    = gsDoc.object();
        QJsonObject gbs;
        if (gs.value(QStringLiteral("ok")).toBool(false)) {
            gbs[QStringLiteral("branch")] =
                gs.value(QStringLiteral("branch")).toString();
            gbs[QStringLiteral("ahead")]  =
                gs.value(QStringLiteral("ahead")).toInt();
            gbs[QStringLiteral("behind")] =
                gs.value(QStringLiteral("behind")).toInt();
            gbs[QStringLiteral("files_changed_count")] =
                gs.value(QStringLiteral("files")).toArray().size();
        } else {
            gbs[QStringLiteral("branch")] = QString();
            gbs[QStringLiteral("ahead")]  = 0;
            gbs[QStringLiteral("behind")] = 0;
            gbs[QStringLiteral("files_changed_count")] = 0;
        }
        result[QStringLiteral("git_branch_state")] = gbs;
    }

    // (4) open_audit_findings_count — error + warning + note from
    // cmdLastAuditSummary. Any non-ok envelope ⇒ 0 (INV-14 spec).
    // Suppressed findings excluded by definition (last_audit_summary
    // doesn't include them in counts.error/warning/note).
    //
    // ANTS-3370 — staleness flag. last_audit_summary already computes an
    // always-on `stale` signal (ANTS-2056: artifact mtime < HEAD commit
    // date ⇒ the count describes a PAST tree where already-fixed findings
    // masquerade as live). Propagate it next to the count so a session
    // doesn't read a drifted count as HEAD's current findings — mirror
    // last_audit_summary.stale / stale_reason rather than recompute it
    // (single source of truth, no extra git probe). Best-effort: when the
    // upstream omitted `stale` (HEAD date unreadable) the flag defaults
    // false. No audit (count 0) ⇒ not stale.
    {
        QJsonObject lsReq;
        lsReq[QStringLiteral("caller_cwd")] = rootCanonical;
        const QJsonDocument lsDoc = cmdLastAuditSummary(lsReq);
        const QJsonObject     ls    = lsDoc.object();
        int total = 0;
        bool    stale = false;
        QString staleReason;
        if (ls.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonObject counts =
                ls.value(QStringLiteral("counts")).toObject();
            total = counts.value(QStringLiteral("error")).toInt()
                  + counts.value(QStringLiteral("warning")).toInt()
                  + counts.value(QStringLiteral("note")).toInt();
            stale       = ls.value(QStringLiteral("stale")).toBool(false);
            staleReason = ls.value(QStringLiteral("stale_reason")).toString();
        }
        result[QStringLiteral("open_audit_findings_count")] = total;
        result[QStringLiteral("open_audit_findings_count_stale")] = stale;
        if (stale && !staleReason.isEmpty())
            result[QStringLiteral("open_audit_findings_count_stale_reason")] =
                staleReason;
    }

    // (5) spec_path — INV-10: present iff active_bullet has an id
    // AND docs/specs/<id>.md exists on disk.
    if (result.contains(QStringLiteral("active_bullet"))) {
        const QString id = result.value(QStringLiteral("active_bullet"))
                              .toObject()
                              .value(QStringLiteral("id"))
                              .toString();
        if (!id.isEmpty()) {
            // ANTS-2160 — specs_dir override for the spec_path probe.
            QString specsDir = QStringLiteral("docs/specs");
            if (const auto sd = ProjectSettings::load(rootCanonical).specsDir;
                sd && QDir(rootCanonical + QLatin1Char('/') + *sd).exists())
                specsDir = *sd;
            const QString specRel =
                specsDir + QLatin1Char('/') + id +
                QStringLiteral(".md");
            const QFileInfo specInfo(
                rootCanonical + QStringLiteral("/") + specRel);
            if (specInfo.exists() && specInfo.isFile()) {
                result[QStringLiteral("spec_path")] = specRel;
            }
        }
    }

    // INV-11 — etag injection happens at the dispatch layer via
    // applyEtagPattern (ANTS-1499). Return the body without an
    // `etag` field; the dispatcher computes and injects it.
    return QJsonDocument(result);
}

// ANTS-1735 — model_switch_stats: read-only aggregation of the model-switch
// effectiveness ledger, scoped to the caller's project. The ledger is a single
// global JSONL (~/.cache/ants-terminal/model-switch-ledger.jsonl); records carry
// a `project` field, so this verb filters to the resolved caller root before
// aggregating. Never writes. Absent ledger → {ok:true, switches:0, …}.
//
// ANTS-1889 — also surfaces the live switcher configuration
// (auto_model_switch_enabled / floor_tier / min_dwell_sec) so callers can
// distinguish "feature OFF" from "feature ON, no candidates yet"; accepts an
// optional `scope:"global"` arg that aggregates across all projects in the
// ledger instead of filtering to the caller. The config triple lives in the
// response body, so the dispatch-layer ETag flips automatically on Settings
// toggle changes.
QJsonDocument RemoteControl::cmdModelSwitchStats(const QJsonObject &req) {
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("model_switch_stats: no MainWindow")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("no_project"),
            QStringLiteral("model_switch_stats: project root unresolved")));
    }

    // ANTS-1889 — scope arg: "project" (default) or "global". Anything else
    // refuses with bad_args so callers don't silently get project scope.
    QString scope = QStringLiteral("project");
    if (req.contains(QStringLiteral("scope"))) {
        scope = req.value(QStringLiteral("scope")).toString();
        if (scope != QStringLiteral("project") &&
                scope != QStringLiteral("global")) {
            return QJsonDocument(csErr(QStringLiteral("bad_args"),
                QStringLiteral("model_switch_stats: scope must be "
                               "\"project\" or \"global\"")));
        }
    }

    // ANTS-1894 — mode arg: "firings" (default) or "near_misses". Unknown
    // values refuse with bad_mode (canonical taxonomy entry in
    // docs/standards/mcp-error-codes.md § "Input validation").
    // ANTS-1960 — firings near_misses.total_24h and near_misses mode's
    // window_24h.total agree ONLY when read at the same moment. Across-time
    // reads legitimately differ because the 24 h window advances; this is
    // expected, not a bug (closes the ANTS-1947 false-alarm class).
    QString mode = QStringLiteral("firings");
    if (req.contains(QStringLiteral("mode"))) {
        mode = req.value(QStringLiteral("mode")).toString();
        if (mode != QStringLiteral("firings") &&
                mode != QStringLiteral("near_misses")) {
            return QJsonDocument(csErr(QStringLiteral("bad_mode"),
                QStringLiteral("model_switch_stats: mode must be "
                               "\"firings\" or \"near_misses\"")));
        }
    }

    // Live config — defaults sourced from the in-memory store; same
    // shape as Config::claudeAutoModel(). RemoteControl can't reach the
    // MainWindow Config directly without a new accessor, so we construct
    // a fresh Config (which reads the same on-disk file). This matches
    // the existing pattern at cmdIndieReviewDispatch's `Config cfg`.
    Config cfg;
    const QJsonObject autoCfg = cfg.claudeAutoModel();
    ModelSwitchLedger::StatsConfig sc;
    sc.switchEnabled = autoCfg.value(QStringLiteral("switch_enabled")).toBool(false);
    sc.floorTier     = autoCfg.value(QStringLiteral("floor")).toString(QStringLiteral("haiku"));
    sc.minDwellSec   = autoCfg.value(QStringLiteral("min_dwell_sec")).toInt(90);
    sc.scope         = scope;
    // ANTS-2033 — surface WHY the switch is off (never_enabled vs
    // user_disabled) via the first-run opt-in latch.
    sc.nudgeShown    = cfg.claudeAutoModelNudgeShown();
    // ANTS-1942 — set windowDays explicitly (shared constant) so the MCP
    // scorecard and the controller's caution dial can never silently diverge on
    // a struct-default change; ANTS-1941 — current-epoch records only.
    sc.windowDays    = ModelSwitchLedger::kDefaultStatsWindowDays;
    sc.minEpoch      = ModelSwitchLedger::kSwitcherEpoch;

    // ANTS-1894 — Read the near-miss ledger once, scope it the same way the
    // firing aggregator does (empty projectScope → global).
    const QString nmProjectScope = (scope == QStringLiteral("global"))
        ? QString()
        : rootCanonical;
    const QList<ModelNearMissLedger::Record> nmRecs =
        ModelNearMissLedger::readRecords(ModelNearMissLedger::defaultLedgerPath());
    const qint64 nmNowMs = QDateTime::currentMSecsSinceEpoch();

    // ANTS-1954 — pass the same epoch boundary as the firing ledger so the
    // near-miss dominant_blocker resets cleanly at each rebuild, not after 24h.
    const int nmMinEpoch = ModelSwitchLedger::kSwitcherEpoch;

    if (mode == QStringLiteral("near_misses")) {
        // ANTS-1894 INV-12 — full breakdown envelope; preserve config triple
        // + scope echo so the caller can still distinguish "feature OFF" / ON.
        QJsonObject env = ModelNearMissLedger::statsFull(
            nmRecs, nmProjectScope, nmNowMs, nmMinEpoch);
        env[QStringLiteral("auto_model_switch_enabled")] = sc.switchEnabled;
        env[QStringLiteral("floor_tier")] = sc.floorTier;
        env[QStringLiteral("min_dwell_sec")] = sc.minDwellSec;
        env[QStringLiteral("scope")] = sc.scope;   // overrides statsFull's
        // ANTS-2033 — mirror the off-reason onto the near_misses envelope
        // so both modes answer "why is it off?" consistently.
        if (!sc.switchEnabled) {
            env[QStringLiteral("auto_model_switch_off_reason")] =
                sc.nudgeShown ? QStringLiteral("user_disabled")
                              : QStringLiteral("never_enabled");
        }
        return QJsonDocument(env);
    }

    // Default mode:"firings" — existing envelope, plus a slim near_misses
    // block (INV-12). ANTS-1909 — compute the slim near-miss block BEFORE
    // statsForScope so the dominant-blocker + 24 h count can flow into the
    // headline composition via StatsConfig (statsEnvelope owns the wording).
    const ModelNearMissLedger::StatsSlim slim =
        ModelNearMissLedger::statsSlim(nmRecs, nmProjectScope, nmNowMs, nmMinEpoch);
    sc.nearMissTotal24h        = slim.total24h;
    sc.nearMissDominantBlocker = slim.dominantBlocker;
    QJsonObject env = ModelSwitchLedger::statsForScope(
        ModelSwitchLedger::defaultLedgerPath(), rootCanonical, sc);
    QJsonObject slimObj;
    slimObj[QStringLiteral("total_24h")]              = slim.total24h;
    slimObj[QStringLiteral("dominant_blocker")]       = slim.dominantBlocker;
    // ANTS-1960 — idle_end_of_session-only near-misses are correct behaviour
    // (suppress at session-end), not missed opportunities; surfaced separately
    // so callers don't mistake them for actionable gaps.
    slimObj[QStringLiteral("idle_end_suppressed_24h")] = slim.idleEndSuppressed24h;
    env[QStringLiteral("near_misses")] = slimObj;
    return QJsonDocument(env);
}

// ANTS-1724 — session_brief: compact session-state envelope for
// fresh /clear session orientation. Composes four on-disk caches
// (roadmap active bullet, git status, last audit summary,
// build/test caches) into one ≤ 512-byte envelope. ETag-eligible.
QJsonDocument RemoteControl::cmdSessionBrief(const QJsonObject &req)
{
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("session_brief: no MainWindow")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("no_project"),
            QStringLiteral("session_brief: project root unresolved")));
    }

    QJsonObject result;
    result[QStringLiteral("ok")] = true;

    // --- roadmap: first 🚧 in document order, else first 📋.
    // Intentional subset of cmdCurrentState's active_bullet: emits only
    // active_id + headline for compactness (INV-8 ≤ 512 bytes budget).
    {
        QJsonObject rqReq;
        rqReq[QStringLiteral("caller_cwd")] = rootCanonical;
        rqReq[QStringLiteral("status")]     = QStringLiteral("active");
        const QJsonObject rq = cmdRoadmapQuery(rqReq).object();
        QJsonObject rm;
        if (rq.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonArray bullets =
                rq.value(QStringLiteral("bullets")).toArray();
            QJsonObject pick;
            for (const QJsonValue &v : bullets) {
                const QJsonObject b = v.toObject();
                if (b.value(QStringLiteral("status")).toString() ==
                        QStringLiteral("🚧")) { pick = b; break; }
            }
            if (pick.isEmpty() && !bullets.isEmpty())
                pick = bullets.first().toObject();
            rm[QStringLiteral("active_id")] =
                pick.value(QStringLiteral("id")).toString();
            rm[QStringLiteral("headline")] =
                pick.value(QStringLiteral("headline_oneline")).toString();
        } else {
            rm[QStringLiteral("active_id")] = QString();
            rm[QStringLiteral("headline")]  = QString();
        }
        result[QStringLiteral("roadmap")] = rm;
    }

    // --- git: branch + ahead/behind + changed-file count ---
    {
        QJsonObject gsReq;
        gsReq[QStringLiteral("caller_cwd")] = rootCanonical;
        gsReq[QStringLiteral("op")]         = QStringLiteral("status");
        const QJsonObject gs = cmdGitState(gsReq).object();
        QJsonObject git;
        if (gs.value(QStringLiteral("ok")).toBool(false)) {
            git[QStringLiteral("branch")] =
                gs.value(QStringLiteral("branch")).toString();
            git[QStringLiteral("ahead")]  =
                gs.value(QStringLiteral("ahead")).toInt();
            git[QStringLiteral("behind")] =
                gs.value(QStringLiteral("behind")).toInt();
            git[QStringLiteral("files_changed_count")] =
                gs.value(QStringLiteral("files")).toArray().size();
        } else {
            git[QStringLiteral("branch")]              = QString();
            git[QStringLiteral("ahead")]               = 0;
            git[QStringLiteral("behind")]              = 0;
            git[QStringLiteral("files_changed_count")] = 0;
        }
        result[QStringLiteral("git")] = git;
    }

    // --- audit: open findings count from last run ---
    {
        QJsonObject lsReq;
        lsReq[QStringLiteral("caller_cwd")] = rootCanonical;
        const QJsonObject ls = cmdLastAuditSummary(lsReq).object();
        QJsonObject audit;
        int total = 0;
        if (ls.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonObject counts =
                ls.value(QStringLiteral("counts")).toObject();
            total = counts.value(QStringLiteral("error")).toInt()
                  + counts.value(QStringLiteral("warning")).toInt()
                  + counts.value(QStringLiteral("note")).toInt();
        }
        audit[QStringLiteral("open_count")] = total;
        result[QStringLiteral("audit")] = audit;
    }

    // --- build: last recorded build result ---
    {
        const auto bOpt = BuildCache::loadBuild(rootCanonical);
        QJsonObject build;
        if (bOpt.has_value()) {
            const auto &b = bOpt.value();
            build[QStringLiteral("result")]   = (b.exitCode == 0)
                ? QStringLiteral("pass") : QStringLiteral("fail");
            build[QStringLiteral("errors")]   = b.errorsCount;
            build[QStringLiteral("warnings")] = b.warningsCount;
            if (b.recordedAtMs > 0) {
                build[QStringLiteral("recorded_at")] =
                    QDateTime::fromMSecsSinceEpoch(b.recordedAtMs)
                        .toUTC().toString(Qt::ISODate);
            }
        } else {
            build[QStringLiteral("result")] = QStringLiteral("unknown");
        }
        result[QStringLiteral("build")] = build;
    }

    // --- test: last recorded test result ---
    {
        const auto tOpt = TestResCache::loadTests(rootCanonical);
        QJsonObject test;
        if (tOpt.has_value()) {
            const auto &t = tOpt.value();
            test[QStringLiteral("result")]  = (t.exitCode == 0)
                ? QStringLiteral("pass") : QStringLiteral("fail");
            test[QStringLiteral("passed")]  = t.passed;
            test[QStringLiteral("failed")]  = t.failed;
            test[QStringLiteral("total")]   = t.total;
            if (t.recordedAtMs > 0) {
                test[QStringLiteral("recorded_at")] =
                    QDateTime::fromMSecsSinceEpoch(t.recordedAtMs)
                        .toUTC().toString(Qt::ISODate);
            }
        } else {
            test[QStringLiteral("result")] = QStringLiteral("unknown");
        }
        result[QStringLiteral("test")] = test;
    }

    // ETag injected at the dispatch layer (isEtagSupportedTool).
    return QJsonDocument(result);
}

// ANTS-1883 — session_orient: bundle of current_state + project_layout
// + roadmap_query mode:section_index status:active. Composer-only,
// no new caches; each upstream call hits its existing cache. The
// dispatch-layer applyEtagPattern wraps the bundle response with one
// ETag — flips when any of the three upstreams' payload changes.
QJsonDocument RemoteControl::cmdSessionOrient(const QJsonObject &req)
{
    if (!m_main) {
        return QJsonDocument(csErr(QStringLiteral("no_window"),
            QStringLiteral("session_orient: no MainWindow")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(csErr(QStringLiteral("no_project"),
            QStringLiteral("session_orient: project root unresolved")));
    }

    QJsonObject result;
    bool allOk = true;
    // ANTS-3587 — a fresh project legitimately lacks a ROADMAP.md (and specs /
    // codebase index). That is a normal state, not an orient failure: keep
    // top-level ok:true when an upstream refuses ONLY because an optional
    // artifact is absent, and surface the absence via notices[]. Reserve
    // ok:false for a real failure (bad cwd, unreadable state).
    QJsonArray notices;
    const auto noteOrFail = [&](const QJsonObject &o, const char *label) {
        if (o.value(QStringLiteral("ok")).toBool(false)) return;
        const QString code = o.value(QStringLiteral("code")).toString();
        if (code == QStringLiteral("no_roadmap_loaded")
                || code == QStringLiteral("no_roadmap")) {
            QJsonObject n;
            n[QStringLiteral("source")] = QLatin1String(label);
            n[QStringLiteral("code")]   = code;
            n[QStringLiteral("note")]   = QStringLiteral(
                "no ROADMAP.md yet — normal for a fresh project, not an error; "
                "create one via roadmap_log when there is work to track");
            notices.append(n);
            return;  // absent-but-optional artifact: does NOT fail orient
        }
        allOk = false;
    };

    // --- current_state (ANTS-1569) ---
    {
        QJsonObject csReq;
        csReq[QStringLiteral("caller_cwd")] = rootCanonical;
        const QJsonObject cs = cmdCurrentState(csReq).object();
        noteOrFail(cs, "current_state");
        result[QStringLiteral("current_state")] = cs;
    }

    // --- project_layout ---
    {
        QJsonObject plReq;
        plReq[QStringLiteral("caller_cwd")] = rootCanonical;
        const QJsonObject pl = cmdProjectLayout(plReq).object();
        noteOrFail(pl, "project_layout");
        result[QStringLiteral("project_layout")] = pl;
    }

    // --- sections_index (roadmap_query mode:section_index status:active) ---
    // Per ANTS-1437 INV-7 as amended by ANTS-1848, status:"active"
    // is meaningful for section_index mode — drops sections whose
    // active_count_id_only:0 (the lean planning slice).
    // ANTS-2052 — on a fully id-less legacy roadmap every section's
    // active_count_id_only is 0, so section_index emits legacy_format +
    // raw_active_count (the true non-zero count). Capture it so the
    // active_bullets block below can recover the headlines.
    int legacyRawActive = 0;
    {
        QJsonObject rqReq;
        rqReq[QStringLiteral("caller_cwd")] = rootCanonical;
        rqReq[QStringLiteral("mode")]       = QStringLiteral("section_index");
        rqReq[QStringLiteral("status")]     = QStringLiteral("active");
        const QJsonObject rq = cmdRoadmapQuery(rqReq).object();
        noteOrFail(rq, "sections_index");
        if (rq.value(QStringLiteral("legacy_format")).toBool(false))
            legacyRawActive = rq.value(QStringLiteral("raw_active_count")).toInt(0);
        result[QStringLiteral("sections_index")] = rq;
    }

    // --- active_bullets (roadmap_query mode:headline_only status:active limit:20) ---
    // ANTS-1922 — top-20 active item headlines inline so the session can
    // pick the next bundle without a follow-up roadmap_query round-trip.
    // Does NOT contribute to allOk — an absent roadmap must not fail orient.
    {
        QJsonObject rqReq;
        rqReq[QStringLiteral("caller_cwd")] = rootCanonical;
        rqReq[QStringLiteral("mode")]       = QStringLiteral("headline_only");
        rqReq[QStringLiteral("status")]     = QStringLiteral("active");
        rqReq[QStringLiteral("limit")]      = 20;
        QJsonObject rq = cmdRoadmapQuery(rqReq).object();
        // ANTS-2052 — on a fully id-less legacy roadmap the default
        // [PROJ-NNNN] id filter drops every active bullet, so the flagship
        // bundle reads as "no active work" even though raw_active_count > 0.
        // When that happens, re-issue including narrator bullets so the
        // session actually sees the open items, and flag the recovery.
        if (rq.value(QStringLiteral("count")).toInt(0) == 0
                && legacyRawActive > 0) {
            rqReq[QStringLiteral("include_narrator_bullets")] = true;
            rq = cmdRoadmapQuery(rqReq).object();
            rq[QStringLiteral("legacy_format_fallback")] = true;
            rq[QStringLiteral("raw_active_count")]       = legacyRawActive;
            rq[QStringLiteral("legacy_format_hint")] = QStringLiteral(
                "id-less legacy roadmap: the [PROJ-NNNN] id filter dropped "
                "all %1 active bullets, so this list was re-issued with "
                "include_narrator_bullets:true — the queue is NOT empty.")
                .arg(legacyRawActive);
        }
        // ANTS-4399 — say loudly when the slice is a MINORITY of the queue.
        // The fields were already correct (count / total / truncated /
        // next_offset); what was missing is prominence. The bundle reads as
        // "here are your active items", so a session that does not inspect
        // `truncated` plans from the first 20 of 95 — ordered by document
        // position, which is not an ordering that means anything. It bit a
        // reporting session indirectly: the first 20 were dominated by one
        // section, and the items worth doing were reachable only because a
        // handoff brief named them.
        //
        // The hint points at `bundles`, which already exists and is the
        // better planning surface — and which nothing pointed at from the
        // orientation call.
        if (rq.value(QStringLiteral("truncated")).toBool()) {
            const int shown = rq.value(QStringLiteral("count")).toInt(0);
            const int total = rq.value(QStringLiteral("total")).toInt(0);
            if (total > shown) {
                rq[QStringLiteral("active_bullets_hint")] = QStringLiteral(
                    "showing %1 of %2 active items, ordered by position in "
                    "the file — NOT by priority. Do not plan from this slice "
                    "alone: call roadmap_query mode:\"bundles\" for the "
                    "thematic view, or page with offset:%1.")
                        .arg(shown).arg(total);
            }
        }
        result[QStringLiteral("active_bullets")] = rq;
    }

    // --- server_build (ANTS-2073) ---
    // The running MCP server's build identity, so a client can compare it
    // against a fix's ship date and self-diagnose "fix shipped, I'm on an
    // old build" vs "fix didn't cover my case" — the recurring false
    // re-report class three sessions hit (MAME Curator / Album Builder /
    // RetroArch). Mirrors the `initialize` serverInfo stamp (ANTS-1952);
    // surfaced here because session_orient is the documented first read.
    {
        QJsonObject sb;
        sb[QStringLiteral("version")]      = QStringLiteral(ANTS_VERSION);
        // ANTS-3582: ANTS_BUILD_* are extern const char[] (defined in the
        // generated build_info_values.cpp), so read them via fromLatin1 — they
        // are not compile-time string literals, so QStringLiteral won't compile.
        sb[QStringLiteral("build_commit")] = QString::fromLatin1(ANTS_BUILD_COMMIT);
        sb[QStringLiteral("build_date")]   = QString::fromLatin1(ANTS_BUILD_DATE);
        sb[QStringLiteral("build_time")]   = QString::fromLatin1(ANTS_BUILD_TIME);
        sb[QStringLiteral("build_type")]   = QString::fromLatin1(ANTS_BUILD_TYPE);
        // ANTS-2152 — steer the reader away from the version-string trap:
        // the version can be identical across the commit that shipped a fix
        // and the older commit a client is still running (a deploy gap, not
        // a regression). Before re-reporting a ✅/shipped item as still
        // broken, compare build_commit / build_date against the fix's ship
        // date — not `version`.
        sb[QStringLiteral("stale_check_hint")] = QStringLiteral(
            "Before re-reporting a shipped/✅ item as still broken, compare "
            "build_commit + build_date against the fix's ship date — NOT the "
            "version string (it can be identical across the pre- and post-fix "
            "commits, so a stale binary looks current by version alone).");
        result[QStringLiteral("server_build")] = sb;
    }

    // --- server_build_stale (ANTS-3499) ---
    // The stale_check_hint above is PASSIVE: it asks the reader to hand-
    // compare build_commit against a fix's ship date — and reporting
    // sessions don't. Eight of the ~14 external findings in the 2026-07-10
    // triage batch were already-shipped fixes re-reported by sessions on old
    // binaries. So do the comparison here: count the commits project HEAD has
    // that the running binary's build commit does NOT (git rev-list --count
    // <build>..HEAD, in caller_cwd) and, when behind, surface a loud advisory
    // block. Advisory only — never sets ok=false (a stale binary can still
    // orient). Skipped silently when the build commit is unknown to the repo
    // (built elsewhere), HEAD equals the build (behind 0), or caller_cwd is
    // not a git checkout — so it never false-alarms. Deterministic for a
    // fixed (build_commit, HEAD), so it does not perturb the dispatch-layer
    // ETag beyond a real HEAD advance.
    {
        const QString buildCommit = QString::fromLatin1(ANTS_BUILD_COMMIT);  // ANTS-3582: extern array, not a literal
        const QString headSha = QString::fromUtf8(
            runGit(rootCanonical, {QStringLiteral("rev-parse"),
                                   QStringLiteral("HEAD")})).trimmed();
        if (!headSha.isEmpty() && !buildCommit.isEmpty()) {
            const QByteArray countRaw = runGit(
                rootCanonical, {QStringLiteral("rev-list"),
                                QStringLiteral("--count"),
                                buildCommit + QStringLiteral("..HEAD")});
            bool okNum = false;
            const int behind =
                QString::fromUtf8(countRaw).trimmed().toInt(&okNum);
            if (okNum && behind > 0) {
                QJsonObject stale;
                stale[QStringLiteral("behind_commits")] = behind;
                stale[QStringLiteral("built")] = buildCommit;
                stale[QStringLiteral("head")]  = headSha.left(12);
                stale[QStringLiteral("hint")]  = QStringLiteral(
                    "Your running MCP-server binary is behind project HEAD — "
                    "relaunch Ants Terminal (rebuild + restart) before trusting "
                    "any ✅/shipped status or re-reporting a fix as still "
                    "broken. behind_commits = commits on HEAD your build does "
                    "not contain.");
                result[QStringLiteral("server_build_stale")] = stale;
            }
        }
    }

    // --- codebase_index (ANTS-2140 / ANTS-1637) ---
    // Make the codebase map ride the blessed first call: invoking
    // cmdCodebaseIndex drives CodebaseIndex::serve() (load -> refresh ->
    // write-back), so the index is eagerly refreshed at session start
    // without the session having to remember a separate call. We embed a
    // TRIMMED summary: the per-call-volatile generated_at_ms /
    // refreshed_files (and any etag) would change every call and break
    // session_orient's dispatch-layer ETag (304) stability, so they are
    // stripped -- the structural summary (file_count / lane_count /
    // lanes / languages / roles / cache_path) is stable while the tree
    // is unchanged. Does NOT contribute to allOk: a project without a
    // src/ tree must not fail orient (parity with active_bullets).
    {
        QJsonObject ciReq;
        ciReq[QStringLiteral("caller_cwd")] = rootCanonical;
        // ANTS-3468 — ride the compact lane→source-file digest on the bundle
        // so the first-call map answers "where does subsystem X live" (jump to
        // file_outline/read_region) instead of only totals. Deterministic, so
        // it keeps this bundle's dispatch-layer 304 ETag stable (below).
        ciReq[QStringLiteral("lane_files")] = true;
        QJsonObject ci = cmdCodebaseIndex(ciReq).object();
        ci.remove(QStringLiteral("generated_at_ms"));
        ci.remove(QStringLiteral("refreshed_files"));
        ci.remove(QStringLiteral("etag"));
        result[QStringLiteral("codebase_index")] = ci;

        // --- project_settings_suggestion (ANTS-2161) ---
        // When no .ants/project.json exists AND the codebase_index came
        // back near-empty, surface the layout detector's suggestion so a
        // non-src/ project (e.g. DOOM's linuxdoom-1.10/) learns it can
        // declare source_roots. Cheap gate: the detect() walk runs ONLY
        // when file_count is below the low-water mark, so a healthy project
        // never pays for it. Omitted entirely otherwise (ETag-stable for
        // standard projects). Does NOT contribute to allOk.
        constexpr int kSuggestLowWaterMark = 5;
        const int fileCount = ci.value(QStringLiteral("file_count")).toInt(-1);
        if (fileCount >= 0 && fileCount < kSuggestLowWaterMark
            && !QFileInfo::exists(rootCanonical
                                  + QStringLiteral("/.ants/project.json"))) {
            const ProjectSettings::Suggestion sug =
                ProjectSettings::detect(rootCanonical);
            if (sug.sourceRoots) {
                QJsonObject suggested;
                suggested[QStringLiteral("source_roots")] =
                    QJsonArray::fromStringList(*sug.sourceRoots);
                QJsonObject sg;
                sg[QStringLiteral("reason")]               = sug.reason;
                sg[QStringLiteral("suggested")]            = suggested;
                sg[QStringLiteral("write_via")]            =
                    QStringLiteral("project_settings op:\"init\"");
                sg[QStringLiteral("next_step")]            =
                    QStringLiteral("run project_settings op:\"init\" to "
                                   "index these source_roots");
                sg[QStringLiteral("default_source_count")] = sug.defaultSourceCount;
                sg[QStringLiteral("total_source_count")]   = sug.totalSourceCount;
                result[QStringLiteral("project_settings_suggestion")] = sg;
            }
        }
    }

    // --- find_sources cold-cache pre-warm (ANTS-3444a) ---
    // find_sources reads up to 256 KiB from each of ~700 src/+tests/ files
    // on every call. Warm that's ~20 ms, but the FIRST call after a
    // relaunch pays the cold-disk cliff (52 s measured 2026-07-04) — the
    // dominant "flaky first-minute" UX. Pull those same file bodies into
    // the OS page cache on a background thread now, riding this blessed
    // first call, so the session's first find_sources reads warm. Page
    // cache only (one reused buffer, no in-process cache) → OS-managed +
    // self-evicting, zero process-heap budget. The worker captures only a
    // QString by value and touches no `this` state, so its lifetime is
    // independent of the server. Fires once per project root per session;
    // a project without src/+tests/ warms 0 files and costs nothing.
    if (!m_prewarmedRoots.contains(rootCanonical)) {
        m_prewarmedRoots.insert(rootCanonical);
        const QString warmRoot = rootCanonical;
        QThread *warm = QThread::create([warmRoot]() {
            FindSources::prewarm(warmRoot);
        });
        connect(warm, &QThread::finished, warm, &QObject::deleteLater);
        warm->start();
    }

    // --- feedback_pending (ANTS-1964, ANTS-1961 follow-on "b") ---
    // Surface the un-triaged-addenda backlog across the cross-session
    // *_Ants_MCP_Feedback.md files at the shared root, so the maintainer
    // session sees at a glance which files have new contributor input
    // without one feedback_query round-trip per file. Reuses the canonical
    // FeedbackFile::parse (no bash reimplementation of the fence-aware
    // parser); the pure-shell SessionStart hooks can't reach it, and
    // session_orient is the documented first read (parity with
    // server_build / codebase_index above).
    //
    // Maintainer-only gate: this is meaningful solely for the Ants repo
    // (it owns the ANTS-NNNN triage). Detect it by the format-standard doc
    // it ships — only the Ants project has docs/standards/mcp-feedback-files.md.
    // Sister projects whose root shares the same parent dir thus omit the
    // block entirely (unchanged response → stable ETag for them). Does NOT
    // contribute to allOk: an absent corpus must not fail orient.
    if (QFileInfo::exists(rootCanonical
            + QLatin1String("/docs/standards/mcp-feedback-files.md"))) {
        const QString sharedRoot = QFileInfo(rootCanonical).absolutePath();
        QDir dir(sharedRoot);
        // Deterministic order → ETag stability across calls.
        const QStringList names = dir.entryList(
            {QStringLiteral("*_Ants_MCP_Feedback.md")},
            QDir::Files | QDir::Readable, QDir::Name);

        QJsonArray pendingFiles;
        int totalAwaiting = 0, filesWithInbox = 0;
        int filesScanned = 0;
        int totalPendingLines = 0;
        // Bound the transient working set (largest corpus file ~150 KB
        // today); skip a pathologically large file rather than read it.
        constexpr qint64 kFeedbackScanCeiling = 4 * 1024 * 1024;
        for (const QString &name : names) {
            const QString full = dir.absoluteFilePath(name);
            QFileInfo fi(full);
            if (fi.size() > kFeedbackScanCeiling) continue;
            QFile f(full);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QString content = QString::fromUtf8(f.readAll());
            f.close();
            ++filesScanned;
            const FeedbackFile::ParseResult pr = FeedbackFile::parse(content);
            const int awaitingHere = int(pr.awaiting.size());
            totalAwaiting += awaitingHere;
            // ANTS-3631 — an awaiting marker is the MAINTAINER'S OUTBOX, not
            // their inbox: it is un-triaged so the question reaches the
            // reporter, and counting it here would put a permanently non-zero
            // to-do at every session start that clears only when someone else
            // replies. So a file is LISTED only when it holds un-triaged
            // findings that are not awaiting markers.
            //
            // The outbox stays visible as a number rather than a row: an
            // awaiting-only file still gets a minimal entry carrying its count
            // and NO `delta_line_count`, and the absence of that key is what
            // marks it as outbox. Dropping it entirely would leave the count
            // with no carrier in the one case the field exists for.
            // "Does the delta hold anything that is NOT an awaiting marker?"
            // The delta is a `\n`-joined concatenation of finding blocks, so
            // its heading count is the finding count; every awaiting finding
            // contributes exactly one. Equal counts mean awaiting-only.
            const int deltaFindings = pr.deltaPresent
                ? int(pr.delta.count(QStringLiteral("\n### "))
                      + (pr.delta.startsWith(QStringLiteral("### ")) ? 1 : 0))
                : 0;
            const bool inboxOnlyAwaiting =
                pr.deltaPresent && awaitingHere > 0 &&
                deltaFindings > 0 && deltaFindings == awaitingHere;
            if (!pr.deltaPresent && awaitingHere == 0) continue;
            QJsonObject entry;
            entry[QStringLiteral("file")] = name;
            if (awaitingHere > 0)
                entry[QStringLiteral("awaiting_count")] = awaitingHere;
            if (pr.deltaPresent && !inboxOnlyAwaiting) {
                entry[QStringLiteral("delta_line_count")] = pr.deltaLineCount;
                totalPendingLines += pr.deltaLineCount;
                ++filesWithInbox;
            }
            pendingFiles.append(entry);
        }

        QJsonObject fp;
        fp[QStringLiteral("shared_root")]        = sharedRoot;
        fp[QStringLiteral("files_scanned")]      = filesScanned;
        // ANTS-3631 — `files_with_pending` counts INBOX files only. An
        // awaiting-only file is now a row (it carries the maintainer's own
        // outstanding-question count) but is not work anyone owes them, and
        // the standard pins these two to the listed-inbox set so a count can
        // never disagree with the rows a reader is looking at.
        fp[QStringLiteral("files_with_pending")]  = filesWithInbox;
        fp[QStringLiteral("total_pending_lines")] = totalPendingLines;
        fp[QStringLiteral("total_awaiting")]      = totalAwaiting;
        fp[QStringLiteral("files")]              = pendingFiles;
        result[QStringLiteral("feedback_pending")] = fp;
    }

    // --- mail_pending (ANTS-4622 § 2.4) ---
    // A mailbox nobody polls is a mailbox nobody reads, so the inbox rides
    // along on the documented first call.
    //
    // Three deliberate differences from feedback_pending above.
    //
    // NOT gated on shipping a document. That gate exists because feedback
    // triage is the Ants repo's job alone; an inbox belongs to every registered
    // project.
    //
    // The INBOX, never the outbox. Mail this project has SENT and nobody has
    // acked contributes nothing — ANTS-3631 measured what the other direction
    // costs, a permanently non-zero to-do only somebody else can clear. For
    // self-addressed mail the sender IS the recipient, so it does appear; that
    // is delivery, not the outbox.
    //
    // Emitted ONLY when the unacked count is non-zero — never as a
    // present-and-zero block. "No mail" means no UNACKED mail, not no rows: a
    // project that has acked everything still holds rows until § 2.6's prune
    // reaches them, and a zero block for those 30 days would churn this
    // envelope's ETag at every session start. Absent also covers a project that
    // is not registered at all, which has no mailbox rather than an empty one.
    {
        RoadmapStore mailStore(RoadmapStore::defaultPath());
        QString mailErr;
        if (mailStore.open(&mailErr)) {
            // projectIdForRoot, never registerProject: the latter is
            // upsert-shaped and would register any root this read was handed,
            // into a machine-global store.
            if (const auto self = mailStore.projectIdForRoot(rootCanonical)) {
                int unacked = 0;
                QStringList senders;
                if (mailStore.mailSummaryFor(*self, &unacked, &senders) && unacked > 0) {
                    QJsonArray from;
                    for (const QString &s : senders)
                        from.append(s);
                    QJsonObject mp;
                    mp[QStringLiteral("unacked_count")] = unacked;
                    mp[QStringLiteral("from")]          = from;
                    result[QStringLiteral("mail_pending")] = mp;
                }
            }
        }
        // A store that will not open is not an orient failure: it does not
        // touch allOk, exactly as feedback_pending's absent corpus does not.
    }

    // ANTS-3587 — surface absent-but-optional artifacts (e.g. no ROADMAP.md on
    // a fresh project) as notices without failing the whole envelope.
    if (!notices.isEmpty()) result[QStringLiteral("notices")] = notices;
    result[QStringLiteral("ok")] = allOk;
    // ETag injected at the dispatch layer (isEtagSupportedTool).
    return QJsonDocument(result);
}

// ----- ANTS-1309 + ANTS-1308 — spec-aware MCP tools ----------------
//
// Shared parser for docs/specs/<id>.md. Recognises both the table
// form (`| INV-N | body | test_surface |` — ANTS-1555-style) and the
// bullet form (`- **INV-N** — body` or `- **INV-N.** body` —
// ANTS-1290 / ANTS-1583 style). Title comes from the leading
// `# ANTS-NNNN — <title>` line; Status / Kind come from the
// `**Status:**` / `**Kind:**` metadata block.

namespace rcdetail {

QJsonObject sqErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = code;
    return o;
}

// Strict id check accepting either <PREFIX>-NNNN (canonical
// docs/specs/ layout) or phase_<NN>_<topic> (docs/phases/ layout
// used by some sister projects — ANTS-1880). The two arms are
// disjoint (arm 1 ends in `-<digits>`, the phase arm never does), so
// the routing in cmdSpecQuery is unambiguous.
bool isValidSpecId(const QString &id) {
    // ANTS-1906 — widened the `phase_*` arm to accept hyphens and
    // mixed case in the topic suffix so non-Ants projects (Vestige
    // ships `phase_22_threading_design`-style filenames already, but
    // others use `phase_22_Foo-Bar`) can drive the routing without
    // an explicit `path` override.
    // ANTS-3356 — widened arm 1 from the hardcoded `ANTS-` prefix to
    // any `<PREFIX>-NNNN` shape (e.g. `DOOM-0009`), mirroring what
    // ANTS-2076 did for roadmap_log's counter-ID prefix. A spec id is
    // routed to docs/specs/ and resolved by resolveSpecRelForId (exact
    // `<id>.md`, then a `<id>-*.md` glob for topic-suffixed files);
    // `path` stays the explicit override.
    // ANTS-3436 — arm 3: a numeric-led `NN` / `NN-topic` id. list mode
    // (`specListEnvelope`) emits `id` = the file stem, so a project whose
    // specs are named `17-emission-model.md` (Album Builder) got `id:
    // "17-emission-model"` back, then had that exact id REJECTED by this
    // guard (arm 1 needs a leading letter; arm 2 needs `phase_`). The read
    // surface must accept the identifiers its own list mode hands out. The
    // char class stays `[A-Za-z0-9_-]` (no `/`/`.`), so routing to
    // `docs/specs/<id>.md` cannot traverse out of the specs dir.
    static const QRegularExpression re(
        QStringLiteral("^([A-Za-z][A-Za-z0-9_-]*-[0-9]+"
                       "|phase_[0-9]+_[A-Za-z0-9_-]+"
                       "|[0-9]+(?:-[A-Za-z0-9_-]+)*)$"));
    return re.match(id).hasMatch();
}

// ANTS-3356 — resolve the spec/phase file for `id` under `dirRel`
// (which carries a trailing slash, e.g. "docs/specs/"). Tries the
// canonical `<dirRel><id>.md` first (ANTS-NNNN + phase_* shapes), then
// globs `<dirRel><id>-*.md` so a project-prefixed spec carrying a topic
// suffix (e.g. docs/specs/DOOM-0009-path-tracer.md) resolves from the
// bare id. Returns the project-relative path of the match, or the
// canonical exact path when nothing exists so the caller's not_found
// message still names the expected file. Shared by spec_query (read)
// and spec_log (write); both treat a spec as edit-in-place of an
// existing file.
QString resolveSpecRelForId(const QString &rootCanonical,
                            const QString &dirRel, const QString &id) {
    const QString exactRel = dirRel + id + QStringLiteral(".md");
    if (QFileInfo::exists(rootCanonical + QLatin1Char('/') + exactRel))
        return exactRel;
    QDir dir(rootCanonical + QLatin1Char('/') + dirRel);
    const QStringList hits = dir.entryList(
        {id + QStringLiteral("-*.md")}, QDir::Files, QDir::Name);
    if (!hits.isEmpty()) return dirRel + hits.first();
    return exactRel;  // nothing matched — keep canonical path for not_found
}


// ANTS-3360 — spec_query list/index mode. With neither `id` nor `path`,
// enumerate the specs dir (`.ants/project.json` specs_dir override
// honoured, ANTS-2160) and return a compact directory of
// {id, title, status, path, size_bytes, mtime_ms} — the spec-side
// analogue of roadmap_query mode:section_index, so a session discovers
// spec ids without a shell `ls`. Phase docs (docs/phases/) are
// intentionally excluded; pass an explicit `path` for those. Bounded at
// kSpecListCap entries (filename order) so a pathological tree can't
// blow the response.
QJsonObject specListEnvelope(const QString &rootCanonical) {
    QString specsDir = QStringLiteral("docs/specs");
    if (const auto sd = ProjectSettings::load(rootCanonical).specsDir;
        sd && QDir(rootCanonical + QLatin1Char('/') + *sd).exists())
        specsDir = *sd;
    QJsonObject out;
    out["ok"]        = true;
    out["mode"]      = QStringLiteral("list");
    out["specs_dir"] = specsDir;
    QJsonArray specs;
    int dropped = 0;
    QDir dir(rootCanonical + QLatin1Char('/') + specsDir);
    if (dir.exists()) {
        const QStringList files = dir.entryList(
            {QStringLiteral("*.md")}, QDir::Files, QDir::Name);
        constexpr int kSpecListCap = 500;
        for (const QString &fname : files) {
            if (specs.size() >= kSpecListCap) {
                dropped = files.size() - specs.size();
                break;
            }
            QJsonObject e;
            QString sid = fname;
            if (sid.endsWith(QStringLiteral(".md"))) sid.chop(3);
            e["id"]   = sid;
            e["path"] = specsDir + QLatin1Char('/') + fname;
            QFileInfo fi(dir.filePath(fname));
            e["size_bytes"] = fi.size();
            e["mtime_ms"]   = fi.lastModified().toMSecsSinceEpoch();
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                // ANTS-3444 — list mode emits only title + status, and the
                // spec standard fixes both in the first lines (measured max
                // Status offset across the corpus: 124 bytes). Read a bounded
                // head instead of the whole body (specs run to ~85 KB) so the
                // full-tree list stops paying a full parseSpecBody per spec.
                const QJsonObject parsed =
                    SpecParse::parseSpecBody(QString::fromUtf8(f.read(8192)));
                f.close();
                e["title"]  = parsed.value(QStringLiteral("title")).toString();
                e["status"] = parsed.value(QStringLiteral("status")).toString();
            }
            specs.append(e);
        }
    }
    out["specs"]     = specs;
    out["count"]     = specs.size();
    out["truncated"] = dropped > 0;
    if (dropped > 0) out["specs_dropped"] = dropped;
    return out;
}

// ANTS-4352 — mode:"gate_drift": which gated specs have been EDITED since
// their last review loop.
//
// A spec was stamped Reviewed on one date; two days later a different roadmap
// item rewrote one of its sections while closing a defect elsewhere — a
// legitimate commit nobody read cold. The stamp stayed. The eventual re-gate
// found 20 verified defects across three loops, two of which would have
// shipped a contrast regression into the only appearance mode low-vision
// users have.
//
// The failure is silent AND self-concealing: the document asserts it was
// reviewed, that assertion is what the next session trusts, and the
// invalidating edit sits in another item's commit where nobody looks.
//
// Both halves were already visible here — the loop log is parsed, and git is
// reachable — so this is a join, not new knowledge. `commits_since` is the
// part that makes it actionable rather than merely alarming: it is what
// distinguishes the gate's OWN fix pass (which does not re-arm the gate) from
// an authoring edit by another item (which does), and in the reported case
// the commit subject was itself the whole diagnosis.
static QString sqLastLoopDate(const QString &body) {
    // The latest ISO date anywhere in the loop-log section, whichever row
    // order the log runs in — this corpus has both (ANTS-4353), so keying on
    // "the last row" would read the wrong end of half the specs.
    const int hdr = body.indexOf(QStringLiteral("loop log"), 0,
                                 Qt::CaseInsensitive);
    if (hdr < 0) return QString();
    int end = body.indexOf(QStringLiteral("\n## "), hdr);
    if (end < 0) end = body.size();
    static const QRegularExpression dateRe(
        QStringLiteral("\\b(\\d{4}-\\d{2}-\\d{2})\\b"));
    QString latest;
    auto it = dateRe.globalMatch(body.mid(hdr, end - hdr));
    while (it.hasNext()) {
        const QString d = it.next().captured(1);
        if (d > latest) latest = d;   // ISO dates sort lexically
    }
    return latest;
}

QJsonObject specGateDriftEnvelope(const QString &rootCanonical) {
    const QJsonObject list = specListEnvelope(rootCanonical);
    QJsonObject out;
    out["ok"]        = true;
    out["mode"]      = QStringLiteral("gate_drift");
    out["specs_dir"] = list.value(QStringLiteral("specs_dir"));

    QJsonArray stale, current, ungated;
    for (const QJsonValue &v : list.value(QStringLiteral("specs")).toArray()) {
        const QJsonObject e   = v.toObject();
        const QString     rel = e.value(QStringLiteral("path")).toString();
        QFile f(rootCanonical + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString body = QString::fromUtf8(f.readAll());
        f.close();

        QJsonObject row;
        row["path"]   = rel;
        row["status"] = e.value(QStringLiteral("status"));

        const QString loopDate = sqLastLoopDate(body);
        if (loopDate.isEmpty()) {
            // No loop log at all. NOT stale — nobody claimed it was gated, so
            // there is no stamp to have outlived anything. Reported so a
            // caller can tell "never gated" from "gated and current", which
            // is the distinction a bare two-bucket answer loses.
            ungated.append(row);
            continue;
        }
        row["last_loop_date"] = loopDate;

        // Commits touching this file since the last loop date.
        //
        // `--after=YYYY-MM-DD` means "after midnight of that day", so it
        // INCLUDES the day's own commits — measured against this repo's 243
        // specs, where the top stale row was `docs: cold-eyes loop 7 Phase 4
        // — accuracy fixes` dated the loop date itself. That is the gate's
        // OWN fix pass, which global rule 14 says does not re-arm the gate,
        // so reporting it as drift would make the common case a false
        // positive.
        //
        // They are kept rather than filtered, because a same-day commit CAN
        // be an authoring edit — but each is flagged `same_day`, so the
        // distinction the reporter wanted from reading subjects is available
        // mechanically as well.
        const auto r = GitWrap::run(rootCanonical, {
            QStringLiteral("log"),
            QStringLiteral("--after=") + loopDate,
            QStringLiteral("--pretty=format:%h %ad %s"),
            QStringLiteral("--date=short"),
            QStringLiteral("--"), rel});
        if (r.exitCode != 0) {
            // Git could not answer. Say so rather than reporting the spec as
            // current — an unanswerable check is not a pass (ANTS-4374).
            row["git_error"] = true;
            ungated.append(row);
            continue;
        }
        QJsonArray since;
        QString lastCommitDate;
        const QStringList outLines =
            QString::fromUtf8(r.stdoutBytes).split(QLatin1Char('\n'),
                                                   Qt::SkipEmptyParts);
        for (const QString &ln : outLines) {
            const int sp1 = ln.indexOf(QLatin1Char(' '));
            if (sp1 < 0) continue;
            const int sp2 = ln.indexOf(QLatin1Char(' '), sp1 + 1);
            if (sp2 < 0) continue;
            QJsonObject c;
            c["sha"]     = ln.left(sp1);
            c["date"]    = ln.mid(sp1 + 1, sp2 - sp1 - 1);
            c["subject"] = ln.mid(sp2 + 1);
            if (lastCommitDate.isEmpty())
                lastCommitDate = c["date"].toString();
            if (c["date"].toString() == loopDate) c["same_day"] = true;
            since.append(c);
        }
        // A spec whose ONLY commits since the loop are same-day is current:
        // that is the gate's own fix pass, and rule 14 is explicit that a
        // document whose only changes came from the gate's fix pass is gated
        // — the run that made those edits WAS the review.
        bool anyLater = false;
        for (const QJsonValue &cv : std::as_const(since))
            if (!cv.toObject().value(QStringLiteral("same_day")).toBool())
                anyLater = true;
        if (since.isEmpty() || !anyLater) {
            if (!since.isEmpty()) row["same_day_commits_only"] = true;
            current.append(row);
        } else {
            row["last_commit_date"] = lastCommitDate;
            row["commits_since"]    = since;
            stale.append(row);
        }
    }
    out["stale"]   = stale;
    out["current"] = current;
    out["ungated"] = ungated;
    out["counts"]  = QJsonObject{
        {QStringLiteral("stale"),   stale.size()},
        {QStringLiteral("current"), current.size()},
        {QStringLiteral("ungated"), ungated.size()}};
    return out;
}


}  // namespace rcdetail

QJsonDocument RemoteControl::cmdSpecQuery(const QJsonObject &req) {
    const QString id = req.value(QStringLiteral("id")).toString();
    // ANTS-1906 — optional `path` escape hatch for projects whose
    // spec / phase docs live at non-standard locations (e.g. Vestige
    // ships `docs/phases/phase_22_threading_design.md`; another
    // project may put them under `docs/design/`). When set, the
    // verb bypasses isValidSpecId + per-shape directory routing and
    // reads that project-relative path directly, deriving the
    // response `id` from the basename. The `id` arg becomes optional
    // under `path`; when both are set the explicit `id` wins (caller
    // wants a specific display id).
    const QString pathArg = req.value(QStringLiteral("path")).toString();
    if (id.isEmpty() && pathArg.isEmpty()) {
        // ANTS-3360 — no id/path → list mode (spec discovery), the
        // spec-side analogue of roadmap_query mode:section_index.
        const QString rootCanonical = resolveRootCanonical(m_main, req);
        if (rootCanonical.isEmpty()) {
            return QJsonDocument(sqErr(
                QStringLiteral("no_project"),
                QStringLiteral("spec_query: project root unresolved")));
        }
        // ANTS-4352 — mode:"gate_drift" answers "has this gated document been
        // edited since its last review loop?", which nothing could answer.
        const QString mode = req.value(QStringLiteral("mode")).toString();
        if (mode == QLatin1String("gate_drift")) {
            return QJsonDocument(specGateDriftEnvelope(rootCanonical));
        }
        // ANTS-4468 — an unrecognised mode used to fall through to the list
        // branch, so `mode:"gate-drift"` returned a spec LIST with ok:true and
        // nothing saying the requested mode had not run. That is the failure
        // the reporter said ignored_args exists to catch and could no longer
        // catch, now that `mode` is a declared property: the name-diff checker
        // sees a known key and says nothing, and the value is never validated.
        // Refuse instead, naming what was accepted — a list returned in answer
        // to a drift question is a confident wrong answer.
        if (!mode.isEmpty() && mode != QLatin1String("list")) {
            return QJsonDocument(sqErr(
                QStringLiteral("bad_args"),
                QStringLiteral("spec_query: unknown mode \"%1\" — expected "
                               "\"gate_drift\", \"list\", or omit it with an "
                               "`id`/`path` to parse one spec")
                    .arg(mode)));
        }
        return QJsonDocument(specListEnvelope(rootCanonical));
    }
    if (!id.isEmpty() && pathArg.isEmpty() && !isValidSpecId(id)) {
        return QJsonDocument(sqErr(
            QStringLiteral("bad_id"),
            QStringLiteral("spec_query: id must match <PREFIX>-NNNN "
                           "(e.g. ANTS-1963, DOOM-0009), "
                           "phase_<NN>_<topic>, a numeric <NN>-<topic> "
                           "(e.g. 17-emission-model), or pass an explicit "
                           "`path` (ANTS-1906)")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(sqErr(
            QStringLiteral("no_project"),
            QStringLiteral("spec_query: project root unresolved")));
    }
    // ANTS-1880 — per-id-shape routing. ANTS-NNNN ids resolve under
    // docs/specs/; phase_* ids resolve under docs/phases/. The two
    // regexes are disjoint (isValidSpecId enforces this), so the
    // routing is unambiguous. Response carries a `source` field so
    // a caller scanning results can branch on layout without
    // re-parsing the path.
    // ANTS-1906 — when `path` is set, skip id-shape routing entirely
    // and read the project-relative path. The `path` is constrained
    // to the project root (no leading slash, no `..` traversal) so
    // the verb stays a read-only on-disk lookup.
    QString rel, full;
    bool isPhase = false;
    QString sourceTag;
    if (!pathArg.isEmpty()) {
        // ANTS-1995 — route the path arg through PathValidation (the
        // canonical, symlink-resolving anchor check) exactly as the
        // write twin spec_log already does. The previous manual
        // leading-'/' + ".."-substring test never canonicalised, so a
        // symlink inside the project (e.g. docs/specs/evil → /etc/passwd)
        // passed the substring checks yet resolved OUTSIDE the project
        // root — a read-side escape.
        const auto check = PathValidation::validatePath(
            pathArg, rootCanonical, QStringLiteral("spec_query"),
            QStringLiteral("path"), /*allowOutsideRoot=*/false);
        if (check.bad) return QJsonDocument(check.err);
        if (!check.resolved.isEmpty()) {
            full = check.resolved;
        } else {
            // Non-existent path: validatePath leaves resolved empty.
            // Rebuild the in-root absolute path so the not_found check
            // below fires with the right message.
            full = QDir::cleanPath(rootCanonical + QLatin1Char('/') + pathArg);
        }
        rel = pathArg;
        sourceTag = QStringLiteral("path");
    } else {
        isPhase = id.startsWith(QStringLiteral("phase_"));
        // ANTS-2160 — specs_dir override (phase routing unchanged).
        QString specsDir = QStringLiteral("docs/specs");
        if (const auto sd = ProjectSettings::load(rootCanonical).specsDir;
            sd && QDir(rootCanonical + QLatin1Char('/') + *sd).exists())
            specsDir = *sd;
        const QString dirRel = isPhase
            ? QStringLiteral("docs/phases/")
            : (specsDir + QLatin1Char('/'));
        // ANTS-3356 — exact `<id>.md`, then a `<id>-*.md` glob so a
        // topic-suffixed project spec (DOOM-0009-path-tracer.md) resolves.
        rel  = resolveSpecRelForId(rootCanonical, dirRel, id);
        full = rootCanonical + QLatin1Char('/') + rel;
        sourceTag = isPhase
            ? QStringLiteral("phases")
            : QStringLiteral("specs");
    }
    QFileInfo fi(full);
    if (!fi.exists() || !fi.isFile()) {
        return QJsonDocument(sqErr(
            QStringLiteral("not_found"),
            QStringLiteral("spec_query: %1 does not exist").arg(rel)));
    }
    QFile f(full);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QJsonDocument(sqErr(
            QStringLiteral("io_error"),
            QStringLiteral("spec_query: cannot open %1").arg(rel)));
    }
    const QString body = QString::fromUtf8(f.readAll());
    f.close();

    QJsonObject result = SpecParse::parseSpecBody(body);
    result["ok"]         = true;
    // ANTS-1906 — derive id from basename when only `path` was passed
    // and no explicit `id` came in. Strips trailing ".md".
    QString displayId = id;
    if (displayId.isEmpty()) {
        displayId = fi.fileName();
        if (displayId.endsWith(QStringLiteral(".md"))) displayId.chop(3);
    }
    result["id"]         = displayId;
    result["path"]       = rel;
    result["size_bytes"] = fi.size();
    result["mtime_ms"]   = fi.lastModified().toMSecsSinceEpoch();
    // ANTS-1880 — source-dir discriminator for the caller.
    // ANTS-1906 — "path" sentinel for the explicit-path mode so a
    // caller scanning results can branch on layout without
    // re-parsing the response path.
    result["source"] = sourceTag;
    return QJsonDocument(result);
}

QJsonDocument RemoteControl::cmdInvariantCheck(const QJsonObject &req) {
    const QJsonArray filesArr =
        req.value(QStringLiteral("files")).toArray();
    if (filesArr.isEmpty()) {
        return QJsonDocument(sqErr(
            QStringLiteral("bad_files"),
            QStringLiteral("invariant_check: \"files\" must be a "
                           "non-empty array of project-relative paths")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(sqErr(
            QStringLiteral("no_project"),
            QStringLiteral("invariant_check: project root unresolved")));
    }

    // Normalise input file list: strip leading ./, dedup, drop empties.
    QStringList needles;
    for (const QJsonValue &v : filesArr) {
        QString s = v.toString().trimmed();
        while (s.startsWith(QStringLiteral("./"))) s = s.mid(2);
        if (s.isEmpty()) continue;
        if (!needles.contains(s)) needles.append(s);
    }
    if (needles.isEmpty()) {
        return QJsonDocument(sqErr(
            QStringLiteral("bad_files"),
            QStringLiteral("invariant_check: \"files\" had no usable "
                           "entries after normalisation")));
    }

    // ANTS-3699 — mode: "summary" (default) omits each spec's invariant
    // BODIES, keeping id/path/title/matched_terms/invariants_count. That is
    // the whole answer to this verb's actual question ("does a spec already
    // claim something about these files?"); spec_query is the drill-in. The
    // full bodies could not be narrowed at all, and one measured call over
    // three files — one of them remotecontrol.cpp — returned 267,070
    // characters, over the response cap and into a spill file. A first-step
    // lookup that can do that is a step people learn to skip, which takes
    // /write-code's other three Phase 0 lookups with it.
    //
    // Summary is the DEFAULT, not an opt-in: a saving nobody knows to ask
    // for is a saving almost no session gets. `invariants` is omitted rather
    // than truncated, so a short list can never be mistaken for a complete
    // one; `invariants_included` states which shape this response is.
    const QString mode = req.value(QStringLiteral("mode")).toString();
    if (!mode.isEmpty() && mode != QLatin1String("summary")
        && mode != QLatin1String("full")) {
        return QJsonDocument(sqErr(
            QStringLiteral("bad_mode"),
            QStringLiteral("invariant_check: mode must be \"summary\" "
                           "(default) or \"full\"")));
    }
    const bool wantBodies = (mode == QLatin1String("full"));

    // ANTS-1880 — walk both docs/specs/ (ANTS-NNNN canonical) and
    // docs/phases/ (phase_<NN>_<topic>) when present, merging hits
    // into one matched_specs[] array. specs_scanned retains its
    // existing specs-only semantics (back-compat); two new sibling
    // fields phases_scanned + total_scanned carry the new counts.
    QJsonArray matched;
    int specsScanned = 0;
    int phasesScanned = 0;
    // ANTS-4644 — the scan reads its needles from here rather than from
    // `needles`, so a fallback pass can swap in shorter path forms without a
    // second copy of the matcher.
    QStringList activeNeedles = needles;
    auto scanOneDir = [&](const QString &dirRel,
                          const QString &glob,
                          int &counter) {
        const QString absDir = rootCanonical + QLatin1Char('/') + dirRel;
        QDir dir(absDir);
        if (!dir.exists()) return;
        const QStringList files =
            dir.entryList({glob},
                          QDir::Files | QDir::Readable, QDir::Name);
        for (const QString &fname : files) {
            ++counter;
            QFile f(absDir + QLatin1Char('/') + fname);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QString text = QString::fromUtf8(f.readAll());
            f.close();
            QStringList hits;
            for (const QString &needle : std::as_const(activeNeedles)) {
                if (text.contains(needle, Qt::CaseSensitive)) {
                    if (!hits.contains(needle)) hits.append(needle);
                }
            }
            if (hits.isEmpty()) continue;
            const QJsonObject parsed = SpecParse::parseSpecBody(text);
            const QJsonArray invs =
                parsed.value(QStringLiteral("invariants")).toArray();
            // ID = filename stem (ANTS-NNNN or phase_<NN>_<topic>).
            QString sid = fname;
            if (sid.endsWith(QStringLiteral(".md"))) sid.chop(3);
            QJsonObject entry;
            entry["id"]    = sid;
            entry["path"]  = dirRel + QLatin1Char('/') + fname;
            entry["title"] = parsed.value(QStringLiteral("title"));
            QJsonArray hitArr;
            for (const QString &h : hits) hitArr.append(h);
            entry["matched_terms"] = hitArr;
            if (wantBodies) entry["invariants"] = invs;   // ANTS-3699
            entry["invariants_count"] = invs.size();
            matched.append(entry);
        }
    };
    // ANTS-4376 — the specs glob was hard-coded `ANTS-*.md`, so this verb saw
    // NOTHING on any project whose id prefix is not ANTS. Measured across four
    // projects on 2026-08-14: Ants Terminal 243 specs scanned, LottoTracker 0
    // (spec_query lists 8), DOOM 0 (lists 19), OneUp 0 — while spec_query
    // resolved docs/specs on every one. It looked like "works only where Ants
    // is rooted"; the truth is duller and worse — this project's prefix simply
    // happens to be ANTS.
    //
    // It matters because /write-code Phase 0 OPENS with this call to surface a
    // documented contract before an edit breaks it, and `matched_count:0` with
    // `ok:true` is indistinguishable from the legitimate "no spec governs
    // these files". So a session edits under a contract it was never shown.
    // Three sessions re-confirmed it before it was diagnosed.
    //
    // Scan every `*.md`, matching what spec_query's list mode already does,
    // and honour `.ants/project.json`'s `specs_dir` (ANTS-2160) the way
    // doc_integrity and spec_query do rather than hard-coding docs/specs.
    const QString specsDir =
        ProjectSettings::load(rootCanonical).specsDir.value_or(
            QStringLiteral("docs/specs"));
    // Every pass reads the same files, so the counters are reset and retaken
    // rather than accumulated — a fallback pass must not double the scan count.
    auto runScan = [&]() {
        matched       = QJsonArray();
        specsScanned  = 0;
        phasesScanned = 0;
        scanOneDir(specsDir, QStringLiteral("*.md"), specsScanned);
        scanOneDir(QStringLiteral("docs/phases"),
                   QStringLiteral("phase_*.md"), phasesScanned);
    };
    runScan();

    // ANTS-4644 — a spec cites a module the way a HUMAN writes it, so the
    // project-relative form this verb's own description prescribes is routinely
    // the one form that matches nothing. Two projects reported it on the same
    // day, and it reproduces here: docs/specs/ANTS-2161.md, the spec governing
    // op:detect, cites `projectsettings.cpp` and is invisible to a query for
    // `src/projectsettings.cpp`.
    //
    // The damage is that `matched_count:0` beside `specs_scanned:247` reads as
    // a definitive "nothing governs this file" — the same confident zero
    // ANTS-4376 fixed for a blind scan, arriving by a different route.
    //
    // So on a zero that DID look, retry with the path's suffixes, longest
    // first. The bare basename is its own tier and runs only when every fuller
    // form has failed, because it is the tier that can collide (two `auth.py`
    // in one repo); `matched_as` names the form that actually matched, so a
    // rescued hit never passes for a direct one.
    QJsonObject matchedAs;
    QString     fallbackKind;
    if (matched.isEmpty() && (specsScanned + phasesScanned) > 0) {
        QList<QPair<QString, QString>> pathForms;   // {form, original}
        QList<QPair<QString, QString>> baseForms;
        for (const QString &n : std::as_const(needles)) {
            const QStringList parts =
                n.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (int i = 1; i < parts.size(); ++i) {
                const QString form =
                    QStringList(parts.mid(i)).join(QLatin1Char('/'));
                if (i < parts.size() - 1) pathForms.append({form, n});
                else                      baseForms.append({form, n});
            }
        }
        const auto tryTier = [&](const QList<QPair<QString, QString>> &tier,
                                 const QString &kind) {
            if (tier.isEmpty() || !matched.isEmpty()) return;
            activeNeedles.clear();
            for (const auto &p : tier)
                if (!activeNeedles.contains(p.first))
                    activeNeedles.append(p.first);
            runScan();
            if (matched.isEmpty()) return;
            fallbackKind = kind;
            QStringList hitForms;
            for (const QJsonValue &v : std::as_const(matched))
                for (const QJsonValue &t :
                     v.toObject().value(QStringLiteral("matched_terms")).toArray())
                    hitForms.append(t.toString());
            // `tier` is built longest-suffix-first, so the first hit per
            // original path is the most specific form that worked.
            for (const auto &p : tier) {
                if (!hitForms.contains(p.first)) continue;
                if (matchedAs.contains(p.second)) continue;
                matchedAs[p.second] = p.first;
            }
        };
        tryTier(pathForms, QStringLiteral("path_suffix"));
        tryTier(baseForms, QStringLiteral("basename"));
    }

    QJsonObject result;
    result["ok"]             = true;
    result["matched_specs"]  = matched;
    result["specs_scanned"]  = specsScanned;
    result["phases_scanned"] = phasesScanned;
    result["total_scanned"]  = specsScanned + phasesScanned;
    result["matched_count"]  = matched.size();
    // ANTS-3699 — say which shape this is, so an absent `invariants` reads as
    // "omitted by mode" and never as "this spec has none".
    result["mode"]                = wantBodies ? QStringLiteral("full")
                                               : QStringLiteral("summary");
    result["invariants_included"] = wantBodies;
    // ANTS-4376 / ANTS-4374 — a scan that read NO specs must not answer in the
    // same shape as a scan that read specs and matched nothing. `ok:true` with
    // `matched_count:0` is the legitimate "no spec governs these files", and
    // for three sessions it was also what a totally blind scan returned. Say
    // which, and say what was looked at.
    QString scannedNothingHint;
    if (specsScanned == 0 && phasesScanned == 0) {
        result["scanned_nothing"] = true;
        result["specs_dir"]       = specsDir;
        scannedNothingHint = QDir(rootCanonical + QLatin1Char('/') + specsDir).exists()
            ? QStringLiteral("no spec files were read: %1 exists but holds no "
                             "*.md. matched_count:0 here means NOTHING WAS "
                             "LOOKED AT, not that no spec governs these files.")
                  .arg(specsDir)
            : QStringLiteral("no spec files were read: %1 does not exist. Set "
                             "`specs_dir` in .ants/project.json if this "
                             "project keeps specs elsewhere.").arg(specsDir);
    }
    // ANTS-4644 — emitted on EVERY reply. An absent flag is indistinguishable
    // from a build that has no fallback, which is the class of ambiguity this
    // item exists to remove.
    result["fallback_match"] = !fallbackKind.isEmpty();
    if (!fallbackKind.isEmpty()) {
        result["fallback_kind"] = fallbackKind;
        result["matched_as"]    = matchedAs;
    }
    // ANTS-4645 — the envelope is confident and complete-looking, and says
    // nothing about the ROADMAP being outside the scan. A project re-built a
    // feature its own roadmap had specified because four matched specs read as
    // a complete answer to "is this under contract?". The harm case is a
    // NON-zero reply, so this rides on every one.
    result["roadmap_scanned"] = false;
    result["scope_note"] = QStringLiteral(
        "scope: %1 + docs/phases only — the ROADMAP was NOT consulted, so work "
        "already planned there will not appear here. Use roadmap_query "
        "(query=<keyword>) or task_priors for that.").arg(specsDir);

    // Hints compose: a fallback hit in summary mode owes the caller both
    // messages, and neither may displace the other.
    QStringList hints;
    if (!scannedNothingHint.isEmpty()) hints << scannedNothingHint;
    if (!fallbackKind.isEmpty()) {
        hints << (fallbackKind == QLatin1String("basename")
            ? QStringLiteral(
                  "no spec mentioned the paths as given; these matched by BARE "
                  "FILENAME (see matched_as) — the lowest-confidence tier, "
                  "since two modules can share a name. Confirm each spec is "
                  "about your file.")
            : QStringLiteral(
                  "no spec mentioned the paths as given; these matched after "
                  "stripping leading directory components (see matched_as). A "
                  "spec cites a module the way its author writes it."));
    }
    if (!wantBodies && !matched.isEmpty()) {
        hints << QStringLiteral(
            "summary mode: invariant bodies omitted (invariants_count is the "
            "real count). Drill into one spec with spec_query, or pass "
            "mode:\"full\" for every body — which on a widely-referenced file "
            "can exceed the response cap.");
    }
    if (!hints.isEmpty()) result["hint"] = hints.join(QLatin1Char('\n'));
    return QJsonDocument(result);
}

// ----- ANTS-1306 / ANTS-1307 — task-start context composers ---------
//
// task_priors: free-text task description → ranked context buckets
// (matching specs / ROADMAP cards / recent commits / ADRs). Pure
// composer over cmdRoadmapQuery + cmdGitState + SpecParse::parseSpecBody.
// project_conventions: task_type → curated {rule, source} table with a
// source-existence check. Both MCP-only. Specs docs/specs/ANTS-1306.md
// and docs/specs/ANTS-1307.md.

namespace rcdetail {

QJsonObject tpErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = code;
    return o;
}

// ANTS-1306 § 3.2.1 — v1 stopword set (English function words +
// generic task verbs that carry no ranking signal). Process-static.
const QSet<QString> &taskPriorStopwords() {
    static const QSet<QString> kStop = {
        QStringLiteral("the"),   QStringLiteral("and"),   QStringLiteral("or"),
        QStringLiteral("but"),   QStringLiteral("nor"),   QStringLiteral("for"),
        QStringLiteral("of"),    QStringLiteral("to"),    QStringLiteral("in"),
        QStringLiteral("on"),    QStringLiteral("at"),    QStringLiteral("by"),
        QStringLiteral("with"),  QStringLiteral("from"),  QStringLiteral("into"),
        QStringLiteral("this"),  QStringLiteral("that"),  QStringLiteral("these"),
        QStringLiteral("those"), QStringLiteral("there"), QStringLiteral("here"),
        QStringLiteral("when"),  QStringLiteral("where"), QStringLiteral("which"),
        QStringLiteral("while"), QStringLiteral("what"),  QStringLiteral("who"),
        QStringLiteral("whom"),  QStringLiteral("will"),  QStringLiteral("would"),
        QStringLiteral("should"),QStringLiteral("could"), QStringLiteral("must"),
        QStringLiteral("shall"), QStringLiteral("not"),   QStringLiteral("are"),
        QStringLiteral("was"),   QStringLiteral("were"),  QStringLiteral("been"),
        QStringLiteral("being"), QStringLiteral("have"),  QStringLiteral("has"),
        QStringLiteral("had"),   QStringLiteral("does"),  QStringLiteral("did"),
        QStringLiteral("doing"), QStringLiteral("your"),  QStringLiteral("you"),
        QStringLiteral("our"),   QStringLiteral("their"), QStringLiteral("its"),
        QStringLiteral("then"),  QStringLiteral("than"),  QStringLiteral("also"),
        QStringLiteral("more"),  QStringLiteral("most"),  QStringLiteral("some"),
        QStringLiteral("any"),   QStringLiteral("all"),   QStringLiteral("each"),
        QStringLiteral("every"), QStringLiteral("both"),  QStringLiteral("add"),
        QStringLiteral("fix"),   QStringLiteral("make"),  QStringLiteral("use"),
        QStringLiteral("new"),   QStringLiteral("run"),   QStringLiteral("get"),
        QStringLiteral("set"),   QStringLiteral("let"),   QStringLiteral("put"),
        QStringLiteral("via"),   QStringLiteral("per"),   QStringLiteral("might"),
        QStringLiteral("may"),   QStringLiteral("can")
    };
    return kStop;
}

struct TpTerms {
    QStringList ids;    // ANTS-NNNN, upper-cased, deduped
    QStringList paths;  // known-extension tokens, deduped
    QStringList terms;  // lower-cased words >=4, minus stopwords, cap 25
};

// ANTS-1306 § 3.2 — deterministic three-set extraction, fixed order.
TpTerms tpExtract(const QString &description) {
    TpTerms t;
    static const QRegularExpression idRe(
        QStringLiteral("ANTS-[0-9]+"),
        QRegularExpression::CaseInsensitiveOption);
    auto idIt = idRe.globalMatch(description);
    while (idIt.hasNext()) {
        const QString id = idIt.next().captured(0).toUpper();
        if (!t.ids.contains(id)) t.ids.append(id);
    }
    static const QRegularExpression pathRe(QStringLiteral(
        R"([\w./-]+\.(?:cpp|cc|cxx|c|h|hpp|hh|hxx|py|lua|sh|md|json|cmake|txt))"));
    auto pIt = pathRe.globalMatch(description);
    while (pIt.hasNext()) {
        QString p = pIt.next().captured(0);
        while (p.startsWith(QStringLiteral("./"))) p = p.mid(2);
        if (!p.isEmpty() && !t.paths.contains(p)) t.paths.append(p);
    }
    static const QRegularExpression termRe(
        QStringLiteral("[A-Za-z_][A-Za-z0-9_]{3,}"));
    const QSet<QString> &stop = taskPriorStopwords();
    auto tIt = termRe.globalMatch(description);
    while (tIt.hasNext()) {
        if (t.terms.size() >= 25) break;
        const QString w = tIt.next().captured(0).toLower();
        if (stop.contains(w)) continue;
        if (!t.terms.contains(w)) t.terms.append(w);
    }
    return t;
}

// Count distinct entries of (ids ∪ paths ∪ terms) present in the
// already-lower-cased haystack. ids/paths are lower-cased here; terms
// arrive lower-cased.
int tpDistinctNeedles(const QString &haystackLower, const TpTerms &t) {
    int n = 0;
    for (const QString &id : t.ids)
        if (haystackLower.contains(id.toLower())) ++n;
    for (const QString &p : t.paths)
        if (haystackLower.contains(p.toLower())) ++n;
    for (const QString &w : t.terms)
        if (haystackLower.contains(w)) ++n;
    return n;
}

// First body line containing any needle, trimmed + capped to 200
// chars; fallback (trimmed) when none matches.
QString tpExcerpt(const QString &body, const TpTerms &t,
                  const QString &fallback) {
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString low = line.toLower();
        if (tpDistinctNeedles(low, t) <= 0) continue;
        QString s = line.trimmed();
        if (s.isEmpty()) continue;
        if (s.size() > 200) s = s.left(199) + QChar(0x2026);
        return s;
    }
    return fallback.trimmed();
}

struct TpScored {
    QJsonObject obj;
    int         score;
    QString     sortKey;
};

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdTaskPriors(const QJsonObject &req) {
    const QString description =
        req.value(QStringLiteral("description")).toString();
    if (description.trimmed().isEmpty()) {
        return QJsonDocument(tpErr(QStringLiteral("bad_args"),
            QStringLiteral("task_priors: missing or empty \"description\"")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(tpErr(QStringLiteral("no_project"),
            QStringLiteral("task_priors: project root unresolved")));
    }
    const TpTerms terms = tpExtract(description);
    if (terms.ids.isEmpty() && terms.paths.isEmpty() &&
        terms.terms.isEmpty()) {
        return QJsonDocument(tpErr(QStringLiteral("bad_args"),
            QStringLiteral("task_priors: \"description\" yielded no "
                           "searchable terms (all stopwords?)")));
    }

    auto clampCap = [&](const char *key, int def) -> int {
        const QJsonValue v = req.value(QLatin1String(key));
        if (!v.isDouble()) return def;
        int n = v.toInt(def);
        if (n < 1)  n = 1;
        if (n > 20) n = 20;
        return n;
    };
    const int maxSpecs   = clampCap("max_specs", 5);
    const int maxCards   = clampCap("max_cards", 5);
    const int maxCommits = clampCap("max_commits", 5);
    const int maxAdrs    = clampCap("max_adrs", 3);

    QJsonObject result;
    result["ok"] = true;
    {
        QJsonArray a;
        for (const QString &s : terms.terms) a.append(s);
        result["terms"] = a;
    }
    {
        QJsonArray a;
        for (const QString &s : terms.ids) a.append(s);
        result["ids"] = a;
    }
    {
        QJsonArray a;
        for (const QString &s : terms.paths) a.append(s);
        result["paths"] = a;
    }

    // (1) specs — docs/specs/ANTS-*.md, score by distinct needles
    // (+5 id-in-filename boost).
    {
        QVector<TpScored> hits;
        QDir dir(rootCanonical + QStringLiteral("/docs/specs"));
        if (dir.exists()) {
            const QStringList files = dir.entryList(
                {QStringLiteral("ANTS-*.md")},
                QDir::Files | QDir::Readable, QDir::Name);
            for (const QString &fname : files) {
                QFile f(dir.filePath(fname));
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
                const QString body = QString::fromUtf8(f.readAll());
                f.close();
                QString stem = fname;
                if (stem.endsWith(QStringLiteral(".md"))) stem.chop(3);
                int score = tpDistinctNeedles(body.toLower(), terms);
                if (terms.ids.contains(stem.toUpper())) score += 5;
                if (score <= 0) continue;
                const QJsonObject parsed = SpecParse::parseSpecBody(body);
                const QString title =
                    parsed.value(QStringLiteral("title")).toString();
                QJsonObject e;
                e["id"]      = stem;
                e["path"]    = QStringLiteral("docs/specs/") + fname;
                e["title"]   = title;
                e["excerpt"] = tpExcerpt(body, terms,
                                         title.isEmpty() ? stem : title);
                e["score"]   = score;
                hits.append({e, score, fname});
            }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const TpScored &a, const TpScored &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.sortKey < b.sortKey;
        });
        result["specs_count"] = static_cast<int>(hits.size());
        QJsonArray arr;
        for (int i = 0; i < hits.size() && i < maxSpecs; ++i)
            arr.append(hits[i].obj);
        result["specs"] = arr;
    }

    // (2) roadmap_cards — compose over cmdRoadmapQuery (status:all,
    // limit:500, include_body). Non-ok ⇒ empty bucket.
    {
        QJsonObject rq;
        rq["caller_cwd"]   = rootCanonical;
        rq["status"]       = QStringLiteral("all");
        rq["limit"]        = 500;
        rq["include_body"] = true;
        const QJsonObject rqObj = cmdRoadmapQuery(rq).object();
        QVector<TpScored> hits;
        if (rqObj.value(QStringLiteral("ok")).toBool(false)) {
            const QJsonArray bullets =
                rqObj.value(QStringLiteral("bullets")).toArray();
            int order = 0;
            for (const QJsonValue &v : bullets) {
                const QJsonObject b = v.toObject();
                const QString id = b.value(QStringLiteral("id")).toString();
                const QString headline =
                    b.value(QStringLiteral("headline_oneline")).toString();
                const QString body =
                    b.value(QStringLiteral("body")).toString();
                const QString hayLow =
                    (id + QLatin1Char(' ') + headline + QLatin1Char(' ') +
                     body).toLower();
                int score = tpDistinctNeedles(hayLow, terms);
                if (!id.isEmpty() && terms.ids.contains(id.toUpper()))
                    score += 5;
                if (score > 0) {
                    QJsonObject e;
                    e["id"]       = id;
                    e["status"]   = b.value(QStringLiteral("status")).toString();
                    e["headline"] = headline;
                    e["score"]    = score;
                    hits.append({e, score,
                                 QString::number(order).rightJustified(
                                     6, QLatin1Char('0'))});
                }
                ++order;
            }
        }
        std::stable_sort(hits.begin(), hits.end(),
                         [](const TpScored &a, const TpScored &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.sortKey < b.sortKey;
        });
        result["cards_count"] = static_cast<int>(hits.size());
        QJsonArray arr;
        for (int i = 0; i < hits.size() && i < maxCards; ++i)
            arr.append(hits[i].obj);
        result["roadmap_cards"] = arr;
    }

    // (3) commits — compose over cmdGitState(op:log). Path-filtered
    // (merged, deduped by %h sha) when paths present, else subject-
    // term-filtered over a recent window. Non-ok ⇒ empty bucket.
    {
        QVector<QJsonObject> merged;
        QSet<QString> seen;
        auto addCommits = [&](const QJsonObject &gs, bool subjectFilter) {
            if (!gs.value(QStringLiteral("ok")).toBool(false)) return;
            const QJsonArray commits =
                gs.value(QStringLiteral("commits")).toArray();
            for (const QJsonValue &cv : commits) {
                const QJsonObject c = cv.toObject();
                const QString sha = c.value(QStringLiteral("sha")).toString();
                if (sha.isEmpty() || seen.contains(sha)) continue;
                if (subjectFilter) {
                    const QString subjLow =
                        c.value(QStringLiteral("subject")).toString().toLower();
                    if (tpDistinctNeedles(subjLow, terms) <= 0) continue;
                }
                seen.insert(sha);
                QJsonObject e;
                e["sha"]     = sha;
                e["subject"] = c.value(QStringLiteral("subject")).toString();
                e["date"]    = c.value(QStringLiteral("date")).toString();
                merged.append(e);
            }
        };
        if (!terms.paths.isEmpty()) {
            int used = 0;
            for (const QString &p : terms.paths) {
                if (used >= 5) break;
                ++used;
                QJsonObject gs;
                gs["caller_cwd"] = rootCanonical;
                gs["op"]         = QStringLiteral("log");
                gs["path"]       = p;
                gs["n"]          = maxCommits;
                addCommits(cmdGitState(gs).object(), false);
            }
        } else {
            QJsonObject gs;
            gs["caller_cwd"] = rootCanonical;
            gs["op"]         = QStringLiteral("log");
            gs["n"]          = 50;
            addCommits(cmdGitState(gs).object(), true);
        }
        std::stable_sort(merged.begin(), merged.end(),
                         [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("date")).toString() >
                   b.value(QStringLiteral("date")).toString();
        });
        result["commits_count"] = static_cast<int>(merged.size());
        QJsonArray arr;
        for (int i = 0; i < merged.size() && i < maxCommits; ++i)
            arr.append(merged[i]);
        result["commits"] = arr;
    }

    // (4) adrs — docs/decisions/*.md minus README.md, score by needles.
    {
        QVector<TpScored> hits;
        QDir dir(rootCanonical + QStringLiteral("/docs/decisions"));
        if (dir.exists()) {
            const QStringList files = dir.entryList(
                {QStringLiteral("*.md")},
                QDir::Files | QDir::Readable, QDir::Name);
            for (const QString &fname : files) {
                if (fname.compare(QStringLiteral("README.md"),
                                  Qt::CaseInsensitive) == 0) continue;
                QFile f(dir.filePath(fname));
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
                const QString body = QString::fromUtf8(f.readAll());
                f.close();
                int score = tpDistinctNeedles(body.toLower(), terms);
                if (score <= 0) continue;
                QString title;
                const QStringList lines = body.split(QLatin1Char('\n'));
                for (const QString &ln : lines) {
                    if (ln.startsWith(QStringLiteral("# "))) {
                        title = ln.mid(2).trimmed();
                        break;
                    }
                }
                QJsonObject e;
                e["path"]  = QStringLiteral("docs/decisions/") + fname;
                e["title"] = title;
                e["score"] = score;
                hits.append({e, score, fname});
            }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const TpScored &a, const TpScored &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.sortKey < b.sortKey;
        });
        result["adrs_count"] = static_cast<int>(hits.size());
        QJsonArray arr;
        for (int i = 0; i < hits.size() && i < maxAdrs; ++i)
            arr.append(hits[i].obj);
        result["adrs"] = arr;
    }

    return QJsonDocument(result);
}

QJsonDocument RemoteControl::cmdProjectConventions(const QJsonObject &req) {
    const QString taskType =
        req.value(QStringLiteral("task_type")).toString();
    static const QStringList kTypes = {
        QStringLiteral("feature"),  QStringLiteral("bugfix"),
        QStringLiteral("refactor"), QStringLiteral("docs"),
        QStringLiteral("test")};
    if (!kTypes.contains(taskType)) {
        return QJsonDocument(tpErr(QStringLiteral("bad_args"),
            QStringLiteral("project_conventions: \"task_type\" must be one "
                           "of feature, bugfix, refactor, docs, test")));
    }
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        return QJsonDocument(tpErr(QStringLiteral("no_project"),
            QStringLiteral("project_conventions: project root unresolved")));
    }

    // ANTS-1307 § 3.2 — curated table. Each rule paraphrases a rule
    // stated in its `source` doc body (grep-verified during spec
    // authoring). Common rows prepended to every task type.
    using RS = QPair<QString, QString>;
    QVector<RS> conv;
    conv.append({QStringLiteral(
        "Commit messages use the `<ID>: <description>` mandate — "
        "imperative subject, one concern per commit."),
        QStringLiteral("docs/standards/commits.md")});
    conv.append({QStringLiteral(
        "Shortest correct implementation; reuse before rewriting "
        "(Rule of Three on the third call-site)."),
        QStringLiteral("docs/standards/coding.md")});

    if (taskType == QLatin1String("feature")) {
        conv.append({QStringLiteral(
            "Write spec.md first as a human-readable contract and get "
            "sign-off before writing the test/code."),
            QStringLiteral("docs/standards/testing.md")});
        conv.append({QStringLiteral(
            "Pair the feature with a tests/features/<name>/ conformance "
            "test (spec.md + GUI-free test_*.cpp added to a subsystem "
            "bundle's SOURCES, label `features;fast`)."),
            QStringLiteral("tests/features/README.md")});
        conv.append({QStringLiteral(
            "Validate at boundaries, not internally; don't write error "
            "paths for scenarios that can't happen at the call site."),
            QStringLiteral("docs/standards/coding.md")});
    } else if (taskType == QLatin1String("bugfix")) {
        conv.append({QStringLiteral(
            "TDD: write a failing test asserting the bug doesn't recur, "
            "confirm it fails on current code, then write the fix "
            "(test first, code second)."),
            QStringLiteral("docs/standards/testing.md")});
        conv.append({QStringLiteral(
            "Verify the test fails on pre-fix code (revert the fix, run, "
            "must FAIL) before locking it in."),
            QStringLiteral("docs/standards/testing.md")});
        conv.append({QStringLiteral(
            "Fix the root cause; no warning-silencing, `try/except: "
            "pass`, `--no-verify`, or disabling checks as a default."),
            QStringLiteral("docs/standards/coding.md")});
    } else if (taskType == QLatin1String("refactor")) {
        conv.append({QStringLiteral(
            "Pure refactors keep the existing tests passing — no "
            "behaviour change, no new test required."),
            QStringLiteral("docs/standards/testing.md")});
        conv.append({QStringLiteral(
            "Reuse before rewriting (call it / refactor-and-call / only "
            "then write new); extract a helper on the third call-site."),
            QStringLiteral("docs/standards/coding.md")});
    } else if (taskType == QLatin1String("docs")) {
        conv.append({QStringLiteral(
            "Keep CMakeLists.txt VERSION, CHANGELOG.md, and the "
            "README.md version line in lockstep (use the /bump recipe); "
            "completed ROADMAP items migrate to the matching CHANGELOG "
            "section."),
            QStringLiteral("CLAUDE.md")});
        conv.append({QStringLiteral(
            "Stable [PROJ-NNNN] IDs are append-only — never renumber, "
            "never reuse."),
            QStringLiteral("docs/standards/roadmap-format.md")});
        conv.append({QStringLiteral(
            "Render the labels you want spoken — Qt strips HTML "
            "aria/alt/role, so set accessibleName/Description on QWidget "
            "subclasses and emit inline text beside decorative emoji."),
            QStringLiteral("docs/standards/documentation.md")});
    } else {  // test
        conv.append({QStringLiteral(
            "Feature-conformance tests pair spec.md with a GUI-free C++ "
            "test under tests/features/<name>/, registered into a "
            "subsystem bundle (not a fresh add_executable unless process "
            "isolation is needed)."),
            QStringLiteral("tests/features/README.md")});
        conv.append({QStringLiteral(
            "Tests test the contract, not the implementation; anchor to "
            "external signals (spec/RFC/CVE), not source structure."),
            QStringLiteral("docs/standards/testing.md")});
        conv.append({QStringLiteral(
            "Audit-rule fixtures are count-based against fixture dirs, "
            "not line-number-based."),
            QStringLiteral("docs/standards/testing.md")});
    }

    QJsonObject result;
    result["ok"]        = true;
    result["task_type"] = taskType;

    QJsonArray cArr;
    QStringList srcOrder;
    for (const RS &rs : conv) {
        QJsonObject e;
        e["rule"]   = rs.first;
        e["source"] = rs.second;
        cArr.append(e);
        if (!srcOrder.contains(rs.second)) srcOrder.append(rs.second);
    }
    result["conventions"]       = cArr;
    result["conventions_count"] = cArr.size();

    QJsonArray sArr;
    for (const QString &s : srcOrder) {
        QJsonObject e;
        e["path"] = s;
        const QFileInfo fi(rootCanonical + QLatin1Char('/') + s);
        e["exists"] = fi.exists() && fi.isFile();
        sArr.append(e);
    }
    result["sources"]       = sArr;
    result["sources_count"] = sArr.size();
    return QJsonDocument(result);
}

// ----- ANTS-1302 — focused_test: run only the ctest subset touching
// the changed files. Pure resolution in FocusedTest; this runs ctest
// and parses with TestResCache. See docs/specs/ANTS-1302.md.

QJsonDocument RemoteControl::cmdFocusedTest(const QJsonObject &req) {
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) {
        return QJsonDocument(tpErr(QStringLiteral("no_project"),
            QStringLiteral("focused_test: project root unresolved")));
    }

    // Re-entrancy gate (mirrors m_verifyInFlight).
    if (m_focusedTestInFlight) {
        return QJsonDocument(tpErr(
            QStringLiteral("focused_test_in_flight"),
            QStringLiteral("focused_test: a previous call is still running")));
    }
    m_focusedTestInFlight = true;
    auto inFlightGuard = qScopeGuard([this]{ m_focusedTestInFlight = false; });

    // Build dir — default <root>/build, optional path-validated override.
    QString buildRel = QStringLiteral("build");
    if (req.contains(QStringLiteral("build_dir"))) {
        const auto pc = PathValidation::validatePath(
            req.value(QStringLiteral("build_dir")).toString(), root,
            QStringLiteral("focused_test"), QStringLiteral("build_dir"));
        if (pc.bad) return QJsonDocument(pc.err);
        buildRel = pc.argvForm;
    }
    const QString buildAbs = root + QLatin1Char('/') + buildRel;
    if (!QFileInfo(buildAbs).isDir() ||
        !QFileInfo(buildAbs + QStringLiteral("/CMakeCache.txt")).isFile()) {
        return QJsonDocument(tpErr(QStringLiteral("no_build_dir"),
            QStringLiteral("focused_test: \"%1\" is not a configured CMake "
                           "build dir (no CMakeCache.txt)").arg(buildRel)));
    }

    // Changed files — explicit array, else auto-derive from git status.
    QStringList changed;
    const bool haveArg = req.contains(QStringLiteral("changed_files"));
    if (haveArg) {
        const QJsonValue cfv = req.value(QStringLiteral("changed_files"));
        if (!cfv.isArray()) {
            return QJsonDocument(tpErr(QStringLiteral("bad_args"),
                QStringLiteral("focused_test: \"changed_files\" must be "
                               "an array of project-relative paths")));
        }
        for (const QJsonValue &v : cfv.toArray()) {
            QString s = v.toString().trimmed();
            while (s.startsWith(QStringLiteral("./"))) s = s.mid(2);
            if (!s.isEmpty() && !changed.contains(s)) changed.append(s);
        }
    }
    if (!haveArg) {
        QJsonObject gs;
        gs[QStringLiteral("caller_cwd")] = root;
        gs[QStringLiteral("op")]         = QStringLiteral("status");
        const QJsonObject gsObj = cmdGitState(gs).object();
        if (gsObj.value(QStringLiteral("ok")).toBool(false)) {
            for (const QJsonValue &v :
                 gsObj.value(QStringLiteral("files")).toArray()) {
                QString s = v.toObject()
                                .value(QStringLiteral("path"))
                                .toString()
                                .trimmed();
                while (s.startsWith(QStringLiteral("./"))) s = s.mid(2);
                if (!s.isEmpty() && !changed.contains(s)) changed.append(s);
            }
        }
    }

    const FocusedTest::CoverageMap map = FocusedTest::loadCoverageMap(root);
    const FocusedTest::Resolution res = FocusedTest::resolve(changed, map);

    int timeoutSec = 300;
    if (req.contains(QStringLiteral("timeout_sec"))) {
        timeoutSec = req.value(QStringLiteral("timeout_sec")).toInt(300);
    }
    if (timeoutSec < 10)   timeoutSec = 10;
    if (timeoutSec > 1800) timeoutSec = 1800;

    QElapsedTimer wall;
    wall.start();

    struct RunOut { bool started; bool timedOut; bool crashed; QString output; };
    auto runCtest = [&](const QString &regex) -> RunOut {
        QStringList argv;
        argv << QStringLiteral("--test-dir") << buildRel
             << QStringLiteral("--output-on-failure");
        // ANTS-3449 — run ctest in parallel. A broad change (e.g.
        // remotecontrol.cpp) resolves to a heuristic that matches 0 tests, so
        // the 0-match safety net re-runs the FULL suite; serially that is ~78 s
        // and outran the MCP transport read-timeout, surfacing as a spurious
        // -32000 even while ctest kept running. -j (capped at 4 — the CLAUDE.md
        // cap tuned for this 32 GiB / earlyoom host, and at the host core count
        // so a smaller machine isn't oversubscribed) brings the full suite to
        // ~19 s, comfortably inside the (ANTS-3444) 60 s bridge budget. Output
        // parsing is line-based, so interleaved parallel output is unaffected.
        const int jobs = qBound(1, QThread::idealThreadCount(), 4);
        argv << QStringLiteral("-j") << QString::number(jobs);
        if (!regex.isEmpty()) argv << QStringLiteral("-R") << regex;
        QProcess p;
        p.setWorkingDirectory(root);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(QStringLiteral("ctest"), argv);
        RunOut o{true, false, false, QString()};
        if (!p.waitForStarted(5000)) { o.started = false; return o; }
        if (!p.waitForFinished(timeoutSec * 1000)) {
            p.kill();
            p.waitForFinished(2000);
            o.timedOut = true;
        }
        if (!o.timedOut && p.exitStatus() != QProcess::NormalExit) {
            o.crashed = true;
        }
        QByteArray out = p.readAllStandardOutput();
        static const int kCap = 2 * 1024 * 1024;  // 2 MiB
        if (out.size() > kCap) out = out.right(kCap);
        o.output = QString::fromUtf8(out);
        return o;
    };

    bool isFull = (res.selection == FocusedTest::Selection::Full);
    QString filter = FocusedTest::buildCtestRegex(res.patterns);

    RunOut run = runCtest(isFull ? QString() : filter);
    if (!run.started) {
        return QJsonDocument(tpErr(QStringLiteral("ctest_missing"),
            QStringLiteral("focused_test: ctest binary not found on PATH")));
    }
    if (run.timedOut) {
        return QJsonDocument(tpErr(QStringLiteral("ctest_failed"),
            QStringLiteral("focused_test: ctest exceeded %1 s budget")
                .arg(timeoutSec)));
    }
    if (run.crashed) {
        return QJsonDocument(tpErr(QStringLiteral("ctest_failed"),
            QStringLiteral("focused_test: ctest crashed")));
    }

    TestResCache::ParsedTests parsed =
        TestResCache::parseCtestOutput(run.output);

    // 0-match safety net: a subset that matched no tests must never
    // report green — re-run the full suite.
    bool    downgraded = false;
    QString downgradeReason;
    if (!isFull && parsed.total == 0) {
        RunOut full = runCtest(QString());
        if (!full.started) {
            return QJsonDocument(tpErr(QStringLiteral("ctest_missing"),
                QStringLiteral("focused_test: ctest binary not found on PATH")));
        }
        if (full.timedOut) {
            return QJsonDocument(tpErr(QStringLiteral("ctest_failed"),
                QStringLiteral("focused_test: full-suite re-run exceeded "
                               "%1 s budget").arg(timeoutSec)));
        }
        if (full.crashed) {
            return QJsonDocument(tpErr(QStringLiteral("ctest_failed"),
                QStringLiteral("focused_test: full-suite re-run crashed")));
        }
        parsed          = TestResCache::parseCtestOutput(full.output);
        downgraded      = true;
        downgradeReason = QStringLiteral("selection matched 0 tests");
        isFull          = true;
        filter.clear();
    }

    if (!parsed.recognised) {
        return QJsonDocument(tpErr(QStringLiteral("unrecognised_output"),
            QStringLiteral("focused_test: ctest output not recognised "
                           "(no summary footer or per-test lines)")));
    }

    auto selToStr = [](FocusedTest::Selection s) -> QString {
        switch (s) {
            case FocusedTest::Selection::Map:       return QStringLiteral("map");
            case FocusedTest::Selection::Heuristic: return QStringLiteral("heuristic");
            case FocusedTest::Selection::Full:      return QStringLiteral("full");
        }
        return QStringLiteral("full");
    };

    QJsonObject env = TestResCache::toJsonWire(parsed);
    env[QStringLiteral("ok")]               = true;
    env[QStringLiteral("selection")]        =
        downgraded ? QStringLiteral("full") : selToStr(res.selection);
    env[QStringLiteral("ctest_filter")]     = isFull ? QString() : filter;
    auto toArr = [](const QStringList &xs) {
        QJsonArray a;
        for (const QString &s : xs) a.append(s);
        return a;
    };
    env[QStringLiteral("changed_files")]    = toArr(changed);
    env[QStringLiteral("mapped_files")]     = toArr(res.mappedFiles);
    env[QStringLiteral("unmapped_files")]   = toArr(res.unmappedFiles);
    env[QStringLiteral("ignored_files")]    = toArr(res.ignoredFiles);
    env[QStringLiteral("selection_reason")] = res.reason;
    env[QStringLiteral("build_dir")]        = buildRel;
    env[QStringLiteral("duration_ms")]      =
        static_cast<qint64>(wall.elapsed());
    if (downgraded) {
        env[QStringLiteral("downgraded_to_full")] = true;
        env[QStringLiteral("downgrade_reason")]   = downgradeReason;
    }
    return QJsonDocument(env);
}

// ----- ANTS-1303 — find_definition / find_caller symbol queries -----

namespace rcdetail {

QJsonObject sqArgErr(const QString &tool) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = tool + QStringLiteral(": \"symbol\" missing, empty, or "
                                       "not a valid identifier "
                                       "(^[A-Za-z_][A-Za-z0-9_]{0,127}$)");
    o["code"]  = QStringLiteral("bad_args");
    return o;
}

QJsonObject sqNoProject(const QString &tool) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = tool + QStringLiteral(": project root unresolved");
    o["code"]  = QStringLiteral("no_project");
    return o;
}

// Normalised `lang` echo for the envelope ("auto" when unset).
QString sqLangEcho(const QJsonObject &req) {
    const QString l = req.value(QStringLiteral("lang")).toString()
                          .trimmed().toLower();
    return l.isEmpty() ? QStringLiteral("auto") : l;
}

// Parse the shared `lang` + `max_results` opts from the request.
SymbolQuery::Options sqOptions(const QJsonObject &req) {
    SymbolQuery::Options opts;
    opts.lang = SymbolQuery::parseLang(
        req.value(QStringLiteral("lang")).toString().trimmed().toLower());
    const QJsonValue mr = req.value(QStringLiteral("max_results"));
    if (mr.isDouble()) {
        const int n = mr.toInt();
        if (n > 0) opts.maxResults = n;
    }
    // ANTS-3805 — find_caller's scope filter. Read here rather than in the
    // handler so it travels with the rest of the options; findDefinition
    // ignores it, which is deliberate — a definition search returns a handful
    // of rows and has never needed narrowing.
    opts.lane = req.value(QStringLiteral("lane")).toString().trimmed();
    return opts;
}

QJsonObject defMatchToJson(const SymbolQuery::DefMatch &d) {
    QJsonObject o;
    o["file"]      = d.file;
    o["line"]      = d.line;
    o["signature"] = d.signature;
    o["lang"]      = d.lang;
    o["kind"]      = d.kind;
    return o;
}

// ANTS-2087 — opt-in symbol body. Reuses read_region's symbol-body
// extractor (FileOutline-resolved range + head-anchored byte cap) so the
// "where is Foo and what does it do" question is one call, not a
// find_definition then read_region two-step. Silent no-op when the file's
// outline can't resolve the symbol (declaration-only match, overload
// ambiguity, generated file): the def is still returned, just without a
// body. `d.file` is project-relative; the abs path is root + '/' + file,
// trusted (it came from our own in-root scan), so no PathValidation here.
void sqAttachBody(QJsonObject &defJson, const QString &root,
                  const QString &symbol) {
    const QString rel = defJson.value(QStringLiteral("file")).toString();
    if (rel.isEmpty() || root.isEmpty()) return;
    ReadRegion::Options o;
    o.symbol = symbol;
    const QJsonObject r =
        ReadRegion::extract(root + QLatin1Char('/') + rel, o);
    if (!r.value(QStringLiteral("ok")).toBool()) return;
    if (r.value(QStringLiteral("symbol_ambiguous")).toBool()) return;
    const QJsonArray lines = r.value(QStringLiteral("lines")).toArray();
    if (lines.isEmpty()) return;
    QStringList ls;
    ls.reserve(lines.size());
    for (const QJsonValue &v : lines) ls << v.toString();
    defJson[QStringLiteral("body")]            = ls.join(QChar('\n'));
    defJson[QStringLiteral("body_start_line")] = r.value(QStringLiteral("start_line"));
    defJson[QStringLiteral("body_end_line")]   = r.value(QStringLiteral("end_line"));
    if (r.value(QStringLiteral("truncated")).toBool())
        defJson[QStringLiteral("body_truncated")] = true;
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdFindDefinition(const QJsonObject &req) {
    const QString symbol = req.value(QStringLiteral("symbol")).toString().trimmed();
    if (!SymbolQuery::isValidSymbol(symbol)) {
        return QJsonDocument(sqArgErr(QStringLiteral("find_definition")));
    }
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) {
        return QJsonDocument(sqNoProject(QStringLiteral("find_definition")));
    }

    const SymbolQuery::DefResult res =
        SymbolQuery::findDefinition(root, symbol, sqOptions(req));

    const bool includeBody =
        req.value(QStringLiteral("include_body")).toBool();
    QJsonArray defs;
    for (const SymbolQuery::DefMatch &d : res.definitions) {
        QJsonObject dj = defMatchToJson(d);
        if (includeBody) sqAttachBody(dj, root, symbol);
        defs.append(dj);
    }

    QJsonObject out;
    out["ok"]                = true;
    out["symbol"]            = symbol;
    out["lang"]              = sqLangEcho(req);
    out["definitions"]       = defs;
    out["definitions_count"] = res.definitionsTotal;
    out["files_scanned"]     = res.filesScanned;
    out["truncated"]         = res.truncated;
    out["walk_capped"]       = res.walkCapped;
    // ANTS-1950 — zero-symbol-but-matches-a-filename nudge. Surfaced only when
    // there are no definitions and the query equals a source file's stem, so a
    // caller asking for `test_reference_harness` (a filename, not a symbol)
    // gets pointed at the file instead of a bare empty result.
    if (!res.fileStemHint.isEmpty()) {
        out["file_stem_hint"] = res.fileStemHint;
        out["hint"] = QStringLiteral("no symbol named '%1'; did you mean the "
                                     "file '%2'?").arg(symbol, res.fileStemHint);
    }
    return QJsonDocument(out);
}

QJsonDocument RemoteControl::cmdFindCaller(const QJsonObject &req) {
    const QString symbol = req.value(QStringLiteral("symbol")).toString().trimmed();
    if (!SymbolQuery::isValidSymbol(symbol)) {
        return QJsonDocument(sqArgErr(QStringLiteral("find_caller")));
    }
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) {
        return QJsonDocument(sqNoProject(QStringLiteral("find_caller")));
    }

    const SymbolQuery::Options opts = sqOptions(req);
    const SymbolQuery::CallResult res =
        SymbolQuery::findCaller(root, symbol, opts);
    // ANTS-3805 — echoed on both emission paths below. Without it a scoped
    // result and a whole-project one are indistinguishable, which is the same
    // partial-reads-as-complete trap `truncated` exists for.
    const QString lane = opts.lane;

    // ANTS-3555 — files_only manifest mode: return the distinct set of files
    // that call the symbol (per-file call count + the exact line numbers) and
    // drop the quoted per-call `context` windows + the redundant per-row
    // `lang`. Those context lines are re-sent bytes when the caller opens the
    // file with read_region next; the manifest is what it actually needs to
    // navigate. Mirrors workspace_search's files_only (ANTS-3549) and
    // indie_review_brief's manifest-not-bodies posture (ANTS-1281). Placed
    // before the full callers[] loop so the context windows are never built.
    const bool filesOnly =
        req.value(QStringLiteral("files_only")).toBool(false);
    if (filesOnly) {
        QStringList order;               // first-seen file order (scan order)
        QHash<QString, int> idx;         // file -> index into the parallel vecs
        QVector<int> counts;
        QVector<QJsonArray> linesByFile;
        for (const SymbolQuery::CallMatch &c : res.callers) {
            const auto it = idx.constFind(c.file);
            int i;
            if (it == idx.constEnd()) {
                i = order.size();
                idx.insert(c.file, i);
                order.append(c.file);
                counts.append(0);
                linesByFile.append(QJsonArray());
            } else {
                i = it.value();
            }
            counts[i] += 1;
            linesByFile[i].append(c.line);
        }
        QJsonArray files;
        for (int i = 0; i < order.size(); ++i) {
            QJsonObject fe;
            fe["file"] = order.at(i);
            // `count` is the returned (possibly max_results-capped) call count
            // for this file and equals lines[].size(); top-level callers_count
            // carries the true pre-cap total.
            fe["count"] = counts.at(i);
            fe["lines"] = linesByFile.at(i);
            files.append(fe);
        }
        QJsonObject out;
        out["ok"]            = true;
        out["symbol"]        = symbol;
        out["lang"]          = sqLangEcho(req);
        out["files"]         = files;
        out["files_count"]   = static_cast<int>(order.size());
        out["callers_count"] = res.callersTotal;
        if (!lane.isEmpty()) out["lane"] = lane;   // ANTS-3805
        out["files_only"]    = true;
        if (res.definition.has_value()) {
            // Keep the "where + who" pairing: the definition is a single small
            // object (a body only when include_body is set), not a context
            // window, so it survives the manifest trim.
            QJsonObject dj = defMatchToJson(res.definition.value());
            if (req.value(QStringLiteral("include_body")).toBool())
                sqAttachBody(dj, root, symbol);
            out["definition"] = dj;
        }
        out["files_scanned"] = res.filesScanned;
        out["truncated"]     = res.truncated;
        out["walk_capped"]   = res.walkCapped;
        return QJsonDocument(out);
    }

    QJsonArray callers;
    for (const SymbolQuery::CallMatch &c : res.callers) {
        QJsonObject o;
        o["file"]    = c.file;
        o["line"]    = c.line;
        o["context"] = c.context;
        o["lang"]    = c.lang;
        callers.append(o);
    }

    QJsonObject out;
    out["ok"]            = true;
    out["symbol"]        = symbol;
    out["lang"]          = sqLangEcho(req);
    out["callers"]       = callers;
    out["callers_count"] = res.callersTotal;
    if (!lane.isEmpty()) out["lane"] = lane;   // ANTS-3805
    if (res.definition.has_value()) {
        // ANTS-2087 — body of the called symbol's definition (not the
        // call sites, which are already context lines).
        QJsonObject dj = defMatchToJson(res.definition.value());
        if (req.value(QStringLiteral("include_body")).toBool())
            sqAttachBody(dj, root, symbol);
        out["definition"] = dj;
    }
    out["files_scanned"] = res.filesScanned;
    out["truncated"]     = res.truncated;
    out["walk_capped"]   = res.walkCapped;
    return QJsonDocument(out);
}

// ----- ANTS-1305 — similar_code shape matcher -----

namespace rcdetail {

QJsonObject scArgErr() {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = QStringLiteral("similar_code: \"shape\" missing, empty, "
                                "longer than 512 chars, or has no usable "
                                "tokens");
    o["code"]  = QStringLiteral("bad_args");
    return o;
}

QJsonObject scNoProject() {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = QStringLiteral("similar_code: project root unresolved");
    o["code"]  = QStringLiteral("no_project");
    return o;
}

SimilarCode::Options scOptions(const QJsonObject &req) {
    SimilarCode::Options opts;
    opts.lang = SimilarCode::parseLang(
        req.value(QStringLiteral("lang")).toString().trimmed().toLower());
    const QJsonValue mr = req.value(QStringLiteral("max_results"));
    if (mr.isDouble()) {
        const int n = mr.toInt();
        if (n > 0) opts.maxResults = n;
    }
    return opts;
}

// ANTS-2156 — derive a read_region-resolvable symbol name from a
// similar_code match's signature line: strip a leading "ReturnType "
// (keep a Class::method qualifier), and for a class/struct take the name
// after the keyword. Empty when nothing parseable (caller falls back).
QString scSymbolFromSignature(const QString &signature, const QString &kind) {
    QString s = signature.trimmed();
    if (kind == QLatin1String("class")) {
        // "class Widget" / "struct Foo : Base" → "Widget" / "Foo".
        static const QRegularExpression rx(
            QStringLiteral("^(?:class|struct|namespace)\\s+([\\w:]+)"));
        const auto m = rx.match(s);
        return m.hasMatch() ? m.captured(1) : QString();
    }
    // func: name = the identifier (possibly Class::method) before '('.
    const int lp = s.indexOf(QLatin1Char('('));
    if (lp > 0) s = s.left(lp).trimmed();
    const int sp = s.lastIndexOf(QLatin1Char(' '));   // drop the return type
    if (sp >= 0) s = s.mid(sp + 1);
    // Trim leading * / & off a pointer/ref return (e.g. "*foo").
    while (!s.isEmpty() && (s.front() == QLatin1Char('*')
                            || s.front() == QLatin1Char('&')))
        s.remove(0, 1);
    return s;
}

}  // namespace rcdetail

QJsonDocument RemoteControl::cmdSimilarCode(const QJsonObject &req) {
    const QString shape = req.value(QStringLiteral("shape")).toString().trimmed();
    if (shape.isEmpty() || shape.size() > 512 ||
        SimilarCode::tokenize(shape).isEmpty()) {
        return QJsonDocument(scArgErr());
    }
    const QString root = resolveRootCanonical(m_main, req);
    if (root.isEmpty()) {
        return QJsonDocument(scNoProject());
    }

    const SimilarCode::Result res =
        SimilarCode::findSimilar(root, shape, scOptions(req));
    if (!res.ok) {  // defensive — lib re-validates the already-checked shape
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = res.error;
        o["code"]  = res.code;
        return QJsonDocument(o);
    }

    // ANTS-2156 — include_bodies returns the FULL enclosing definition for
    // each (already top-N, score-ranked) match, so a session copying an
    // in-repo idiom gets the complete exemplar in ONE call instead of N
    // follow-up Reads. Reuses ReadRegion's symbol-body extractor.
    const bool includeBodies =
        req.value(QStringLiteral("include_bodies")).toBool(false);

    QJsonArray matches;
    for (const SimilarCode::Match &m : res.matches) {
        QJsonObject o;
        o["file"]      = m.file;
        o["line"]      = m.line;
        o["signature"] = m.signature;
        o["kind"]      = m.kind;
        o["lang"]      = m.lang;
        o["score"]     = m.score;
        if (includeBodies) {
            const QString sym = scSymbolFromSignature(m.signature, m.kind);
            bool got = false;
            if (!sym.isEmpty()) {
                ReadRegion::Options ro;
                ro.symbol = sym;
                const QJsonObject body =
                    ReadRegion::extract(root + QLatin1Char('/') + m.file, ro);
                if (body.value(QStringLiteral("ok")).toBool(false)) {
                    o["symbol"]          = sym;
                    o["body"]            = body.value(QStringLiteral("lines"));
                    o["body_start_line"] = body.value(QStringLiteral("start_line"));
                    o["body_end_line"]   = body.value(QStringLiteral("end_line"));
                    o["body_truncated"]  = body.value(QStringLiteral("truncated"));
                    got = true;
                }
            }
            // Graceful fallback — the signature didn't resolve to an outline
            // symbol; the caller still has file:line to open it.
            if (!got) o["body_unavailable"] = true;
        }
        matches.append(o);
    }

    QJsonObject out;
    out["ok"]            = true;
    out["shape"]         = shape;
    out["lang"]          = sqLangEcho(req);
    out["matches"]       = matches;
    out["matches_count"] = res.matchesTotal;
    out["files_scanned"] = res.filesScanned;
    out["truncated"]     = res.truncated;
    out["walk_capped"]   = res.walkCapped;
    if (includeBodies) {
        out["bodies_note"] = QStringLiteral(
            "Full enclosing definitions for the top matches, ranked by "
            "structural similarity (score desc). Copy the idiom from these "
            "rather than opening each file.");
    }
    return QJsonDocument(out);
}

// ----- ANTS-1112 — five `indie_review_*` MCP-tool handlers ---------

namespace rcdetail {

QJsonObject irErr(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"]      = false;
    o["error"]   = message;
    o["code"]    = code;
    return o;
}

}  // namespace rcdetail

