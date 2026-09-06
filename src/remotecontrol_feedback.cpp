// ANTS-3833 TU 9/17 — Feedback and audit verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "build_info.h"    // ANTS-4741 — the stale-binary comparison
#include "guithread.h"
#include "feedbackfile.h"        // ANTS-1961 / ANTS-1962
#include "readlog.h"
#include "speclog.h"             // ANTS-1963
#include "gitwrap.h"
#include "claudeintegration.h"
#include "mainwindow.h"
#include "pathvalidation.h"
#include "projectsettings.h"    // ANTS-2160 — .ants/project.json overrides
#include "falseposledger.h"
#include "resolvedroot.h"
#include "terminalwidget.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include "auditfpledger.h"

using namespace rcdetail;  // ANTS-3833

// ANTS-3802 — the cross-repo id resolver, lifted out of cmdFeedbackQuery so
// compact_resolved uses the SAME one. Extracting it is the fix, not a tidy-up:
// a *_Ants_MCP_Feedback.md file is by construction written from one project
// ABOUT another project's tooling, so its ids are never the caller's. A verb
// that resolves only against caller_cwd's ROADMAP can therefore never collapse
// a shipped finding in the very file it exists to compact — it succeeds only
// when pointed at a file whose ids happen to be the caller's, which is the Ants
// Terminal maintainer case and nothing else.
//
// Cost-gated on a foreign id being present; RAM-bounded — at most ONE full
// roadmap parse per distinct foreign prefix, retaining only the mapped foreign
// ids, discarded after. No persistent cache (a rare triage-time path). The
// caller's own roadmap is skipped.
//
// Returns id → {status, resolved_from, shipped_date?}.
//
// ANTS-3793 — a member rather than a free function now, and only because of
// that: each sibling roadmap it opens belongs to a project that may itself be
// migrated, so the read goes through the owner wrapper, which needs a
// RemoteControl to hold the store connection. Nothing else about it changed.
QHash<QString, QJsonObject> RemoteControl::rlResolveForeignFeedbackIds(
    const QString &feedbackFilePath,     // its parent dir is the shared root
    const QString &callerRoadmapPath,    // skipped when encountered
    const QSet<QString> &callerPrefixes,
    const QStringList &wantedIds,
    const QSet<QString> &alreadyResolved) const {
    // An EMPTY callerPrefixes means "the caller owns no prefixes" — because it
    // has no roadmap, or none with ids — which makes every wanted id foreign
    // rather than none of them. The inherited form returned early here, and
    // that is precisely the branch a contributor project hits: no local
    // roadmap, every id another project's, nothing resolved.
    QHash<QString, QJsonObject> foreignResolved;

    static const QString kCheckF = QString::fromUtf8("\xE2\x9C\x85");  // ✅
    const auto idPrefixOf = [](const QString &id) {
        const int dash = id.lastIndexOf(QLatin1Char('-'));
        return dash > 0 ? id.left(dash) : id;
    };

    QSet<QString> neededForeignIds;
    QSet<QString> foreignPrefixes;
    for (const QString &id : wantedIds) {
        if (alreadyResolved.contains(id)) continue;
        const QString pfx = idPrefixOf(id);
        if (!callerPrefixes.contains(pfx)) {
            neededForeignIds.insert(id);
            foreignPrefixes.insert(pfx);
        }
    }
    if (foreignPrefixes.isEmpty())
        return foreignResolved;

    // ANTS-4905 — the owning project is a SIBLING of the feedback file, which
    // held while the corpus WAS the shared root. Move the corpus into a
    // directory of its own — which is what happens once a machine carries two
    // dozen feedback files — and that directory has no project subdirs at all,
    // so nothing resolves and every id falls back to foreign_repo. Measured
    // 2026-09-06 from ~/.claude: 64 of 64.
    //
    // So: the corpus directory first, then its parent. Ordered, because the
    // first hit for a prefix wins and the nearer directory is the better
    // guess; deduped, so the pre-move layout still scans one directory once.
    // Nothing is resolved from a project that does not OWN the prefix — the
    // head sniff below is unchanged and is what decides ownership.
    QStringList searchRoots;
    const QString corpusDir = QFileInfo(feedbackFilePath).absolutePath();
    searchRoots << corpusDir;
    const QString corpusParent = QFileInfo(corpusDir).absolutePath();
    if (!corpusParent.isEmpty() && corpusParent != corpusDir)
        searchRoots << corpusParent;

    QStringList subs;                 // absolute paths, in search order
    QSet<QString> seenSub;
    for (const QString &rootPath : searchRoots) {
        const QDir rootDir(rootPath);
        const QStringList names =
            rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &name : names) {
            const QString abs = rootDir.absoluteFilePath(name);
            if (seenSub.contains(abs)) continue;
            seenSub.insert(abs);
            subs << abs;
        }
    }
    for (const QString &sub : subs) {
        if (foreignPrefixes.isEmpty()) break;  // every prefix owned
        const QString subCanon = QFileInfo(sub).canonicalFilePath();
        const QString sibRoadmap =
            subCanon.isEmpty() ? QString() : findRoadmapUnder(subCanon);
        if (sibRoadmap.isEmpty() || sibRoadmap == callerRoadmapPath)
            continue;
        QFile sf(sibRoadmap);
        if (!sf.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        // Cheap ownership sniff from the file head — no full parse.
        const QByteArray head = sf.read(32 * 1024);
        const QString sibPrefix = rlDetectCounterPrefix(QString::fromUtf8(head));
        if (sibPrefix.isEmpty() || !foreignPrefixes.contains(sibPrefix)) {
            sf.close();
            continue;
        }
        sf.close();
        // ANTS-3863 — the sibling's text as a provider rather than a readAll().
        // This resolver wants ids, so a migrated sibling costs the detector's
        // window and not its 3 MiB body; the head sniff above already had its
        // own bounded read and is untouched.
        auto sibText = RoadmapSource::RoadmapText::fromFile(sibRoadmap);
        const QString sibLeaf = QFileInfo(subCanon).fileName();
        // ANTS-3793 — the sibling's own store when IT is migrated. A refusal
        // skips this sibling rather than parsing its markdown: INV-1's "served
        // neither" is a skip here, and this resolver is best-effort by design.
        RoadmapSource::ReadError sibWhy = RoadmapSource::ReadError::None;
        QString sibErr;
        const auto sibBullets = roadmapBullets(subCanon, sibText,
                                               /*includeArchive=*/false,
                                               &sibWhy, &sibErr);
        if (sibWhy != RoadmapSource::ReadError::None)
            continue;
        for (const auto &b : sibBullets) {
            if (b.id.isEmpty() || !neededForeignIds.contains(b.id))
                continue;
            QJsonObject fr;
            fr[QStringLiteral("status")]        = b.status;
            fr[QStringLiteral("resolved_from")] = sibLeaf;
            if (b.status == kCheckF) {
                const QString d = FeedbackFile::shipDateFromRoadmapBody(b.body);
                if (!d.isEmpty())
                    fr[QStringLiteral("shipped_date")] = d;
            }
            foreignResolved.insert(b.id, fr);  // later row wins
        }
        foreignPrefixes.remove(sibPrefix);
    }
    return foreignResolved;
}

