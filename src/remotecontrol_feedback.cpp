// ANTS-3833 TU 10/25 — Feedback and audit verbs.
#include "remotecontrol.h"
#include "mcprefusal.h"
#include "remotecontrol_internal.h"
#include "build_info.h"    // ANTS-4741 — the stale-binary comparison
#include "guithread.h"
#include "feedbackfile.h"        // ANTS-1961 / ANTS-1962
#include "feedbackstore.h"       // ANTS-4513
#include "markdownscan.h"
#include "readlog.h"
#include "readregion.h"          // ANTS-4983 — the shared candidate ranker
#include "speclog.h"             // ANTS-1963
#include "gitwrap.h"
#include "claudeintegration.h"
#include "rootprovider.h"
#include "pathvalidation.h"
#include "projectsettings.h"    // ANTS-2160 — .ants/project.json overrides
#include "falseposledger.h"
#include "resolvedroot.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include "auditfpledger.h"

using namespace rcdetail;  // ANTS-3833

// How the feedback verbs read a roadmap's ids: the ✅ status mark, and an
// id's prefix ("ANTS" of "ANTS-12").
static QString fbShippedMark() { return QString::fromUtf8("\xE2\x9C\x85"); }

static QString fbIdPrefix(const QString &id) {
    const int dash = id.lastIndexOf(QLatin1Char('-'));
    return dash > 0 ? id.left(dash) : id;
}

// ANTS-3802 — the cross-repo id resolver feedback_query uses (compact_resolved
// shared it until ANTS-4513 retired that op). A *_Ants_MCP_Feedback.md file is
// by construction written from one project ABOUT another project's tooling, so
// its ids are never the caller's, and resolving only against caller_cwd's
// ROADMAP would leave them unresolved.
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

    QSet<QString> neededForeignIds;
    QSet<QString> foreignPrefixes;
    for (const QString &id : wantedIds) {
        if (alreadyResolved.contains(id)) continue;
        const QString pfx = fbIdPrefix(id);
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
            if (b.status == fbShippedMark()) {
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
// ANTS-3376 — a derived path is reported as such; ANTS-5500 adds the reason
// where the derivation was not the plain leaf convention.
static void fbMarkDerived(QJsonObject &out, const QString &reason) {
    out[QStringLiteral("path_derived")] = true;
    if (!reason.isEmpty())
        out[QStringLiteral("path_derived_reason")] = reason;
}

// ANTS-4513 § 4.7 — the store every feedback verb reads and writes. A fresh
// connection per call, as session_message opens one: § 4.4's BEGIN IMMEDIATE
// needs a connection with no transaction of its own already open. A store
// that does not open is a refusal; there is no file fallback, which would be
// the second storage path § 3 rules out.
static std::unique_ptr<RoadmapStore> fbOpenStore(const QString &verb,
                                                 QJsonObject *refusal) {
    auto store = std::make_unique<RoadmapStore>(RoadmapStore::defaultPath());
    QString err;
    if (store->open(&err)) return store;
    *refusal = mcp::refusal(QStringLiteral("store_failed"),
                     QStringLiteral("%1: cannot open the roadmap store: %2")
                         .arg(verb, err));
    return nullptr;
}

// A FeedbackStore refusal as the verb's envelope. `notFound` is the op's own
// wording for a reporter with neither a row nor a file.
static QJsonObject fbStoreRefusal(const FeedbackStore::Result &r,
                                  const QString &verb, const QString &notFound,
                                  const QString &path, const QString &callerLeaf) {
    if (r.code == QStringLiteral("not_found"))
        return fbNotFound(notFound, path, callerLeaf);
    QJsonObject e = mcp::refusal(r.code, verb + QStringLiteral(": ") + r.message);
    if (r.code == QStringLiteral("confirm_required")) {
        e[QStringLiteral("store_bytes")] = r.storeBytes;
        e[QStringLiteral("file_bytes")]  = r.fileBytes;
    }
    return e;
}

QJsonDocument RemoteControl::cmdFeedbackQuery(const QJsonObject &req) {
    QString resolved;
    bool exists = false;
    bool derived = false;
    QString derivedReason;   // ANTS-5500
    QJsonObject err;
    if (!resolveFeedbackPath(req, QStringLiteral("feedback_query"),
                             resolved, exists, err, &derived,
                             &derivedReason)) {
        return QJsonDocument(err);
    }
    // ANTS-4513 § 4.6 — the answer comes from the store, importing the file on
    // first touch. A reporter is found when it has a row, file or not.
    QJsonObject storeRefusal;
    const auto fbStore = fbOpenStore(QStringLiteral("feedback_query"), &storeRefusal);
    if (!fbStore) return QJsonDocument(storeRefusal);
    const FeedbackStore::Result loaded = FeedbackStore::load(*fbStore, resolved);
    if (!loaded.code.isEmpty())
        return QJsonDocument(mcp::refusal(loaded.code,
                                   QStringLiteral("feedback_query: ") + loaded.message));
    if (!loaded.found) {
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
        fbMarkDerived(nf, derivedReason);
        nf[QStringLiteral("found")]                  = false;
        nf[QStringLiteral("delta")]                  = QString();
        nf[QStringLiteral("delta_present")]          = false;
        nf[QStringLiteral("delta_line_count")]       = 0;
        nf[QStringLiteral("mapped_ids")]             = QJsonArray();
        nf[QStringLiteral("maintainer_block_count")] = 0;
        nf[QStringLiteral("truncated")]              = false;
        return QJsonDocument(nf);
    }
    const FeedbackFile::ParseResult pr = FeedbackFile::parse(loaded.content);

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
            // ANTS-5098 — cut at the byte cap and back off over continuation
            // bytes, so the head keeps every whole character that fits and
            // the string is encoded once, not once per 64-character step.
            int cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80)
                --cut;
            delta = QString::fromUtf8(utf8.constData(), cut);
            truncated = true;
        }
    }

    QJsonObject out;
    out["ok"]                    = true;
    // ANTS-4104 — the field the never-filed envelope sets to false. Always
    // present on a success, so one branch covers both.
    out["found"]                 = true;
    out["path"]                  = resolved;
    if (derived) fbMarkDerived(out, derivedReason);  // ANTS-3376
    out["delta"]                 = delta;
    out["delta_present"]         = pr.deltaPresent;
    out["delta_line_count"]      = pr.deltaLineCount;
    out["delta_start_line"]      = pr.deltaStartLine;
    out["mapped_ids"]            = QJsonArray::fromStringList(pr.mappedIds);
    out["maintainer_block_count"] = pr.maintainerBlockCount;
    out["last_maintainer_line"]  = pr.lastMaintainerLine;
    out["truncated"]             = truncated;
    // § 4.5 — the file was edited outside the verbs; this answer is the store's.
    if (loaded.drift) out["export_drift"] = true;

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
        QHash<QString, QString> idToStatus;
        QHash<QString, QString> idToShipDate;   // ANTS-3504 — ✅ ids' ship-date
        // ANTS-3518 — the caller roadmap's own id-prefixes, so a mapped id
        // whose prefix is absent here can be flagged cross-repo ("foreign_repo")
        // rather than the ambiguous "unknown".
        QSet<QString> callerPrefixes;
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
                    callerPrefixes.insert(fbIdPrefix(b.id));  // ANTS-3518
                    // ANTS-3504 — capture the ship-date for ✅ ids (shared
                    // extractor with compact_resolved) so a contributor can
                    // compare it against their server_build.build_date.
                    if (b.status == fbShippedMark()) {
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
                    !callerPrefixes.contains(fbIdPrefix(id))) {
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
            } else if (status == fbShippedMark() && idToShipDate.contains(id)) {
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
        bool anyUndated = false;
        for (QJsonValueRef v : statusArr) {
            QJsonObject o = v.toObject();
            const QString shipped =
                o.value(QStringLiteral("shipped_date")).toString();
            // ANTS-4843 — a SHIPPED id with no parseable ship date is exactly
            // the case the comparison cannot rule out, and skipping it left the
            // flag silent where a session most needed it. Flag it, and name
            // why, so it reads differently from a dated same-day ship.
            if (shipped.isEmpty() && !buildDate.isEmpty()
                && o.value(QStringLiteral("status")).toString() == fbShippedMark()) {
                o[QStringLiteral("possibly_stale_binary")] = true;
                o[QStringLiteral("stale_check")] = QStringLiteral("no_shipped_date");
                anyStale = true;
                anyUndated = true;
                v = o;
                continue;
            }
            if (shipped.isEmpty() || buildDate.isEmpty()) continue;
            if (shipped >= buildDate) {   // ISO dates compare lexically
                o[QStringLiteral("possibly_stale_binary")] = true;
                anyStale = true;
                v = o;
            }
        }
        out["mapped_id_status"] = statusArr;
        // ANTS-4079 § 2.4 / ANTS-3748 — a cited id that was split into parts
        // reports them, so "the parent shipped" is not read as "all of it did".
        // mapped_id_status keeps its meaning: the id's own status. Read from the
        // machine-global store, where every migrated project's rows live.
        {
            QJsonObject partsById;
            RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
            if (RoadmapStore *store = roadmapStoreOrNull(&why, nullptr)) {
                for (const QString &id : pr.mappedIds) {
                    const auto t = store->resolveLinkTarget(0, id);
                    if (!t || !t->itemPk)
                        continue;
                    const auto rows = store->linksFor(*t->itemPk);
                    if (!rows)
                        continue;
                    QJsonArray parts;
                    for (const auto &r : *rows)
                        if (r.reverse && r.type == QLatin1String("splits-from"))
                            parts.append(QJsonObject{
                                {QStringLiteral("id"), r.id},
                                {QStringLiteral("status"), rcStatusEmoji(r.status)}});
                    if (!parts.isEmpty())
                        partsById.insert(id, parts);
                }
            }
            if (!partsById.isEmpty())
                out[QStringLiteral("mapped_id_parts")] = partsById;
        }
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
                "compare against the build time.").arg(buildDate)
                + (anyUndated
                       ? QStringLiteral(
                             " An id marked stale_check:\"no_shipped_date\" is "
                             "shipped but has no Resolved date, so the "
                             "comparison could not run for it.")
                       : QString());
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