// ANTS-1961 — feedback_query: return the un-triaged delta + mapped IDs.
QJsonDocument RemoteControl::cmdFeedbackQuery(const QJsonObject &req) {
    QString resolved;
    bool exists = false;
    bool derived = false;
    QJsonObject err;
    if (!resolveFeedbackPath(req, QStringLiteral("feedback_query"),
                             resolved, exists, err, &derived)) {
        return QJsonDocument(err);
    }
    if (!exists) {
        QJsonObject nf = fbNotFound(
            QStringLiteral("feedback_query: \"%1\" does not exist")
                .arg(resolved),
            resolved, feedbackCallerLeaf(req));
        // ANTS-4104 — "nobody has filed anything here yet" is the state EVERY
        // project starts in, and reporting it down the failure channel left a
        // caller — told to read the un-triaged tail before filing — deciding
        // whether the failure was real, with no way to tell "never filed" from
        // "the derived path is wrong" short of reading the hint prose. found
        // says it in a field that can be branched on; the candidates + hint that
        // made the old refusal useful are carried over unchanged. An EXPLICIT
        // path that does not resolve stays ok:false — that one is a caller
        // error, not an empty state. ANTS-3587 made the same call for
        // session_orient's absent ROADMAP.
        if (!derived) return QJsonDocument(nf);
        nf[QStringLiteral("ok")]     = true;
        nf[QStringLiteral("reason")] = nf.value(QStringLiteral("error")).toString();
        nf.remove(QStringLiteral("error"));
        nf.remove(QStringLiteral("code"));
        nf[QStringLiteral("path")]                   = resolved;
        nf[QStringLiteral("path_derived")]           = true;
        nf[QStringLiteral("found")]                  = false;
        nf[QStringLiteral("delta")]                  = QString();
        nf[QStringLiteral("delta_present")]          = false;
        nf[QStringLiteral("delta_line_count")]       = 0;
        nf[QStringLiteral("mapped_ids")]             = QJsonArray();
        nf[QStringLiteral("maintainer_block_count")] = 0;
        nf[QStringLiteral("truncated")]              = false;
        return QJsonDocument(nf);
    }
    QFile f(resolved);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QJsonDocument(fbErr(
            QStringLiteral("read_failed"),
            QStringLiteral("feedback_query: cannot open \"%1\"")
                .arg(resolved)));
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    const FeedbackFile::ParseResult pr = FeedbackFile::parse(content);

    // Byte cap on the emitted delta (default 512 KiB, 4 MiB ceiling —
    // ANTS-1855 read-tool defaults). Keep the HEAD of the delta.
    int maxBytes = req.value(QStringLiteral("max_bytes")).toInt(0);
    if (maxBytes <= 0) maxBytes = ReadLog::kDefaultBytesCap;
    if (maxBytes > ReadLog::kMaxBytesCeiling)
        maxBytes = ReadLog::kMaxBytesCeiling;

    QString delta = pr.delta;
    bool truncated = false;
    {
        const QByteArray utf8 = delta.toUtf8();
        if (utf8.size() > maxBytes) {
            // Keep whole UTF-8 code points: truncate the QString and
            // re-encode until it fits (cheap — only a few iterations).
            int keep = delta.size();
            while (keep > 0 &&
                   delta.left(keep).toUtf8().size() > maxBytes) {
                keep -= 64;
            }
            if (keep < 0) keep = 0;
            delta = delta.left(keep);
            truncated = true;
        }
    }

    QJsonObject out;
    out["ok"]                    = true;
    // ANTS-4104 — the field the never-filed envelope sets to false. Always
    // present on a success, so one branch covers both.
    out["found"]                 = true;
    out["path"]                  = resolved;
    if (derived) out["path_derived"] = true;  // ANTS-3376
    out["delta"]                 = delta;
    out["delta_present"]         = pr.deltaPresent;
    out["delta_line_count"]      = pr.deltaLineCount;
    out["delta_start_line"]      = pr.deltaStartLine;
    out["mapped_ids"]            = QJsonArray::fromStringList(pr.mappedIds);
    out["maintainer_block_count"] = pr.maintainerBlockCount;
    out["last_maintainer_line"]  = pr.lastMaintainerLine;
    out["truncated"]             = truncated;

    // ANTS-3478 — resolve each mapped id's LIVE status from the caller
    // project's ROADMAP.md, so the triager sees at a glance which assigned
    // findings are 📋 / 🚧 / ✅ without a second roadmap_query per id. Cost-
    // gated on a non-empty mapped_ids (a fresh / untriaged file reads nothing);
    // parsed ONCE into an id→status map (not one parse per id). An id absent
    // from the live roadmap renders "unknown" (it may have archived to
    // CHANGELOG / docs/roadmap) — never silently "shipped". Best-effort: an
    // absent/unreadable roadmap leaves every id "unknown" rather than failing
    // the read.
    if (!pr.mappedIds.isEmpty()) {
        static const QString kCheckQ = QString::fromUtf8("\xE2\x9C\x85");  // ✅
        QHash<QString, QString> idToStatus;
        QHash<QString, QString> idToShipDate;   // ANTS-3504 — ✅ ids' ship-date
        // ANTS-3518 — the caller roadmap's own id-prefixes, so a mapped id
        // whose prefix is absent here can be flagged cross-repo ("foreign_repo")
        // rather than the ambiguous "unknown".
        QSet<QString> callerPrefixes;
        const auto idPrefix = [](const QString &id) {
            const int dash = id.lastIndexOf(QLatin1Char('-'));
            return dash > 0 ? id.left(dash) : id;
        };
        const QString callerRaw =
            req.value(QStringLiteral("caller_cwd")).toString();
        const QString callerCanonical = callerRaw.isEmpty()
            ? QString() : QFileInfo(callerRaw).canonicalFilePath();
        const QString roadmapPath = callerCanonical.isEmpty()
            ? QString() : findRoadmapUnder(callerCanonical);
        if (!roadmapPath.isEmpty()) {
            auto rmText = RoadmapSource::RoadmapText::fromFile(roadmapPath);
            // ANTS-3863 — openFailed() forces the open, so this guard is the
            // QFile::open() one it replaces, in the same place.
            if (!rmText.openFailed()) {
                // ANTS-3793 — the caller project's store when it is migrated.
                // Best-effort like the rest of this block: a refusal leaves
                // every mapped id "unknown", which is what an unreadable
                // roadmap already produces, and never falls back to markdown.
                RoadmapSource::ReadError srcWhy =
                    RoadmapSource::ReadError::None;
                QString srcErr;
                const auto rmBullets = roadmapBullets(callerCanonical,
                                                      rmText,
                                                      /*includeArchive=*/false,
                                                      &srcWhy, &srcErr);
                for (const auto &b : rmBullets) {
                    if (b.id.isEmpty() || !RoadmapIndex::isCanonicalId(b.id))
                        continue;
                    idToStatus.insert(b.id, b.status);  // later wins (live row)
                    callerPrefixes.insert(idPrefix(b.id));  // ANTS-3518
                    // ANTS-3504 — capture the ship-date for ✅ ids (shared
                    // extractor with compact_resolved) so a contributor can
                    // compare it against their server_build.build_date.
                    if (b.status == kCheckQ) {
                        const QString d =
                            FeedbackFile::shipDateFromRoadmapBody(b.body);
                        if (!d.isEmpty()) idToShipDate.insert(b.id, d);
                        else idToShipDate.remove(b.id);
                    }
                }
            }
        }
        // ANTS-3519 — resolve foreign-prefix mapped ids (absent from the
        // caller roadmap, prefix not among its own) against the sibling
        // project whose roadmap OWNS that prefix. Feedback files live at the
        // shared root; the owning roadmap (e.g. Ants Terminal for ANTS-*) is a
        // sibling subdir. Cost-gated on a foreign id being present; RAM-bounded
        // — at most ONE full roadmap parse per distinct foreign prefix,
        // retaining only the mapped foreign ids, discarded after. No persistent
        // cache (a rare triage-time path). The caller roadmap itself is skipped.
        // ANTS-3802 — now a shared helper, so compact_resolved resolves ids the
        // same way this verb does. The two disagreeing about which ROADMAP owns
        // an id was the defect.
        QSet<QString> alreadyResolved;
        for (auto it = idToStatus.constBegin(); it != idToStatus.constEnd(); ++it)
            alreadyResolved.insert(it.key());
        const QHash<QString, QJsonObject> foreignResolved =
            rlResolveForeignFeedbackIds(resolved, roadmapPath, callerPrefixes,
                                        pr.mappedIds, alreadyResolved);

        QJsonArray statusArr;
        bool anyForeign = false;
        for (const QString &id : pr.mappedIds) {
            QJsonObject o;
            QString status = idToStatus.value(id, QString());
            if (status.isEmpty()) {
                // ANTS-3518 — an id absent from the caller roadmap is either
                // archived (same repo, migrated to CHANGELOG) or cross-repo.
                // Feedback files are authored in consumer projects but cite
                // ANTS ids that live in the Ants Terminal roadmap, so resolving
                // against the caller roadmap always misses them. When the id's
                // prefix isn't among the caller roadmap's own prefixes, mark it
                // "foreign_repo" (not "unknown") so a consumer session doesn't
                // misread a shipped suggestion as never-shipped. (finbreak
                // 2026-07-14.) The prefix set is empty when the caller roadmap
                // was unreadable — fall back to "unknown" there.
                if (foreignResolved.contains(id)) {
                    // ANTS-3519 — resolved from the owning sibling roadmap.
                    status = foreignResolved.value(id)
                                 .value(QStringLiteral("status")).toString();
                } else if (!callerPrefixes.isEmpty() &&
                    !callerPrefixes.contains(idPrefix(id))) {
                    status = QStringLiteral("foreign_repo");
                    anyForeign = true;
                } else {
                    status = QStringLiteral("unknown");
                }
            }
            o[QStringLiteral("id")]     = id;
            o[QStringLiteral("status")] = status;
            // ANTS-3519 — a cross-repo resolved id carries resolved_from (+ its
            // ship date if ✅). ANTS-3504 — otherwise the caller-roadmap ship
            // date, present only on ✅ ids with a parseable Resolved date
            // (never on non-✅ / dateless ids; never fabricated).
            if (foreignResolved.contains(id)) {
                const QJsonObject fr = foreignResolved.value(id);
                o[QStringLiteral("resolved_from")] =
                    fr.value(QStringLiteral("resolved_from"));
                if (fr.contains(QStringLiteral("shipped_date")))
                    o[QStringLiteral("shipped_date")] =
                        fr.value(QStringLiteral("shipped_date"));
            } else if (status == kCheckQ && idToShipDate.contains(id)) {
                o[QStringLiteral("shipped_date")] = idToShipDate.value(id);
            }
            statusArr.append(o);
        }
        // ANTS-4741 — do the stale-binary comparison rather than describe it.
        //
        // This verb already carries `shipped_date`, and its own description
        // told the caller to fetch session_orient's `server_build.build_date`
        // and compare. Both operands are server-side at reply time, so the
        // second call bought nothing — and the guidance lived on a different
        // verb from the one surfacing the ship date.
        //
        // SAME-DAY SETS THE FLAG. `shipped_date` has no time component, so a
        // fix that shipped after the running build was made is indistinguishable
        // from one that shipped before it when the dates match — and same-day
        // is the case most likely to be misread. Flagging it is the safe
        // direction: the cost of a false flag is one extra check, the cost of a
        // missed one is a maintainer's triage cycle on a fix that already
        // shipped.
        const QString buildDate = QString::fromLatin1(ANTS_BUILD_DATE);
        bool anyStale = false;
        for (QJsonValueRef v : statusArr) {
            QJsonObject o = v.toObject();
            const QString shipped =
                o.value(QStringLiteral("shipped_date")).toString();
            if (shipped.isEmpty() || buildDate.isEmpty()) continue;
            if (shipped >= buildDate) {   // ISO dates compare lexically
                o[QStringLiteral("possibly_stale_binary")] = true;
                anyStale = true;
                v = o;
            }
        }
        out["mapped_id_status"] = statusArr;
        if (anyStale) {
            out[QStringLiteral("server_build_date")]   = buildDate;
            out[QStringLiteral("server_build_commit")] =
                QString::fromLatin1(ANTS_BUILD_COMMIT);
            out[QStringLiteral("possibly_stale_binary_hint")] = QStringLiteral(
                "one or more ids shipped on or after this server binary was "
                "built (%1), so the running MCP does NOT necessarily carry "
                "their fix. Verify against a relaunched build before "
                "re-reporting any of them as still broken — a same-day ship "
                "sets this flag, because shipped_date has no time component to "
                "compare against the build time.").arg(buildDate);
        }
        if (anyForeign)
            out[QStringLiteral("mapped_id_status_note")] = QStringLiteral(
                "Some mapped ids belong to a different project's roadmap "
                "(their id-prefix is absent from this project's ROADMAP.md) and "
                "are marked \"foreign_repo\" rather than resolved. ANTS-* ids "
                "live in the Ants Terminal roadmap — check there for live "
                "status.");
    }

    // ANTS-3448 — marker-aware v2 delta. `format_version` (0/1/2/…) lets a
    // caller see which rule produced the delta; `suspected_untagged[]` lists
    // v2 `### ` finding-shaped blocks a hand editor left with no
    // `**Proposed ID:**` line (empty on v1 / a clean v2 file). Both additive.
    out["format_version"]        = pr.formatVersion;
    // ANTS-4702 — a version this build does not define is a fact the caller
    // can act on, not something to widen past in silence. The delta rule is
    // "v2 or higher", so such a file parses and every verb works; the marker
    // is still wrong, and the failure would land on whichever verb first
    // gates on an exact version, far from the file that caused it. Rides the
    // true arm only, like every other diagnostic here. Same shape as
    // spec_lint's skipped[] and doc_integrity's *_suppressed counters: a
    // widening the caller cannot see reads as a clean pass.
    if (pr.formatVersion > FeedbackFile::kMaxKnownFormatVersion) {
        out["format_version_unrecognised"] = true;
        out["format_version_hint"] = QStringLiteral(
            "this file declares format version %1; this build defines up to "
            "%2 (see docs/standards/mcp-feedback-files.md). It was read under "
            "the v2 rule, which is what \"v2 or higher\" means — the delta is "
            "sound, the marker is not.")
                .arg(pr.formatVersion)
                .arg(FeedbackFile::kMaxKnownFormatVersion);
    }
    QJsonArray suspected;
    for (const FeedbackFile::SuspectedFinding &sf : pr.suspectedUntagged) {
        QJsonObject o;
        o[QStringLiteral("heading")] = sf.heading;
        o[QStringLiteral("line")]    = sf.line;
        suspected.append(o);
    }
    out["suspected_untagged"]    = suspected;

    // ANTS-3631 — the awaiting subset. ALWAYS emitted, empty array included,
    // for the reason `sections_checked` is never omitted when false: a caller
    // cannot tell an absent key from "no outstanding questions", and this one
    // is the maintainer's outstanding-question list. It is a SUBSET of the
    // delta, not a partition of it — an awaiting finding appears in both,
    // because the reporter must see the question and the maintainer must see
    // that it is still unanswered.
    QJsonArray awaiting;
    for (const FeedbackFile::AwaitingFinding &af : pr.awaiting) {
        QJsonObject o;
        o[QStringLiteral("heading")]  = af.heading;
        o[QStringLiteral("line")]     = af.line;
        o[QStringLiteral("question")] = af.question;
        awaiting.append(o);
    }
    out["awaiting"]              = awaiting;

    // ANTS-3371 — opt-in maintainer tracking rows. The recurring
    // "mark my prior suggestions that shipped" workflow needs per-item
    // status, which `mapped_ids` (a flat ID list) can't give. When
    // include_tracking is set, surface every maintainer-table data row
    // [{item, ids, status, notes?}] in document order across all
    // maintainer blocks (later rows supersede earlier ones for the same
    // ID) — saves a full-file Read + table hand-parse.
    if (req.value(QStringLiteral("include_tracking")).toBool(false)) {
        QJsonArray tracking;
        for (const FeedbackFile::TrackingRow &tr : pr.trackingRows) {
            QJsonObject o;
            o[QStringLiteral("item")]   = tr.item;
            o[QStringLiteral("ids")]    = QJsonArray::fromStringList(tr.ids);
            o[QStringLiteral("status")] = tr.status;
            if (!tr.notes.isEmpty()) o[QStringLiteral("notes")] = tr.notes;
            tracking.append(o);
        }
        out[QStringLiteral("tracking")] = tracking;
    }
    // The `etag` field is injected centrally by the dispatch site
    // (ClaudeIntegration::applyEtagPattern) for tools listed in
    // isEtagSupportedTool — it is the sha256 of this envelope, which
    // changes iff the file content (and thus the delta) changes.
    return QJsonDocument(out);
}

// ANTS-1962 — test seam for the atomic write in cmdFeedbackLog.
static bool g_forceFeedbackWriteFail = false;
void RemoteControl::setForceFeedbackWriteFailForTest(bool on) {
    g_forceFeedbackWriteFail = on;
}

// ANTS-1962 — feedback_log: append a contributor or maintainer block.
// ANTS-4671 — the disposition composition, extracted so op:"assign_id" and
// op:"assign_id_batch" cannot answer one assignment differently. Every rule
// here was previously inline in assign_id and is unchanged: exactly one of
// ids / closure / awaiting, the ^ANTS-[0-9]+$ id gate with first-occurrence
// de-duplication, and the control-character fold that keeps the written line
// single-line.
//
// Returns true on success. On failure it fills *err with the bad_args
// envelope the single op used to return directly, so the batch can put that
// same envelope into skipped[] rather than inventing a second wording.
static bool fbComposeAssignValue(const QJsonObject &a, QString *value,
                                 bool *isClosure, QString *note,
                                 QJsonObject *err) {
    const QJsonArray idsArr = a.value(QStringLiteral("ids")).toArray();
    const bool hasIds     = !idsArr.isEmpty();
    const bool hasClosure = a.contains(QStringLiteral("closure")) &&
                            a.value(QStringLiteral("closure")).isString();
    const bool hasAwaiting = a.contains(QStringLiteral("awaiting")) &&
                             a.value(QStringLiteral("awaiting")).isString();
    const int dispositions = int(hasIds) + int(hasClosure) + int(hasAwaiting);
    if (dispositions != 1) {
        *err = fbErr(
            QStringLiteral("bad_args"),
            QStringLiteral("feedback_log: assign_id needs exactly one of a "
                           "non-empty \"ids\" array, a \"closure\" string, "
                           "or an \"awaiting\" question string (got %1)")
                .arg(dispositions));
        return false;
    }

    if (hasIds) {
        static const QRegularExpression idOkRe(QStringLiteral("^ANTS-[0-9]+$"));
        QStringList idList;   // de-duplicated, keeping first occurrence
        for (const QJsonValue &v : idsArr) {
            const QString id = v.toString().trimmed();
            if (!idOkRe.match(id).hasMatch()) {
                *err = fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: assign_id \"ids\" entries "
                                   "must match ^ANTS-[0-9]+$ (got \"%1\")")
                        .arg(id));
                return false;
            }
            if (!idList.contains(id)) idList.append(id);
        }
        *value = idList.join(QStringLiteral(", "));
    } else if (hasAwaiting) {
        QString q = a.value(QStringLiteral("awaiting")).toString();
        static const QRegularExpression awCtrlRe(QStringLiteral("[\\x00-\\x1F]"));
        q.replace(awCtrlRe, QStringLiteral(" "));
        q = q.trimmed();
        *value = q.isEmpty()
                     ? QStringLiteral("_(awaiting reporter)_")
                     : QString::fromUtf8("_(awaiting reporter \xE2\x80\x94 ")
                           + q + QStringLiteral(")_");
    } else {
        QString reason = a.value(QStringLiteral("closure")).toString();
        static const QRegularExpression ctrlRe(QStringLiteral("[\\x00-\\x1F]"));
        reason.replace(ctrlRe, QStringLiteral(" "));
        reason = reason.trimmed();
        *value = reason.isEmpty()
                     ? QStringLiteral("n/a")
                     : QString::fromUtf8("n/a \xE2\x80\x94 ") + reason;
    }
    *isClosure = hasClosure;

    QString n = a.value(QStringLiteral("note")).toString();
    static const QRegularExpression noteCtrlRe(QStringLiteral("[\\x00-\\x1F]"));
    n.replace(noteCtrlRe, QStringLiteral(" "));
    *note = n.trimmed();
    return true;
}

QJsonDocument RemoteControl::cmdFeedbackLog(const QJsonObject &req) {
    QString resolved;
    bool exists = false;
    bool derived = false;
    QJsonObject err;
    if (!resolveFeedbackPath(req, QStringLiteral("feedback_log"),
                             resolved, exists, err, &derived)) {
        return QJsonDocument(err);
    }

    const QString op = req.value(QStringLiteral("op")).toString();
    if (op != QStringLiteral("append_finding") &&
        op != QStringLiteral("append_tracking") &&
        op != QStringLiteral("compact_shipped") &&
        op != QStringLiteral("prune_tracking") &&
        op != QStringLiteral("compact_resolved") &&
        op != QStringLiteral("migrate_v2") &&
        op != QStringLiteral("assign_id") &&
        op != QStringLiteral("assign_id_batch") &&   // ANTS-4671
        op != QStringLiteral("set_format_version") &&   // ANTS-4707
        op != QStringLiteral("set_title")) {
        return QJsonDocument(fbErr(
            QStringLiteral("bad_mode"),
            QStringLiteral("feedback_log: op must be \"append_finding\", "
                           "\"append_tracking\", \"compact_shipped\", "
                           "\"prune_tracking\", \"compact_resolved\", "
                           "\"migrate_v2\", \"assign_id\", "
                           "\"assign_id_batch\", "
                           "\"set_format_version\" or "
                           "\"set_title\"")));
    }

    // ANTS-3421 — maintainer compaction: collapse confirmed-shipped
    // contributor blocks to a one-line stub. Distinct request shape (targets[]
    // + batch outcomes + in-place rewrite), so it returns early before the
    // append-specific date/render path below.
    if (op == QStringLiteral("compact_shipped")) {
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: compact_shipped on an absent "
                               "file (nothing to compact)"),
                resolved, feedbackCallerLeaf(req)));
        }
        const QJsonArray targetsArr =
            req.value(QStringLiteral("targets")).toArray();
        if (targetsArr.isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: compact_shipped requires a "
                               "non-empty \"targets\" array")));
        }
        // Project token (basename minus suffix) — the per-target `session`
        // default; today for `date`.
        QString projectToken = QFileInfo(resolved).fileName();
        projectToken.chop(static_cast<int>(qstrlen(kFeedbackSuffix)));
        const QString today =
            QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
        static const QRegularExpression compactIdRe(
            QStringLiteral("^ANTS-[0-9]+$"));
        static const QRegularExpression compactDateRe(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
        QVector<FeedbackFile::CompactTarget> targets;
        for (const QJsonValue &v : targetsArr) {
            const QJsonObject to = v.toObject();
            FeedbackFile::CompactTarget tg;
            tg.heading = to.value(QStringLiteral("heading")).toString();
            if (tg.heading.trimmed().isEmpty()) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: every target needs a "
                                   "non-empty \"heading\"")));
            }
            tg.id = to.value(QStringLiteral("id")).toString();
            if (!compactIdRe.match(tg.id).hasMatch()) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: each target \"id\" must "
                                   "match ANTS-NNNN (got \"%1\")").arg(tg.id)));
            }
            if (to.contains(QStringLiteral("heading_line")))
                tg.headingLine =
                    to.value(QStringLiteral("heading_line")).toInt(-1);
            tg.session =
                to.value(QStringLiteral("session")).toString(projectToken);
            if (tg.session.isEmpty()) tg.session = projectToken;
            tg.date = to.value(QStringLiteral("date")).toString(today);
            if (tg.date.isEmpty()) tg.date = today;
            if (!compactDateRe.match(tg.date).hasMatch()) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: target \"date\" must be "
                                   "YYYY-MM-DD (got \"%1\")").arg(tg.date)));
            }
            targets.append(tg);
        }

        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        const FeedbackFile::CompactResult cr =
            FeedbackFile::compactShipped(content, targets);
        const bool dryRunC = req.value(QStringLiteral("dry_run")).toBool();

        QJsonArray outcomes;
        QJsonArray skipped;
        int appliedCount = 0;
        for (const FeedbackFile::CompactOutcome &o : cr.results) {
            if (o.applied) {
                ++appliedCount;
                QJsonObject e;
                e[QStringLiteral("heading")]      = o.heading;
                e[QStringLiteral("id")]           = o.id;
                e[QStringLiteral("applied")]      = true;
                e[QStringLiteral("start_line")]   = o.startLine;
                e[QStringLiteral("end_line")]     = o.endLine;
                e[QStringLiteral("bytes_before")] = o.bytesBefore;
                e[QStringLiteral("bytes_after")]  = o.bytesAfter;
                e[QStringLiteral("bytes_saved")]  =
                    o.bytesBefore - o.bytesAfter;
                outcomes.append(e);
            } else {
                QJsonObject e;
                e[QStringLiteral("heading")] = o.heading;
                e[QStringLiteral("id")]      = o.id;
                e[QStringLiteral("code")]    = o.code;
                e[QStringLiteral("reason")]  = o.reason;
                if (!o.candidates.isEmpty()) {
                    QJsonArray cands;
                    for (int c : o.candidates) cands.append(c);
                    e[QStringLiteral("candidates")] = cands;
                }
                skipped.append(e);
            }
        }

        // Only write when something applied and this is not a dry run.
        if (!dryRunC && appliedCount > 0) {
            const QByteArray utf8 = cr.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }

        QJsonObject out;
        out[QStringLiteral("ok")]            = true;
        out[QStringLiteral("op")]            = op;
        out[QStringLiteral("path")]          = resolved;
        out[QStringLiteral("dry_run")]       = dryRunC;
        out[QStringLiteral("applied_count")] = appliedCount;
        out[QStringLiteral("bytes_saved")]   =
            static_cast<qint64>(cr.bytesSaved);
        out[QStringLiteral("outcomes")]      = outcomes;
        out[QStringLiteral("skipped")]       = skipped;
        if (derived) out[QStringLiteral("path_derived")] = true;  // ANTS-3376
        return QJsonDocument(out);
    }

    // ANTS-3442 — maintainer row-dedup: remove superseded duplicate tracking
    // rows (keep the authoritative last-per-id row). Distinct request shape
    // (scope_ids + removed[] + in-place rewrite), returns early like
    // compact_shipped.
    if (op == QStringLiteral("prune_tracking")) {
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: prune_tracking on an absent "
                               "file (nothing to prune)"),
                resolved, feedbackCallerLeaf(req)));
        }
        FeedbackFile::PruneOptions opts;
        static const QRegularExpression pruneIdRe(
            QStringLiteral("^ANTS-[0-9]+$"));
        if (req.contains(QStringLiteral("scope_ids"))) {
            const QJsonArray sc =
                req.value(QStringLiteral("scope_ids")).toArray();
            if (sc.isEmpty()) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: prune_tracking \"scope_ids\" "
                                   "must be non-empty (omit it to prune every "
                                   "superseded row)")));
            }
            for (const QJsonValue &v : sc) {
                const QString id = v.toString();
                if (!pruneIdRe.match(id).hasMatch()) {
                    return QJsonDocument(fbErr(
                        QStringLiteral("bad_args"),
                        QStringLiteral("feedback_log: prune_tracking scope_ids "
                                       "element must match ANTS-NNNN (got "
                                       "\"%1\")").arg(id)));
                }
                opts.scopeIds.append(id);
            }
        }

        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        const FeedbackFile::PruneResult prn =
            FeedbackFile::pruneTracking(content, opts);
        const bool dryRunP = req.value(QStringLiteral("dry_run")).toBool();

        QJsonArray removed;
        for (const FeedbackFile::PrunedRow &r : prn.removed) {
            QJsonObject e;
            QJsonArray ids;
            for (const QString &id : r.ids) ids.append(id);
            e[QStringLiteral("ids")]    = ids;
            e[QStringLiteral("status")] = r.status;
            e[QStringLiteral("line")]   = r.line;
            removed.append(e);
        }
        const int rowsRemoved = static_cast<int>(prn.removed.size());

        if (!dryRunP && rowsRemoved > 0) {
            const QByteArray utf8 = prn.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }

        QJsonObject out;
        out[QStringLiteral("ok")]           = true;
        out[QStringLiteral("op")]           = op;
        out[QStringLiteral("path")]         = resolved;
        out[QStringLiteral("dry_run")]      = dryRunP;
        out[QStringLiteral("rows_removed")] = rowsRemoved;
        out[QStringLiteral("bytes_saved")]  =
            static_cast<qint64>(prn.bytesSaved);
        out[QStringLiteral("removed")]      = removed;
        if (!opts.scopeIds.isEmpty()) {
            QJsonArray sc;
            for (const QString &id : opts.scopeIds) sc.append(id);
            out[QStringLiteral("scope_ids")] = sc;
        } else {
            out[QStringLiteral("scope_ids")] = QJsonValue(QJsonValue::Null);
        }
        if (derived) out[QStringLiteral("path_derived")] = true;  // ANTS-3376
        return QJsonDocument(out);
    }

    // ANTS-3443 — maintainer v2 compaction: collapse each shipped finding's
    // write-up to a roadmap-driven stub. Distinct request shape (auto-discovery
    // + collapsed[]/skipped[] + in-place rewrite), returns early like the two
    // v1 compaction ops above. It additionally reads ROADMAP.md (via the caller
    // project) to resolve each finding's assigned id to its live status.
    if (op == QStringLiteral("compact_resolved")) {
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: compact_resolved on an absent "
                               "file (nothing to compact)"),
                resolved, feedbackCallerLeaf(req)));
        }
        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        // Version gate — v2 or later (spec § 2.4). A v1 file's findings predate
        // the structural `**Proposed ID:**` line, so none would be recognised as
        // a finding; refuse and direct the caller to op:migrate_v2.
        // ANTS-3621 — the floor is `< 2`, NOT `!= 2`. The parser gates on
        // `formatVersion >= 2` (feedbackfile.cpp:142), so a future v3+ file is
        // read with v2 inline-ID semantics; an `!= 2` gate here refused it as
        // `not_v2` and told the caller to run migrate_v2 — which is a no-op on
        // an already-migrated file, leaving no way forward. Keep the two
        // version predicates agreeing.
        static const QRegularExpression markerRe(
            QStringLiteral("<!--\\s*ants-mcp-feedback:\\s*([0-9]+)\\s*-->"));
        const auto mm = markerRe.match(content);
        const int ver = mm.hasMatch() ? mm.captured(1).toInt() : 0;
        if (ver < 2) {
            return QJsonDocument(fbErr(
                QStringLiteral("not_v2"),
                QStringLiteral("feedback_log: compact_resolved requires a v2 or "
                               "later file (<!-- ants-mcp-feedback: 2 -->); this "
                               "file is v%1 — run op:migrate_v2 first").arg(ver)));
        }

        // Roadmap resolution (spec § 2.3): build shippedIds (status ✅) +
        // roadmapIds (every canonical id, any status) from the caller
        // project's ROADMAP.md. Absent/unreadable roadmap ⟹ roadmap_unavailable
        // (an unknowable "is it ✅?"); a readable-but-empty roadmap is fine
        // (every finding then reads roadmap_unresolved_ids — the safe no-op).
        const QString callerRaw =
            req.value(QStringLiteral("caller_cwd")).toString();
        const QString callerCanonical = callerRaw.isEmpty()
            ? QString() : QFileInfo(callerRaw).canonicalFilePath();
        const QString roadmapPath = callerCanonical.isEmpty()
            ? QString() : findRoadmapUnder(callerCanonical);
        FeedbackFile::ResolveOptions ropts;
        static const QString kCheck = QString::fromUtf8("\xE2\x9C\x85");  // ✅
        QSet<QString> callerPrefixes;
        const auto idPrefixOfC = [](const QString &id) {
            const int dash = id.lastIndexOf(QLatin1Char('-'));
            return dash > 0 ? id.left(dash) : id;
        };

        // The caller's own roadmap, when it has one. ANTS-3802 — an ABSENT one
        // is no longer fatal: this file's ids are by construction another
        // project's, so the cross-repo pass below is the path that matters and
        // refusing before reaching it is what made this verb a no-op.
        if (!roadmapPath.isEmpty()) {
            auto rmText = RoadmapSource::RoadmapText::fromFile(roadmapPath);
            // ANTS-3863 — openFailed() forces the open, so this guard is the
            // QFile::open() one it replaces, in the same place.
            if (!rmText.openFailed()) {
                // ANTS-3793 — the caller project's store when it is migrated.
                // A refusal leaves the id sets empty, which this verb already
                // handles (every finding reads roadmap_unresolved_ids); it
                // never reads markdown behind the store's back.
                RoadmapSource::ReadError srcWhy =
                    RoadmapSource::ReadError::None;
                QString srcErr;
                const auto rmBullets = roadmapBullets(callerCanonical,
                                                      rmText,
                                                      /*includeArchive=*/false,
                                                      &srcWhy, &srcErr);
                for (const auto &b : rmBullets) {
                    if (b.id.isEmpty() || !RoadmapIndex::isCanonicalId(b.id)) continue;
                    ropts.roadmapIds.insert(b.id);
                    callerPrefixes.insert(idPrefixOfC(b.id));
                    if (b.status == kCheck) {
                        ropts.shippedIds.insert(b.id);
                        // ANTS-3504 — capture the ship-date from the same pass so
                        // the collapse stub can carry it (shared extractor with
                        // feedback_query).
                        const QString shipDate =
                            FeedbackFile::shipDateFromRoadmapBody(b.body);
                        if (!shipDate.isEmpty()) ropts.shipDates.insert(b.id, shipDate);
                    }
                }
            }
        }

        // ANTS-3802 — the cross-repo pass, through the SAME resolver
        // feedback_query uses. Without it this verb could only ever collapse a
        // finding whose id happened to live in the caller's own roadmap, which
        // for a *_Ants_MCP_Feedback.md file is close to never.
        const FeedbackFile::ParseResult prC = FeedbackFile::parse(content);
        const QHash<QString, QJsonObject> foreignC =
            rlResolveForeignFeedbackIds(resolved, roadmapPath, callerPrefixes,
                                        prC.mappedIds, ropts.roadmapIds);
        for (auto it = foreignC.constBegin(); it != foreignC.constEnd(); ++it) {
            ropts.roadmapIds.insert(it.key());
            if (it.value().value(QStringLiteral("status")).toString() == kCheck) {
                ropts.shippedIds.insert(it.key());
                const QString d =
                    it.value().value(QStringLiteral("shipped_date")).toString();
                if (!d.isEmpty()) ropts.shipDates.insert(it.key(), d);
            }
        }

        // Only now is "nothing to resolve against" a real refusal — and it says
        // WHICH roadmaps were searched, because "unresolved" previously read as
        // "this id does not exist" when it meant "not in the file I opened".
        if (ropts.roadmapIds.isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("roadmap_unavailable"),
                QStringLiteral("feedback_log: compact_resolved found no roadmap ids — "
                               "searched the caller project (%1) and the sibling "
                               "projects under %2")
                    .arg(roadmapPath.isEmpty() ? QStringLiteral("no ROADMAP.md found")
                                               : roadmapPath,
                         QFileInfo(resolved).absolutePath())));
        }

        const FeedbackFile::ResolveResult rr =
            FeedbackFile::compactResolved(content, ropts);
        const bool dryRunR = req.value(QStringLiteral("dry_run")).toBool();

        QJsonArray collapsed, skipped;
        int collapsedCount = 0;
        for (const FeedbackFile::ResolvedFinding &f : rr.findings) {
            if (f.collapsed) {
                ++collapsedCount;
                QJsonObject e;
                e[QStringLiteral("heading")]      = f.heading;
                e[QStringLiteral("ids")]          =
                    QJsonArray::fromStringList(f.ids);
                e[QStringLiteral("line")]         = f.line;
                e[QStringLiteral("bytes_before")] = f.bytesBefore;
                e[QStringLiteral("bytes_after")]  = f.bytesAfter;
                e[QStringLiteral("bytes_saved")]  = f.bytesBefore - f.bytesAfter;
                collapsed.append(e);
            } else {
                QJsonObject e;
                e[QStringLiteral("heading")] = f.heading;
                e[QStringLiteral("line")]    = f.line;
                e[QStringLiteral("code")]    = f.code;
                e[QStringLiteral("ids")]     =
                    QJsonArray::fromStringList(f.ids);
                if (!f.openIds.isEmpty())
                    e[QStringLiteral("open_ids")] =
                        QJsonArray::fromStringList(f.openIds);
                if (!f.unresolvedIds.isEmpty())
                    e[QStringLiteral("unresolved_ids")] =
                        QJsonArray::fromStringList(f.unresolvedIds);
                skipped.append(e);
            }
        }

        // ANTS-4646(a) — retire any legacy v1 tracking heading whose every id
        // has shipped, under the gate this verb already resolved. Runs on
        // rr.newContent so one write carries both, and it is a no-op on a file
        // carrying no such heading — which is every file born v2.
        const FeedbackFile::RetireResult retire =
            FeedbackFile::retireTrackingHeadings(rr.newContent, ropts);
        QJsonArray retiredArr;
        int retiredCount = 0;
        for (const FeedbackFile::TrackingHeading &th : retire.headings) {
            QJsonObject e;
            e[QStringLiteral("heading")] = th.heading;
            e[QStringLiteral("line")]    = th.line;
            e[QStringLiteral("retired")] = th.retired;
            e[QStringLiteral("ids")]     = QJsonArray::fromStringList(th.ids);
            if (!th.code.isEmpty()) e[QStringLiteral("code")] = th.code;
            if (!th.openIds.isEmpty())
                e[QStringLiteral("open_ids")] =
                    QJsonArray::fromStringList(th.openIds);
            if (!th.unresolvedIds.isEmpty())
                e[QStringLiteral("unresolved_ids")] =
                    QJsonArray::fromStringList(th.unresolvedIds);
            if (th.retired) ++retiredCount;
            retiredArr.append(e);
        }

        if (!dryRunR && (collapsedCount > 0 || retiredCount > 0)) {
            const QByteArray utf8 = retire.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }

        QJsonObject out;
        out[QStringLiteral("ok")]                 = true;
        out[QStringLiteral("op")]                 = op;
        out[QStringLiteral("path")]               = resolved;
        out[QStringLiteral("dry_run")]            = dryRunR;
        out[QStringLiteral("findings_collapsed")] = collapsedCount;
        out[QStringLiteral("bytes_saved")]        =
            static_cast<qint64>(rr.bytesSaved + retire.bytesSaved);
        out[QStringLiteral("collapsed")]          = collapsed;
        out[QStringLiteral("skipped")]            = skipped;
        // ANTS-4646(a) — always present, so "this build cannot retire one" is
        // never confused with "there was none to retire".
        out[QStringLiteral("headings_retired")]   = retiredCount;
        out[QStringLiteral("retired_headings")]   = retiredArr;
        if (derived) out[QStringLiteral("path_derived")] = true;  // ANTS-3376
        return QJsonDocument(out);
    }

    // ANTS-4646(b) — set_title: rewrite the H1's project name. The FILENAME is
    // derived from caller_cwd's leaf and is always right; the H1 was writable
    // by no op, so a rename left the title contradicting the filename and the
    // only route was a hand edit — against the file's own instruction, at the
    // moment a session is most likely to get it wrong.
    // ANTS-4707 — correct the format-version marker. The only route before this
    // was the hand edit the assign-id pipeline exists to avoid, and a real file
    // declares a version that has never been defined.
    if (op == QStringLiteral("set_format_version")) {
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: set_format_version on an absent "
                               "file (nothing to correct)"),
                resolved, feedbackCallerLeaf(req)));
        }
        const QJsonValue vv = req.value(QStringLiteral("version"));
        if (!vv.isDouble()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: set_format_version requires an "
                               "integer \"version\"")));
        }
        QFile vf(resolved);
        if (!vf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString vcontent = QString::fromUtf8(vf.readAll());
        vf.close();

        const FeedbackFile::SetFormatVersionResult vr =
            FeedbackFile::setFormatVersion(vcontent, vv.toInt());
        if (!vr.code.isEmpty()) {
            QJsonObject e = fbErr(
                vr.code,
                QStringLiteral("feedback_log: set_format_version refused on "
                               "\"%1\"").arg(resolved));
            if (!vr.detail.isEmpty())
                e[QStringLiteral("hint")] = vr.detail;
            return QJsonDocument(e);
        }
        const bool dryRunV = req.value(QStringLiteral("dry_run")).toBool();
        if (vr.changed && !dryRunV) {
            const QByteArray utf8 = vr.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }
        QJsonObject out;
        out[QStringLiteral("ok")]          = true;
        out[QStringLiteral("op")]          = op;
        out[QStringLiteral("path")]        = resolved;
        out[QStringLiteral("dry_run")]     = dryRunV;
        // An already-correct marker is a SUCCESS, not a refusal — a corpus
        // sweep must be re-runnable.
        out[QStringLiteral("changed")]     = vr.changed;
        out[QStringLiteral("old_version")] = vr.oldVersion;
        out[QStringLiteral("new_version")] = vr.newVersion;
        if (derived) out[QStringLiteral("path_derived")] = true;
        return QJsonDocument(out);
    }

    if (op == QStringLiteral("set_title")) {
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: set_title on an absent file "
                               "(nothing to retitle)"),
                resolved, feedbackCallerLeaf(req)));
        }
        const QString wanted =
            req.value(QStringLiteral("title")).toString().trimmed();
        if (wanted.isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: set_title requires a non-empty "
                               "\"title\" (the project name, not the whole "
                               "heading)")));
        }
        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        const FeedbackFile::SetTitleResult tr =
            FeedbackFile::setTitle(content, wanted);
        if (!tr.code.isEmpty()) {
            return QJsonDocument(fbErr(
                tr.code,
                QStringLiteral("feedback_log: set_title found no `# ` heading "
                               "in \"%1\" — the format fixes the H1's "
                               "position and this verb will not invent one")
                    .arg(resolved)));
        }
        const bool dryRunT = req.value(QStringLiteral("dry_run")).toBool();
        if (tr.changed && !dryRunT) {
            const QByteArray utf8 = tr.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }
        QJsonObject out;
        out[QStringLiteral("ok")]        = true;
        out[QStringLiteral("op")]        = op;
        out[QStringLiteral("path")]      = resolved;
        out[QStringLiteral("dry_run")]   = dryRunT;
        // `changed:false` on an already-correct title is a SUCCESS, not a
        // refusal — a rename script must be re-runnable.
        out[QStringLiteral("changed")]   = tr.changed;
        out[QStringLiteral("old_title")] = tr.oldTitle;
        out[QStringLiteral("new_title")] = tr.newTitle;
        if (derived) out[QStringLiteral("path_derived")] = true;
        return QJsonDocument(out);
    }

    // ANTS-3446 — migrate_v2: one-shot mechanical v1→v2 migration. Bumps the
    // version marker and stamps blank `**Proposed ID:**` placeholders on
    // finding-shaped, below-watermark `### ` blocks; leaves the v1 tracking
    // tables in place (spec § 1.1). Distinct whole-file request shape (no
    // targets, no roadmap read), returns early like the sibling compaction ops.
    if (op == QStringLiteral("migrate_v2")) {
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: migrate_v2 on an absent file "
                               "(nothing to migrate)"),
                resolved, feedbackCallerLeaf(req)));
        }
        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        // ANTS-3474 — opt-in: backfill inline Proposed-IDs from the file's own
        // v1 tracking tables (confidence-gated; blank on any uncertainty).
        const bool backfill =
            req.value(QStringLiteral("backfill_from_tracking")).toBool();
        const FeedbackFile::MigrateResult mr =
            FeedbackFile::migrateV2(content, backfill);
        const bool dryRunM = req.value(QStringLiteral("dry_run")).toBool();

        auto stampArr = [](const QVector<FeedbackFile::MigrateStamp> &v) {
            QJsonArray a;
            for (const auto &s : v) {
                QJsonObject e;
                e[QStringLiteral("heading")] = s.heading;
                e[QStringLiteral("line")]    = s.line;
                a.append(e);
            }
            return a;
        };
        QJsonArray orphans;
        for (const auto &o : mr.orphans) {
            QJsonObject e;
            e[QStringLiteral("heading")] = o.heading;
            e[QStringLiteral("line")]    = o.line;
            e[QStringLiteral("reason")]  = o.reason;
            orphans.append(e);
        }
        // ANTS-3474 — findings whose id was carried in from the tracking tables.
        QJsonArray backfilled;
        for (const auto &b : mr.backfilled) {
            QJsonObject e;
            e[QStringLiteral("heading")]       = b.heading;
            e[QStringLiteral("line")]          = b.line;
            e[QStringLiteral("id")]            = b.id;
            e[QStringLiteral("confidence_pct")] = b.confidencePct;
            backfilled.append(e);
        }

        // Write only when there is something to change (marker bump and/or a
        // stamp). An already-v2 file is a clean no-op — never re-written.
        if (!dryRunM && !mr.alreadyV2) {
            const QByteArray utf8 = mr.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }

        QJsonObject out;
        out[QStringLiteral("ok")]            = true;
        out[QStringLiteral("op")]            = op;
        out[QStringLiteral("path")]          = resolved;
        out[QStringLiteral("dry_run")]       = dryRunM;
        out[QStringLiteral("already_v2")]    = mr.alreadyV2;
        out[QStringLiteral("stamped_count")] = mr.stamped.size();
        out[QStringLiteral("backfilled_count")] = mr.backfilled.size();  // ANTS-3474
        out[QStringLiteral("bytes_delta")]   =
            static_cast<qint64>(mr.bytesDelta);
        out[QStringLiteral("stamped")]       = stampArr(mr.stamped);
        out[QStringLiteral("backfilled")]    = backfilled;               // ANTS-3474
        out[QStringLiteral("orphans")]       = orphans;
        out[QStringLiteral("unclassified")]  = stampArr(mr.unclassified);
        if (derived) out[QStringLiteral("path_derived")] = true;  // ANTS-3376
        return QJsonDocument(out);
    }

    // ANTS-3447 — assign_id: v2 inline triage write. Fill ONE `### ` finding's
    // `**Proposed ID:**` line with the maintainer's assigned id(s) or an
    // `n/a — <reason>` closure. Distinct request shape (heading + ids|closure,
    // no roadmap read); returns early like the sibling compaction ops.
    // ANTS-4671 — assign_id_batch: N findings' slots in ONE read and ONE
    // atomic write. The argument for it was already accepted three times on
    // the roadmap side (flip_batch, annotate_batch, amend_batch's item), and
    // the shape is stronger here: a triage is inherently a batch — findings
    // are read together via feedback_query, decided together against one dedup
    // sweep, and several legitimately share one id. Nothing between the calls
    // can have changed the file, which is exactly where a per-call read and
    // write is least justified.
    //
    // FeedbackFile::assignId() is pure (content -> content), so the batch is
    // the single op's body with the read and write hoisted out of the loop and
    // the content threaded through. Per-assignment failures land in skipped[]
    // and cost only their own assignment — which matters more here than
    // elsewhere, because a heading is matched verbatim and one mistyped
    // heading must not cost the batch its other closures.
    if (op == QStringLiteral("assign_id_batch")) {
        const QJsonArray assignments =
            req.value(QStringLiteral("assignments")).toArray();
        if (assignments.isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: assign_id_batch requires a "
                               "non-empty \"assignments\" array")));
        }
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: assign_id_batch on an absent file"),
                resolved, feedbackCallerLeaf(req)));
        }
        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"").arg(resolved)));
        }
        QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        const bool dryRunB = req.value(QStringLiteral("dry_run")).toBool();
        QJsonArray applied, skipped;
        bool anyChange = false;
        int index = -1;
        for (const QJsonValue &av : assignments) {
            ++index;
            const QJsonObject a = av.toObject();
            auto skip = [&](const QJsonObject &e) {
                QJsonObject row = e;
                row[QStringLiteral("index")]   = index;
                row[QStringLiteral("heading")] =
                    a.value(QStringLiteral("heading")).toString();
                skipped.append(row);
            };

            const QString heading = a.value(QStringLiteral("heading")).toString();
            if (heading.trimmed().isEmpty()) {
                skip(fbErr(QStringLiteral("bad_args"),
                     QStringLiteral("feedback_log: assign_id requires a "
                                    "non-empty \"heading\" (the target `### ` "
                                    "finding line)")));
                continue;
            }
            QString value, note;
            bool isClosure = false;
            QJsonObject cerr;
            if (!fbComposeAssignValue(a, &value, &isClosure, &note, &cerr)) {
                skip(cerr);
                continue;
            }

            FeedbackFile::AssignTarget tgt;
            tgt.heading     = heading;
            tgt.headingLine = a.value(QStringLiteral("heading_line")).toInt(-1);
            tgt.value       = value;
            tgt.isClosure   = isClosure;
            tgt.note        = note;
            const FeedbackFile::AssignResult ar =
                FeedbackFile::assignId(content, tgt);
            if (!ar.code.isEmpty()) {       // target_not_found / target_ambiguous
                QJsonObject e = fbErr(
                    ar.code,
                    ar.code == QStringLiteral("target_ambiguous")
                        ? QStringLiteral("feedback_log: assign_id heading "
                                         "matches %1 `### ` blocks; pass "
                                         "heading_line").arg(ar.candidates.size())
                        : QStringLiteral("feedback_log: assign_id found no "
                                         "`### ` finding matching \"%1\"")
                              .arg(heading));
                if (!ar.candidates.isEmpty()) {
                    QJsonArray cand;
                    for (int c : ar.candidates) cand.append(c);
                    e[QStringLiteral("candidates")] = cand;
                }
                skip(e);
                continue;
            }

            // Thread the content forward: each assignment sees the previous
            // one's result, so two assignments to the same finding behave as
            // they would in two separate calls.
            content = ar.newContent;
            if (ar.changed) anyChange = true;
            QJsonObject row;
            row[QStringLiteral("index")]    = index;
            row[QStringLiteral("heading")]  = ar.heading;
            row[QStringLiteral("line")]     = ar.line;
            row[QStringLiteral("value")]    = ar.value;
            row[QStringLiteral("inserted")] = ar.inserted;
            row[QStringLiteral("changed")]  = ar.changed;
            if (ar.noteWritten) row[QStringLiteral("note_written")] = true;
            applied.append(row);
        }

        // ANTS-4470's rule, kept: an all-failed batch is a refusal, not a
        // success reporting nothing. A caller that reads only `ok` must not
        // read "every heading was mistyped" as a completed triage.
        if (applied.isEmpty()) {
            QJsonObject e = fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: assign_id_batch applied no "
                               "assignment — every one was refused"));
            e[QStringLiteral("skipped")]       = skipped;
            e[QStringLiteral("skipped_count")] = skipped.size();
            return QJsonDocument(e);
        }

        if (!dryRunB && anyChange) {
            const QByteArray utf8 = content.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }

        QJsonObject out;
        out[QStringLiteral("ok")]            = true;
        out[QStringLiteral("op")]            = op;
        out[QStringLiteral("path")]          = resolved;
        out[QStringLiteral("dry_run")]       = dryRunB;
        out[QStringLiteral("applied")]       = applied;
        out[QStringLiteral("applied_count")] = applied.size();
        out[QStringLiteral("skipped")]       = skipped;
        out[QStringLiteral("skipped_count")] = skipped.size();
        out[QStringLiteral("changed")]       = anyChange;
        if (derived) out[QStringLiteral("path_derived")] = true;   // ANTS-3376
        return QJsonDocument(out);
    }

    if (op == QStringLiteral("assign_id")) {
        const QString heading =
            req.value(QStringLiteral("heading")).toString();
        if (heading.trimmed().isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: assign_id requires a non-empty "
                               "\"heading\" (the target `### ` finding line)")));
        }

        // ANTS-4671 — composition, validation and the control-char folds now
        // live in fbComposeAssignValue so this op and assign_id_batch cannot
        // answer one assignment differently. Behaviour is unchanged: exactly
        // one of ids / closure / awaiting, the ^ANTS-[0-9]+$ gate, and the
        // note fold that ANTS-3571 added.
        QString value, note;
        bool hasClosure = false;
        {
            QJsonObject cerr;
            if (!fbComposeAssignValue(req, &value, &hasClosure, &note, &cerr))
                return QJsonDocument(cerr);
        }

        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: assign_id on an absent file"),
                resolved, feedbackCallerLeaf(req)));
        }
        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        const QString content = QString::fromUtf8(rf.readAll());
        rf.close();

        FeedbackFile::AssignTarget tgt;
        tgt.heading     = heading;
        tgt.headingLine =
            req.value(QStringLiteral("heading_line")).toInt(-1);
        tgt.value       = value;
        tgt.isClosure   = hasClosure;
        tgt.note        = note;
        const FeedbackFile::AssignResult ar =
            FeedbackFile::assignId(content, tgt);

        if (!ar.code.isEmpty()) {   // target_not_found / target_ambiguous
            QJsonObject e = fbErr(
                ar.code,
                ar.code == QStringLiteral("target_ambiguous")
                    ? QStringLiteral("feedback_log: assign_id heading matches "
                                     "%1 `### ` blocks; pass heading_line")
                          .arg(ar.candidates.size())
                    : QStringLiteral("feedback_log: assign_id found no `### ` "
                                     "finding matching \"%1\"").arg(heading));
            if (!ar.candidates.isEmpty()) {
                QJsonArray cand;
                for (int c : ar.candidates) cand.append(c);
                e[QStringLiteral("candidates")] = cand;
            }
            return QJsonDocument(e);
        }

        const bool dryRunA = req.value(QStringLiteral("dry_run")).toBool();
        // Write only on a real change (skip the byte-identical no-op so the
        // mtime/ETag is untouched — INV-8).
        if (!dryRunA && ar.changed) {
            const QByteArray utf8 = ar.newContent.toUtf8();
            QSaveFile sf(resolved);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: cannot open \"%1\" for "
                                   "writing").arg(resolved)));
            }
            bool committed = (sf.write(utf8) == utf8.size());
            if (committed) {
                if (g_forceFeedbackWriteFail) {
                    sf.cancelWriting();
                    committed = false;
                } else {
                    committed = sf.commit();
                }
            }
            if (!committed) {
                return QJsonDocument(fbErr(
                    QStringLiteral("write_failed"),
                    QStringLiteral("feedback_log: atomic write of \"%1\" "
                                   "failed").arg(resolved)));
            }
        }

        QJsonObject out;
        out[QStringLiteral("ok")]          = true;
        out[QStringLiteral("op")]          = op;
        out[QStringLiteral("path")]        = resolved;
        out[QStringLiteral("dry_run")]     = dryRunA;
        out[QStringLiteral("heading")]     = ar.heading;
        out[QStringLiteral("line")]        = ar.line;
        out[QStringLiteral("value")]       = ar.value;
        out[QStringLiteral("inserted")]    = ar.inserted;
        out[QStringLiteral("changed")]     = ar.changed;
        // ANTS-3571 — echo whether a note bullet was rendered so the caller
        // can confirm the note landed (only when a note was supplied).
        if (ar.noteWritten) out[QStringLiteral("note_written")] = true;
        out[QStringLiteral("bytes_delta")] =
            static_cast<qint64>(ar.bytesDelta);
        if (derived) out[QStringLiteral("path_derived")] = true;  // ANTS-3376
        return QJsonDocument(out);
    }

    // ANTS-2227 — dry_run preview: render the block + compute the would-be
    // bytes/created, then return WITHOUT writing the file.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // date: default to today; an explicit value must match YYYY-MM-DD.
    QString date = req.value(QStringLiteral("date")).toString();
    if (date.isEmpty()) {
        date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
    } else {
        static const QRegularExpression dateRe(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
        if (!dateRe.match(date).hasMatch()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: \"date\" must be "
                               "YYYY-MM-DD")));
        }
    }

    bool created = false;
    QString rendered;

    if (op == QStringLiteral("append_finding")) {
        const QJsonArray findingsArr =
            req.value(QStringLiteral("findings")).toArray();
        if (findingsArr.isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: append_finding requires a "
                               "non-empty \"findings\" array")));
        }
        QVector<FeedbackFile::Finding> findings;
        for (const QJsonValue &v : findingsArr) {
            const QJsonObject fo = v.toObject();
            FeedbackFile::Finding fnd;
            fnd.title        = fo.value(QStringLiteral("title")).toString();
            if (fnd.title.isEmpty()) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: every finding needs a "
                                   "non-empty \"title\"")));
            }
            fnd.what         = fo.value(QStringLiteral("what")).toString();
            fnd.repro        = fo.value(QStringLiteral("repro")).toString();
            fnd.impact       = fo.value(QStringLiteral("impact")).toString();
            fnd.suggestedFix =
                fo.value(QStringLiteral("suggested_fix")).toString();
            findings.append(fnd);
        }
        const QString sessionLabel =
            req.value(QStringLiteral("session_label")).toString();
        const QString headingLevel =
            req.value(QStringLiteral("heading_level"))
               .toString(QStringLiteral("h2"));
        const bool h1 = (headingLevel == QStringLiteral("h1"));
        const QString note = req.value(QStringLiteral("note")).toString();

        if (!exists) {
            // Create with a conforming skeleton; <Project> derived from
            // the basename minus the suffix.
            QString project = QFileInfo(resolved).fileName();
            project.chop(static_cast<int>(
                qstrlen(kFeedbackSuffix)));
            rendered = FeedbackFile::skeleton(project);
            created = true;
        }
        // Defer the on-disk content read to the append step below.
        rendered += FeedbackFile::renderFindingBlock(
            date, sessionLabel, h1, note, findings);
    } else {  // append_tracking
        if (!exists) {
            return QJsonDocument(fbNotFound(
                QStringLiteral("feedback_log: append_tracking on an "
                               "absent file (nothing to triage)"),
                resolved, feedbackCallerLeaf(req)));
        }
        // ANTS-3477 — append_tracking is the v1 (tracking-table) triage write.
        // On a v2 (inline-ID) file it must not re-grow a maintainer table:
        // refuse and point at op:assign_id (the v2 triage write that fills each
        // finding's `**Proposed ID:**` line in place). A v1 / un-migrated file
        // (`markerVersion < 2`, incl. an absent marker) stays valid.
        {
            QFile vf(resolved);
            if (vf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString vcontent = QString::fromUtf8(vf.readAll());
                vf.close();
                if (FeedbackFile::markerVersion(vcontent) >= 2) {
                    return QJsonDocument(fbErr(
                        QStringLiteral("not_v1"),
                        QStringLiteral(
                            "feedback_log: append_tracking is the v1 "
                            "tracking-table write and is retired on a v2 "
                            "(<!-- ants-mcp-feedback: 2 -->) file — use "
                            "op:assign_id to fill each finding's "
                            "\"**Proposed ID:**\" line in place")));
                }
            }
        }
        const QJsonArray rowsArr =
            req.value(QStringLiteral("rows")).toArray();
        if (rowsArr.isEmpty()) {
            return QJsonDocument(fbErr(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: append_tracking requires a "
                               "non-empty \"rows\" array")));
        }
        static const QSet<QString> kStatusSet = {
            QString::fromUtf8("\xF0\x9F\x93\x8B"),  // 📋
            QString::fromUtf8("\xF0\x9F\x9A\xA7"),  // 🚧
            QString::fromUtf8("\xE2\x9C\x85"),      // ✅
            QString::fromUtf8("\xF0\x9F\x92\xAD"),  // 💭
            QString::fromUtf8("\xF0\x9F\x94\x84"),  // 🔄
            QString::fromUtf8("\xE2\x9D\x93"),      // ❓
        };
        static const QRegularExpression idRe(
            QStringLiteral("^ANTS-[0-9]+$"));
        QVector<FeedbackFile::TrackingRow> rows;
        for (const QJsonValue &v : rowsArr) {
            const QJsonObject ro = v.toObject();
            FeedbackFile::TrackingRow row;
            row.item   = ro.value(QStringLiteral("item")).toString();
            if (row.item.isEmpty()) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_args"),
                    QStringLiteral("feedback_log: every row needs a "
                                   "non-empty \"item\"")));
            }
            row.status = ro.value(QStringLiteral("status")).toString();
            if (!kStatusSet.contains(row.status)) {
                return QJsonDocument(fbErr(
                    QStringLiteral("bad_status"),
                    QStringLiteral("feedback_log: row \"status\" must be "
                                   "one of 📋 🚧 ✅ 💭 🔄 ❓")));
            }
            for (const QJsonValue &iv :
                     ro.value(QStringLiteral("ids")).toArray()) {
                const QString idv = iv.toString();
                if (!idRe.match(idv).hasMatch()) {
                    return QJsonDocument(fbErr(
                        QStringLiteral("bad_args"),
                        QStringLiteral("feedback_log: each id must match "
                                       "ANTS-NNNN (got \"%1\")").arg(idv)));
                }
                row.ids.append(idv);
            }
            row.notes = ro.value(QStringLiteral("notes")).toString();
            rows.append(row);
        }
        const QString note = req.value(QStringLiteral("note")).toString();
        const bool sentinel =
            req.value(QStringLiteral("sentinel")).toBool(true);
        rendered = FeedbackFile::renderTrackingBlock(
            date, note, rows, sentinel);
    }

    // Read existing content (when the file exists) and append-at-end,
    // ensuring a blank-line separator before the new block.
    QString existing;
    if (exists) {
        QFile rf(resolved);
        if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QJsonDocument(fbErr(
                QStringLiteral("read_failed"),
                QStringLiteral("feedback_log: cannot open \"%1\"")
                    .arg(resolved)));
        }
        existing = QString::fromUtf8(rf.readAll());
        rf.close();
    }

    QString full = existing;
    if (!full.isEmpty()) {
        if (!full.endsWith(QLatin1Char('\n'))) full += QLatin1Char('\n');
        // Guarantee a blank-line separator between the prior content and
        // the appended block.
        if (!full.endsWith(QStringLiteral("\n\n")))
            full += QLatin1Char('\n');
    }
    full += rendered;

    const QByteArray utf8 = full.toUtf8();
    const QByteArray addedUtf8 = rendered.toUtf8();

    if (dryRun) {
        QJsonObject out;
        out["ok"]             = true;
        out["dry_run"]        = true;
        out["op"]             = op;
        out["path"]           = resolved;
        out["bytes_appended"] = static_cast<qint64>(addedUtf8.size());
        out["date"]           = date;
        out["created"]        = created;   // would-create
        if (derived) out["path_derived"] = true;  // ANTS-3376
        return QJsonDocument(out);
    }

    QSaveFile sf(resolved);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return QJsonDocument(fbErr(
            QStringLiteral("write_failed"),
            QStringLiteral("feedback_log: cannot open \"%1\" for writing")
                .arg(resolved)));
    }
    bool committed = (sf.write(utf8) == utf8.size());
    if (committed) {
        if (g_forceFeedbackWriteFail) {
            sf.cancelWriting();
            committed = false;
        } else {
            committed = sf.commit();
        }
    }
    if (!committed) {
        return QJsonDocument(fbErr(
            QStringLiteral("write_failed"),
            QStringLiteral("feedback_log: atomic write of \"%1\" failed")
                .arg(resolved)));
    }

    QJsonObject out;
    out["ok"]             = true;
    out["op"]             = op;
    out["path"]           = resolved;
    out["bytes_appended"] = static_cast<qint64>(addedUtf8.size());
    out["date"]           = date;
    out["created"]        = created;
    if (derived) out["path_derived"] = true;  // ANTS-3376
    return QJsonDocument(out);
}