// ANTS-1962 — test seam for the export write. ANTS-4513 moved the write into
// FeedbackStore, so the seam forwards there.
void RemoteControl::setForceFeedbackWriteFailForTest(bool on) {
    FeedbackStore::setForceExportFailForTest(on);
}

// The ids these ops write are the ones the Ants MCP maintainer assigns at
// triage, from this project's roadmap, so only an ANTS id is accepted.
// Another project's ids are only ever read (rlResolveForeignFeedbackIds).
// assign_id's refusal, single and batch. target_ambiguous lists the colliding
// lines; target_not_found (ANTS-4983) the closest headings, for the caller to
// copy, since it was copying one when it missed.
static QJsonObject fbAssignRefusal(const FeedbackFile::AssignResult &ar,
                                   const QString &heading) {
    if (ar.code == QStringLiteral("target_ambiguous")) {
        QJsonObject e = mcp::refusal(ar.code,
            QStringLiteral("feedback_log: assign_id heading matches %1 `### ` "
                           "blocks; pass heading_line").arg(ar.candidates.size()));
        QJsonArray lines;
        for (const int line : ar.candidates) lines.append(line);
        e[QStringLiteral("candidates")] = lines;
        return e;
    }
    QJsonObject e = mcp::refusal(ar.code,
        QStringLiteral("feedback_log: assign_id found no `### ` finding matching "
                       "\"%1\"").arg(heading));
    QStringList slugs;   // file order, which the ranker breaks ties by
    QHash<QString, QString> headingBySlug;
    for (const QString &h : ar.headings) {
        const QString slug = MarkdownScan::headingSlug(h);
        if (headingBySlug.contains(slug)) continue;
        slugs.append(slug);
        headingBySlug.insert(slug, h);
    }
    QJsonArray nearest;
    for (const QString &slug :
         ReadRegion::rankSectionCandidates(MarkdownScan::headingSlug(heading), slugs))
        nearest.append(headingBySlug.value(slug));
    if (!nearest.isEmpty()) e[QStringLiteral("candidates")] = nearest;
    return e;
}