// ----- ANTS-1963 — spec_log MCP tool --------------------------------

static bool g_forceSpecWriteFail = false;
void RemoteControl::setForceSpecWriteFailForTest(bool on) {
    g_forceSpecWriteFail = on;
}

QJsonDocument RemoteControl::cmdSpecLog(const QJsonObject &req) {
    auto slErr = [](const QString &code, const QString &message) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = message;
        o["code"]  = code;
        return QJsonDocument(o);
    };

    const QString id      = req.value(QStringLiteral("id")).toString();
    const QString pathArg = req.value(QStringLiteral("path")).toString();
    if (id.isEmpty() && pathArg.isEmpty()) {
        return slErr(QStringLiteral("bad_id"),
                     QStringLiteral("spec_log: pass \"id\" or \"path\""));
    }
    if (!id.isEmpty() && pathArg.isEmpty() && !isValidSpecId(id)) {
        return slErr(QStringLiteral("bad_id"),
                     QStringLiteral("spec_log: id must match <PREFIX>-NNNN "
                                    "(e.g. ANTS-1963, DOOM-0009), optionally "
                                    "with a topic suffix as invariant_check "
                                    "returns it (LOTTO-0014-http-surface), "
                                    "phase_<NN>_<topic>, or a numeric "
                                    "<NN>-<topic> (e.g. 17-emission-model)"));
    }

    // Root: caller_cwd canonical (m_main-independent, mirrors
    // changelog_log). Empty → no_project.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString rootCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (rootCanonical.isEmpty()) {
        return slErr(QStringLiteral("no_project"),
                     QStringLiteral("spec_log: project root unresolved"));
    }

    QString rel, full;
    if (!pathArg.isEmpty()) {
        // ANTS-1295 — route the path arg through PathValidation
        // (allowOutsideRoot=false; specs live in-root). A malformed or
        // root-escaping path → bad_path.
        const auto check = PathValidation::validatePath(
            pathArg, rootCanonical, QStringLiteral("spec_log"),
            QStringLiteral("path"), /*allowOutsideRoot=*/false);
        if (check.bad) return QJsonDocument(check.err);
        if (!check.resolved.isEmpty()) {
            full = check.resolved;
        } else {
            // Non-existent (validatePath leaves resolved empty); rebuild
            // the in-root absolute path so the not_found check below
            // fires with the right message.
            full = QDir::cleanPath(
                rootCanonical + QLatin1Char('/') + pathArg);
        }
        rel = pathArg;
    } else {
        const bool isPhase = id.startsWith(QStringLiteral("phase_"));
        // ANTS-2160 — specs_dir override (phase routing unchanged).
        QString specsDir = QStringLiteral("docs/specs");
        if (const auto sd = ProjectSettings::load(rootCanonical).specsDir;
            sd && QDir(rootCanonical + QLatin1Char('/') + *sd).exists())
            specsDir = *sd;
        const QString dirRel = isPhase ? QStringLiteral("docs/phases/")
                                        : specsDir + QLatin1Char('/');
        // ANTS-3356 — exact `<id>.md`, then a `<id>-*.md` glob so a
        // topic-suffixed project spec (DOOM-0009-path-tracer.md) resolves.
        rel  = resolveSpecRelForId(rootCanonical, dirRel, id);
        full = rootCanonical + QLatin1Char('/') + rel;
    }

    QFileInfo fi(full);
    if (!fi.exists() || !fi.isFile()) {
        return slErr(QStringLiteral("not_found"),
                     QStringLiteral("spec_log: %1 does not exist")
                         .arg(rel));
    }

    const QString op = req.value(QStringLiteral("op")).toString();
    if (op != QStringLiteral("set_status") &&
        op != QStringLiteral("append_loop") &&
        op != QStringLiteral("append_inv")) {
        return slErr(QStringLiteral("bad_mode"),
                     QStringLiteral("spec_log: op must be \"set_status\", "
                                    "\"append_loop\", or \"append_inv\""));
    }

    QFile f(full);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return slErr(QStringLiteral("read_failed"),
                     QStringLiteral("spec_log: cannot open %1").arg(rel));
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    SpecLog::EditResult res;
    if (op == QStringLiteral("set_status")) {
        const QString status =
            req.value(QStringLiteral("status")).toString();
        if (status.isEmpty()) {
            return slErr(QStringLiteral("bad_args"),
                         QStringLiteral("spec_log: set_status requires a "
                                        "non-empty \"status\""));
        }
        // ANTS-4136 — opt in to keeping the field's continuation lines, so
        // changing one word does not delete the review history wrapped
        // underneath it. Off by default: the old behaviour stays byte-exact
        // for every existing caller.
        res = SpecLog::setStatus(
            content, status,
            req.value(QStringLiteral("preserve_body")).toBool(false));
    } else if (op == QStringLiteral("append_loop")) {
        const QString label =
            req.value(QStringLiteral("loop_label")).toString();
        const QString body = req.value(QStringLiteral("body")).toString();
        // ANTS-4364 — the TABLE form. One string per column, ordered to match
        // the table's own header; the engine refuses rather than writing a
        // bullet into a table.
        QStringList loopCells;
        for (const QJsonValue &cv :
                 req.value(QStringLiteral("cells")).toArray())
            loopCells.append(cv.toString());
        if (loopCells.isEmpty() && (label.isEmpty() || body.isEmpty())) {
            return slErr(QStringLiteral("bad_args"),
                         QStringLiteral("spec_log: append_loop requires "
                                        "\"loop_label\" and \"body\" (bullet "
                                        "form) or \"cells\" (table form)"));
        }
        res = SpecLog::appendLoop(content, label, body, loopCells);
    } else {  // append_inv
        const QString invId =
            req.value(QStringLiteral("inv_id")).toString();
        const QString body = req.value(QStringLiteral("body")).toString();
        static const QRegularExpression invRe(
            QStringLiteral("^INV-[0-9]+$"));
        if (!invRe.match(invId).hasMatch()) {
            return slErr(QStringLiteral("bad_args"),
                         QStringLiteral("spec_log: \"inv_id\" must match "
                                        "INV-N"));
        }
        if (body.isEmpty()) {
            return slErr(QStringLiteral("bad_args"),
                         QStringLiteral("spec_log: append_inv requires "
                                        "\"body\""));
        }
        const QString test = req.value(QStringLiteral("test")).toString();
        res = SpecLog::appendInv(content, invId, body, test);
    }

    if (!res.ok) {
        // The pure layer carries only structural codes
        // (unrecognised_format / bad_args), surfaced verbatim.
        return slErr(res.code, res.error);
    }

    const QByteArray utf8 = res.content.toUtf8();

    // ANTS-2136 — dry_run: spec_log is section-routed (set_status rewrites
    // the Status line in place; append_loop / append_inv splice at a
    // computed section end, synthesising a heading when absent), so the
    // landing `line` and resulting size aren't knowable in advance.
    // Preview them without writing the spec (parity with roadmap_log /
    // changelog_log dry_run).
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]      = true;
        out["op"]      = op;
        out["dry_run"] = true;
        if (!id.isEmpty()) out["id"] = id;
        out["path"]    = rel;
        out["line"]    = res.line;
        out["bytes"]   = static_cast<qint64>(utf8.size());
        if (op == QStringLiteral("set_status"))
            out["previous_status"] = res.previousValue;
        // ANTS-4353/4364 — say which SHAPE was written and which DIRECTION
        // was inferred, so a caller can see which way the row went instead of
        // re-reading the file to find out.
        if (!res.rowShape.isEmpty()) out["row_shape"] = res.rowShape;
        if (!res.rowOrder.isEmpty()) out["row_order"] = res.rowOrder;
        return QJsonDocument(out);
    }

    // ANTS-3724 — capture the pre-write size so bytes_written can be the
    // ADDED-bytes delta rather than the rewritten file, matching roadmap_log
    // (ANTS-3702) and changelog_log (ANTS-3723). spec_log was the last write
    // verb on the old convention.
    const qint64 specBefore = QFileInfo(full).size();

    QSaveFile sf(full);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return slErr(QStringLiteral("write_failed"),
                     QStringLiteral("spec_log: cannot open %1 for writing")
                         .arg(rel));
    }
    bool committed = (sf.write(utf8) == utf8.size());
    if (committed) {
        if (g_forceSpecWriteFail) {
            sf.cancelWriting();
            committed = false;
        } else {
            committed = sf.commit();
        }
    }
    if (!committed) {
        return slErr(QStringLiteral("write_failed"),
                     QStringLiteral("spec_log: atomic write of %1 failed")
                         .arg(rel));
    }

    QJsonObject out;
    out["ok"]            = true;
    out["op"]            = op;
    if (!id.isEmpty()) out["id"] = id;
    out["path"]          = rel;
    out["line"]          = res.line;
    // ANTS-4114 — set_status echoes the value it replaced. A project whose
    // standard defines its own Status vocabulary otherwise learns of a wrong
    // value at its lint gate minutes later; the replaced value makes the
    // mismatch visible in the write's own envelope.
    if (op == QStringLiteral("set_status"))
        out["previous_status"] = res.previousValue;
    // ANTS-4353/4364 — the shape written and the direction inferred.
    if (!res.rowShape.isEmpty()) out["row_shape"] = res.rowShape;
    if (!res.rowOrder.isEmpty()) out["row_order"] = res.rowOrder;
    rcSetWriteBytes(out, specBefore, static_cast<qint64>(utf8.size()));
    return QJsonDocument(out);
}

// ANTS-1250: git_state — single tool collapsing status / log / diff
// behind an `op` discriminator. Saves ~240 permanent schema tokens
// vs three separate tools (cold-eyes pass 2). All git invocations go
// through gitwrap (shell-less argv, 5 s + 200 ms kill, 4 KiB stderr
// cap). Argv-injection guards: strict regex on `range`, `--`
// separator before user-derived positional args, `./` prefix on
// `-`-leading paths.
namespace rcdetail {
constexpr int kGitLogMaxN          = 100;   // ANTS-1250-INV-3
constexpr int kGitLogBodyCapBytes  = 1024;  // ANTS-1250-INV-3

QJsonObject gitErr(const char *code, const QString &message,
                   const QByteArray &stderrTail) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = QString::fromLatin1(code);
    if (!stderrTail.isEmpty()) {
        o["stderr"] = QString::fromUtf8(stderrTail);
    }
    return o;
}

// ANTS-1250-INV-4: stricter range regex — first char of each
// rev-component MUST NOT be `-` (closes leading-flag injection).
// Subsequent chars allow `-` so `HEAD~3..HEAD-1` style is still valid.
bool isValidRange(const QString &range) {
    static const QRegularExpression re(
        QStringLiteral(
            R"(^[a-zA-Z0-9._/^~][a-zA-Z0-9._/^~\-]*)"
            R"((\.\.\.?[a-zA-Z0-9._/^~][a-zA-Z0-9._/^~\-]*)?$)"));
    const auto m = re.match(range);
    return m.hasMatch() && m.capturedLength(0) == range.size();
}