static bool fbIsAntsId(const QString &id) {
    static const QRegularExpression re(QStringLiteral("^ANTS-[0-9]+$"));
    return re.match(id).hasMatch();
}

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
        *err = mcp::refusal(
            QStringLiteral("bad_args"),
            QStringLiteral("feedback_log: assign_id needs exactly one of a "
                           "non-empty \"ids\" array, a \"closure\" string, "
                           "or an \"awaiting\" question string (got %1)")
                .arg(dispositions));
        return false;
    }

    if (hasIds) {
        QStringList idList;   // de-duplicated, keeping first occurrence
        for (const QJsonValue &v : idsArr) {
            const QString id = v.toString().trimmed();
            if (!fbIsAntsId(id)) {
                *err = mcp::refusal(
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

// ANTS-1962 — feedback_log. ANTS-4513 — every write goes through the store
// (FeedbackStore::write, § 4.4); the text edits are FeedbackFile's, unchanged.
QJsonDocument RemoteControl::cmdFeedbackLog(const QJsonObject &req) {
    QString resolved;
    bool exists = false;
    bool derived = false;
    QString derivedReason;   // ANTS-5500
    QJsonObject err;
    if (!resolveFeedbackPath(req, QStringLiteral("feedback_log"),
                             resolved, exists, err, &derived,
                             &derivedReason)) {
        return QJsonDocument(err);
    }

    const QString op = req.value(QStringLiteral("op")).toString();
    // ANTS-4513 § 4.6 — ops that kept a file tidy or served format version 1.
    // The store made each unnecessary, so each refuses and writes nothing.
    static const QStringList kRetired = {
        QStringLiteral("compact_resolved"), QStringLiteral("compact_shipped"),
        QStringLiteral("prune_tracking"),   QStringLiteral("append_tracking"),
        QStringLiteral("migrate_v2"),       QStringLiteral("set_format_version")};
    if (kRetired.contains(op)) {
        return QJsonDocument(mcp::refusal(
            QStringLiteral("retired"),
            QStringLiteral("feedback_log: op \"%1\" is retired. The feedback "
                           "corpus now lives in the roadmap store (ANTS-4513), "
                           "which made it unnecessary.").arg(op)));
    }
    if (op != QStringLiteral("append_finding") &&
        op != QStringLiteral("assign_id") &&
        op != QStringLiteral("assign_id_batch") &&   // ANTS-4671
        op != QStringLiteral("set_title") &&
        op != QStringLiteral("reimport")) {           // ANTS-4513 § 4.5
        return QJsonDocument(mcp::refusal(
            QStringLiteral("bad_mode"),
            QStringLiteral("feedback_log: op must be \"append_finding\", "
                           "\"assign_id\", \"assign_id_batch\", \"set_title\" "
                           "or \"reimport\"")));
    }
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();
    const QString leaf = feedbackCallerLeaf(req);
    auto withPath = [&](QJsonObject out) {
        out[QStringLiteral("ok")]   = true;
        out[QStringLiteral("op")]   = op;
        out[QStringLiteral("path")] = resolved;
        if (derived) fbMarkDerived(out, derivedReason);  // ANTS-3376
        return QJsonDocument(out);
    };

    // ANTS-4513 § 4.5 — settle a hand-edited export by keeping the file's text.
    if (op == QStringLiteral("reimport")) {
        QJsonObject refusal;
        const auto store = fbOpenStore(QStringLiteral("feedback_log"), &refusal);
        if (!store) return QJsonDocument(refusal);
        const FeedbackStore::Result r = FeedbackStore::reimport(
            *store, resolved, req.value(QStringLiteral("confirm")).toBool());
        if (!r.code.isEmpty())
            return QJsonDocument(fbStoreRefusal(r, QStringLiteral("feedback_log"),
                QStringLiteral("feedback_log: reimport on an absent file"),
                resolved, leaf));
        QJsonObject out;
        out[QStringLiteral("changed")]  = r.changed;
        out[QStringLiteral("imported")] = r.imported;
        return withPath(out);
    }

    // ANTS-4646(b) — set_title: rewrite the H1's project name. The FILENAME is
    // derived from caller_cwd's leaf and is always right; the H1 was writable
    // by no op, so a rename left the title contradicting the filename.
    if (op == QStringLiteral("set_title")) {
        const QString wanted =
            req.value(QStringLiteral("title")).toString().trimmed();
        if (wanted.isEmpty()) {
            return QJsonDocument(mcp::refusal(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: set_title requires a non-empty "
                               "\"title\" (the project name, not the whole "
                               "heading)")));
        }
        QJsonObject refusal;
        const auto store = fbOpenStore(QStringLiteral("feedback_log"), &refusal);
        if (!store) return QJsonDocument(refusal);
        FeedbackFile::SetTitleResult tr;
        const FeedbackStore::Result r = FeedbackStore::write(
            *store, resolved, QString(), dryRun,
            [&](const QString &in, bool, QString *text) {
                tr = FeedbackFile::setTitle(in, wanted);
                *text = tr.newContent;
                return tr.code.isEmpty();
            });
        if (r.code == QStringLiteral("edit_refused")) {
            return QJsonDocument(mcp::refusal(
                tr.code,
                QStringLiteral("feedback_log: set_title found no `# ` heading "
                               "in \"%1\" — the format fixes the H1's "
                               "position and this verb will not invent one")
                    .arg(resolved)));
        }
        if (!r.code.isEmpty())
            return QJsonDocument(fbStoreRefusal(r, QStringLiteral("feedback_log"),
                QStringLiteral("feedback_log: set_title on an absent file "
                               "(nothing to retitle)"), resolved, leaf));
        QJsonObject out;
        out[QStringLiteral("dry_run")]   = dryRun;
        // `changed:false` on an already-correct title is a SUCCESS, not a
        // refusal — a rename script must be re-runnable.
        out[QStringLiteral("changed")]   = tr.changed;
        out[QStringLiteral("old_title")] = tr.oldTitle;
        out[QStringLiteral("new_title")] = tr.newTitle;
        return withPath(out);
    }

    // ANTS-4671 — assign_id_batch: the single op's body with the content
    // threaded through one write. Per-assignment failures land in skipped[]
    // and cost only their own assignment, because a heading is matched
    // verbatim and one mistyped heading must not cost the batch the rest.
    if (op == QStringLiteral("assign_id_batch")) {
        const QJsonArray assignments =
            req.value(QStringLiteral("assignments")).toArray();
        if (assignments.isEmpty()) {
            return QJsonDocument(mcp::refusal(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: assign_id_batch requires a "
                               "non-empty \"assignments\" array")));
        }
        QJsonObject refusal;
        const auto store = fbOpenStore(QStringLiteral("feedback_log"), &refusal);
        if (!store) return QJsonDocument(refusal);

        QJsonArray applied, skipped;
        bool anyChange = false;
        const FeedbackStore::Result r = FeedbackStore::write(
            *store, resolved, QString(), dryRun,
            [&](const QString &in, bool, QString *text) {
                QString content = in;
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
                    const QString heading =
                        a.value(QStringLiteral("heading")).toString();
                    if (heading.trimmed().isEmpty()) {
                        skip(mcp::refusal(QStringLiteral("bad_args"),
                             QStringLiteral("feedback_log: assign_id requires a "
                                            "non-empty \"heading\" (the target "
                                            "`### ` finding line)")));
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
                    if (!ar.code.isEmpty()) {
                        skip(fbAssignRefusal(ar, heading));
                        continue;
                    }
                    // Each assignment sees the previous one's result, so two
                    // to one finding behave as two separate calls would.
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
                *text = content;
                return !applied.isEmpty();
            });
        // ANTS-4470's rule, kept: an all-failed batch is a refusal, not a
        // success reporting nothing.
        if (r.code == QStringLiteral("edit_refused")) {
            QJsonObject e = mcp::refusal(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: assign_id_batch applied no "
                               "assignment — every one was refused"));
            rcSetAllRefusedSkipped(e, skipped);
            return QJsonDocument(e);
        }
        if (!r.code.isEmpty())
            return QJsonDocument(fbStoreRefusal(r, QStringLiteral("feedback_log"),
                QStringLiteral("feedback_log: assign_id_batch on an absent file"),
                resolved, leaf));
        QJsonObject out;
        out[QStringLiteral("dry_run")]       = dryRun;
        out[QStringLiteral("applied")]       = applied;
        out[QStringLiteral("applied_count")] = applied.size();
        out[QStringLiteral("skipped")]       = skipped;
        out[QStringLiteral("skipped_count")] = skipped.size();
        out[QStringLiteral("changed")]       = anyChange;
        return withPath(out);
    }

    if (op == QStringLiteral("assign_id")) {
        const QString heading =
            req.value(QStringLiteral("heading")).toString();
        if (heading.trimmed().isEmpty()) {
            return QJsonDocument(mcp::refusal(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: assign_id requires a non-empty "
                               "\"heading\" (the target `### ` finding line)")));
        }
        // ANTS-4671 — composition, validation and the control-char folds live
        // in fbComposeAssignValue so this op and assign_id_batch cannot answer
        // one assignment differently.
        QString value, note;
        bool hasClosure = false;
        {
            QJsonObject cerr;
            if (!fbComposeAssignValue(req, &value, &hasClosure, &note, &cerr))
                return QJsonDocument(cerr);
        }
        QJsonObject refusal;
        const auto store = fbOpenStore(QStringLiteral("feedback_log"), &refusal);
        if (!store) return QJsonDocument(refusal);

        FeedbackFile::AssignTarget tgt;
        tgt.heading     = heading;
        tgt.headingLine = req.value(QStringLiteral("heading_line")).toInt(-1);
        tgt.value       = value;
        tgt.isClosure   = hasClosure;
        tgt.note        = note;
        FeedbackFile::AssignResult ar;
        const FeedbackStore::Result r = FeedbackStore::write(
            *store, resolved, QString(), dryRun,
            [&](const QString &in, bool, QString *text) {
                ar = FeedbackFile::assignId(in, tgt);
                *text = ar.newContent;
                return ar.code.isEmpty();
            });
        if (r.code == QStringLiteral("edit_refused"))
            return QJsonDocument(fbAssignRefusal(ar, heading));
        if (!r.code.isEmpty())
            return QJsonDocument(fbStoreRefusal(r, QStringLiteral("feedback_log"),
                QStringLiteral("feedback_log: assign_id on an absent file"),
                resolved, leaf));
        QJsonObject out;
        out[QStringLiteral("dry_run")]     = dryRun;
        out[QStringLiteral("heading")]     = ar.heading;
        out[QStringLiteral("line")]        = ar.line;
        out[QStringLiteral("value")]       = ar.value;
        out[QStringLiteral("inserted")]    = ar.inserted;
        out[QStringLiteral("changed")]     = ar.changed;
        // ANTS-3571 — echo whether a note bullet was rendered.
        if (ar.noteWritten) out[QStringLiteral("note_written")] = true;
        out[QStringLiteral("bytes_delta")] = static_cast<qint64>(ar.bytesDelta);
        return withPath(out);
    }

    // append_finding. date: default to today; an explicit value must match
    // YYYY-MM-DD.
    QString date = req.value(QStringLiteral("date")).toString();
    if (date.isEmpty()) {
        date = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
    } else {
        static const QRegularExpression dateRe(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
        if (!dateRe.match(date).hasMatch()) {
            return QJsonDocument(mcp::refusal(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: \"date\" must be "
                               "YYYY-MM-DD")));
        }
    }
    const QJsonArray findingsArr =
        req.value(QStringLiteral("findings")).toArray();
    if (findingsArr.isEmpty()) {
        return QJsonDocument(mcp::refusal(
            QStringLiteral("bad_args"),
            QStringLiteral("feedback_log: append_finding requires a "
                           "non-empty \"findings\" array")));
    }
    QVector<FeedbackFile::Finding> findings;
    for (const QJsonValue &v : findingsArr) {
        const QJsonObject fo = v.toObject();
        FeedbackFile::Finding fnd;
        fnd.title = fo.value(QStringLiteral("title")).toString();
        if (fnd.title.isEmpty()) {
            return QJsonDocument(mcp::refusal(
                QStringLiteral("bad_args"),
                QStringLiteral("feedback_log: every finding needs a "
                               "non-empty \"title\"")));
        }
        fnd.what         = fo.value(QStringLiteral("what")).toString();
        fnd.repro        = fo.value(QStringLiteral("repro")).toString();
        fnd.impact       = fo.value(QStringLiteral("impact")).toString();
        fnd.suggestedFix = fo.value(QStringLiteral("suggested_fix")).toString();
        findings.append(fnd);
    }
    const QString block = FeedbackFile::renderFindingBlock(
        date, req.value(QStringLiteral("session_label")).toString(),
        req.value(QStringLiteral("heading_level")).toString(QStringLiteral("h2"))
            == QStringLiteral("h1"),
        req.value(QStringLiteral("note")).toString(), findings);

    // § 4.3 — a reporter with no row and no file is registered here, from the
    // skeleton; <Project> is the basename minus the suffix.
    QString project = QFileInfo(resolved).fileName();
    project.chop(static_cast<int>(qstrlen(kFeedbackSuffix)));
    const QString skeleton = FeedbackFile::skeleton(project);

    QJsonObject refusal;
    const auto store = fbOpenStore(QStringLiteral("feedback_log"), &refusal);
    if (!store) return QJsonDocument(refusal);
    qint64 bytesAppended = 0;
    const FeedbackStore::Result r = FeedbackStore::write(
        *store, resolved, skeleton, dryRun,
        [&](const QString &in, bool created, QString *text) {
            // A new file is the skeleton with the block straight after it,
            // byte for byte what the file-backed create wrote (INV-9).
            QString full = in;
            if (!created && !full.isEmpty()) {
                if (!full.endsWith(QLatin1Char('\n'))) full += QLatin1Char('\n');
                // A blank-line separator before the appended block.
                if (!full.endsWith(QStringLiteral("\n\n")))
                    full += QLatin1Char('\n');
            }
            full += block;
            bytesAppended = (created ? skeleton + block : block).toUtf8().size();
            *text = full;
            return true;
        });
    if (!r.code.isEmpty())
        return QJsonDocument(fbStoreRefusal(r, QStringLiteral("feedback_log"),
            QStringLiteral("feedback_log: \"%1\" does not exist").arg(resolved),
            resolved, leaf));
    QJsonObject out;
    if (dryRun) out[QStringLiteral("dry_run")] = true;
    out[QStringLiteral("bytes_appended")] = bytesAppended;
    out[QStringLiteral("date")]           = date;
    out[QStringLiteral("created")]        = r.created;   // would-create on a dry run
    return withPath(out);
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
        const QString specsDir = ProjectSettings::specsDirOf(rootCanonical);
        const QString dirRel = isPhase ? QStringLiteral("docs/phases/")
                                        : specsDir + QLatin1Char('/');
        // ANTS-3356 — exact `<id>.md`, then a `<id>-*.md` glob so a
        // topic-suffixed project spec (DOOM-0009-path-tracer.md) resolves.
        rel  = resolveSpecRelForId(rootCanonical, dirRel, id);
        full = rootCanonical + QLatin1Char('/') + rel;
        // ANTS-6434 — the spec the id names may be a symlink out of the root.
        if (QFileInfo::exists(full) && !PathValidation::isInsideProject(rootCanonical, full))
            return slErr(QStringLiteral("bad_path"),
                         QStringLiteral("spec_log: %1 resolves outside the project").arg(rel));
    }

    QFileInfo fi(full);
    if (!fi.exists() || !fi.isFile()) {
        return slErr(QStringLiteral("not_found"),
                     QStringLiteral("spec_log: %1 does not exist")
                         .arg(rel));
    }

    const QString op = req.value(QStringLiteral("op")).toString();
    // ANTS-4959 — both Status ops echo the value they started from.
    const bool statusOp = op == QStringLiteral("set_status") ||
                          op == QStringLiteral("append_status");
    if (!statusOp &&
        op != QStringLiteral("append_loop") &&
        op != QStringLiteral("append_inv")) {
        return slErr(QStringLiteral("bad_mode"),
                     QStringLiteral("spec_log: op must be \"set_status\", "
                                    "\"append_status\", \"append_loop\", or "
                                    "\"append_inv\""));
    }

    QFile f(full);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return slErr(QStringLiteral("read_failed"),
                     QStringLiteral("spec_log: cannot open %1").arg(rel));
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    SpecLog::EditResult res;
    QString followedFrom;   // ANTS-5346 — the spec whose pointer was followed
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
    } else if (op == QStringLiteral("append_status")) {
        const QString text = req.value(QStringLiteral("status")).toString();
        if (text.trimmed().isEmpty()) {
            return slErr(QStringLiteral("bad_args"),
                         QStringLiteral("spec_log: append_status requires a "
                                        "non-empty \"status\" (the text to add)"));
        }
        res = SpecLog::appendStatus(content, text);
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
        // ANTS-5346 — a loop-log section that only links the log's own file
        // (documentation.md § 9.1) sends the row there. One hop: the linked
        // file is not searched for a pointer of its own.
        QString logContent = content;
        const QString pointer = SpecLog::loopLogPointer(content);
        if (!pointer.isEmpty()) {
            const QString target = QDir::cleanPath(
                QFileInfo(full).absolutePath() + QLatin1Char('/') + pointer);
            const QString targetRel = QDir(rootCanonical).relativeFilePath(target);
            if (targetRel == QStringLiteral("..") ||
                targetRel.startsWith(QStringLiteral("../")))
                return slErr(QStringLiteral("bad_path"),
                             QStringLiteral("spec_log: %1's loop log points at "
                                            "%2, outside the project. Nothing "
                                            "was written.").arg(rel, pointer));
            if (!QFileInfo(target).isFile())
                return slErr(QStringLiteral("not_found"),
                             QStringLiteral("spec_log: %1's loop log points at "
                                            "%2, which does not exist. Nothing "
                                            "was written.").arg(rel, targetRel));
            QFile lf(target);
            if (!PathValidation::openInsideProject(lf, rootCanonical, target))
                return slErr(QStringLiteral("bad_path"),
                             QStringLiteral("spec_log: %1's loop log points at "
                                            "%2, which resolves outside the "
                                            "project. Nothing was written.")
                                 .arg(rel, targetRel));
            logContent = QString::fromUtf8(lf.readAll());
            followedFrom = rel;
            rel  = targetRel;
            full = target;
        }
        res = SpecLog::appendLoop(logContent, label, body, loopCells);
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
        if (statusOp) {
            out["previous_status"] = res.previousValue;
            out["new_status"]      = res.newValue;   // ANTS-6364
        }
        // ANTS-4353/4364 — say which SHAPE was written and which DIRECTION
        // was inferred, so a caller can see which way the row went instead of
        // re-reading the file to find out.
        if (!res.rowShape.isEmpty()) out["row_shape"] = res.rowShape;
        if (!res.rowOrder.isEmpty()) out["row_order"] = res.rowOrder;
        if (!followedFrom.isEmpty()) out["followed_from"] = followedFrom;
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
    if (statusOp) {
        out["previous_status"] = res.previousValue;
        out["new_status"]      = res.newValue;   // ANTS-6364
    }
    // ANTS-4353/4364 — the shape written and the direction inferred.
    if (!res.rowShape.isEmpty()) out["row_shape"] = res.rowShape;
    if (!res.rowOrder.isEmpty()) out["row_order"] = res.rowOrder;
    // ANTS-5346 — `path` is the file written; this names the spec that led there.
    if (!followedFrom.isEmpty()) out["followed_from"] = followedFrom;
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

// How every git_state op sorts a failed git run into a refusal. `sub` names
// the git subcommand in the message. `badRange` holds the stderr phrases that
// mean an unknown revision, which each subcommand words its own way. Under
// `exit1IsNoMatch` (git grep), exit 1 with empty stderr is a success.
// Returns an empty object when git succeeded.
QJsonObject gitFailure(const GitWrap::Result &g, const QString &sub,
                       const QStringList &badRange = {},
                       bool exit1IsNoMatch = false) {
    if (!g.started) {
        return gitErr("git_missing",
            QStringLiteral("git_state: git binary not found on PATH"));
    }
    if (g.hardKilled) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git %1 exceeded %2 s wall budget")
                .arg(sub).arg(GitWrap::kHardKillMs / 1000),
            g.stderrTail);
    }
    if (g.crashed) {
        return gitErr("git_failed",
            QStringLiteral("git_state: git %1 crashed").arg(sub), g.stderrTail);
    }
    if (g.exitCode == 0 ||
        (exit1IsNoMatch && g.exitCode == 1 && g.stderrTail.isEmpty())) {
        return {};
    }
    // ANTS-1250-INV-11: not-a-git-repo → distinct code.
    const QString s = QString::fromUtf8(g.stderrTail);
    if (s.contains(QStringLiteral("not a git repository"), Qt::CaseInsensitive)) {
        return gitErr("not_git_repo",
            QStringLiteral("git_state: not a git repository"));
    }
    for (const QString &needle : badRange) {
        if (s.contains(needle, Qt::CaseInsensitive)) {
            return gitErr("bad_range",
                QStringLiteral("git_state: range refers to unknown revision"),
                g.stderrTail);
        }
    }
    return gitErr("git_failed",
        QStringLiteral("git_state: git %1 exit %2").arg(sub).arg(g.exitCode),
        g.stderrTail);
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
QString resolveRootCanonical(const ants::RootProvider *roots) {
    QString rootCwd;
    // ANTS-3725 — a null provider falls through to the process cwd rather
    // than refusing, so every verb on this overload stays testable at the
    // handler layer. ANTS-4932 — the provider marshals its own reads, so this
    // is safe on the dispatch worker.
    // ANTS-5098 — a provider that answers empty (a refused marshal at
    // shutdown, or no focused tab) refuses via the caller's anchor-failure
    // code; it never falls back to the process cwd (ANTS-2132 § 2.5).
    if (roots) rootCwd = roots->fallbackRoot();
    else       rootCwd = QDir::currentPath();
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
QString resolveRootCanonical(const ants::RootProvider *roots,
                             const QJsonObject &req) {
    const QString rawCaller =
        req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr =
        ants::resolveCallerCwdRoot(roots, rawCaller);
    switch (rr.source) {
        case ants::ResolvedRoot::Source::EmptyFallback:
        case ants::ResolvedRoot::Source::ServerCwd:
            return resolveRootCanonical(roots);
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
// `resolveRootCanonical(roots, req)`, the `caller_cwd_info` MCP verb
// (ANTS-1400), and the per-tool contract dispatcher (ANTS-1404).
namespace ants {

ResolvedRoot resolveCallerCwdRoot(const RootProvider *roots,
                                  const QString &callerCwd) {
    ResolvedRoot rr;
    // ANTS-3725 — a null provider still resolves an EXPLICIT caller_cwd; what
    // it costs is the fallback and the tab walk, nothing else.
    if (callerCwd.isEmpty()) {
        // Case 1 — empty caller_cwd → the host's fallback (ANTS-4932 § 2.4:
        // the focused tab in the terminal, the process cwd in ants-mcpd).
        rr.source = roots ? roots->fallbackSource()
                          : ResolvedRoot::Source::EmptyFallback;
        if (!roots) return rr;
        const QString fallback = roots->fallbackRoot();
        if (!fallback.isEmpty())
            rr.cwd = QFileInfo(fallback).canonicalFilePath();
        rr.tabIndex = roots->fallbackTab();
        return rr;
    }
    const QString wantCanonical =
        QFileInfo(callerCwd).canonicalFilePath();
    if (wantCanonical.isEmpty()) {
        // Case 4 — present but unresolvable.
        rr.source = ResolvedRoot::Source::Unresolvable;
        return rr;
    }
    // INV-5 — deterministic lowest-index tie-break, owned by the provider.
    if (roots) {
        if (const auto tab = roots->tabForCwd(wantCanonical)) {
            // Case 2 — explicit caller_cwd hits an open tab.
            rr.source   = ResolvedRoot::Source::ExplicitMatch;
            rr.cwd      = wantCanonical;
            rr.tabIndex = *tab;
            return rr;
        }
    }
    // Case 3 — explicit caller_cwd, no open tab matches.
    rr.source = ResolvedRoot::Source::NoMatch;
    rr.cwd    = wantCanonical;
    return rr;
}

QString requiredCallerCwd(const RootProvider *roots, const QJsonObject &req,
                          const QString &verb, QJsonObject *refusal) {
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const ResolvedRoot rr = resolveCallerCwdRoot(roots, callerRaw);
    if (rr.source != ResolvedRoot::Source::Unresolvable && !rr.cwd.isEmpty())
        return rr.cwd;
    *refusal = QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("code"), QStringLiteral("no_project")},
        {QStringLiteral("error"),
         QStringLiteral("%1: caller_cwd \"%2\" does not resolve to a directory")
             .arg(verb, callerRaw)}};
    return QString();
}

// ANTS-4932 § 2.4 — ants-mcpd's fallback is its process cwd, and its roadmap
// is the one a caller_cwd naming that directory would get.
QString ServerCwdRootProvider::fallbackRoot() const {
    return QDir::currentPath();
}

QString ServerCwdRootProvider::fallbackRoadmapPath() const {
    const QString root = QFileInfo(fallbackRoot()).canonicalFilePath();
    return root.isEmpty() ? QString() : rcdetail::findRoadmapUnder(root);
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
QJsonObject runStatusOp(const ants::RootProvider *roots, const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(roots, req);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    QStringList argv;
    argv << QStringLiteral("status")
         << QStringLiteral("--porcelain=v1")
         << QStringLiteral("-b");
    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (const QJsonObject e = gitFailure(g, QStringLiteral("status"));
        !e.isEmpty())
        return e;

    QJsonObject out;
    out["ok"]   = true;
    out["op"]   = QStringLiteral("status");
    QJsonArray files;
    QJsonArray untracked;
    QList<QByteArray> lines = g.stdoutBytes.split('\n');
    // ANTS-5098 — past GitWrap's stdout cap the last line can be a path cut
    // mid-name: drop it, and say the list is incomplete.
    if (g.stdoutTruncated && !lines.isEmpty()) {
        lines.removeLast();
        out["truncated"] = true;
    }
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

// ANTS-1340 / ANTS-5508 — a single `path` or a `paths` array, each validated
// through PathValidation (INV-5 argv `--` follows). False → `err` holds the
// refusal.
static bool gitPathArgv(const QJsonObject &req, const QString &rootCanonical,
                        QStringList &argv, QJsonObject &err) {
    const QJsonValue pathsVal = req.value("paths");
    if (pathsVal.isArray()) {
        for (const QJsonValue &pv : pathsVal.toArray()) {
            const auto pcp = PathValidation::validatePath(
                pv.toString(), rootCanonical,
                QStringLiteral("git_state"), QStringLiteral("paths"));
            if (pcp.bad) { err = pcp.err; return false; }
            if (!pcp.argvForm.isEmpty()) argv << pcp.argvForm;
        }
        return true;
    }
    const auto pc = PathValidation::validatePath(
        req.value("path").toString(), rootCanonical,
        QStringLiteral("git_state"), QStringLiteral("path"));
    if (pc.bad) { err = pc.err; return false; }
    if (!pc.argvForm.isEmpty()) argv << pc.argvForm;
    return true;
}

QJsonObject runLogOp(const ants::RootProvider *roots, const QJsonObject &req) {
    // ANTS-1391: prefer caller_cwd when present.
    const QString rootCanonical = resolveRootCanonical(roots, req);
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

    // ANTS-1340 — a `paths` array lets the lane recent_changes composer
    // (cmdSubsystem) resolve the union of touching commits in ONE git call.
    // `git log -- f1 f2 …` already dedups + date-sorts.
    QStringList pathArgv;
    QJsonObject pathErr;
    if (!gitPathArgv(req, rootCanonical, pathArgv, pathErr)) return pathErr;
    // ANTS-5508 — grep keeps commits whose message holds the text, any case;
    // stat adds each commit's --numstat files.
    const QString grep = req.value("grep").toString();
    const bool wantStat = req.value("stat").toBool(false);

    // Format: SHA<US>SUBJECT<US>DATE(<US>BODY)?<RS>
    // Use 0x1f (US) between fields, 0x1e (RS) between commits.
    const QChar US(0x1f);
    const QChar RS(0x1e);
    const QString fmtNoBody = QStringLiteral("%h\x1f%s\x1f%cs");
    const QString fmtWith   = QStringLiteral("%h\x1f%s\x1f%cs\x1f%b");

    QStringList argv;
    argv << QStringLiteral("log")
         << QStringLiteral("--no-color");
    if (wantStat) {
        // ANTS-5508 — RS opens each record and 0x1d closes its fields, so the
        // numstat lines git prints after them stay with their commit.
        argv << QStringLiteral("--numstat")
             << QStringLiteral("--pretty=format:") + QChar(0x1e)
                    + (wantBody ? fmtWith : fmtNoBody) + QChar(0x1d);
    } else {
        // -z emits NUL between commits. Override the inter-commit separator
        // by adding %x1e at end of format and splitting on RS.
        argv << QStringLiteral("-z")
             << QStringLiteral("--pretty=format:")
                    + (wantBody ? fmtWith : fmtNoBody) + QChar(0x1e);
    }
    if (!grep.isEmpty())
        argv << QStringLiteral("--fixed-strings")
             << QStringLiteral("--regexp-ignore-case")
             << QStringLiteral("--grep=") + grep;
    // Fetch n+1 to detect truncation (INV-3).
    argv << QStringLiteral("-n") << QString::number(n + 1);
    // ANTS-1250-INV-5: argv -- separator before user-derived path(s).
    argv << QStringLiteral("--");
    argv << pathArgv;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (const QJsonObject e = gitFailure(g, QStringLiteral("log"));
        !e.isEmpty())
        return e;

    // Parse: split on 0x1e, then per record split on 0x1f.
    QVector<GitWrap::LogRecord> records;
    bool cut = false;
    if (wantStat) {
        records = GitWrap::parseLogNumstat(g.stdoutBytes);
        // Past GitWrap's stdout cap the last commit's file list may be cut.
        cut = g.stdoutTruncated && !records.isEmpty();
        if (cut) records.removeLast();
    } else {
        const QString stdoutAll = QString::fromUtf8(g.stdoutBytes);
        for (const QString &rec : stdoutAll.split(RS, Qt::SkipEmptyParts)) {
            // Trim leading NUL that -z inserts between records.
            QString r = rec;
            while (!r.isEmpty() && r.at(0) == QLatin1Char('\0')) r = r.mid(1);
            if (r.isEmpty()) continue;
            records.append({r.split(US), {}});
        }
    }
    QJsonArray commits;
    for (const GitWrap::LogRecord &rec : std::as_const(records)) {
        const QStringList &fields = rec.fields;
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
        if (wantStat) {
            QJsonArray files;
            for (const GitWrap::NumstatRow &f : rec.files) {
                QJsonObject fo;
                fo["path"]    = f.path;
                // null for a binary file, as op:"diff" reports it
                fo["added"]   = f.added   < 0 ? QJsonValue() : QJsonValue(f.added);
                fo["removed"] = f.removed < 0 ? QJsonValue() : QJsonValue(f.removed);
                files.append(fo);
            }
            c["files"] = files;
        }
        commits.append(c);
    }
    bool truncated = cut;
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

QJsonObject runDiffOp(const ants::RootProvider *roots, const QJsonObject &req) {
    // ANTS-1391: prefer caller_cwd when present.
    const QString rootCanonical = resolveRootCanonical(roots, req);
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
    QStringList pathArgv;   // ANTS-5508 — path or paths[]
    QJsonObject pathErr;
    if (!gitPathArgv(req, rootCanonical, pathArgv, pathErr)) return pathErr;

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
    // ANTS-5508 — only the lines the diff added; added_match narrows them.
    const QString addedMatch = req.value("added_match").toString();
    const bool addedOnly = req.value("added_lines").toBool() || !addedMatch.isEmpty();
    if (addedOnly && hunks) {
        return gitErr("bad_args",
            QStringLiteral("git_state: \"added_lines\" and \"hunks\" are "
                           "mutually exclusive"));
    }

    QStringList argv;
    argv << QStringLiteral("diff")
         << QStringLiteral("--no-color");
    if (addedOnly)  argv << QStringLiteral("--unified=0");
    else if (hunks) argv << QStringLiteral("--unified=%1").arg(context);
    else            argv << QStringLiteral("--numstat");
    if (staged)           argv << QStringLiteral("--cached");
    if (!range.isEmpty()) argv << range;
    argv << QStringLiteral("--");
    argv << pathArgv;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (const QJsonObject e = gitFailure(g, QStringLiteral("diff"),
            {QStringLiteral("unknown revision"), QStringLiteral("bad revision")});
        !e.isEmpty())
        return e;

    // The envelope flags which diff the caller received.
    auto stampKind = [&](QJsonObject &out) {
        if (staged)            out["staged"]   = true;
        else if (worktreeDiff) out["worktree"] = true;
        else                   out["range"]    = range;
    };

    if (addedOnly) {
        QJsonArray afiles;
        int lineTotal = 0;
        for (const GitWrap::DiffFile &df :
             GitWrap::parseDiffHunks(g.stdoutBytes, true)) {
            const QVector<GitWrap::AddedLine> lines =
                GitWrap::addedLines(df, addedMatch);
            if (lines.isEmpty()) continue;
            QJsonArray ls;
            for (const GitWrap::AddedLine &l : lines) {
                QJsonObject lo;
                lo["line"] = l.line;
                lo["text"] = l.text;
                ls.append(lo);
            }
            QJsonObject f;
            f["path"]  = df.path;
            f["added"] = ls;
            afiles.append(f);
            lineTotal += lines.size();
        }
        QJsonObject out;
        out["ok"] = true;
        out["op"] = QStringLiteral("diff");
        stampKind(out);
        if (!addedMatch.isEmpty()) out["added_match"] = addedMatch;
        out["files"] = afiles;
        QJsonObject totals;
        totals["files"] = afiles.size();
        totals["lines"] = lineTotal;
        out["totals"] = totals;
        if (g.stdoutTruncated) out["truncated"] = true;
        return out;
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
        stampKind(out);
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
    QList<QByteArray> lines = g.stdoutBytes.split('\n');
    // ANTS-5098 — as runStatusOp: drop a cut last line, flag the rest.
    const bool cut = g.stdoutTruncated && !lines.isEmpty();
    if (cut) lines.removeLast();
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
    stampKind(out);
    out["files"]  = files;
    out["totals"] = totals;
    if (cut) out["truncated"] = true;
    return out;
}

// ANTS-5508 — every line still using `old`, as a whole word, in the tracked
// files: at the range's last commit, or in the working tree without one.
QJsonObject runRenameSweepOp(const ants::RootProvider *roots,
                             const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(roots, req);
    if (rootCanonical.isEmpty()) {
        return gitErr("bad_path",
            QStringLiteral("git_state: project root does not exist"));
    }
    const QString old = req.value("old").toString();
    if (old.isEmpty()) {
        return gitErr("bad_args",
            QStringLiteral("git_state: rename_sweep needs \"old\""));
    }
    const QString range = req.value("range").toString();
    if (!range.isEmpty() && !isValidRange(range)) {
        return gitErr("bad_range",
            QStringLiteral("git_state: \"range\" failed validation"));
    }
    QStringList pathArgv;
    QJsonObject pathErr;
    if (!gitPathArgv(req, rootCanonical, pathArgv, pathErr)) return pathErr;

    const QString rev = range.isEmpty() ? QString() : GitWrap::rangeTip(range);
    QStringList argv;
    // -e keeps an `old` that starts with `-` from reading as an option.
    argv << QStringLiteral("grep") << QStringLiteral("--no-color")
         << QStringLiteral("-n") << QStringLiteral("-z")
         << QStringLiteral("-I") << QStringLiteral("-F")
         << QStringLiteral("-w") << QStringLiteral("-e") << old;
    if (!rev.isEmpty()) argv << rev;
    argv << QStringLiteral("--") << pathArgv;

    GitWrap::Result g = GitWrap::run(rootCanonical, argv);
    if (const QJsonObject e = gitFailure(g, QStringLiteral("grep"),
            {QStringLiteral("unable to resolve revision")}, true);
        !e.isEmpty())
        return e;

    QByteArray bytes = g.stdoutBytes;
    if (g.stdoutTruncated) {   // drop a hit cut mid-line
        const int nl = bytes.lastIndexOf('\n');
        bytes.truncate(nl < 0 ? 0 : nl + 1);
    }
    QJsonArray hits;
    QSet<QString> files;
    for (const GitWrap::GrepHit &h : GitWrap::parseGrepZ(bytes, rev)) {
        QJsonObject o;
        o["path"] = h.path;
        o["line"] = h.line;
        o["text"] = h.text;
        hits.append(o);
        files.insert(h.path);
    }
    QJsonObject out;
    out["ok"]  = true;
    out["op"]  = QStringLiteral("rename_sweep");
    out["old"] = old;
    if (rev.isEmpty()) out["worktree"] = true;
    else               out["rev"]      = rev;
    out["hits"]  = hits;
    out["files"] = files.size();
    if (g.stdoutTruncated) out["truncated"] = true;
    return out;
}
}  // namespace rcdetail

// ANTS-2129 — audit_falsepos_log: append a confirmed false positive to
// <root>/.ants_review_falsepos.jsonl via the atomic O_APPEND in
// ants::falsepos::appendEntry. See docs/specs/ANTS-2129-audit-falsepos-log-mcp-write.md.
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
        "review-code and check-code, plus the cold_eyes_brief and "
        "test_audit_brief verbs (ANTS-5595)");
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
namespace {

struct AdParsed {
    ants::auditfp::RecordRequest request;
    bool computed = false;
};

// One finding's arguments. `item` is the call itself on the single form and
// one element of entries[] on the batch, where `rule`, `reason` and
// `replace_reason` fall back to the call's. Returns the bad_args message, or
// an empty string on success.
QString adParse(const QJsonObject &item, const QJsonObject &req, AdParsed *out) {
    const auto shared = [&](const QString &key) {
        return item.contains(key) ? item.value(key) : req.value(key);
    };

    const QString rule = shared(QStringLiteral("rule")).toString().trimmed();
    if (rule.isEmpty()) {
        return QStringLiteral("audit_dismiss: `rule` is required (the audit "
                              "check id the finding came from)");
    }

    // Two input shapes: a caller-supplied fingerprint, or (file, message)
    // from which the server computes it. Computing server-side is the
    // preferred form — it guarantees the same hash the engine will look up.
    static const QRegularExpression rxFp(QStringLiteral("^[0-9a-f]{16}$"));
    QString fingerprint =
        item.value(QStringLiteral("fingerprint")).toString().trimmed();
    if (!fingerprint.isEmpty()) {
        if (!rxFp.match(fingerprint).hasMatch()) {
            return QStringLiteral("audit_dismiss: `fingerprint` must be 16 "
                                  "lowercase hex chars (got \"%1\")")
                .arg(fingerprint);
        }
    } else {
        const QString file = item.value(QStringLiteral("file")).toString();
        const QString message =
            item.value(QStringLiteral("message")).toString();
        if (file.isEmpty() || message.isEmpty()) {
            return QStringLiteral("audit_dismiss: pass either `fingerprint`, "
                                  "or both `file` and `message` so the server "
                                  "can compute it");
        }
        fingerprint = ants::auditfp::computeFingerprint(file, rule, message);
        out->computed = true;
    }

    out->request.entry.fingerprint = fingerprint;
    out->request.entry.rule        = rule;
    out->request.entry.reason      = shared(QStringLiteral("reason")).toString();
    out->request.replaceReason =
        shared(QStringLiteral("replace_reason")).toBool();
    if (out->request.replaceReason
        && out->request.entry.reason.trimmed().isEmpty()) {
        return QStringLiteral("audit_dismiss: `replace_reason` needs a "
                              "non-empty `reason` to replace the stored one "
                              "with");
    }
    return {};
}

// What one finding's reply says, on both forms. `reason` and `timestamp` are
// what the ledger holds, which is not what the caller sent when nothing was
// written.
QJsonObject adResultFields(const AdParsed &parsed,
                           const ants::auditfp::RecordResult &result) {
    using ants::auditfp::RecordOutcome;
    QJsonObject o;
    o["fingerprint"] = result.entry.fingerprint;
    // True when the server derived the fingerprint from (file, rule, message)
    // rather than taking the caller's — lets a caller confirm the hash it
    // will need if it wants to reference the entry later.
    o["computed"]    = parsed.computed;
    o["rule"]        = result.entry.rule;
    o["reason"]      = result.entry.reason;
    o["timestamp"]   = result.entry.timestamp;
    switch (result.outcome) {
    case RecordOutcome::Recorded:
        o["outcome"] = QStringLiteral("recorded");
        break;
    case RecordOutcome::AlreadyRecorded:
        o["outcome"] = QStringLiteral("already_recorded");
        if (!parsed.request.entry.reason.isEmpty()
            && parsed.request.entry.reason != result.entry.reason) {
            o["hint"] = QStringLiteral("the stored reason was kept; pass "
                                       "replace_reason:true to replace it");
        }
        break;
    case RecordOutcome::ReasonReplaced:
        o["outcome"] = QStringLiteral("reason_replaced");
        break;
    }
    return o;
}

QJsonObject adWriteFailed(const QString &path) {
    return mcp::refusal(QStringLiteral("write_failed"),
        QStringLiteral("audit_dismiss: could not append to %1").arg(path));
}

// ANTS-5603 — entries[]: N findings in one lock, one read and one write. A bad
// entry lands in skipped[] and costs only itself; a batch that applied nothing
// is a refusal, as on feedback_log's assign_id_batch.
QJsonObject adRecordBatch(const QJsonObject &req, const QString &root,
                          const QString &path, bool dryRun) {
    for (const char *key : {"fingerprint", "file", "message"}) {
        if (req.contains(QLatin1String(key))) {
            return mcp::refusal(QStringLiteral("bad_args"),
                QStringLiteral("audit_dismiss: `%1` names one finding, so it "
                               "goes inside each entry, not beside `entries`")
                    .arg(QLatin1String(key)));
        }
    }
    const QJsonArray entries = req.value(QStringLiteral("entries")).toArray();
    if (entries.isEmpty()) {
        return mcp::refusal(QStringLiteral("bad_args"),
            QStringLiteral("audit_dismiss: `entries` must be a non-empty array"));
    }

    QList<AdParsed> parsed;
    QList<int> indexes;
    QJsonArray skipped;
    for (int i = 0; i < entries.size(); ++i) {
        AdParsed p;
        const QString error = adParse(entries.at(i).toObject(), req, &p);
        if (!error.isEmpty()) {
            QJsonObject row = mcp::refusal(QStringLiteral("bad_args"), error);
            row.remove(QStringLiteral("ok"));
            row["index"] = i;
            skipped.append(row);
            continue;
        }
        parsed.append(p);
        indexes.append(i);
    }
    if (parsed.isEmpty()) {
        QJsonObject e = mcp::refusal(QStringLiteral("bad_args"),
            QStringLiteral("audit_dismiss: applied no entry — every one was "
                           "refused"));
        rcSetAllRefusedSkipped(e, skipped);
        return e;
    }

    QList<ants::auditfp::RecordRequest> requests;
    for (const AdParsed &p : parsed) requests.append(p.request);
    const auto results = ants::auditfp::recordEntries(root, requests, dryRun);
    if (!results) return adWriteFailed(path);

    QJsonArray applied;
    bool changed = false;
    for (qsizetype i = 0; i < results->size(); ++i) {
        QJsonObject row = adResultFields(parsed.at(i), results->at(i));
        row["index"] = indexes.at(i);
        applied.append(row);
        changed = changed || results->at(i).outcome
                                 != ants::auditfp::RecordOutcome::AlreadyRecorded;
    }

    QJsonObject out;
    out["ok"]            = true;
    if (dryRun) out["dry_run"] = true;
    out["path"]          = path;
    out["applied"]       = applied;
    out["applied_count"] = applied.size();
    out["skipped"]       = skipped;
    out["skipped_count"] = skipped.size();
    out["changed"]       = changed;
    return out;
}

}  // namespace

QJsonDocument RemoteControl::cmdAuditDismiss(const QJsonObject &req) {
    // Root resolution mirrors cmdAuditFalseposLog: caller_cwd is Required by
    // contract, so the canonical path is all we need (no tab fallback).
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString root = QFileInfo(callerRaw).canonicalFilePath();
    if (root.isEmpty()) {
        return QJsonDocument(mcp::refusal(QStringLiteral("no_project"),
            QStringLiteral("audit_dismiss: caller_cwd does not resolve to "
                           "an existing project directory")));
    }

    const QString path = root + QStringLiteral("/.audit_cache/learned-fp.jsonl");
    // dry_run: validate + compute + report the would-be record without the
    // append (ANTS-2227's convention on the sibling write verbs).
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    if (req.contains(QStringLiteral("entries")))
        return QJsonDocument(adRecordBatch(req, root, path, dryRun));

    AdParsed parsed;
    const QString error = adParse(req, req, &parsed);
    if (!error.isEmpty())
        return QJsonDocument(mcp::refusal(QStringLiteral("bad_args"), error));

    const auto results =
        ants::auditfp::recordEntries(root, {parsed.request}, dryRun);
    if (!results) return QJsonDocument(adWriteFailed(path));

    QJsonObject out = adResultFields(parsed, results->first());
    out["ok"]   = true;
    if (dryRun) out["dry_run"] = true;
    out["path"] = path;
    return QJsonDocument(out);
}