// Project root resolution mirrors cmdWorkspaceSearch / cmdFileOutline:
// focused tab's shellCwd, fall back to QDir::current. Empty string
// returned when canonicalisation fails (caller maps to bad_path or
// not_git_repo depending on context).
QString resolveRootCanonical(MainWindow *main) {
    QString rootCwd;
    // ANTS-3725 — `ants::resolveCallerCwdRoot` guards a null MainWindow and
    // answers EmptyFallback, which routes straight back here and dereferenced
    // it anyway: the guard defended nothing. Production never passes null, but
    // it made every verb on this overload untestable at the handler layer
    // (segfault, not a refusal), which is why several of their tests scrape
    // source instead of driving the handler. Fall through to the process cwd.
    // ANTS-2132 — MainWindow read, marshalled: this runs on the dispatch
    // worker for every off-thread verb that reaches it.
    const auto focused = ants::onGuiThread([main]() -> QString {
        auto *t = main ? main->currentTerminal() : nullptr;
        return t ? t->shellCwd() : QString();
    });
    if (focused) rootCwd = *focused;
    if (rootCwd.isEmpty()) rootCwd = QDir::currentPath();
    return QFileInfo(rootCwd).canonicalFilePath();
}

// ANTS-1391: read-verb overload. When the request body carries
// `caller_cwd`, anchor the read to that cwd's project instead of the
// focused tab's. Use case: a Claude session in project B asks Ants
// "what's in ROADMAP?" — without this, the focused-tab default would
// reply with project A's ROADMAP whenever the user's attention is on
// a different tab. Empty/absent caller_cwd preserves back-compat (use
// focused tab). Present-but-unresolvable caller_cwd returns "" so
// callers' existing bad_path envelope fires — no new error code needed.
// Mutating verbs continue to enforce strict match via RcGate (ANTS-1372);
// read verbs just route, without refusing on mismatch.
//
// ANTS-1401 refactor: body is now a wrapper around `resolveCallerCwdRoot`,
// the single source of truth shared with `MainWindow::terminalForCaller`
// and the `caller_cwd_info` MCP verb (ANTS-1400). Pre-refactor mapping:
//   EmptyFallback              → focused-tab root
//   ExplicitMatch / NoMatch    → canonical caller_cwd (no tab walk —
//                                this overload trusts the caller's
//                                claim; tab-finding is the other
//                                wrapper's job)
//   Unresolvable               → empty string
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req) {
    const QString rawCaller =
        req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr =
        ants::resolveCallerCwdRoot(main, rawCaller);
    switch (rr.source) {
        case ants::ResolvedRoot::Source::EmptyFallback:
            return resolveRootCanonical(main);
        case ants::ResolvedRoot::Source::ExplicitMatch:
        case ants::ResolvedRoot::Source::NoMatch:
            return rr.cwd;
        case ants::ResolvedRoot::Source::Unresolvable:
            return QString();
    }
    return QString();  // -Wreturn-type
}

}  // namespace rcdetail (opened above — closed early so the
   // `ants::resolveCallerCwdRoot` definition below has external
   // linkage and matches its declaration in resolvedroot.h).

// ANTS-1401 — Central `caller_cwd` resolution helper. Single source of
// truth for the four-case decision tree introduced in ANTS-1396 and now
// shared with `MainWindow::terminalForCaller`,
// `resolveRootCanonical(main, req)`, the `caller_cwd_info` MCP verb
// (ANTS-1400), and the per-tool contract dispatcher (ANTS-1404).
namespace ants {

ResolvedRoot resolveCallerCwdRoot(const MainWindow *main,
                                  const QString &callerCwd) {
    ResolvedRoot rr;
    // ANTS-3725 — a null MainWindow (defensive; MCP dispatch always has one)
    // used to short-circuit here, which threw away an EXPLICIT caller_cwd and
    // answered EmptyFallback. That is the one case the caller told us the
    // answer to. What a null window actually costs is the tab walk — nothing
    // else — so the guard now sits at the two places that need `main`, and an
    // explicit caller_cwd resolves the same way it always did (Case 3, the
    // no-open-tab-matches branch). Behaviour with a live MainWindow is
    // unchanged in all four cases.
    if (callerCwd.isEmpty()) {
        // Case 1 — empty caller_cwd → focused fallback.
        rr.source = ResolvedRoot::Source::EmptyFallback;
        if (!main) return rr;
        // ANTS-2132 — both MainWindow reads in ONE marshal, so an off-thread
        // verb pays a single round-trip rather than one per accessor.
        const auto focus = ants::onGuiThread([main]() {
            auto *t = main->focusedTerminal();
            return QPair<QString, int>(t ? t->shellCwd() : QString(),
                                       main->currentTabIndexForRemote());
        });
        if (focus) {
            if (!focus->first.isEmpty())
                rr.cwd = QFileInfo(focus->first).canonicalFilePath();
            if (focus->second >= 0) rr.tabIndex = focus->second;
        }
        return rr;
    }
    const QString wantCanonical =
        QFileInfo(callerCwd).canonicalFilePath();
    if (wantCanonical.isEmpty()) {
        // Case 4 — present but unresolvable.
        rr.source = ResolvedRoot::Source::Unresolvable;
        return rr;
    }
    // INV-5 — deterministic lowest-index tie-break. for-loop walks
    // indices ascending; first match wins.
    // ANTS-2132 — snapshot every tab's cwd in ONE marshal, then canonicalise
    // off-thread. Marshalling per iteration would be a blocking round-trip per
    // tab; QFileInfo is thread-safe, so only the widget reads need the GUI
    // thread. Index order is preserved, so INV-5's lowest-index tie-break is
    // unchanged.
    QList<QPair<int, QString>> tabs;
    if (main) {
        const auto snap = ants::onGuiThread([main]() {
            QList<QPair<int, QString>> v;
            for (int i = 0; i < main->tabCount(); ++i) {
                if (auto *t = main->terminalAtTab(i))
                    v.append(QPair<int, QString>(i, t->shellCwd()));
            }
            return v;
        });
        if (snap) tabs = *snap;
    }
    for (const auto &entry : tabs) {
        const int i = entry.first;
        const QString tabCwd = entry.second;
        if (tabCwd.isEmpty()) continue;
        const QString tabCanonical =
            QFileInfo(tabCwd).canonicalFilePath();
        if (!tabCanonical.isEmpty() &&
            tabCanonical == wantCanonical) {
            // Case 2 — explicit caller_cwd hits an open tab.
            rr.source   = ResolvedRoot::Source::ExplicitMatch;
            rr.cwd      = wantCanonical;
            rr.tabIndex = i;
            return rr;
        }
    }
    // Case 3 — explicit caller_cwd, no open tab matches.
    rr.source = ResolvedRoot::Source::NoMatch;
    rr.cwd    = wantCanonical;
    return rr;
}

// ANTS-1390 — global Claude config sentinel. Path tools
// (workspace_search, file_outline) detect this *before* the normal
// caller_cwd resolution above, so a Claude session editing global
// config under ~/.claude/ can search / outline without owning a
// project root. Returns empty when not a sentinel (caller falls
// through to the regular resolveRootCanonical path).
QString expandGlobalConfigSentinel(const QString &callerCwd) {
    if (callerCwd != QLatin1String("~global") &&
        callerCwd != QLatin1String("~claude-config")) {
        return QString();
    }
    const QString claudeDir =
        QDir::homePath() + QStringLiteral("/.claude");
    const QFileInfo fi(claudeDir);
    if (!fi.exists() || !fi.isDir()) return QString();
    return fi.canonicalFilePath();
}

}  // namespace ants

namespace rcdetail {  // reopen the helper namespace closed above so the
                      // rest of the gitwrap helpers (parseStatusHeader,
                      // runStatusOp, etc.) keep their placement. External
                      // linkage since ANTS-3833 — TU 9 calls them.

// ANTS-1250-INV-8 / ANTS-1295: per-call path validation now lives in
// the central PathValidation chokepoint. See src/pathvalidation.{h,cpp}.

// Status header line: `## branch...origin/branch [ahead 1, behind 2]`
// or `## branch` for an untracked branch.
void parseStatusHeader(const QString &headerLine, QJsonObject &out) {
    // Strip leading "## ".
    QString rest = headerLine;
    if (rest.startsWith(QStringLiteral("## "))) rest = rest.mid(3);
    // Split off the bracketed ahead/behind suffix if present.
    int aheadN = 0;
    int behindN = 0;
    int bracket = rest.indexOf(QLatin1Char('['));
    if (bracket >= 0) {
        const int close = rest.indexOf(QLatin1Char(']'), bracket);
        if (close > bracket) {
            const QString inside = rest.mid(bracket + 1, close - bracket - 1);
            // tokens: "ahead N", "behind N", or "ahead N, behind N"
            const QStringList parts = inside.split(QLatin1Char(','),
                                                   Qt::SkipEmptyParts);
            for (const QString &raw : parts) {
                const QString t = raw.trimmed();
                if (t.startsWith(QStringLiteral("ahead "))) {
                    aheadN = QStringView{t}.mid(6).toInt();
                } else if (t.startsWith(QStringLiteral("behind "))) {
                    behindN = QStringView{t}.mid(7).toInt();
                }
            }
            rest.truncate(bracket);
            rest = rest.trimmed();
        }
    }
    // rest is now "branch" or "branch...upstream" or "branch...upstream"
    // or just "HEAD (no branch)" for detached.
    QString branch  = rest;
    QString upstream;
    const int dots = rest.indexOf(QStringLiteral("..."));
    if (dots >= 0) {
        branch   = rest.left(dots);
        upstream = rest.mid(dots + 3);
    }
    out["branch"]   = branch;
    out["upstream"] = upstream;
    out["ahead"]    = aheadN;
    out["behind"]   = behindN;
}

// ANTS-1391: req carries optional caller_cwd; pass-through to the
// read-verb resolveRootCanonical overload.
QJsonObject runStatusOp(MainWindow *main, const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(main, req);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    QStringList argv;
    argv << QStringLiteral("status")
         << QStringLiteral("--porcelain=v1")
         << QStringLiteral("-b");
    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git status exceeded 5 s wall budget"),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git status crashed"),
            g.stderrTail);
    }
    if (g.exitCode != 0) {
        // ANTS-1250-INV-11: not-a-git-repo → distinct code.
        const QString s = QString::fromUtf8(g.stderrTail);
        if (s.contains(QStringLiteral("not a git repository"),
                       Qt::CaseInsensitive)) {
            return gitErr("not_git_repo",
                QStringLiteral("git_state: not a git repository"));
        }
        return gitErr("git_failed",
            QStringLiteral("git_state: git status exit %1").arg(g.exitCode),
            g.stderrTail);
    }

    QJsonObject out;
    out["ok"]   = true;
    out["op"]   = QStringLiteral("status");
    QJsonArray files;
    QJsonArray untracked;
    const QList<QByteArray> lines = g.stdoutBytes.split('\n');
    for (const QByteArray &lineBytes : lines) {
        if (lineBytes.isEmpty()) continue;
        const QString line = QString::fromUtf8(lineBytes);
        if (line.startsWith(QStringLiteral("## "))) {
            parseStatusHeader(line, out);
            continue;
        }
        // porcelain v1 format: "XY path"
        if (line.size() < 3) continue;
        const QString xy   = line.left(2);
        const QString path = line.mid(3);
        if (xy == QStringLiteral("??")) {
            // ANTS-1522 — merge untracked paths into files[] with
            // `index:"?"` + `worktree:"?"` so callers iterating
            // files[] hit `git status --porcelain` parity. Keep
            // untracked[] populated in parallel as a derived field
            // for one release with a DEPRECATED marker (removed in
            // 0.7.93 — same horizon as session_memory's `cwd`).
            QJsonObject f;
            f["path"]     = path;
            f["index"]    = QStringLiteral("?");
            f["worktree"] = QStringLiteral("?");
            files.append(f);
            untracked.append(path);
            continue;
        }
        QJsonObject f;
        f["path"]     = path;
        f["index"]    = QString(xy.at(0));
        f["worktree"] = QString(xy.at(1));
        files.append(f);
    }
    // Backstop fields if header missing (e.g. detached HEAD without
    // -b output — porcelain emits at least the branch line, but be safe).
    if (!out.contains("branch"))   out["branch"]   = QString();
    if (!out.contains("upstream")) out["upstream"] = QString();
    if (!out.contains("ahead"))    out["ahead"]    = 0;
    if (!out.contains("behind"))   out["behind"]   = 0;
    out["files"]     = files;
    out["untracked"] = untracked;
    return out;
}

QJsonObject runLogOp(MainWindow *main, const QJsonObject &req) {
    // ANTS-1391: prefer caller_cwd when present.
    const QString rootCanonical = resolveRootCanonical(main, req);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    // ANTS-1250-INV-3: server-clamp n to [1, 100].
    int n = 10;
    const QJsonValue nVal = req.value("n");
    if (nVal.isDouble()) {
        const int requested = nVal.toInt();
        if (requested > 0) n = std::min(requested, kGitLogMaxN);
    }
    const bool wantBody = req.value("body").toBool(false);

    // ANTS-1340 — accept either a single `path` or a `paths` array. The lane
    // recent_changes composer (cmdSubsystem) passes the whole lane file list so
    // git resolves the union of touching commits in ONE invocation instead of
    // forking git once per file (worst case N×5 s serial, blocking the GUI on a
    // wedged repo). `git log -- f1 f2 …` already dedups + date-sorts, so the
    // result is identical to the prior per-file-merge. Every path is validated
    // through PathValidation exactly as the single-path form (INV-5 argv `--`).
    QStringList pathArgv;
    const QJsonValue pathsVal = req.value("paths");
    if (pathsVal.isArray()) {
        for (const QJsonValue &pv : pathsVal.toArray()) {
            const auto pcp = PathValidation::validatePath(
                pv.toString(), rootCanonical,
                QStringLiteral("git_state"), QStringLiteral("paths"));
            if (pcp.bad) return pcp.err;
            if (!pcp.argvForm.isEmpty()) pathArgv << pcp.argvForm;
        }
    } else {
        const auto pc = PathValidation::validatePath(
            req.value("path").toString(), rootCanonical,
            QStringLiteral("git_state"), QStringLiteral("path"));
        if (pc.bad) return pc.err;
        if (!pc.argvForm.isEmpty()) pathArgv << pc.argvForm;
    }

    // Format: SHA<US>SUBJECT<US>DATE(<US>BODY)?<RS>
    // Use 0x1f (US) between fields, 0x1e (RS) between commits.
    const QChar US(0x1f);
    const QChar RS(0x1e);
    const QString fmtNoBody = QStringLiteral("%h\x1f%s\x1f%cs");
    const QString fmtWith   = QStringLiteral("%h\x1f%s\x1f%cs\x1f%b");

    QStringList argv;
    argv << QStringLiteral("log")
         << QStringLiteral("--no-color")
         << QStringLiteral("-z")  // null-terminate per commit (we use RS)
         ;
    // -z emits NUL between commits. Override the inter-commit separator
    // by adding %x1e at end of format and splitting on RS.
    const QString fmt = (wantBody ? fmtWith : fmtNoBody) + QChar(0x1e);
    argv << QStringLiteral("--pretty=format:") + fmt;
    // Fetch n+1 to detect truncation (INV-3).
    argv << QStringLiteral("-n") << QString::number(n + 1);
    // ANTS-1250-INV-5: argv -- separator before user-derived path(s).
    argv << QStringLiteral("--");
    argv << pathArgv;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git log exceeded 5 s wall budget"),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git log crashed"),
            g.stderrTail);
    }
    if (g.exitCode != 0) {
        const QString s = QString::fromUtf8(g.stderrTail);
        if (s.contains(QStringLiteral("not a git repository"),
                       Qt::CaseInsensitive)) {
            return gitErr("not_git_repo",
                QStringLiteral("git_state: not a git repository"));
        }
        return gitErr("git_failed",
            QStringLiteral("git_state: git log exit %1").arg(g.exitCode),
            g.stderrTail);
    }

    // Parse: split on 0x1e, then per record split on 0x1f.
    const QString stdoutAll = QString::fromUtf8(g.stdoutBytes);
    const QStringList rawCommits = stdoutAll.split(RS, Qt::SkipEmptyParts);
    QJsonArray commits;
    for (const QString &rec : rawCommits) {
        // Trim leading NUL that -z inserts between records.
        QString r = rec;
        while (!r.isEmpty() && r.at(0) == QLatin1Char('\0')) r = r.mid(1);
        if (r.isEmpty()) continue;
        const QStringList fields = r.split(US);
        if (fields.size() < 3) continue;
        QJsonObject c;
        c["sha"]     = fields.value(0);
        c["subject"] = fields.value(1);
        c["date"]    = fields.value(2);
        if (wantBody && fields.size() >= 4) {
            QString body = fields.value(3);
            // Strip trailing NUL (-z emits one between records that ends
            // up after the body in the final field of all but the last).
            while (!body.isEmpty() && body.endsWith(QLatin1Char('\0'))) {
                body.chop(1);
            }
            const QByteArray b = body.toUtf8();
            if (b.size() > kGitLogBodyCapBytes) {
                body = QString::fromUtf8(b.left(kGitLogBodyCapBytes - 1)) +
                       QChar(0x2026);  // ellipsis
            }
            c["body"] = body;
        }
        commits.append(c);
    }
    bool truncated = false;
    if (commits.size() > n) {
        truncated = true;
        // Drop the n+1th probe commit.
        while (commits.size() > n) commits.removeLast();
    }
    QJsonObject out;
    out["ok"]        = true;
    out["op"]        = QStringLiteral("log");
    out["commits"]   = commits;
    out["truncated"] = truncated;
    return out;
}

QJsonObject runDiffOp(MainWindow *main, const QJsonObject &req) {
    // ANTS-1391: prefer caller_cwd when present.
    const QString rootCanonical = resolveRootCanonical(main, req);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    const QString range = req.value("range").toString();
    // ANTS-3377 — staged: diff the index vs HEAD (`git diff --cached`) so a
    // commit split can verify what has been staged. A cached diff of an
    // explicit range is nonsensical, so refuse the combination rather than
    // silently picking one.
    const bool staged = req.value("staged").toBool();
    if (staged && !range.isEmpty()) {
        return gitErr("bad_args",
            QStringLiteral("git_state: \"staged\" and \"range\" are "
                           "mutually exclusive"));
    }
    // ANTS-2074: range omitted (and not staged) → the working-tree diff
    // (unstaged changes vs the index), matching bare `git diff`. This is the
    // single most common "what have I changed so far" call, so it needs no args.
    const bool worktreeDiff = range.isEmpty() && !staged;
    // ANTS-1250-INV-4: strict regex; first char excludes `-`.
    if (!range.isEmpty() && !isValidRange(range)) {
        return gitErr("bad_range",
            QStringLiteral("git_state: \"range\" failed validation"));
    }
    const auto pc = PathValidation::validatePath(
        req.value("path").toString(), rootCanonical,
        QStringLiteral("git_state"), QStringLiteral("path"));
    if (pc.bad) return pc.err;

    // ANTS-3377 — hunks: emit per-file `@@` hunk headers (for a clean commit
    // split) instead of the --numstat line counts. include_lines attaches the
    // raw hunk body; context sets the unified-context width (default 3,
    // clamped 0..10).
    const bool hunks        = req.value("hunks").toBool();
    const bool includeLines = req.value("include_lines").toBool();
    int context = req.contains(QStringLiteral("context"))
                      ? req.value("context").toInt(3) : 3;
    if (context < 0)  context = 0;
    if (context > 10) context = 10;

    QStringList argv;
    argv << QStringLiteral("diff")
         << QStringLiteral("--no-color");
    if (hunks) argv << QStringLiteral("--unified=%1").arg(context);
    else       argv << QStringLiteral("--numstat");
    if (staged)           argv << QStringLiteral("--cached");
    if (!range.isEmpty()) argv << range;
    argv << QStringLiteral("--");
    if (!pc.argvForm.isEmpty()) argv << pc.argvForm;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git diff exceeded 5 s wall budget"),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git diff crashed"),
            g.stderrTail);
    }
    if (g.exitCode != 0) {
        const QString s = QString::fromUtf8(g.stderrTail);
        if (s.contains(QStringLiteral("not a git repository"),
                       Qt::CaseInsensitive)) {
            return gitErr("not_git_repo",
                QStringLiteral("git_state: not a git repository"));
        }
        if (s.contains(QStringLiteral("unknown revision"),
                       Qt::CaseInsensitive) ||
            s.contains(QStringLiteral("bad revision"),
                       Qt::CaseInsensitive)) {
            return gitErr("bad_range",
                QStringLiteral("git_state: range refers to unknown revision"),
                g.stderrTail);
        }
        return gitErr("git_failed",
            QStringLiteral("git_state: git diff exit %1").arg(g.exitCode),
            g.stderrTail);
    }

    // ANTS-3377 — hunk-header mode: parse the unified diff into per-file
    // `@@` headers via the pure GitWrap helper (git-free, unit-tested).
    if (hunks) {
        const QVector<GitWrap::DiffFile> parsed =
            GitWrap::parseDiffHunks(g.stdoutBytes, includeLines);
        QJsonArray hfiles;
        for (const GitWrap::DiffFile &df : parsed) {
            QJsonObject f;
            f["path"] = df.path;
            QJsonArray hs;
            for (const GitWrap::DiffHunk &h : df.hunks) {
                QJsonObject ho;
                ho["header"]    = h.header;
                ho["old_start"] = h.oldStart;
                ho["old_count"] = h.oldCount;
                ho["new_start"] = h.newStart;
                ho["new_count"] = h.newCount;
                if (includeLines) {
                    QJsonArray ls;
                    for (const QString &l : h.lines) ls.append(l);
                    ho["lines"] = ls;
                }
                hs.append(ho);
            }
            f["hunks"] = hs;
            hfiles.append(f);
        }
        QJsonObject out;
        out["ok"] = true;
        out["op"] = QStringLiteral("diff");
        if (staged)            out["staged"]   = true;
        else if (worktreeDiff) out["worktree"] = true;
        else                   out["range"]    = range;
        out["files"] = hfiles;
        QJsonObject totals;
        totals["files"] = hfiles.size();
        out["totals"] = totals;
        // ANTS-1839 — flag a diff that hit the 1 MiB stdout cap (the parsed
        // tail may be incomplete).
        if (g.stdoutTruncated) out["truncated"] = true;
        return out;
    }

    QJsonArray files;
    int totalAdded   = 0;
    int totalRemoved = 0;
    const QList<QByteArray> lines = g.stdoutBytes.split('\n');
    for (const QByteArray &lineBytes : lines) {
        if (lineBytes.isEmpty()) continue;
        const QString line = QString::fromUtf8(lineBytes);
        // numstat format: "<added>\t<removed>\t<path>"
        // Binary files appear as "-\t-\t<path>".
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 3) continue;
        bool addOk = false;
        bool remOk = false;
        const int added   = parts.at(0).toInt(&addOk);
        const int removed = parts.at(1).toInt(&remOk);
        QJsonObject f;
        f["path"] = parts.mid(2).join(QLatin1Char('\t'));
        if (addOk) {
            f["added"]    = added;
            totalAdded   += added;
        } else {
            f["added"]    = QJsonValue();  // null for binary
        }
        if (remOk) {
            f["removed"]  = removed;
            totalRemoved += removed;
        } else {
            f["removed"]  = QJsonValue();
        }
        files.append(f);
    }
    QJsonObject totals;
    totals["added"]   = totalAdded;
    totals["removed"] = totalRemoved;
    totals["files"]   = files.size();

    QJsonObject out;
    out["ok"]     = true;
    out["op"]     = QStringLiteral("diff");
    // ANTS-2074: echo the explicit range when given; otherwise flag the
    // working-tree (or ANTS-3377 staged) default so the caller knows which
    // diff it received.
    if (staged) {
        out["staged"] = true;
    } else if (worktreeDiff) {
        out["worktree"] = true;
    } else {
        out["range"] = range;
    }
    out["files"]  = files;
    out["totals"] = totals;
    return out;
}
}  // namespace rcdetail

// ANTS-2129 — audit_falsepos_log: append a confirmed false positive to
// <root>/.ants_review_falsepos.jsonl via the atomic O_APPEND in
// ants::falsepos::appendEntry. See docs/specs/ANTS-2129.md.
QJsonDocument RemoteControl::cmdAuditFalseposLog(const QJsonObject &req) {
    auto afErr = [](const QString &code, const QString &message) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = message;
        o["code"]  = code;
        return QJsonDocument(o);
    };

    // Resolve the root directly from caller_cwd (m_main-independent, like the
    // sibling write verbs roadmap_log / feedback_log / spec_log). For a
    // present caller_cwd this is the same canonical path
    // resolveCallerCwdRoot would return (its Case-2/3 both yield
    // QFileInfo(callerCwd).canonicalFilePath()), and the Required contract
    // guarantees caller_cwd is present.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString root = QFileInfo(callerRaw).canonicalFilePath();
    if (root.isEmpty()) {
        return afErr(QStringLiteral("no_project"),
            QStringLiteral("audit_falsepos_log: caller_cwd does not resolve "
                           "to an existing project directory"));
    }

    ants::falsepos::LedgerEntry e;
    e.reviewKind = req.value(QStringLiteral("review_kind")).toString();
    e.lane       = req.value(QStringLiteral("lane")).toString();
    e.claim      = req.value(QStringLiteral("claim")).toString();
    e.rationale  = req.value(QStringLiteral("rationale")).toString();
    e.topic      = req.value(QStringLiteral("topic")).toString();
    e.loggedBy   = req.value(QStringLiteral("logged_by")).toString();
    // timestamp defaults to today; appendEntry validates a present value.
    e.timestamp  = req.value(QStringLiteral("timestamp")).toString();
    if (e.timestamp.isEmpty())
        e.timestamp = QDate::currentDate()
                          .toString(QStringLiteral("yyyy-MM-dd"));
    if (e.loggedBy.isEmpty())
        e.loggedBy = QStringLiteral("cc-session");

    // ANTS-2227 — dry_run: validate + build the record + decide create/append,
    // return the would-be result without the O_APPEND write.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();
    const ants::falsepos::AppendResult r =
        ants::falsepos::appendEntry(root, e, dryRun);
    if (!r.ok) {
        return afErr(r.code,
            QStringLiteral("audit_falsepos_log: ") + r.message);
    }

    QJsonObject out;
    out["ok"]             = true;
    if (dryRun) out["dry_run"] = true;
    out["path"]           =
        root + QStringLiteral("/.ants_review_falsepos.jsonl");
    out["bytes_appended"] = r.bytesAppended;
    out["created"]        = r.created;
    out["timestamp"]      = r.timestamp;
    out["review_kind"]    = e.reviewKind;
    // ANTS-4105 — say, on the wire, which ledger this is. A session that
    // logged six tool findings here and re-ran audit_run watched all six come
    // back: this file is prose-grain brief material for the AI reviewers, and
    // the engine's suppressions:"auto" filters on the FINGERPRINT ledger that
    // audit_dismiss writes. Both are working as designed; nothing said so, and
    // /audit names this verb as the preferred route. The two are not merged —
    // their keys differ (a prose claim vs a file+rule+message hash), so
    // matching one against the other would be fuzzy by construction.
    out["consumed_by"] = QStringLiteral(
        "AI-review briefs (cold_eyes_brief / indie_review_brief)");
    out["hint"] = QStringLiteral(
        "this ledger does NOT suppress audit_run findings — to dismiss a "
        "static-analysis TOOL finding so later sweeps drop it, call "
        "audit_dismiss (it writes .audit_cache/learned-fp.jsonl, which is "
        "what suppressions:\"auto\" reads)");
    return QJsonDocument(out);
}

// ANTS-1713 — audit_dismiss: record a learned-false-positive verdict into
// <root>/.audit_cache/learned-fp.jsonl from a Claude Code session. Deferred
// v2 of ANTS-1708, which shipped the ledger + the GUI recording path but no
// MCP write side — so only the Audit dialog could teach the ledger, and a
// session that had just reasoned a finding to be a false positive had no way
// to say so. Both audit_run (ANTS-1820) and the dialog read this ledger, so
// one dismissal suppresses the finding on every later sweep.
//
// Distinct from audit_falsepos_log above: that is the PROSE ledger the
// review skills read (.ants_review_falsepos.jsonl, claim + rationale); this
// is the FINGERPRINT ledger the audit engine filters on.
QJsonDocument RemoteControl::cmdAuditDismiss(const QJsonObject &req) {
    auto adErr = [](const QString &code, const QString &message) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = message;
        o["code"]  = code;
        return QJsonDocument(o);
    };

    // Root resolution mirrors cmdAuditFalseposLog: caller_cwd is Required by
    // contract, so the canonical path is all we need (no tab fallback).
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString root = QFileInfo(callerRaw).canonicalFilePath();
    if (root.isEmpty()) {
        return adErr(QStringLiteral("no_project"),
            QStringLiteral("audit_dismiss: caller_cwd does not resolve to "
                           "an existing project directory"));
    }

    const QString rule = req.value(QStringLiteral("rule")).toString().trimmed();
    if (rule.isEmpty()) {
        return adErr(QStringLiteral("bad_args"),
            QStringLiteral("audit_dismiss: `rule` is required (the audit "
                           "check id the finding came from)"));
    }

    // Two input shapes: a caller-supplied fingerprint, or (file, message)
    // from which the server computes it. Computing server-side is the
    // preferred form — it guarantees the same hash the engine will look up.
    static const QRegularExpression rxFp(QStringLiteral("^[0-9a-f]{16}$"));
    QString fingerprint =
        req.value(QStringLiteral("fingerprint")).toString().trimmed();
    bool computed = false;
    if (!fingerprint.isEmpty()) {
        if (!rxFp.match(fingerprint).hasMatch()) {
            return adErr(QStringLiteral("bad_args"),
                QStringLiteral("audit_dismiss: `fingerprint` must be 16 "
                               "lowercase hex chars (got \"%1\")")
                    .arg(fingerprint));
        }
    } else {
        const QString file = req.value(QStringLiteral("file")).toString();
        const QString message =
            req.value(QStringLiteral("message")).toString();
        if (file.isEmpty() || message.isEmpty()) {
            return adErr(QStringLiteral("bad_args"),
                QStringLiteral("audit_dismiss: pass either `fingerprint`, or "
                               "both `file` and `message` so the server can "
                               "compute it"));
        }
        fingerprint =
            ants::auditfp::computeFingerprint(file, rule, message);
        computed = true;
    }

    ants::auditfp::Entry e;
    e.fingerprint = fingerprint;
    e.rule        = rule;
    e.reason      = req.value(QStringLiteral("reason")).toString();
    // Stamp here rather than letting appendEntry default it, so the response
    // can report the exact value written.
    e.timestamp   = QDateTime::currentDateTimeUtc()
                        .toString(Qt::ISODate);

    const QString path = root + QStringLiteral("/.audit_cache/learned-fp.jsonl");

    // dry_run: validate + compute + report the would-be record without the
    // append (ANTS-2227's convention on the sibling write verbs).
    if (req.value(QStringLiteral("dry_run")).toBool()) {
        QJsonObject out;
        out["ok"]          = true;
        out["dry_run"]     = true;
        out["path"]        = path;
        out["fingerprint"] = fingerprint;
        out["computed"]    = computed;
        out["rule"]        = rule;
        out["timestamp"]   = e.timestamp;
        return QJsonDocument(out);
    }

    if (!ants::auditfp::appendEntry(root, e)) {
        return adErr(QStringLiteral("write_failed"),
            QStringLiteral("audit_dismiss: could not append to %1").arg(path));
    }

    QJsonObject out;
    out["ok"]          = true;
    out["path"]        = path;
    out["fingerprint"] = fingerprint;
    // True when the server derived the fingerprint from (file, rule, message)
    // rather than taking the caller's — lets a caller confirm the hash it
    // will need if it wants to reference the entry later.
    out["computed"]    = computed;
    out["rule"]        = rule;
    out["timestamp"]   = e.timestamp;
    return QJsonDocument(out);
}

