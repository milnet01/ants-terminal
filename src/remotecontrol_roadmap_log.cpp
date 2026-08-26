// ANTS-3833 TU 5/17 — Roadmap single-item write ops.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "roadmapfoldin.h"
#include "roadmapclock.h"   // ANTS-4501 § 2.2 — the injectable "today"
#include "wrapmatch.h"     // ANTS-4550 — the wrapped-match rule
#include "readregion.h"   // ANTS-4556 — the shared section-slug ranker
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <algorithm>   // ANTS-4070 — std::sort over the rotation's move set

using namespace rcdetail;  // ANTS-3833

// ---------------------------------------------------------------- ANTS-4501 --
// The date stamps. Spec: docs/specs/ANTS-4501-roadmap-report.md § 2.2.
//
// They live HERE, in the callers, and deliberately not inside
// RoadmapStore::setItemField(): Loader::rebuildElements() reaches that method
// too, so a stamp placed there would date every migrated row to the migration
// day on the next re-run — the `last_modified` rewrite § 5 rejects, and INV-4
// with it. The migration loader is exempt by construction, because it never
// comes through this TU.
//
// Every one reads "today" through RoadmapClock rather than QDate::currentDate()
// so INV-5 and INV-6 can advance the day between two writes; on one real clock
// both writes land on the same date and those clauses pass against exactly the
// build they exist to catch.

// The local calendar date, formatted to satisfy the item table's GLOB CHECK.
// Local and not UTC: a user asking "what did I close today?" means their day.
QString rcdetail::rlStampToday() {
    return RoadmapClock::today().toString(QStringLiteral("yyyy-MM-dd"));
}

// `last_modified`, once per op rather than once per column: the value is the
// same date either way, and the column records when the item last changed, not
// how many of its columns moved. Provenance is `store-generated`
// (roadmap-data-model.md § 7.7) — no author supplied this value.
bool rcdetail::rlStampModified(RoadmapStore &store, qint64 itemPk, QString *err) {
    return store.setItemField(itemPk, QStringLiteral("last_modified"),
                              rlStampToday(),
                              QStringLiteral("store-generated"), err);
}

// `shipped` moves ONLY on a transition: set entering `shipped`, cleared leaving
// it. A write to an item that was already shipped and still is moves nothing
// (INV-5) — that is what stops one re-render dating the whole backlog to today,
// after which every throughput figure is wrong in the same direction. A
// reopened item carries no closure date (INV-6).
bool rcdetail::rlStampShipped(RoadmapStore &store, qint64 itemPk,
                           const QString &oldStatus, const QString &newStatus,
                           QString *err) {
    const bool was = oldStatus == QLatin1String("shipped");
    const bool now = newStatus == QLatin1String("shipped");
    if (was == now)
        return true;
    if (now)
        return store.setItemField(itemPk, QStringLiteral("shipped"),
                                  rlStampToday(),
                                  QStringLiteral("store-generated"), err);
    return store.clearItemField(itemPk, QStringLiteral("shipped"),
                                QStringLiteral("store-generated"), err);
}

// ANTS-1424 — append path, split out of cmdRoadmapLog (ANTS-1433) so a
// test can drive it without the m_main guard. op-dispatch + m_main
// guard stay in cmdRoadmapLog; everything from field validation onward
// lives here.
QJsonDocument RemoteControl::cmdRoadmapLogAppend(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        if (code == QStringLiteral("missing_field") ||
            code == QStringLiteral("no_roadmap")) {
            QJsonObject ex;
            ex[QStringLiteral("op")]         = QStringLiteral("append");
            ex[QStringLiteral("caller_cwd")] = QStringLiteral("<your $PWD>");
            ex[QStringLiteral("section")]    = QStringLiteral("<roadmap H2/H3 slug>");
            ex[QStringLiteral("status")]     = QStringLiteral("planned");
            ex[QStringLiteral("headline")]   = QStringLiteral("<one-line bullet headline>");
            ex[QStringLiteral("kind")]       = QStringLiteral("implement");
            ex[QStringLiteral("source")]     = QStringLiteral("<origin tag>");
            env[QStringLiteral("example")] = ex;
        }
        return QJsonDocument(env);
    };

    // ANTS-1424-INV-6: validate every required field before any IO.
    // Anchor for the source-scrape regression test.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    const QString section =
        req.value(QStringLiteral("section")).toString();
    const QString status =
        req.value(QStringLiteral("status")).toString();
    const QString headline =
        req.value(QStringLiteral("headline")).toString();
    const QString kind =
        req.value(QStringLiteral("kind")).toString();
    const QString source =
        req.value(QStringLiteral("source")).toString();

    if (callerRaw.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    }
    if (section.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: section is required"));
    }

    // ANTS-4377 — `note` belongs to op:"flip"/"annotate"; op:"append" takes
    // `body`. Both are top-level optional strings on one verb differing only
    // by which op consumes them, and the unused one was DISCARDED IN SILENCE.
    // A caller who sent 2.3 KB of body prose as `note` got ok:true with a
    // plausible bytes_written — the headline, Kind and Source lines are real
    // bytes — and a bullet with no body at all, which is where the
    // measurement, the reasoning and the blockers live. They caught it only
    // because the byte count looked small; a shorter note passes unnoticed.
    // Refuse rather than discard: the verb already uses bad_op_combo for
    // other op/argument mismatches. Placed ahead of the pass-headings route
    // so it holds on both dialects.
    if (req.contains(QStringLiteral("note"))) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: `note` is the parameter for "
                           "op:\"flip\" / op:\"annotate\" — op:\"append\" "
                           "takes `body`. Nothing was written; re-send with "
                           "`body`."));
    }

    // ANTS-2126 — pass-headings roadmaps route to the heading-format
    // writer here, BEFORE the GFM status/kind/source validation +
    // counter allocation below: pass append ignores kind/source,
    // validates status/pass itself (INV-3 bad_args), and never touches
    // .roadmap-counter (INV-10). One extra read+parse on the GFM path is
    // the cost of detecting before the counter machinery; appends are a
    // low-frequency op so the absolute cost is negligible.
    {
        const QString cc = QFileInfo(callerRaw).canonicalFilePath();
        const QString rp = cc.isEmpty() ? QString() : findRoadmapUnder(cc);
        if (!rp.isEmpty()) {
            QFile pf(rp);
            if (pf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString md = QString::fromUtf8(pf.readAll());
                pf.close();
                if (rcBulletsArePassHeadings(rlParse(md, cc)))   // ANTS-3771
                    return cmdRoadmapLogPassAppend(req, rp, md);
            }
        }
    }

    if (status.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: status is required"));
    }
    if (headline.isEmpty()) {
        return rlErr(QStringLiteral("headline_empty"),
            QStringLiteral("roadmap_log: headline must not be empty"));
    }
    if (kind.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: kind is required"));
    }
    if (source.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: source is required"));
    }

    // ANTS-1424-INV-5: status → emoji map. Word form is the wire
    // contract; the verb writes the emoji.
    QString statusEmoji;
    if      (status == QLatin1String("planned"))     statusEmoji = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
    else if (status == QLatin1String("in-progress")) statusEmoji = QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
    else if (status == QLatin1String("shipped"))     statusEmoji = QString::fromUtf8("\xE2\x9C\x85");     // ✅
    else if (status == QLatin1String("considered"))  statusEmoji = QString::fromUtf8("\xF0\x9F\x92\xAD"); // 💭
    else {
        return rlErr(QStringLiteral("bad_status"),
            QStringLiteral("roadmap_log: unknown status \"%1\" — "
                           "expected planned / in-progress / "
                           "shipped / considered").arg(status));
    }

    // ANTS-1424 — kind enum check. Mirrors the schema's enum list.
    static const QSet<QString> validKinds = {
        QStringLiteral("implement"),    QStringLiteral("fix"),
        QStringLiteral("audit-fix"),    QStringLiteral("review-fix"),
        QStringLiteral("doc"),          QStringLiteral("doc-fix"),
        QStringLiteral("refactor"),     QStringLiteral("test"),
        QStringLiteral("chore"),        QStringLiteral("release"),
        QStringLiteral("perf"),         QStringLiteral("security"),
        QStringLiteral("feature"),      QStringLiteral("enhancement"),
        QStringLiteral("investigate"),  QStringLiteral("research"),
        QStringLiteral("accessibility"),QStringLiteral("optimize"),
        QStringLiteral("package"),      QStringLiteral("marketing"),
        QStringLiteral("ux"),
    };
    if (!validKinds.contains(kind)) {
        return rlErr(QStringLiteral("bad_kind"),
            QStringLiteral("roadmap_log: unknown kind \"%1\" — see "
                           "docs/standards/roadmap-format.md § 3.5.3 "
                           "for recognised values").arg(kind));
    }

    // Resolve ROADMAP.md path under caller_cwd. Anchored via
    // canonicalFilePath; if the path doesn't resolve to a real
    // directory, return no_roadmap rather than walking parents.
    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty()) {
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    }
    // ANTS-1459 — shared findRoadmapUnder helper widens the search
    // to docs/, docs/private/, docs/internal/, .github/.
    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty()) {
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));
    }

    // Counter path next to the RESOLVED ROADMAP.md. ANTS-3350 — when
    // caller_cwd is a subdirectory, findRoadmapUnder resolved the roadmap in a
    // parent; the counter lives beside it, not under caller_cwd.
    const QString counterPath =
        QFileInfo(roadmapPath).absolutePath() + QLatin1Char('/') +
        QStringLiteral(".roadmap-counter");

    // ANTS-1905 — id_strategy switch. Default "counter" (back-compat
    // ANTS-NNNN allocator). "stable_prefix" lets a project that uses
    // stable string IDs (Sh4, Ts20-FL1, MT8) drive op:append by passing
    // the new id explicitly via `stable_id` instead of bumping
    // .roadmap-counter. Unknown values refuse with bad_args.
    QString idStrategy =
        req.value(QStringLiteral("id_strategy")).toString();
    if (!idStrategy.isEmpty() &&
        idStrategy != QStringLiteral("counter") &&
        idStrategy != QStringLiteral("stable_prefix")) {
        return rlErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: id_strategy must be "
                           "\"counter\" or \"stable_prefix\""));
    }

    // ANTS-2076 — explicit counter-ID prefix override (counter strategy
    // only; ignored under stable_prefix where stable_id carries the full
    // id). Validated here; resolution precedence (id_prefix > sniffed >
    // project-dir default) lives in rlResolveCounterPrefix.
    const QString idPrefixArg =
        req.value(QStringLiteral("id_prefix")).toString();
    if (!idPrefixArg.isEmpty() &&
        !kIdPrefixShape.match(idPrefixArg).hasMatch()) {
        return rlErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: id_prefix \"%1\" must contain a "
                           "letter and be 1-16 chars of [A-Za-z0-9_-] "
                           "(e.g. ANTS, 3D_E) — ANTS-3492").arg(idPrefixArg));
    }
    const bool dryRun =
        req.value(QStringLiteral("dry_run")).toBool();

    static const QRegularExpression kStableIdShape(
        QStringLiteral("^[A-Za-z][A-Za-z0-9_-]+$"));

    // ANTS-1905 — explicit stable_prefix strategy: validate stable_id and skip
    // the counter machinery. Works regardless of whether .roadmap-counter
    // exists (a project that USES the counter for some bullets and stable IDs
    // for others is pathological but the verb stays predictable: it honours
    // whichever strategy the caller named).
    //
    // ANTS-3809 hoisted this out of the counter block below so the store path
    // shares it. Behaviour-identical: under this strategy the counter file was
    // never opened either way, and nothing between here and its old home did
    // any IO.
    QString stableId;
    const bool useStablePrefix =
        (idStrategy == QStringLiteral("stable_prefix"));
    if (useStablePrefix) {
        stableId = req.value(QStringLiteral("stable_id")).toString();
        if (stableId.isEmpty()) {
            return rlErr(QStringLiteral("missing_field"),
                QStringLiteral("roadmap_log: id_strategy="
                               "\"stable_prefix\" requires "
                               "`stable_id` (the full ID string, "
                               "e.g. \"Ts20-SP6\")"));
        }
        if (!kStableIdShape.match(stableId).hasMatch()) {
            return rlErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: stable_id \"%1\" "
                               "does not match the stable-prefix "
                               "shape ^[A-Za-z][A-Za-z0-9_-]+$")
                    .arg(stableId));
        }
        QString declCode;   // ANTS-3771 § 2.3 — rlDeclaredIdRefusal()'s doc
        const QString declMsg = rcdetail::rlDeclaredIdRefusal(
            stableId, rlDecl(callerCanonical), &declCode);
        if (!declMsg.isEmpty())
            return rlErr(declCode, QStringLiteral("roadmap_log: ") + declMsg);
    }

    // ANTS-3809 § 2.2 — the store path, AHEAD of the counter machinery below.
    // § 2.3 allocates from the store and leaves .roadmap-counter alone; the
    // block below would otherwise auto-create, seed or refuse on a counter file
    // a migrated project does not allocate from. The extra read is the one the
    // pass-headings gate above already pays, for the same reason and at the
    // same low frequency.
    {
        // ANTS-3863 § 1 — the near-miss site. This text has exactly ONE other
        // consumer, rlStoreCounterPrefix()'s last fallback below, which fires
        // only after an explicit id_prefix and the store's own idPrefixFor()
        // have both come up empty — and a migrated project normally has a
        // stored prefix. So the provider moves the body read onto that rare
        // fallback instead of paying 3 MiB for it at the top of the block.
        // openFailed() forces the open, which is what keeps this refusal here.
        auto storeText = RoadmapSource::RoadmapText::fromFile(roadmapPath);
        if (storeText.openFailed()) {
            return rlErr(QStringLiteral("roadmap_read_failed"),
                QStringLiteral("roadmap_log: could not read \"%1\"")
                    .arg(roadmapPath));
        }

        RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
        QString seamErr;
        const auto target =
            roadmapWriteTarget(callerCanonical, storeText, &why, &seamErr);
        QJsonObject refusal;
        if (rcRoadmapSourceRefused(refusal, why, seamErr))
            return QJsonDocument(refusal);
        if (target) {
            RoadmapStore &store = *target->store;
            const qint64 projectId = target->projectId;

            // § 2.2 — create_section and bundle_row resolve a SECTION rather
            // than an item, through findSection(); so does append, which files
            // its new item into one.
            const auto sectionId =
                store.findSection(projectId, section, &seamErr);
            if (!sectionId) {
                // ANTS-4591 — rank near-misses here too. ANTS-4556 gave the
                // FILE-backed refusal `candidates[]` + `sections_total`, and
                // this arm — which every migrated project takes — kept a bare
                // message under a different code. So the feature reached the
                // path almost nobody runs, which is the same shape ANTS-4634
                // had to repair for `would_be_id`.
                //
                // Same shared ranker, never a second copy. A hint, never a
                // resolution: the write still refuses, because guessing which
                // section was meant is how a composed body lands in the wrong
                // one.
                QJsonObject env;
                env["ok"]    = false;
                env["code"]  = QStringLiteral("section_not_found");
                env["error"] = QStringLiteral(
                    "roadmap_log: section \"%1\" is not in the "
                    "roadmap store").arg(section);
                // listSectionsOrdered, not listSections: the ranker does not
                // read document order, but ANTS-4620 deliberately un-exempted
                // this half of the TU split so a new caller here is caught,
                // and weakening that guard to skip a sort on a refusal path
                // would be a bad trade.
                if (const auto sections =
                        store.listSectionsOrdered(projectId)) {
                    QStringList slugs;
                    slugs.reserve(sections->size());
                    for (const RoadmapStore::SectionRow &sr : *sections)
                        slugs << sr.slug;
                    env["candidates"] = QJsonArray::fromStringList(
                        ReadRegion::rankSectionCandidates(section, slugs));
                    env["sections_total"] = int(slugs.size());
                }
                return QJsonDocument(env);
            }

            // End-of-section position, pinned exactly as bundle_row's create
            // case pins it: element carries UNIQUE (section_id, position) and
            // item rows occupy positions too, so "at the end" left unpinned is a
            // constraint violation rather than a mis-placement.
            const auto elements = store.listElements(*sectionId, &seamErr);
            if (!elements)
                return rlErr(QStringLiteral("store_failed"), seamErr);
            int maxPos = -1;
            for (const RoadmapStore::ElementRow &e : *elements)
                maxPos = std::max(maxPos, e.position);

            // § 2.3 — allocation. `stable_prefix` is untouched by cutover: it
            // takes the caller's id verbatim and consults neither idHighWater()
            // nor raiseIdHighWater(), because a stable string id is not a
            // counter value and seeding a counter from one would corrupt the
            // next counter-project allocation on the same store.
            QString idStr = stableId;
            QString prefix;
            qint64 allocated = 0;
            if (!useStablePrefix) {
                prefix = rlStoreCounterPrefix(store, projectId, idPrefixArg,
                                              storeText, callerCanonical);
                // ANTS-4631 — the store's own id columns decide this, with
                // no reference to the rendered file. The corpus floor that
                // used to live inside rlStoreIdHighWater() read a documented
                // sample id as an allocation and burned ~5,370 ids.
                const qint64 highWater =
                    rlStoreIdHighWater(store, projectId, prefix);
                allocated = highWater + 1;
                if (req.contains(QStringLiteral("id_hint"))) {
                    // The rule is the markdown path's, unchanged; only the
                    // comparison's right-hand side moves to the floor
                    // allocation itself uses.
                    const qint64 hint =
                        req.value(QStringLiteral("id_hint")).toInteger();
                    if (hint <= highWater)
                        return rlErr(QStringLiteral("id_taken"),
                            QStringLiteral("roadmap_log: id_hint %1 is at or "
                                           "below the highest existing %2 id "
                                           "(%3) — pick a value > %3 or omit "
                                           "the hint")
                                .arg(hint).arg(prefix).arg(highWater));
                    allocated = hint;
                }
                idStr = allocated > 9999
                            ? QStringLiteral("%1-%2").arg(prefix).arg(allocated)
                            : QStringLiteral("%1-%2").arg(prefix)
                                  .arg(allocated, 4, 10, QLatin1Char('0'));
            }

            RoadmapStore::ItemWrite w;
            w.projectId = projectId;
            w.id        = idStr;
            // roadmap-data-model.md § 7.1: `synthesised` is "the model made it
            // … and every id the store allocates after cutover". A caller's
            // `stable_id` is the same kind of thing — `parsed` would claim it
            // matched the grammar in source text, and `quarantined` would file
            // a first-class project id with the junk (ANTS-3761 sorts those
            // last). The column CHECKs exactly these three.
            w.idOrigin  = QStringLiteral("synthesised");
            w.status    = status;
            w.headline  = rcSanitizeBulletField(headline, 500);
            w.sectionId = *sectionId;
            w.position  = maxPos + 1;
            // INV-10 — provenance is per field. `status` and `headline` came
            // from the caller; the five trailer keys and `body` are added by
            // the fill below.
            // ANTS-3838 — `id` is the one field here the caller may not have
            // supplied, so it is the one that branches. roadmap-data-model.md
            // § 7.7 reserves `store-generated` for "the `write
            // (store-populated)` fields of § 4.1", and § 4.1 marks `id`
            // exactly that: a counter / high-water allocation is the store's
            // value, not an author's. Only an `id_strategy:"stable_prefix"`
            // id was genuinely asserted by the caller. Distinct from
            // `idOrigin`, which is `synthesised` on BOTH branches — that
            // records how the id was FORMED, this records who SUPPLIED it.
            w.provenance = QJsonObject{
                {QStringLiteral("id"),
                 useStablePrefix ? QStringLiteral("asserted")
                                 : QStringLiteral("store-generated")},
                {QStringLiteral("status"),   QStringLiteral("asserted")},
                {QStringLiteral("headline"), QStringLiteral("asserted")},
            };

            // ANTS-4501 § 2.2 — `created` and `last_modified` at insert, on the
            // store-write path only. § 2.2's `shipped` rule is a transition
            // INTO shipped, and an insert whose status IS shipped is that
            // transition: there is no earlier value that was already shipped,
            // so INV-5's re-stamp trap cannot fire here. Left NULL the row
            // would be shipped-with-no-date on the one path where the date is
            // known exactly, and only an opt-in backfill would ever repair it.
            w.created      = rlStampToday();
            w.lastModified = w.created;
            if (w.status == QLatin1String("shipped"))
                w.shipped = w.created;
            w.provenance.insert(QStringLiteral("created"),
                                QStringLiteral("store-generated"));
            w.provenance.insert(QStringLiteral("last_modified"),
                                QStringLiteral("store-generated"));
            if (!w.shipped.isEmpty())
                w.provenance.insert(QStringLiteral("shipped"),
                                    QStringLiteral("store-generated"));

            // § 2.5 / § 2.6 — body, the five trailer columns, and the refusal
            // for a column the body would out-vote.
            QStringList scrubbedNames;
            int scrubbedUnnamed = 0;              // ANTS-4572
            QString shadowErr;
            if (!rlFillItemBody(req, w, scrubbedNames, &shadowErr,
                                &scrubbedUnnamed))
                return rlErr(QStringLiteral("body_shadowed"),
                    QStringLiteral("roadmap_log: %1").arg(shadowErr));

            const auto mutate = [&](QString *err) -> bool {
                if (!store.putItem(w, err))
                    return false;
                if (useStablePrefix)
                    return true;
                // § 2.3 — record the allocation so the next one cannot reissue
                // it. Advances only upward, so an id_hint below the store's
                // high-water (already refused above) could not lower it anyway.
                return store.raiseIdHighWater(projectId, prefix, allocated, err);
            };

            // ANTS-4426 — the advisory's records, taken from the STORE and not
            // from the file. This was `storeText.full()` + parseBullets over
            // the whole of ROADMAP.md, which is 3 MiB on this project and was
            // the one consumer keeping ANTS-3863's saving off op:append.
            //
            // Still PRE-write, and the position is still the whole of it:
            // `mutate` puts the new item inside commitAndRender(), so reading
            // the store after it would find the new bullet and report it as a
            // duplicate of itself — the same trap the file read had.
            //
            // ANTS-3793 INV-2 is what makes the swap invisible: the store's
            // records equal the rendered file's record-for-record, and
            // includeArchive=false is the membership ROADMAP.md itself carries.
            // rcComputePossibleDuplicates reads only `id` and `headline`, both
            // of which INV-2 covers.
            //
            // A read that refuses drops the advisory rather than the append.
            // StoreFailed is moot — commitAndRender() is about to refuse on the
            // same store — and TooLarge means a project over kItemCeiling, for
            // which roadmap_query's own store path already refuses outright.
            RoadmapSource::ReadError dupWhy = RoadmapSource::ReadError::None;
            const auto preWriteBullets = RoadmapSource::bulletsFromStore(
                store, projectId, /*includeArchive=*/false, &dupWhy);

            RoadmapRender::Outcome outcome;
            QString writeErr;
            const auto r = RoadmapWrite::commitAndRender(
                store, projectId, callerCanonical, roadmapPath, dryRun, mutate,
                &outcome, &writeErr);
            QJsonObject env;
            // ANTS-4593 — under dry_run the row keyed `idStr` is created inside
            // the transaction and rolled back, so the gate can name an id that
            // will exist nowhere. Hand it over so the refusal reports it apart
            // from genuine offenders. Empty on a real write, where it is real.
            if (rcRoadmapWriteRefused(
                    env, r, writeErr, outcome,
                    dryRun ? QStringList{idStr} : QStringList{}))
                return QJsonDocument(env);

            env[QStringLiteral("ok")]   = true;
            // ANTS-4634 — a preview reports `would_be_id`, never `id`, for the
            // reason ANTS-4508 gives: a caller reading one field takes the real
            // write's key for a reservation, and nothing is reserved. That
            // rename landed on the MARKDOWN branch only, and every migrated
            // project takes THIS one — so the guard covered the path almost
            // nobody runs while the path everybody runs still emitted `id`.
            // Reported by finbreak and reproduced here against this project.
            env[dryRun ? QStringLiteral("would_be_id")
                       : QStringLiteral("id")] = idStr;
            env[QStringLiteral("file")] = QStringLiteral("ROADMAP.md");
            // No `line` / `bytes_written`: a store has no lines (ANTS-3793
            // INV-2's declared field difference) and the render decides
            // placement. `.roadmap-counter` is not the allocation SOURCE here
            // either — but it is a cache of it, and the block below keeps it
            // in step, so the two counter fields are reported when it moves.
            //
            // ANTS-4141 part 2 — reconcile the derived counter cache with the
            // allocation the store just made. The store owns allocation (that
            // item's own ruling) and roadmap-format.md § 3.5.1 calls
            // `.roadmap-counter` a derived, per-machine cache rather than
            // source. But everything that still READS it — a fresh clone, a
            // hand append, a tool — allocates from it, and this path never
            // wrote it, so it drifted by one per allocation. Measured
            // 2026-08-19 on this project: 4402 against a store high-water of
            // 4501.
            //
            // On EVERY allocation rather than only after a migration, which is
            // the other defensible reading of that item: a cache refreshed
            // only at migration time starts drifting again with the next
            // append, and the drift is what the collision needs.
            //
            // BEST EFFORT, deliberately, and this is the half that decides the
            // shape: the item is already committed and the files already
            // rendered by the time we get here, so there is nothing to roll
            // back, and failing an append because a CACHE could not be
            // refreshed would turn a completed write into an error. The
            // markdown path treats the same failure as fatal because there the
            // counter IS the allocation source and the write can still be
            // undone.
            //
            // Only when the file already exists: a project with no counter has
            // nothing drifting, and creating one here would hand a
            // stable-id or counter-less project a file it never had. Advance
            // only upward, for the reason raiseIdHighWater() does the same.
            // ANTS-4635 — lifted into rcRoadmapReconcileCounterCache() so
            // op:append_batch's store branch can run the same reconciliation,
            // which it never did. The reasoning that used to sit here is on the
            // declaration in remotecontrol_internal.h.
            if (!dryRun && !useStablePrefix)
                rcRoadmapReconcileCounterCache(env, counterPath, allocated);
            rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
            // ANTS-2043 — the near-duplicate advisory, kept: the records are
            // the store's own items, read BEFORE the write so the new bullet
            // cannot match itself (ANTS-4426).
            if (preWriteBullets) {
                const QJsonArray possibleDuplicates =
                    rcComputePossibleDuplicates(*preWriteBullets, headline);
                if (!possibleDuplicates.isEmpty())
                    env[QStringLiteral("possible_duplicates")] =
                        possibleDuplicates;
            }
            // ANTS-4572 — fire when the scrub removed ANYTHING. It fired only
            // on a matched <parameter name="X"> pair before, so a stray
            // </invoke> or an ANTS-4609 `<tag>scalar` line was removed in
            // silence — and a caller who has read that bodies are scrubbed
            // takes ok:true for "the body is clean" and never re-reads it.
            if (!scrubbedNames.isEmpty() || scrubbedUnnamed > 0) {
                QJsonArray names;
                for (const QString &n : scrubbedNames) names.append(n);
                QJsonObject warn;
                warn["code"]    = QStringLiteral("body_scrubbed_tool_xml");
                warn["message"] = QStringLiteral(
                    "Stripped leaked tool-call XML from body; resend any "
                    "named siblings as proper JSON fields if you intended "
                    "them, and re-read the stored body if the count is "
                    "unexpected.");
                if (!names.isEmpty()) warn["lost_parameters"] = names;
                if (scrubbedUnnamed > 0)
                    warn["unnamed_fragments_removed"] = scrubbedUnnamed;
                env[QStringLiteral("warnings")] = QJsonArray{ warn };
            }
            if (rcReturnHeadlineOnly(req))
                env[QStringLiteral("post_bullets")] =
                    QJsonArray{ rcCompactBullet(idStr, status, headline) };
            return QJsonDocument(env);
        }
    }

    // ANTS-1424-INV-3 — counter allocation. Reads the high-water
    // mark; honours id_hint if present (must be > counter); writes
    // the bumped value back atomically.
    //
    // ANTS-1877 — split the legacy `counter_read_failed` blanket
    // refusal:
    //   - File doesn't exist:        diagnose project shape first.
    //     Stable-string IDs detected → stable_prefix_unsupported
    //     (with detected_prefix_example + follow_up). Otherwise
    //     counter_missing + the `echo 0 > .roadmap-counter` recipe.
    //   - File exists empty / whitespace-only: counter_missing
    //     (caller likely `touch`ed without initialising).
    //   - File exists but unreadable / not-a-number: keep the
    //     existing counter_read_failed (back-compat for any caller
    //     branching on that code).
    // ANTS-1905 — stable_prefix path skips the counter entirely (validated and
    // decided above).
    qint64 counter = 0;
    qint64 newId = 0;
    // ANTS-4691 — set when the counter value came from the in-memory seed
    // below (dry run) rather than from the file, so the shared read block
    // does not re-read a handle that was never opened.
    bool counterResolved = false;
    if (!useStablePrefix) {
        QFile cf(counterPath);
        const bool counterExists = QFile::exists(counterPath);

        if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (counterExists) {
                // File present but unreadable (permissions). Keep the
                // back-compat code for any caller branching on it.
                return rlErr(QStringLiteral("counter_read_failed"),
                    QStringLiteral("roadmap_log: could not read "
                                   ".roadmap-counter at \"%1\"")
                        .arg(counterPath));
            }
            {
                // Re-read the roadmap for the prefix sniffer. The
                // markdown was about to be read for the section
                // splice anyway; pull it here so we can diagnose
                // before the splice setup.
                QFile rmf(roadmapPath);
                QString rmText;
                if (rmf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    rmText = QString::fromUtf8(rmf.readAll());
                }
                const QString stablePrefix =
                    rlDetectStablePrefixId(rmText, rlDecl(callerCanonical));
                if (!stablePrefix.isEmpty()) {
                    // ANTS-1905 — surface the new escape hatch as the
                    // recommended fix; back-compat callers branching
                    // on `code:stable_prefix_unsupported` still match
                    // (the code is unchanged), but the hint now points
                    // at the working path.
                    QJsonObject env;
                    env["ok"]    = false;
                    env["code"]  = QStringLiteral(
                        "stable_prefix_unsupported");
                    env["error"] = QStringLiteral(
                        "roadmap_log op:append needs "
                        ".roadmap-counter; this project uses "
                        "stable-string IDs (e.g. \"%1\"). Pass "
                        "id_strategy:\"stable_prefix\" + "
                        "stable_id:\"<your-id>\" to bypass the "
                        "counter (ANTS-1905).")
                            .arg(stablePrefix);
                    env["detected_prefix_example"] = stablePrefix;
                    env["hint"] = QStringLiteral(
                        "Re-call with id_strategy:\"stable_prefix\" "
                        "and stable_id:\"%1\" (or your own "
                        "project-shaped id); the verb writes the "
                        "bullet without touching .roadmap-counter.")
                            .arg(stablePrefix);
                    env["follow_up"] = QStringLiteral("ANTS-1905");
                    return QJsonDocument(env);
                }
                // ANTS-3450 — `.roadmap-counter` is a derived, gitignored
                // cache; on a fresh clone it is simply absent. Recover the
                // high-water mark from the committed corpus (ROADMAP +
                // CHANGELOG + docs/roadmap/*.md) and seed the counter to it,
                // rather than refusing. corpusHighWater sniffs the project
                // prefix and returns 0 both for a truly greenfield roadmap
                // (→ first id allocates as <prefix>-0001, the ANTS-3397
                // behaviour) AND for a stable-string-id project with no
                // counter-style ids to recover — in that second case a
                // roadmap that DOES carry ids is a genuine desync we still
                // surface (rlRoadmapHasAnyBulletId discriminates the two).
                const qint64 seed = RoadmapFoldIn::corpusHighWater(
                    QFileInfo(counterPath).absolutePath());
                if (seed == 0 &&
                    rlRoadmapHasAnyBulletId(rmText, rlDecl(callerCanonical))) {
                    return rlErr(QStringLiteral("counter_missing"),
                        QStringLiteral("roadmap_log: .roadmap-counter does "
                                       "not exist at \"%1\" and no "
                                       "counter-style ids were found to "
                                       "recover a high-water mark from — "
                                       "restore it with: echo <highest-id> "
                                       "> %1").arg(counterPath));
                }
                // ANTS-4691 — a dry run must not touch disk. This seed runs
                // before the section and target gates, so without the guard a
                // REFUSED preview still created the file: the reporter's tree
                // gained an untracked .roadmap-counter from a call that wrote
                // no bullet. `seed` is already the value the file would hold,
                // so the preview uses it directly and reports the same
                // would_be_id the real allocate would hand out.
                if (dryRun) {
                    counter = seed;
                    counterResolved = true;
                } else {
                    QSaveFile init(counterPath);
                    const QByteArray seedBytes = QByteArray::number(seed) + '\n';
                    if (!init.open(QIODevice::WriteOnly | QIODevice::Text) ||
                        init.write(seedBytes) != seedBytes.size() ||
                        !init.commit()) {
                        return rlErr(QStringLiteral("counter_write_failed"),
                            QStringLiteral("roadmap_log: could not "
                                           "auto-create .roadmap-counter at "
                                           "\"%1\"").arg(counterPath));
                    }
                    if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        return rlErr(QStringLiteral("counter_read_failed"),
                            QStringLiteral("roadmap_log: could not read the "
                                           "just-created .roadmap-counter at "
                                           "\"%1\"").arg(counterPath));
                    }
                }
                // cf now reads "0"; fall through to the shared
                // counter-read block below (counter=0 → newId=1,
                // id_hint honoured).
            }
        }
        if (!counterResolved) {
            const QByteArray raw = cf.readAll().trimmed();
            if (raw.isEmpty()) {
                // Empty / whitespace-only file — caller likely `touch`ed
                // it without initialising. Route to counter_missing with
                // the same recipe hint.
                return rlErr(QStringLiteral("counter_missing"),
                    QStringLiteral("roadmap_log: .roadmap-counter at "
                                   "\"%1\" is empty — initialise with: "
                                   "echo 0 > %1")
                        .arg(counterPath));
            }
            bool ok = false;
            counter = QString::fromUtf8(raw).toLongLong(&ok);
            if (!ok) {
                return rlErr(QStringLiteral("counter_read_failed"),
                    QStringLiteral("roadmap_log: .roadmap-counter is "
                                   "not a number"));
            }
            newId = counter + 1;
            if (req.contains(QStringLiteral("id_hint"))) {
                const qint64 hint =
                    req.value(QStringLiteral("id_hint")).toInteger();
                if (hint <= counter) {
                    return rlErr(QStringLiteral("id_taken"),
                        QStringLiteral("roadmap_log: id_hint %1 is at "
                                       "or below current counter %2 — "
                                       "pick a value > counter or omit "
                                       "the hint")
                            .arg(hint).arg(counter));
                }
                newId = hint;
            }
        }
    }

    // Read ROADMAP.md for section lookup + body splice.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"")
                .arg(roadmapPath));
    }
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();

    // ANTS-1429 — unrecognised_format gate (write path). Refuse
    // to splice an Ants-emoji bullet into a file we can't parse.
    // Runs before RoadmapIndex::buildIndex so a foreign-dialect
    // roadmap (e.g. Vestige's GFM task-list) returns the typed
    // error instead of misleading bad_section / silent corruption.
    // Envelope shape parity with the cmdRoadmapQuery gate above:
    // path + bytes + hint inline-constructed (not via rlErr).
    const auto preflightBullets = rlParse(markdown, callerCanonical);
    const qint64 markdownBytes = markdown.toUtf8().size();
    if (preflightBullets.isEmpty() &&
        markdownBytes > kRoadmapMinParseableSize) {
        // ANTS-1463 — shared hint + expected_format envelope fields.
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("unrecognised_format");
        env["error"] = QStringLiteral(
            "roadmap_log: \"%1\" parsed zero bullets from %2 "
            "bytes — format not recognised; cannot safely splice")
                .arg(roadmapPath).arg(markdownBytes);
        env["path"]            = roadmapPath;
        env["bytes"]           = markdownBytes;
        env["hint"]            = kUnrecognisedFormatHint();
        env["expected_format"] = kUnrecognisedFormatExpected();
        return QJsonDocument(env);
    }
    // ANTS-2126 — a pass-headings roadmap was already routed to
    // cmdRoadmapLogPassAppend by the early gate above (before counter
    // allocation), so by here the roadmap is GFM / ants-v1.

    // ANTS-1424-INV-4 — locate the named section via RoadmapIndex.
    const auto index = RoadmapIndex::buildIndex(markdown);
    const auto *sec = RoadmapIndex::findBySlug(index, section);
    if (!sec) {
        // Sanitise echo (≤ 64 B + control-char filter) per the
        // cmdRoadmapQuery convention so we don't reflect arbitrary
        // bytes through the response.
        QString verbatim = section;
        if (verbatim.size() > 64) verbatim.truncate(64);
        for (int i = 0; i < verbatim.size(); ++i) {
            if (verbatim.at(i).unicode() < 0x20) {
                verbatim[i] = QChar('?');
            }
        }
        // ANTS-1524 — bad_case parity with cmdRoadmapQuery so a
        // caller that case-mangled the slug gets a loud refusal
        // with the canonical form instead of a silent-miss
        // bad_section.
        const QString sectionCi = section.toLower();
        for (const auto &s : index) {
            if (s.slug.toLower() == sectionCi && s.slug != section) {
                QJsonObject env;
                env["ok"]             = false;
                env["code"]           = QStringLiteral("bad_case");
                env["error"]          = QStringLiteral(
                    "roadmap_log: section slug case mismatch: \"%1\" — "
                    "did you mean \"%2\"?").arg(verbatim, s.slug);
                env["canonical_slug"] = s.slug;
                return QJsonDocument(env);
            }
        }
        // ANTS-4556 — a bad_section that offers nothing discards the body the
        // caller just composed and leaves one route: a section_index call
        // answering 141 slugs. Three sibling refusals already rank near-misses
        // (read_region section mode on both section_ambiguous and
        // section_not_found; apply_edits on not_found and ambiguous), so this
        // verb was the outlier rather than the precedent. Ranked by the SHARED
        // ranker, never a second copy of it.
        //
        // A hint, never a resolution: the write still refuses. Guessing which
        // section the caller meant is how a 2.4 KB body lands in the wrong one.
        QStringList slugs;
        slugs.reserve(index.size());
        for (const auto &s : index) slugs << s.slug;
        const QStringList cands =
            ReadRegion::rankSectionCandidates(section, slugs);
        QJsonObject env;
        env["ok"]         = false;
        env["code"]       = QStringLiteral("bad_section");
        env["error"]      = QStringLiteral("roadmap_log: unknown section slug "
                                           "\"%1\"").arg(verbatim);
        env["candidates"] = QJsonArray::fromStringList(cands);
        env["sections_total"] = index.size();
        return QJsonDocument(env);
    }

    // ANTS-2055 — refuse an append into a parent section that has
    // subsections; splicing at sec.lineEnd would orphan the bullet past
    // the last child heading (where roadmap_query mis-attributes it).
    {
        const QStringList childSlugs = rcSectionChildSlugs(index, *sec);
        if (!childSlugs.isEmpty()) {
            return rcSectionHasSubsectionsRefusal(sec->slug, childSlugs);
        }
    }

    // Construct the bullet via the shared helper (ANTS-1879 INV-10 —
    // extracted so cmdRoadmapLogAppendBatch can format each bullet
    // through the same code path). ANTS-1905 — under stable_prefix
    // strategy the id is the caller-supplied stable_id, not an
    // ANTS-NNNN string allocated from .roadmap-counter.
    QString idStr;
    bool counterReconciled = false;   // ANTS-2179 — counter lagged the file
    qint64 counterAdvancedPast = 0;   // ANTS-4493 — the occupied high-water
    if (useStablePrefix) {
        idStr = stableId;
    } else {
        // ANTS-2054 / ANTS-2076 — render the project's own counter
        // prefix. Precedence: explicit id_prefix > prefix sniffed from
        // existing IDs > project-dir default (DOOM_Ants → "DOOM"). No
        // longer falls back to a hardcoded "ANTS" for a fresh / id-less
        // roadmap.
        const QString pfx =
            rlResolveCounterPrefix(idPrefixArg, markdown, callerCanonical);
        // ANTS-2179 — reconcile newId against the file's true max id for
        // this prefix so a stale .roadmap-counter can't reissue a live id.
        // preflightBullets is already in hand, so the scan is free.
        // ANTS-3450 — also floor to ids that have migrated OUT of ROADMAP.md
        // (shipped → CHANGELOG, closed minors → docs/roadmap/*.md); with the
        // counter now an untracked cache, this committed-corpus floor is what
        // keeps a recovered/absent counter from reissuing a shipped id.
        qint64 maxFileId =
            rlMaxExistingIdForPrefix(preflightBullets, pfx);
        maxFileId = std::max(maxFileId, RoadmapFoldIn::corpusHighWater(
            QFileInfo(counterPath).absolutePath(), pfx));
        // ANTS-4493 — and floor to the STORE's high-water for this prefix.
        // A project that has been MIGRATED but is not SERVED from the store
        // reaches this path: roadmapWriteTarget() resolves through
        // migratedProject(), which returns nullopt for every dialect but
        // ants-v1. The ids that project's migration SYNTHESISED live in the
        // store and in no file, so neither term above can see them — and the
        // next append reissues one, once per migrated project, silently, with
        // ok:true and a normal-looking id. Reported against a
        // github-task-list project whose file max was 0611: this path issued
        // 0612, which the store had already given to an unrelated bullet.
        //
        // readProjectByRoot(), NOT migratedProject(): the question here is
        // whether the store holds ids for this ROOT, which is true whatever
        // dialect the file is in — and migratedProject() answers nullopt for
        // exactly the projects that reach this branch.
        //
        // A store that will not open is not this verb's failure: the markdown
        // path is what it was before, and refusing an append because an
        // unrelated store is unreadable would be a regression.
        if (RoadmapStore *store = roadmapStoreOrNull(nullptr, nullptr)) {
            QString storeErr;
            if (const auto row =
                    store->readProjectByRoot(callerCanonical, &storeErr)) {
                QString hwErr;
                if (const auto hw = store->idHighWater(row->projectId, pfx, &hwErr))
                    maxFileId = std::max(maxFileId, *hw);
            }
        }
        if (req.contains(QStringLiteral("id_hint"))) {
            // An explicit hint already cleared the counter (above); also
            // refuse when it collides with a live id the lagging counter
            // never knew about (id_hint at or below the file's high-water).
            if (newId <= maxFileId) {
                return rlErr(QStringLiteral("id_taken"),
                    QStringLiteral("roadmap_log: id_hint %1 is at or below "
                                   "the highest existing %2-NNNN id in "
                                   "ROADMAP.md (%3) — pick a value > %3 or "
                                   "omit the hint")
                        .arg(newId).arg(pfx).arg(maxFileId));
            }
        } else if (maxFileId >= newId) {
            // Counter lagged the file: skip past the live max so we never
            // write a duplicate, and let the counter rewrite below self-heal.
            newId = maxFileId + 1;
            counterReconciled = true;
            // ANTS-4493 — report what it advanced PAST as well as what it
            // advanced TO. "advanced to 612" reads as safe until you know 612
            // was the OCCUPIED high-water rather than the first free slot, and
            // that is exactly the reading that made the reported collision look
            // like a normal allocation.
            counterAdvancedPast = maxFileId;
        }
        idStr = QStringLiteral("%1-%2").arg(pfx).arg(newId, 4, 10,
                                                     QLatin1Char('0'));
        if (newId > 9999)
            idStr = QStringLiteral("%1-%2").arg(pfx).arg(newId);
    }
    QStringList scrubbedNames;
    int scrubbedUnnamed = 0;                      // ANTS-4572
    const QString bullet =
        formatRoadmapBullet(req, idStr, statusEmoji, scrubbedNames,
                            &scrubbedUnnamed);

    // Splice bullet at the section's lineEnd. lineEnd is 0-indexed
    // and exclusive — i.e. the line index of the next heading (or
    // total_lines for the last section). Inserting at that line
    // pushes the heading down by one block.
    QStringList lines = markdown.split(QChar('\n'));
    const int insertAt = sec->lineEnd;  // 0-indexed
    QString bulletNoTrailNl = bullet;
    if (bulletNoTrailNl.endsWith(QChar('\n'))) {
        bulletNoTrailNl.chop(1);
    }
    const QStringList bulletLines = bulletNoTrailNl.split(QChar('\n'));
    for (int i = bulletLines.size() - 1; i >= 0; --i) {
        lines.insert(insertAt, bulletLines.at(i));
    }
    const QString updated = lines.join(QChar('\n'));

    // ANTS-2077 — dry_run preview: return the would-be id, formatted
    // bullet and 1-based insertion line WITHOUT writing ROADMAP.md or
    // bumping .roadmap-counter. Lets a caller verify prefix / format /
    // section for free instead of a write-then-correct round-trip.
    //
    // ANTS-4508 — the id is `would_be_id`, NOT `id`. A preview that reports it
    // under the key a real write uses reads as a RESERVATION: measured, a
    // probe's id went into a commit message written before the real write and
    // two commits had to be amended. The envelope does carry dry_run:true, and
    // a caller reading a single field does not see it. Anything written
    // against a previewed id is wrong if any other write intervenes — wrong in
    // a way nothing detects, since the id then names a different item or none.
    // Same `would_*` naming as ANTS-4463's `would_write` on this envelope.
    if (dryRun) {
        QJsonObject out;
        out["ok"]          = true;
        out["dry_run"]     = true;
        out["would_be_id"] = idStr;
        out["file"]    = QStringLiteral("ROADMAP.md");
        out["line"]    = insertAt + 1;  // 1-based for humans
        out["bullet"]  = bulletNoTrailNl;
        out["bytes"]   = static_cast<qint64>(bullet.toUtf8().size());
        if (counterReconciled) {
            out["counter_advanced_to"]   = newId;          // ANTS-2179
            out["counter_advanced_past"] = counterAdvancedPast;  // ANTS-4493
        }
        const QJsonArray possibleDuplicates =
            rcComputePossibleDuplicates(preflightBullets, headline);
        if (!possibleDuplicates.isEmpty()) {
            out["possible_duplicates"] = possibleDuplicates;
        }
        return QJsonDocument(out);
    }

    // Write ROADMAP.md atomically via QSaveFile.
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for "
                           "writing").arg(roadmapPath));
    }
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit()) {
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" "
                           "failed").arg(roadmapPath));
    }

    // Counter rewrite. ANTS-1433: this is the second half of a
    // two-stage commit — ROADMAP.md is already on disk above. If the
    // counter commit fails here the state desyncs (the appended bullet
    // carries newId but the counter still points one behind, so the
    // next roadmap_log reuses newId → duplicate IDs). Roll ROADMAP.md
    // back to its pre-splice content (`markdown`, captured before the
    // splice) so the operation is all-or-nothing.
    // ANTS-1905 — stable_prefix strategy skips the counter rewrite
    // entirely (no counter file involved in the allocation).
    if (!useStablePrefix) {
        auto rollbackRoadmap = [&]() {
            QSaveFile restore(roadmapPath);
            if (!restore.open(QIODevice::WriteOnly | QIODevice::Text)) return;
            const QByteArray orig = markdown.toUtf8();
            if (restore.write(orig) == orig.size()) restore.commit();
        };
        QSaveFile cw(counterPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            rollbackRoadmap();
            return rlErr(QStringLiteral("counter_write_failed"),
                QStringLiteral("roadmap_log: could not open "
                               ".roadmap-counter for writing"));
        }
        const QByteArray cv =
            (QString::number(newId) + QChar('\n')).toUtf8();
        bool counterCommitted = (cw.write(cv) == cv.size());
        if (counterCommitted) {
            if (g_forceCounterCommitFail) {
                // ANTS-1433 test seam: drop the staged temp file so the
                // original .roadmap-counter is left untouched, exactly as a
                // real QSaveFile::commit() failure would.
                cw.cancelWriting();
                counterCommitted = false;
            } else {
                counterCommitted = cw.commit();
            }
        }
        if (!counterCommitted) {
            rollbackRoadmap();
            return rlErr(QStringLiteral("counter_write_failed"),
                QStringLiteral("roadmap_log: atomic write of "
                               ".roadmap-counter failed"));
        }
    }

    // ANTS-1424-INV-8 — success envelope: id (full ANTS-NNNN
    // string), file (basename), line (1-based insertion point),
    // bytes_written (the appended bullet's UTF-8 byte size).
    QJsonObject out;
    out["ok"]            = true;
    out["id"]            = idStr;
    out["file"]          = QStringLiteral("ROADMAP.md");
    out["line"]          = insertAt + 1;  // 1-based for humans
    out["bytes_written"] = static_cast<qint64>(bullet.toUtf8().size());
    if (counterReconciled) {
        out["counter_advanced_to"]   = newId;                // ANTS-2179
        out["counter_advanced_past"] = counterAdvancedPast;  // ANTS-4493
    }
    // ANTS-2043 — non-blocking near-duplicate advisory. preflightBullets
    // was parsed BEFORE the splice, so the just-appended bullet can't
    // match itself. Surfaced only when there's at least one candidate.
    const QJsonArray possibleDuplicates =
        rcComputePossibleDuplicates(preflightBullets, headline);
    if (!possibleDuplicates.isEmpty()) {
        out["possible_duplicates"] = possibleDuplicates;
    }
    // ANTS-1551 — if the defensive scrub stripped leaked tool-call
    // XML, surface the recognised sibling-parameter names so the
    // caller knows which typed arguments were lost in transit.
    if (!scrubbedNames.isEmpty() || scrubbedUnnamed > 0) {   // ANTS-4572
        QJsonArray names;
        for (const QString &n : scrubbedNames) names.append(n);
        QJsonObject warn;
        warn["code"]    = QStringLiteral("body_scrubbed_tool_xml");
        warn["message"] = QStringLiteral(
            "Stripped leaked tool-call XML from body; resend any named "
            "siblings as proper JSON fields if you intended them, and "
            "re-read the stored body if the count is unexpected.");
        if (!names.isEmpty()) warn["lost_parameters"] = names;
        if (scrubbedUnnamed > 0)
            warn["unnamed_fragments_removed"] = scrubbedUnnamed;
        out["warnings"] = QJsonArray{ warn };
    }
    // ANTS-2080 — confirm-after compact echo of the appended bullet.
    if (rcReturnHeadlineOnly(req)) {
        out["post_bullets"] =
            QJsonArray{ rcCompactBullet(idStr, status, headline) };
    }
    return QJsonDocument(out);
}

// ANTS-3566 — rank sibling roadmap IDs by shared case-insensitive prefix with a
// (not-found) ID locator, best first. For a bullet_not_found on an ID locator,
// ranking candidate HEADLINES by overlap with the id STRING is meaningless (an
// id like "3D_E-0031" shares a leading "3" with every headline starting "3");
// same-project siblings that share an id prefix are the useful hint. Returns
// indices into `candidateIds` (≤ limit); empty when nothing shares a prefix, so
// the caller emits no suggestions rather than misleading ones. Shared by the
// GFM step-7 block and the ants-v1 zero-match block below.
static QVector<int> rcRankIdsBySharedPrefix(const QStringList &candidateIds,
                                            const QString &locId, int limit) {
    const QString need = locId.toLower();
    QVector<QPair<int, int>> scored;  // (sharedPrefixLen, index)
    for (int i = 0; i < candidateIds.size(); ++i) {
        const QString cand = candidateIds.at(i).toLower();
        if (cand.isEmpty()) continue;
        int shared = 0;
        const int lim = std::min(cand.size(), need.size());
        while (shared < lim && cand.at(shared) == need.at(shared)) ++shared;
        if (shared > 0) scored.append(qMakePair(shared, i));
    }
    std::sort(scored.begin(), scored.end(),
        [](const QPair<int, int> &a, const QPair<int, int> &b) {
            return a.first > b.first;
        });
    QVector<int> out;
    for (int k = 0; k < scored.size() && out.size() < limit; ++k)
        out.append(scored.at(k).second);
    return out;
}

// ANTS-1428 — roadmap_log op:"flip". Adapter-mode write path for
// GFM-format ROADMAP.md files. Locator: bold-ID → caret anchor →
// headline-hash; on first touch of a bullet that has neither a
// bold-ID nor an existing anchor, the same write injects a
// `^prefix-NNNN` caret anchor on the last line of the headline
// content. Counter consumes only on anchor injection. See
// docs/specs/ANTS-1428.md § Tier 2.
QJsonDocument RemoteControl::cmdRoadmapLogFlip(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };
    auto rlSugErr = [](const QString &code, const QString &message,
                       const QJsonArray &suggestions, int matched) {
        QJsonObject env;
        env["ok"]          = false;
        env["code"]        = code;
        env["error"]       = message;
        env["matched"]     = matched;
        env["suggestions"] = suggestions;
        return QJsonDocument(env);
    };

    // ANTS-1717 — annotate mode shares this handler. annotate appends
    // a body note and leaves status untouched (no emoji swap, no anchor
    // injection); flip optionally carries a note too (ANTS-1793).
    const bool annotateMode =
        req.value(QStringLiteral("op")).toString() ==
            QStringLiteral("annotate");
    // ANTS-2136 — dry_run preview flag (parity with op:append). When set,
    // every locator/format/status refusal still fires, but a resolving
    // call returns the would-be edit and writes nothing.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // 1. Required fields: caller_cwd, to_status (flip only), plus one
    //    of the three locators (id / anchor / headline).
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    }

    // ANTS-2126 — pass-headings roadmaps route to the heading-format
    // flip/annotate writer BEFORE the GFM to_status/note/locator guards
    // below, so the pass path owns its own validation (INV-3 bad_args for
    // a missing to_status / locator, INV-8 bad_args for an empty annotate
    // note — deliberately the documented code, where the GFM guard at the
    // note-empty check still emits missing_field; ANTS-2128 converges it).
    {
        const QString cc = QFileInfo(callerRaw).canonicalFilePath();
        const QString rp = cc.isEmpty() ? QString() : findRoadmapUnder(cc);
        if (!rp.isEmpty()) {
            QFile pf(rp);
            if (pf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString md = QString::fromUtf8(pf.readAll());
                pf.close();
                if (rcBulletsArePassHeadings(rlParse(md, cc)))   // ANTS-3771
                    return cmdRoadmapLogPassFlip(req, rp, md);
            }
        }
    }

    const QString toStatus =
        req.value(QStringLiteral("to_status")).toString();
    // to_status accepts either the word form or the emoji directly.
    // Unused under annotate (status is preserved).
    QString targetEmoji;
    // ANTS-3809 § 2.2 — the same choice as a lifecycle WORD, which is what
    // setItemField(itemPk, "status", …) takes. Resolved here beside the emoji
    // rather than mapped back from it later, so the two cannot disagree.
    QString targetStatusWord;
    if (annotateMode) {
        if (!toStatus.isEmpty()) {
            return rlErr(QStringLiteral("bad_op_combo"),
                QStringLiteral("roadmap_log: to_status is not accepted "
                               "under op:\"annotate\" — annotate leaves "
                               "status unchanged; use op:\"flip\" with a "
                               "`note` to change status and annotate in "
                               "one call"));
        }
    } else {
        if (toStatus.isEmpty()) {
            return rlErr(QStringLiteral("missing_field"),
                QStringLiteral("roadmap_log: to_status is required "
                               "under op:\"flip\""));
        }
        // ANTS-1932 — synonym expansion (case-insensitive). Accept the
        // natural English words a caller reaches for on the first try, mapping
        // them to the canonical form before the check below. The canonical
        // names + emojis remain the documented API; synonyms are accept-only.
        // ANTS-2126 — extracted to rlCanonicalToStatus (shared with the
        // pass-headings flip path).
        const QString toStatusResolved = rlCanonicalToStatus(toStatus);
        if      (toStatusResolved == QStringLiteral("planned")     ||
                 toStatusResolved == QStringLiteral("📋")) { targetEmoji = QStringLiteral("📋"); targetStatusWord = QStringLiteral("planned"); }
        else if (toStatusResolved == QStringLiteral("in-progress") ||
                 toStatusResolved == QStringLiteral("🚧")) { targetEmoji = QStringLiteral("🚧"); targetStatusWord = QStringLiteral("in-progress"); }
        else if (toStatusResolved == QStringLiteral("shipped")     ||
                 toStatusResolved == QStringLiteral("✅")) { targetEmoji = QStringLiteral("✅"); targetStatusWord = QStringLiteral("shipped"); }
        else if (toStatusResolved == QStringLiteral("considered")  ||
                 toStatusResolved == QStringLiteral("💭")) { targetEmoji = QStringLiteral("💭"); targetStatusWord = QStringLiteral("considered"); }
        else {
            return rlErr(QStringLiteral("bad_status"),
                QStringLiteral("roadmap_log: unknown to_status \"%1\" — "
                               "expected planned / in-progress / shipped "
                               "/ considered (or one of 📋/🚧/✅/💭); "
                               "synonyms also accepted: done/complete/completed "
                               "→ shipped, wip/in_progress → in-progress, "
                               "todo/open → planned, maybe/idea → considered")
                    .arg(toStatus));
        }
    }

    // 1b. The note to append (ANTS-1793 flip-with-note / ANTS-1717
    //     annotate). Scrubbed identically to op:"append"'s body.
    //     Required + non-empty under annotate; optional under flip.
    QString note = req.value(QStringLiteral("note")).toString();
    // ANTS-1995 — reject an oversize note before the backtracking scrub.
    if (note.size() > kRcMaxNoteChars) {
        return rlErr(QStringLiteral("too_large"),
            QStringLiteral("roadmap_log: note exceeds %1-char cap (got %2)")
                .arg(kRcMaxNoteChars).arg(note.size()));
    }
    QStringList noteScrubbedNames;
    rcScrubLeakedToolXml(note, noteScrubbedNames);
    if (annotateMode && note.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"annotate\" requires a "
                           "non-empty `note` to append to the bullet"));
    }
    // ANTS-4549 — here, before the locate and before either the format or the
    // backend split, so all three note-carrying ops and both backends give one
    // answer. The note is appended to the body and § 2.6 re-derives the trailer
    // columns from it, so a bare `Kind:` in prose rewrites a column the caller
    // never mentioned — op:"append" has refused exactly this as body_shadowed
    // since ANTS-3809 § 2.5, and this routes the note through that guard rather
    // than adding a second one.
    {
        QString shadowErr;
        if (rlNoteDeclaresTrailer(note, &shadowErr))
            return rlErr(QStringLiteral("body_shadowed"),
                QStringLiteral("roadmap_log: %1").arg(shadowErr));
    }

    // 2. id_hint is bad_op_combo under op:"flip"/"annotate" — counter
    //    is consumed only when an anchor is injected, never explicitly
    //    requested. See ANTS-1428 spec § Counter file.
    if (req.contains(QStringLiteral("id_hint"))) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: id_hint is not accepted under "
                           "op:\"flip\"/\"annotate\" — counter is "
                           "consumed only on anchor injection"));
    }

    // 3. Pick the locator. id wins over anchor (INV-12 explicit
    //    precedence); headline alongside id or anchor is bad_op_combo
    //    because there's no precedence rule for that combination.
    const QString locId =
        req.value(QStringLiteral("id")).toString();
    const QString locAnchor =
        req.value(QStringLiteral("anchor")).toString();
    const QString locHeadline =
        req.value(QStringLiteral("headline")).toString();
    if (locId.isEmpty() && locAnchor.isEmpty() &&
        locHeadline.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"flip\" needs at least "
                           "one locator — `id`, `anchor`, or "
                           "`headline`"));
    }
    if (!locHeadline.isEmpty() &&
        (!locId.isEmpty() || !locAnchor.isEmpty())) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: headline locator is not "
                           "permitted alongside id or anchor — pick "
                           "the canonical handle when one exists"));
    }
    // ANTS-3387 — an `id` locator that is id-token SHAPED but fails the
    // canonical PROJ-NNNN gate (e.g. a digit-leading prefix like
    // "3D_E-0022") is never adopted as a bullet id, so it can never match
    // on EITHER the GFM boldId path or the ants-v1 native path (which
    // refuses first with bullet_not_found — hence this format-independent
    // early guard, not a per-branch one). A bare bullet_not_found reads as
    // "the item vanished"; name the real cause. Shape-only, so a
    // conforming-but-absent id (INV-4) still gets the ordinary
    // bullet_not_found.
    if (rcIsNonconformingIdToken(locId)) {
        return rlErr(QStringLiteral("bad_id_format"),
            QStringLiteral("roadmap_log: \"%1\" is id-shaped but not a "
                           "canonical [PROJ-NNNN] token (roadmap-format.md "
                           "§ 3.5.1 requires a letter-leading prefix); it "
                           "can never match a bullet id — re-target by "
                           "`headline` (exact text) or `anchor`.")
                .arg(locId));
    }

    // 4. Resolve caller_cwd → ROADMAP.md path. Same logic as append.
    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty()) {
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    }
    // ANTS-1459 — shared findRoadmapUnder helper widens the search
    // to docs/, docs/private/, docs/internal/, .github/.
    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty()) {
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));
    }

    // 5. Read markdown.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"")
                .arg(roadmapPath));
    }
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();
    const qint64 markdownBytes = markdown.toUtf8().size();

    // ANTS-2126 — a pass-headings roadmap was already routed to
    // cmdRoadmapLogPassFlip by the early gate above (before the GFM
    // to_status/note guards), so by here the roadmap is GFM / ants-v1.

    // 6. Walk GFM bullets first. If none found AND the file is big
    //    enough to be a real roadmap, fall through to ANTS-1441's
    //    ants-v1 native walker before refusing.
    QStringList lines = markdown.split(QChar('\n'));
    const QVector<GfmBullet> bullets = walkGfmBullets(lines, rlDecl(callerCanonical));   // ANTS-3771

    // ANTS-3561 — apply an op:flip / op:annotate to a single, already-located
    // ants-v1 native bullet (index into `v1bullets`): status-emoji swap
    // (skipped under annotate) + optional note, then a dry_run preview or an
    // atomic write + success envelope. Shared by TWO locate sites — the
    // GFM-empty branch (a pure ants-v1 roadmap) and the GFM-no-match fallback
    // (a MIXED GFM+ants-v1 file whose target is an appended `- 📋 [ID]` emoji
    // bullet that walkGfmBullets never sees — the Vestige 3D_E-NNNN case).
    auto applyAntsV1FlipResult =
        [&](const QVector<AntsV1Bullet> &v1bullets, int matchIdx)
            -> QJsonDocument {
        const AntsV1Bullet &v1target = v1bullets.at(matchIdx);
        if (v1target.insideFenced) {
            return rlErr(QStringLiteral("anchor_unsafe_context"),
                QStringLiteral("roadmap_log: located bullet is "
                               "inside a fenced code block — "
                               "refusing to edit")
                    + rcFenceOpenerHint(v1target.fenceOpenLine));
        }
        // ANTS-3809 § 2.2 — the store path, at the TOP of this lambda and so
        // INSIDE the ants-v1 branch. § 2.4 requires exactly that ordering: the
        // `anchor` locator's bad_op_combo is a FORMAT refusal and must run
        // ahead of the store-versus-markdown dispatch, or an anchor locator
        // would be matched against a field the store path fills as empty and
        // would silently match nothing. Locating is shared for the same reason
        // create_section shares its validation — on a migrated project the file
        // is the render's own output, so the same walk finds the same bullet.
        {
            RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
            QString seamErr;
            // ANTS-3863 — fromMemory, NOT fromFile: this op locates its bullet
            // by walking `markdown`, on the store path too (the file is the
            // render's own output), so the whole text is already in hand and a
            // file-backed provider here would read the roadmap a second time.
            auto seamText = RoadmapSource::RoadmapText::fromMemory(markdown);
            const auto target =
                roadmapWriteTarget(callerCanonical, seamText, &why, &seamErr);
            QJsonObject refusal;
            if (rcRoadmapSourceRefused(refusal, why, seamErr))
                return QJsonDocument(refusal);
            if (target) {
                RoadmapStore &store = *target->store;
                const qint64 projectId = target->projectId;

                // § 2.2's two-step locate. AntsV1Bullet::headline is the
                // post-strip headline with its `**` wrappers removed, which is
                // the form ItemRef::headline holds, so the step-2 fallback
                // compares equal without a truncation allowance.
                RoadmapParse::BulletRecord rec;
                rec.id           = v1target.id;
                rec.headlineFull = v1target.headline;
                QString pkCode, pkErr;
                const auto itemPk =
                    rlStoreItemPk(store, projectId, rec, &pkCode, &pkErr);
                if (!itemPk)
                    return rlErr(pkCode, QStringLiteral("roadmap_log: %1").arg(pkErr));
                const auto before = store.readItem(*itemPk, &seamErr);
                if (!before)
                    return rlErr(QStringLiteral("store_failed"), seamErr);

                // Idempotent re-annotate, mirroring appendBodyNote()'s
                // noteAlreadyPresent: the markdown path does not append a note
                // the bullet already carries, and a caller re-running an
                // annotate must not get a second copy for having migrated.
                const bool alreadyPresent =
                    !note.isEmpty() && before->body.contains(note);
                const QString newBody =
                    (note.isEmpty() || alreadyPresent)
                        ? before->body
                        : rlAppendBodyNote(before->body, note);

                // ANTS-3822 § 2.5 — one stamp for the whole op, computed before
                // mutate() runs rather than per row, so a write that straddles a
                // second boundary is still one revision.
                HistoryContext hist;
                hist.changedAt = rlHistoryStamp();

                // ANTS-4577 — carries the derivation's own refusal code out of
                // the mutate, where the return type is a bare bool.
                QString deriveCode;

                const auto mutate = [&](QString *err) -> bool {
                    // ANTS-4501 § 2.2 — `wrote` is what decides the
                    // `last_modified` stamp. An annotate whose note is already
                    // present writes nothing at all, and an item nothing
                    // touched must not read as modified today.
                    bool wrote = false;
                    if (!annotateMode) {
                        if (!store.setItemField(*itemPk, QStringLiteral("status"),
                                                targetStatusWord,
                                                QStringLiteral("asserted"), err))
                            return false;
                        hist.record(*itemPk, QStringLiteral("status"),
                                    before->status, targetStatusWord);
                        // ANTS-4501 § 2.2 — set entering shipped, cleared
                        // leaving it; a same-status write moves nothing.
                        if (!rlStampShipped(store, *itemPk, before->status,
                                            targetStatusWord, err))
                            return false;
                        wrote = true;
                    }
                    if (newBody == before->body) {
                        if (wrote && !rlStampModified(store, *itemPk, err))
                            return false;
                        return rlFlushHistory(store, hist, err);
                    }
                    if (!store.setItemField(*itemPk, QStringLiteral("body"),
                                            newBody, QStringLiteral("asserted"), err))
                        return false;
                    hist.record(*itemPk, QStringLiteral("body"), before->body, newBody);
                    // § 2.6 — a body write re-derives every trailer column the
                    // request did not supply, which for flip/annotate is all
                    // five. This is what keeps `Layman:` from being a body line
                    // the render's gate can never see.
                    if (!rlDeriveTrailerColumns(store, *itemPk, *before,
                                                newBody, {}, &hist, err,
                                                nullptr, &deriveCode))
                        return false;
                    // ANTS-4501 § 2.2 — after every column this op writes,
                    // including the trailer columns above.
                    if (!rlStampModified(store, *itemPk, err))
                        return false;
                    // ANTS-3822 — flushed here, after every column this op
                    // writes has been recorded, because the cap is asked once
                    // for the whole set (§ 2.3).
                    return rlFlushHistory(store, hist, err);
                };

                RoadmapRender::Outcome outcome;
                QString writeErr;
                const auto r = RoadmapWrite::commitAndRender(
                    store, projectId, callerCanonical, roadmapPath, dryRun,
                    mutate, &outcome, &writeErr);
                QJsonObject env;
                if (rcRoadmapWriteRefused(env, r, writeErr, outcome)) {
                    // ANTS-4577 — applied AFTER the mapper, because the mapper
                    // is what fills `code` and it has only the commitAndRender
                    // result to go on.
                    if (!deriveCode.isEmpty())
                        env[QStringLiteral("code")] = deriveCode;
                    return QJsonDocument(env);
                }
                rlAttachHistoryNote(env, store, hist);   // ANTS-3822 § 2.3.1

                env[QStringLiteral("ok")]          = true;
                env[QStringLiteral("op")]          = annotateMode
                                                        ? QStringLiteral("annotate")
                                                        : QStringLiteral("flip");
                env[QStringLiteral("format")]      = QStringLiteral("ants-v1");
                // ANTS-4466 — from the STORE, not from `v1target`, which is the
                // parsed FILE. On this path the file is the render's output, so
                // the two normally agree; where they do not — a `git checkout --`
                // that reverted ROADMAP.md while the store kept the flip — the
                // file is the stale one, and reporting it described the op's
                // INPUT while the render had already committed the correct
                // RESULT. A caller confirming a write from the envelope got the
                // wrong answer in exactly the divergence case where confirming
                // matters most.
                const QString storeFromEmoji = rcStatusEmoji(before->status);
                env[QStringLiteral("from_status")] = storeFromEmoji;
                env[QStringLiteral("to_status")]   = annotateMode
                                                        ? storeFromEmoji
                                                        : targetEmoji;
                // Rides on the true arm only, like ANTS-4463/4465's fields: a
                // divergence is news, and a key present on every write restating
                // from_status is a key nobody reads.
                if (v1target.status != storeFromEmoji)
                    env[QStringLiteral("file_status")] = v1target.status;
                env[QStringLiteral("file")]        = QStringLiteral("ROADMAP.md");
                env[QStringLiteral("id")]          = v1target.id;
                // No `line` / `bytes` / `note_line`: a store has no lines
                // (ANTS-3793 INV-2's declared field difference), and the render
                // decides placement. anchor_injected stays, and stays false —
                // ants-v1 never injects one.
                env[QStringLiteral("anchor_injected")] = false;
                rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
                if (!note.isEmpty()) {
                    // ANTS-4463 — same rule as the file list: on a dry run the
                    // note was NOT appended, so the past-tense key is absent.
                    env[dryRun ? QStringLiteral("note_would_append")
                               : QStringLiteral("note_appended")] =
                        !alreadyPresent;
                    if (alreadyPresent)
                        env[QStringLiteral("note_already_present")] = true;
                }
                if (!noteScrubbedNames.isEmpty()) {
                    QJsonArray dropped;
                    for (const QString &n : noteScrubbedNames) dropped.append(n);
                    env[QStringLiteral("note_scrubbed_params")] = dropped;
                }
                // ANTS-4464 — name the path that ran. The two paths declare
                // different field sets on purpose (ANTS-3793 INV-2: a store has
                // no lines, so `line` / `note_line` / `bytes_written` cannot be
                // resolved without re-reading the file the render just wrote,
                // which ANTS-3863 exists to avoid). What was missing is not the
                // fields but the STATEMENT: a caller reading `note_line`
                // unconditionally got null on a successful write with nothing
                // saying why, and identical calls returned two shapes across a
                // migration. Named `write_path` rather than the reported
                // `path` — this envelope already carries `file`, and `path` is
                // a filesystem word everywhere else in the verb layer.
                env[QStringLiteral("write_path")] = QStringLiteral("render");
                // ANTS-4464 — the file path emits this under
                // return:"headline_only"; the store path did not, so a
                // documented echo went silently missing on migrated projects.
                // Same divergence class as the fields above.
                if (rcReturnHeadlineOnly(req)) {
                    env[QStringLiteral("post_bullets")] = QJsonArray{
                        rcCompactBullet(
                            v1target.id,
                            rcStatusWord(env.value(QStringLiteral("to_status"))
                                             .toString()),
                            v1target.headline) };
                }
                if (dryRun)
                    env[QStringLiteral("dry_run")] = true;
                return QJsonDocument(env);
            }
        }

        // Apply flip (skipped under annotate) + optional note, then
        // atomic write. applyAntsV1Flip edits the headline in place
        // (no line-count change), so appendBodyNote's firstLine
        // index stays valid when both run.
        const QString fromStatus = v1target.status;
        if (!annotateMode) {
            applyAntsV1Flip(lines, v1target, targetEmoji);
        }
        int noteLine = -1;
        bool noteAlreadyPresent = false;
        if (!note.isEmpty()) {
            noteLine = appendBodyNote(lines, v1target.firstLine, note,
                                      &noteAlreadyPresent);
        }
        const QString updated = lines.join(QChar('\n'));
        // ANTS-2136 — dry_run preview (ants-v1 path): the locator
        // resolved and the surgery is computed in-memory; return the
        // would-be edit WITHOUT writing ROADMAP.md.
        if (dryRun) {
            const QByteArray previewUtf8 = updated.toUtf8();
            QJsonObject out;
            out["ok"]              = true;
            out["op"]              = annotateMode
                                       ? QStringLiteral("annotate")
                                       : QStringLiteral("flip");
            out["dry_run"]         = true;
            out["format"]          = QStringLiteral("ants-v1");
            out["from_status"]     = fromStatus;
            out["to_status"]       = annotateMode ? fromStatus
                                                  : targetEmoji;
            out["file"]            = QStringLiteral("ROADMAP.md");
            out["line"]            = v1target.firstLine + 1;
            out["bytes"]           = static_cast<qint64>(previewUtf8.size());
            out["anchor_injected"] = false;
            out["write_path"]      = QStringLiteral("patch");   // ANTS-4464
            out["id"]              = v1target.id;
            if (!note.isEmpty()) {
                out["note_appended"] = !noteAlreadyPresent;
                out["note_line"]     = noteLine + 1;
                if (noteAlreadyPresent)
                    out["note_already_present"] = true;
            }
            return QJsonDocument(out);
        }
        const qint64 sizeBefore = QFileInfo(roadmapPath).size();  // ANTS-3702
        QSaveFile rw(roadmapPath);
        if (!rw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return rlErr(QStringLiteral("roadmap_write_failed"),
                QStringLiteral("roadmap_log: could not open \"%1\" "
                               "for writing").arg(roadmapPath));
        }
        const QByteArray utf8 = updated.toUtf8();
        if (rw.write(utf8) != utf8.size() || !rw.commit()) {
            return rlErr(QStringLiteral("roadmap_write_failed"),
                QStringLiteral("roadmap_log: atomic write of \"%1\" "
                               "failed").arg(roadmapPath));
        }
        QJsonObject out;
        out["ok"]              = true;
        out["op"]              = annotateMode
                                   ? QStringLiteral("annotate")
                                   : QStringLiteral("flip");
        out["format"]          = QStringLiteral("ants-v1");
        out["from_status"]     = fromStatus;
        out["to_status"]       = annotateMode ? fromStatus
                                              : targetEmoji;
        out["file"]            = QStringLiteral("ROADMAP.md");
        out["line"]            = v1target.firstLine + 1;
        rcSetWriteBytes(out, sizeBefore, static_cast<qint64>(utf8.size()));
        out["anchor_injected"] = false;
        out["write_path"]      = QStringLiteral("patch");   // ANTS-4464
        out["id"]              = v1target.id;
        if (!note.isEmpty()) {
            out["note_appended"] = !noteAlreadyPresent;
            out["note_line"]     = noteLine + 1;
            if (noteAlreadyPresent)
                out["note_already_present"] = true;
        }
        if (!noteScrubbedNames.isEmpty()) {
            QJsonArray dropped;
            for (const QString &n : noteScrubbedNames) dropped.append(n);
            out["note_scrubbed_params"] = dropped;
        }
        // ANTS-2089 — confirm-after compact echo (ants-v1 flip).
        if (rcReturnHeadlineOnly(req)) {
            out["post_bullets"] = QJsonArray{ rcCompactBullet(
                v1target.id,
                rcStatusWord(out.value(QStringLiteral("to_status"))
                                 .toString()),
                v1target.headline) };
        }
        return QJsonDocument(out);
    };

    if (bullets.isEmpty() &&
        markdownBytes > kRoadmapMinParseableSize) {
        // ANTS-1441 — try ants-v1 native format. Different bullet
        // shape (`- 📋 [ANTS-NNNN] **headline**`); no anchor
        // injection or counter use.
        if (locAnchor.isEmpty()) {
            // anchor locator is GFM-specific; ants-v1 only supports
            // id + headline. If caller passed anchor, it's a hard
            // mismatch — surface as bad_op_combo only after we've
            // confirmed the file is ants-v1.
        }
        const QVector<AntsV1Bullet> v1bullets = walkAntsV1Bullets(lines);
        if (!v1bullets.isEmpty()) {
            // Reject anchor locator (ants-v1 doesn't use caret anchors).
            if (!locAnchor.isEmpty()) {
                return rlErr(QStringLiteral("bad_op_combo"),
                    QStringLiteral("roadmap_log: anchor locator is not "
                                   "supported on ants-v1 native format "
                                   "— use `id` (e.g. \"ANTS-1394\") or "
                                   "`headline` instead"));
            }
            // Locate target by id or headline.
            QVector<int> v1matches;
            if (!locId.isEmpty()) {
                for (int i = 0; i < v1bullets.size(); ++i) {
                    if (v1bullets.at(i).id == locId)
                        v1matches.append(i);
                }
            } else {
                const quint64 needHash =
                    rcFnv1a64(rcNormaliseHeadline(locHeadline));
                for (int i = 0; i < v1bullets.size(); ++i) {
                    if (rcFnv1a64(
                            rcNormaliseHeadline(v1bullets.at(i).headline))
                        == needHash) {
                        v1matches.append(i);
                    }
                }
            }
            const int v1matchedCount = v1matches.size();
            if (v1matchedCount == 0 || v1matchedCount > 1) {
                QJsonArray suggestions;
                if (v1matchedCount > 1) {
                    for (int i = 0; i < v1matches.size() &&
                                    suggestions.size() < 3; ++i) {
                        const auto &b = v1bullets.at(v1matches.at(i));
                        QJsonObject s;
                        s["headline"] = b.headline;
                        s["id"]       = b.id;
                        suggestions.append(s);
                    }
                } else if (!locId.isEmpty()) {
                    // ANTS-3566 — ID locator: rank sibling ids by shared
                    // prefix, not headline overlap with the id string.
                    QStringList ids;
                    for (const auto &b : v1bullets) ids.append(b.id);
                    for (const int idx :
                         rcRankIdsBySharedPrefix(ids, locId, 3)) {
                        const auto &b = v1bullets.at(idx);
                        QJsonObject s;
                        s["headline"] = b.headline;
                        s["id"]       = b.id;
                        suggestions.append(s);
                    }
                } else {
                    const QString norm = rcNormaliseHeadline(locHeadline);
                    QVector<QPair<int, int>> scored;
                    for (int i = 0; i < v1bullets.size(); ++i) {
                        const QString h = rcNormaliseHeadline(
                            v1bullets.at(i).headline);
                        int sharedLen = 0;
                        const int lim = std::min(h.size(), norm.size());
                        while (sharedLen < lim &&
                               h.at(sharedLen) == norm.at(sharedLen)) {
                            ++sharedLen;
                        }
                        if (sharedLen > 0)
                            scored.append(qMakePair(sharedLen, i));
                    }
                    std::sort(scored.begin(), scored.end(),
                        [](const QPair<int,int> &a,
                           const QPair<int,int> &b) {
                            return a.first > b.first;
                        });
                    for (int k = 0; k < scored.size() &&
                                    suggestions.size() < 3; ++k) {
                        const auto &b = v1bullets.at(scored.at(k).second);
                        QJsonObject s;
                        s["headline"] = b.headline;
                        s["id"]       = b.id;
                        suggestions.append(s);
                    }
                }
                const QString code = (v1matchedCount == 0)
                    ? QStringLiteral("bullet_not_found")
                    : QStringLiteral("bullet_ambiguous");
                const QString msg = (v1matchedCount == 0)
                    ? QStringLiteral("roadmap_log: locator matched "
                                     "zero ants-v1 bullets")
                    : QStringLiteral("roadmap_log: locator matched "
                                     "%1 ants-v1 bullets — narrow with id")
                        .arg(v1matchedCount);
                return rlSugErr(code, msg, suggestions, v1matchedCount);
            }
            // ANTS-3561 — the surgery + envelope is shared with the
            // mixed-file GFM-no-match fallback (see step 7 below).
            return applyAntsV1FlipResult(v1bullets, v1matches.first());
        }
        // Neither GFM nor ants-v1 — genuinely unrecognised.
        // (Pass-headings is caught earlier; see the format_mismatch
        // gate just after the file read — ANTS-2031.)
        // ANTS-1463 — refusal envelope gains shared hint +
        // expected_format fields for shape parity with the other
        // three unrecognised_format sites.
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = QStringLiteral("unrecognised_format");
        env["error"] = QStringLiteral(
            "roadmap_log: \"%1\" parsed zero bullets from %2 bytes "
            "(neither GFM-task-list nor ants-v1 native format "
            "recognised) — cannot safely flip")
                .arg(roadmapPath).arg(markdownBytes);
        env["path"]            = roadmapPath;
        env["bytes"]           = markdownBytes;
        env["hint"]            = kUnrecognisedFormatHint();
        env["expected_format"] = kUnrecognisedFormatExpected();
        return QJsonDocument(env);
    }

    // 7. Locator. id > anchor > headline.
    auto headlineHash = [](const QString &h) {
        return rcFnv1a64(rcNormaliseHeadline(h));
    };
    QVector<int> matchIndices;
    if (!locId.isEmpty()) {
        for (int i = 0; i < bullets.size(); ++i) {
            if (bullets.at(i).boldId == locId) {
                matchIndices.append(i);
            }
        }
    } else if (!locAnchor.isEmpty()) {
        for (int i = 0; i < bullets.size(); ++i) {
            if (bullets.at(i).anchor == locAnchor) {
                matchIndices.append(i);
            }
        }
    } else {
        // ANTS-3378 — match against every headline form the bullet can be
        // located by (canonical roadmap_query headline, de-marked-up head,
        // bold-ID label, legacy raw head), not just the raw stored head.
        const quint64 needHash = headlineHash(locHeadline);
        for (int i = 0; i < bullets.size(); ++i) {
            if (rcGfmHeadlineMatchHashes(bullets.at(i).headline,
                                         bullets.at(i).boldId)
                    .contains(needHash)) {
                matchIndices.append(i);
            }
        }
    }
    const int matchedCount = matchIndices.size();
    // ANTS-2053 — a roadmap_query synthetic id (content-hash, 10 lowercase
    // base36 chars; emitted with synthetic:true for ID-less GFM bullets)
    // is NOT a roadmap_log locator: the write path matches on boldId, which
    // is empty for exactly those bullets. Rather than the generic
    // bullet_not_found + unrelated nearest-neighbour suggestions, refuse
    // with a targeted code that names the working `headline` / `anchor`
    // fallback. We do NOT resolve the hash here: the write-path GFM walker
    // and the read-path parser are separate code, so cross-parser hash
    // parity isn't guaranteed — naming the fallback is the honest fix.
    // Fires only on a zero-match locId (a real 10-char id would have
    // matched a bullet), so it never shadows a legitimate locator.
    if (matchedCount == 0 && !locId.isEmpty()) {
        static const QRegularExpression kSyntheticIdShape(
            QStringLiteral("^[0-9a-z]{10}$"));
        if (kSyntheticIdShape.match(locId).hasMatch()) {
            QJsonObject env;
            env["ok"]      = false;
            env["code"]    = QStringLiteral("synthetic_id_not_locatable");
            env["error"]   = QStringLiteral(
                "roadmap_log: \"%1\" looks like a roadmap_query synthetic "
                "id (content-hash for an ID-less bullet); those are not "
                "valid write locators. Re-target by `headline` (exact "
                "text) or `anchor`.").arg(locId);
            env["locator"] = locId;
            env["hint"]    = QStringLiteral(
                "Use the bullet's `headline` from the same roadmap_query "
                "result as the locator — or `headline_full` when the "
                "headline was truncated (ANTS-2075) — or a `line_range`. "
                "On a flip roadmap_log injects a durable caret anchor you "
                "can reuse next time.");
            return QJsonDocument(env);
        }
    }
    // ANTS-3561 — mixed-format fallback: a GFM-majority roadmap can carry
    // ants-v1 emoji bullets (`- 📋 [ID] **…**`) appended by op:append, which
    // walkGfmBullets does not recognise. When the GFM locator matched none
    // (and the caller used id/headline, not a GFM-only anchor), try the
    // ants-v1 walker before refusing — so those bullets stay addressable by
    // id/headline. Falls through to the GFM refusal below on any non-single
    // ants-v1 match, preserving the pure-GFM not-found path (incl. the
    // synthetic-id refusal above).
    if (matchedCount == 0 && locAnchor.isEmpty()) {
        const QVector<AntsV1Bullet> v1bullets = walkAntsV1Bullets(lines);
        if (!v1bullets.isEmpty()) {
            QVector<int> v1matches;
            if (!locId.isEmpty()) {
                for (int i = 0; i < v1bullets.size(); ++i)
                    if (v1bullets.at(i).id == locId) v1matches.append(i);
            } else if (!locHeadline.isEmpty()) {
                const quint64 needHash =
                    rcFnv1a64(rcNormaliseHeadline(locHeadline));
                for (int i = 0; i < v1bullets.size(); ++i)
                    if (rcFnv1a64(rcNormaliseHeadline(
                            v1bullets.at(i).headline)) == needHash)
                        v1matches.append(i);
            }
            if (v1matches.size() == 1)
                return applyAntsV1FlipResult(v1bullets, v1matches.first());
        }
    }
    if (matchedCount == 0 || matchedCount > 1) {
        // Suggestions: for ambiguous, the actual matches (≤ 3); for
        // not-found, up to 3 nearest-neighbour bullets ranked by
        // shared headline prefix with the locator string.
        QJsonArray suggestions;
        if (matchedCount > 1) {
            for (int i = 0; i < matchIndices.size() &&
                            suggestions.size() < 3; ++i) {
                const auto &b = bullets.at(matchIndices.at(i));
                QJsonObject s;
                s["headline"] = b.headline;
                if (!b.anchor.isEmpty()) s["anchor"] = b.anchor;
                if (!b.boldId.isEmpty()) s["id"]     = b.boldId;
                suggestions.append(s);
            }
        } else if (!locId.isEmpty()) {
            // ANTS-3566 — ID locator: rank sibling bold-IDs by shared prefix,
            // not headline overlap with the id string (which surfaces every
            // bullet whose headline merely starts with the id's leading char).
            // Empty when nothing shares a prefix — no misleading suggestions.
            QStringList ids;
            for (const auto &b : bullets) ids.append(b.boldId);
            for (const int idx : rcRankIdsBySharedPrefix(ids, locId, 3)) {
                const auto &b = bullets.at(idx);
                QJsonObject s;
                s["headline"] = rcGfmCanonicalHeadline(b.headline);
                s["line"]     = b.firstLine + 1;
                if (!b.anchor.isEmpty()) s["anchor"] = b.anchor;
                if (!b.boldId.isEmpty()) s["id"]     = b.boldId;
                suggestions.append(s);
            }
        } else {
            // ANTS-3378 — rank nearest neighbours by token overlap against
            // each bullet's CANONICAL headline (the form roadmap_query
            // reports), not the raw stored head. Comparing the raw head
            // (markdown + bold-ID + em-dash tail) made the old shared-prefix
            // ranking surface unrelated bullets. Each suggestion carries the
            // bullet line so the caller can re-target precisely.
            const QString needle = !locHeadline.isEmpty()
                ? locHeadline : locAnchor;
            const QString norm = rcNormaliseHeadline(needle);
            const QStringList needleToks =
                norm.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const QSet<QString> needleTokSet(needleToks.begin(),
                                             needleToks.end());
            // Score = token-Jaccard (≥1 shared token); shared-prefix as a
            // small tie-breaker for the zero-overlap-but-near-prefix case.
            QVector<QPair<double, int>> scored;
            for (int i = 0; i < bullets.size(); ++i) {
                const QString canon =
                    rcGfmCanonicalHeadline(bullets.at(i).headline);
                const QString h = rcNormaliseHeadline(canon);
                const QStringList hToks =
                    h.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                const QSet<QString> hTokSet(hToks.begin(), hToks.end());
                double score = rcHeadlineJaccard(needleTokSet, hTokSet, 1);
                if (score < 0.0) {
                    int sharedLen = 0;
                    const int lim = std::min(h.size(), norm.size());
                    while (sharedLen < lim &&
                           h.at(sharedLen) == norm.at(sharedLen)) {
                        ++sharedLen;
                    }
                    // Keep below any real Jaccard hit (∈ (0,1]).
                    if (sharedLen > 0) score = sharedLen / 100000.0;
                }
                if (score > 0.0) scored.append(qMakePair(score, i));
            }
            std::sort(scored.begin(), scored.end(),
                [](const QPair<double,int> &a, const QPair<double,int> &b) {
                    return a.first > b.first;
                });
            for (int k = 0; k < scored.size() &&
                            suggestions.size() < 3; ++k) {
                const auto &b = bullets.at(scored.at(k).second);
                QJsonObject s;
                s["headline"] = rcGfmCanonicalHeadline(b.headline);
                s["line"]     = b.firstLine + 1;
                if (!b.anchor.isEmpty()) s["anchor"] = b.anchor;
                if (!b.boldId.isEmpty()) s["id"]     = b.boldId;
                suggestions.append(s);
            }
        }
        const QString code = (matchedCount == 0)
            ? QStringLiteral("bullet_not_found")
            : QStringLiteral("bullet_ambiguous");
        // ANTS-4574 — "narrow with anchor or id" is only advice when the
        // caller has an unused locator left. Measured on a GFM roadmap with
        // two bullets leading `**Photo mode**`: the id WAS the ambiguous
        // locator and neither bullet carried a caret anchor, so every route
        // the message named was closed and the caller could reach neither
        // bullet. Name only routes that exist, and say so when none does.
        QString msg;
        if (matchedCount == 0) {
            msg = QStringLiteral("roadmap_log: locator matched zero bullets");
        } else if (!locId.isEmpty()) {
            bool anyAnchor = false;
            for (const int mi : matchIndices) {
                if (!bullets.at(mi).anchor.isEmpty()) { anyAnchor = true; break; }
            }
            // Whether the headline route can actually separate them: two
            // bullets sharing an id AND a headline are reachable by neither,
            // and saying so beats naming a third route that also fails.
            QSet<QString> distinctHeadlines;
            for (const int mi : matchIndices) {
                distinctHeadlines.insert(
                    rcGfmCanonicalHeadline(bullets.at(mi).headline));
            }
            msg = QStringLiteral("roadmap_log: id \"%1\" matched %2 bullets — "
                                 "that id is not unique in this file")
                      .arg(locId).arg(matchedCount);
            if (anyAnchor) {
                msg += QStringLiteral("; narrow with `anchor`, which the "
                                      "matched bullets carry");
            } else if (distinctHeadlines.size() == matchedCount) {
                msg += QStringLiteral("; the matched bullets carry no anchor, "
                                      "so narrow with `headline` using one of "
                                      "the headlines in suggestions[]");
            } else {
                msg += QStringLiteral("; the matched bullets carry no anchor "
                                      "and do not have distinct headlines, so "
                                      "no locator can address them — "
                                      "disambiguate them in the file first");
            }
        } else {
            msg = QStringLiteral("roadmap_log: locator matched %1 bullets — "
                                 "narrow with anchor or id").arg(matchedCount);
        }
        return rlSugErr(code, msg, suggestions, matchedCount);
    }
    const GfmBullet &target = bullets.at(matchIndices.first());

    // 8. Fenced-code refusal (INV-13).
    if (target.insideFenced) {
        return rlErr(QStringLiteral("anchor_unsafe_context"),
            QStringLiteral("roadmap_log: located bullet is inside a "
                           "fenced code block — cannot edit it safely "
                           "(anchor inject / flip / note append)")
                + rcFenceOpenerHint(target.fenceOpenLine));
    }

    // 9. Determine if anchor injection is needed (INV-5). Annotate
    //    never injects an anchor — it is purely additive prose.
    const bool needInjection = !annotateMode &&
        target.boldId.isEmpty() && target.anchor.isEmpty();

    // 10. If injection needed, derive prefix + consume counter.
    QString anchorToInject;
    qint64 newCounter = -1;
    QString counterPath;
    if (needInjection) {
        QString prefix = req.value(QStringLiteral("prefix_hint"))
                            .toString();
        if (prefix.isEmpty()) {
            const QString leaf =
                QFileInfo(callerCanonical).fileName();
            prefix = leaf.left(4).toUpper();
            if (prefix.isEmpty()) prefix = QStringLiteral("ROOT");
        } else {
            static const QRegularExpression rxPrefix(
                QStringLiteral("^[A-Z][A-Z0-9_-]{0,15}$"));
            if (!rxPrefix.match(prefix).hasMatch()) {
                return rlErr(QStringLiteral("bad_op_combo"),
                    QStringLiteral("roadmap_log: prefix_hint \"%1\" "
                                   "does not match "
                                   "^[A-Z][A-Z0-9_-]{0,15}$")
                        .arg(prefix));
            }
        }
        counterPath = callerCanonical + QLatin1Char('/') +
            QStringLiteral(".roadmap-counter");
        qint64 counter = 0;
        if (QFile::exists(counterPath)) {
            QFile cf(counterPath);
            if (!cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return rlErr(QStringLiteral("counter_read_failed"),
                    QStringLiteral("roadmap_log: could not read "
                                   ".roadmap-counter at \"%1\"")
                        .arg(counterPath));
            }
            const QByteArray raw = cf.readAll().trimmed();
            if (!raw.isEmpty()) {
                bool ok = false;
                counter =
                    QString::fromUtf8(raw).toLongLong(&ok);
                if (!ok) {
                    return rlErr(QStringLiteral("counter_read_failed"),
                        QStringLiteral("roadmap_log: "
                                       ".roadmap-counter is not a "
                                       "number"));
                }
            }
        }
        // Open Q 3 resolution: create on first use. counter == 0
        // → newCounter == 1.
        newCounter = counter + 1;
        const QString idPart = QStringLiteral("%1")
            .arg(newCounter, 4, 10, QLatin1Char('0'));
        anchorToInject =
            prefix.toLower() + QLatin1Char('-') + idPart;
    }

    // 11. Apply the surgery in-place on `lines`. applyGfmFlip is
    //     skipped under annotate (status preserved, no anchor); the
    //     note is appended after the body. applyGfmFlip edits the
    //     headline line in place (no line-count change) so the
    //     headlineLine index stays valid for appendBodyNote.
    if (!annotateMode) {
        applyGfmFlip(lines, target, targetEmoji, anchorToInject);
    }
    int noteLine = -1;
    bool noteAlreadyPresent = false;
    if (!note.isEmpty()) {
        noteLine = appendBodyNote(lines, target.headlineLine, note,
                                  &noteAlreadyPresent);
    }
    const QString updated = lines.join(QChar('\n'));

    // ANTS-2136 — dry_run preview: the locator resolved, the surgery is
    // computed in-memory (status emoji / appended note / would-be anchor),
    // and we return the preview WITHOUT writing ROADMAP.md or bumping
    // .roadmap-counter. Mirrors the success envelope below but carries
    // dry_run:true and `bytes` (would-be) instead of bytes_written.
    if (dryRun) {
        const QByteArray previewUtf8 = updated.toUtf8();
        QJsonObject out;
        out["ok"]              = true;
        out["op"]              = annotateMode ? QStringLiteral("annotate")
                                              : QStringLiteral("flip");
        out["dry_run"]         = true;
        out["from_status"]     = target.status;
        out["to_status"]       = annotateMode ? target.status : targetEmoji;
        out["file"]            = QStringLiteral("ROADMAP.md");
        out["line"]            = target.firstLine + 1;
        out["bytes"]           = static_cast<qint64>(previewUtf8.size());
        out["anchor_injected"] = !anchorToInject.isEmpty();
        // ANTS-4464 — GFM is reachable only on a file-backed project (a
        // migrated project's ROADMAP.md is the render's own ants-v1 output), so
        // this arm is always the patch path. Declared anyway: the field is
        // useful precisely because a caller can read it unconditionally.
        out["write_path"]      = QStringLiteral("patch");
        if (!anchorToInject.isEmpty()) out["anchor"] = anchorToInject;
        if (!target.boldId.isEmpty()) out["id"] = target.boldId;
        if (needInjection && newCounter >= 0) out["counter"] = newCounter;
        if (!note.isEmpty()) {
            out["note_appended"] = !noteAlreadyPresent;
            out["note_line"]     = noteLine + 1;
            if (noteAlreadyPresent) out["note_already_present"] = true;
        }
        return QJsonDocument(out);
    }

    // 12. Write ROADMAP.md atomically.
    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for "
                           "writing").arg(roadmapPath));
    }
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit()) {
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" "
                           "failed").arg(roadmapPath));
    }

    // 13. Counter rewrite — only when an anchor was injected (INV-8).
    if (needInjection && newCounter >= 0) {
        QSaveFile cw(counterPath);
        if (!cw.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return rlErr(QStringLiteral("counter_write_failed"),
                QStringLiteral("roadmap_log: could not open "
                               ".roadmap-counter for writing"));
        }
        const QByteArray cv =
            (QString::number(newCounter) + QChar('\n')).toUtf8();
        if (cw.write(cv) != cv.size() || !cw.commit()) {
            return rlErr(QStringLiteral("counter_write_failed"),
                QStringLiteral("roadmap_log: atomic write of "
                               ".roadmap-counter failed"));
        }
    }

    // 14. Success envelope.
    QJsonObject out;
    out["ok"]              = true;
    out["op"]              = annotateMode ? QStringLiteral("annotate")
                                          : QStringLiteral("flip");
    out["from_status"]     = target.status;
    out["to_status"]       = annotateMode ? target.status : targetEmoji;
    out["file"]            = QStringLiteral("ROADMAP.md");
    out["line"]            = target.firstLine + 1;  // 1-based
    rcSetWriteBytes(out, sizeBefore, static_cast<qint64>(utf8.size()));
    out["anchor_injected"] = !anchorToInject.isEmpty();
    out["write_path"]      = QStringLiteral("patch");   // ANTS-4464
    if (!anchorToInject.isEmpty()) out["anchor"] = anchorToInject;
    if (!target.boldId.isEmpty()) out["id"]      = target.boldId;
    if (needInjection && newCounter >= 0)
        out["counter"] = newCounter;
    if (!note.isEmpty()) {
        out["note_appended"] = !noteAlreadyPresent;
        out["note_line"]     = noteLine + 1;
        if (noteAlreadyPresent) out["note_already_present"] = true;
    }
    if (!noteScrubbedNames.isEmpty()) {
        QJsonArray dropped;
        for (const QString &n : noteScrubbedNames) dropped.append(n);
        out["note_scrubbed_params"] = dropped;
    }
    // ANTS-2089 — confirm-after compact echo (GFM flip).
    if (rcReturnHeadlineOnly(req)) {
        out["post_bullets"] = QJsonArray{ rcCompactBullet(
            target.boldId,
            rcStatusWord(out.value(QStringLiteral("to_status")).toString()),
            target.headline) };
    }
    return QJsonDocument(out);
}

// ANTS-4097 — the wrapped paragraph an amend_body edit landed in, echoed
// on the success envelope. amend_body matches within one physical line, so
// changing a phrase that spans a hard-wrapped paragraph takes N calls; each
// one succeeds, each looks right in isolation, and the N-line paragraph they
// jointly produce is checked by nothing. {amended, body_line, bytes_written}
// has no view of it and there is no prompt to re-read precisely BECAUSE the
// calls succeeded. Returning the paragraph is the smallest thing that makes
// the joint result visible without re-reading the file.
// `editedIdx` is 0-based into `lines`; a bullet marker or a blank bounds the
// paragraph, so the run never spills into a neighbouring bullet.
static QString rcAmendedParagraph(const QStringList &lines, int editedIdx) {
    if (editedIdx < 0 || editedIdx >= lines.size()) return QString();
    const auto isBoundary = [](const QString &s) {
        const QString t = s.trimmed();
        return t.isEmpty() || t.startsWith(QStringLiteral("- ")) ||
               t.startsWith(QStringLiteral("* "));
    };
    int lo = editedIdx, hi = editedIdx;
    while (lo > 0 && !isBoundary(lines.at(lo - 1))) --lo;
    while (hi + 1 < lines.size() && !isBoundary(lines.at(hi + 1))) ++hi;
    QString out = lines.mid(lo, hi - lo + 1).join(QChar('\n'));
    if (out.size() > 2000) { out.truncate(2000); out += QStringLiteral("…"); }
    return out;
}

// ANTS-3406 — roadmap_log op:"amend_body". Locate a bullet (id > anchor
// > headline, same rules as flip) and replace an exact substring of its
// continuation body (`old_text` → `new_text`), guarded by
// amendBodyExact's single-occurrence uniqueness check so it can never
// silently clobber unrelated prose. ANTS-4550 — "exact" is exact apart
// from whitespace runs, which match across a hard-wrapped line break on a
// second pass taken only when the exact one finds nothing. Standalone handler — deliberately NOT
// threaded into the delicate flip/annotate path (ANTS-2059 history); it
// reuses the free walk/scrub/write helpers so flip/annotate stay
// untouched. Exact-match patch only this pass; full-body-replace is out
// of scope (see tests/features/roadmap_log_amend_body/spec.md).
// m_main-independent (caller_cwd + filesystem only).
QJsonDocument RemoteControl::cmdRoadmapLogAmendBody(const QJsonObject &req,
                                                    bool headlineMode) {
    // ANTS-4372 — every message below names the op the CALLER used, so a
    // refusal from the shared machinery never tells them about an op they did
    // not call.
    const QString opName = headlineMode ? QStringLiteral("amend_headline")
                                        : QStringLiteral("amend_body");
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env["ok"]    = false;
        env["code"]  = code;
        env["error"] = message;
        return QJsonDocument(env);
    };
    auto rlSugErr = [](const QString &code, const QString &message,
                       int matched) {
        QJsonObject env;
        env["ok"]      = false;
        env["code"]    = code;
        env["error"]   = message;
        env["matched"] = matched;
        return QJsonDocument(env);
    };

    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // 1. caller_cwd + amend-specific fields.
    const QString callerRaw =
        req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    }
    // to_status / id_hint are meaningless for amend_body (status is
    // preserved; no anchor injection, so the counter is never consumed).
    if (!req.value(QStringLiteral("to_status")).toString().isEmpty()) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: to_status is not accepted under "
                           "op:\"%1\" — it leaves status unchanged; use "
                           "op:\"flip\" to change status").arg(opName));
    }
    if (req.contains(QStringLiteral("id_hint"))) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: id_hint is not accepted under "
                           "op:\"%1\"").arg(opName));
    }
    const QString oldText = req.value(QStringLiteral("old_text")).toString();
    if (oldText.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"%1\" requires a non-empty "
                           "`old_text` — the exact substring to replace "
                           "inside the located bullet's %2")
                .arg(opName, headlineMode ? QStringLiteral("headline")
                                          : QStringLiteral("body")));
    }
    if (!req.contains(QStringLiteral("new_text"))) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"%1\" requires `new_text` "
                           "(may be an empty string to delete the matched "
                           "phrase)").arg(opName));
    }
    QString newText = req.value(QStringLiteral("new_text")).toString();
    // ANTS-1995 — cap both operands. old_text is matched with a linear
    // QString::count (no regex → no ReDoS) but is capped for sanity;
    // new_text is routed through rcScrubLeakedToolXml's backtracking scrub.
    if (oldText.size() > kRcMaxNoteChars ||
        newText.size() > kRcMaxNoteChars) {
        return rlErr(QStringLiteral("too_large"),
            QStringLiteral("roadmap_log: old_text / new_text exceeds %1-char "
                           "cap").arg(kRcMaxNoteChars));
    }
    QStringList newTextScrubbedNames;
    rcScrubLeakedToolXml(newText, newTextScrubbedNames);
    // ANTS-4576 — `new_text` is caller prose landing in a body § 2.6 re-parses,
    // which is what a `note` is (ANTS-4549); one rule, one owner, named for the
    // argument it fired on. Body mode only: the headline is not a trailer
    // surface, and a guard stricter than the parser refuses text never at risk.
    // Here, beside the scrub, for ANTS-4549's reason — before the locate, the
    // format split and the backend split, so both backends give one answer.
    if (!headlineMode) {
        QString shadowErr;
        if (rcdetail::rlNoteDeclaresTrailer(newText, &shadowErr, "new_text"))
            return rlErr(QStringLiteral("body_shadowed"),
                         QStringLiteral("roadmap_log: %1").arg(shadowErr));
    }

    // 2. Locator (id > anchor > headline) — same rules as flip.
    const QString locId       = req.value(QStringLiteral("id")).toString();
    const QString locAnchor   = req.value(QStringLiteral("anchor")).toString();
    const QString locHeadline =
        req.value(QStringLiteral("headline")).toString();
    if (locId.isEmpty() && locAnchor.isEmpty() && locHeadline.isEmpty()) {
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"%1\" needs at least one "
                           "locator — `id`, `anchor`, or `headline`")
                .arg(opName));
    }
    if (!locHeadline.isEmpty() &&
        (!locId.isEmpty() || !locAnchor.isEmpty())) {
        return rlErr(QStringLiteral("bad_op_combo"),
            QStringLiteral("roadmap_log: headline locator is not permitted "
                           "alongside id or anchor"));
    }

    // 3. Resolve caller_cwd → ROADMAP.md.
    const QString callerCanonical =
        QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty()) {
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not "
                           "canonicalise to an existing directory")
                .arg(callerRaw));
    }
    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty()) {
        return rlErr(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));
    }

    // 4. Read markdown.
    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return rlErr(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"")
                .arg(roadmapPath));
    }
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();
    const qint64 markdownBytes = markdown.toUtf8().size();

    // Pass-headings roadmaps store bodies under #### Pass headings, not
    // indented bullet continuation lines — refuse clearly rather than
    // mis-edit (parity with the ANTS-2031 format gate).
    if (rcBulletsArePassHeadings(rlParse(markdown, callerCanonical))) {
        return rlErr(QStringLiteral("unsupported_format"),
            QStringLiteral("roadmap_log: op:\"%1\" is not supported on "
                           "pass-headings roadmaps — edit it with a text "
                           "edit").arg(opName));
    }

    QStringList lines = markdown.split(QChar('\n'));

    // 5. Locate the target's headline line (body span anchor) + the
    //    1-based bullet line + id. GFM first; fall through to ants-v1 for
    //    a big-enough file (mirrors cmdRoadmapLogFlip's format detection).
    int     bodyAnchorLine = -1;
    int     reportLine     = -1;
    QString matchedId;
    // ANTS-3809 § 2.2 — the store path's step-2 locate keys on the headline, so
    // the ants-v1 branch below keeps it rather than only the id.
    QString matchedHeadline;
    QString format;

    const QVector<GfmBullet> bullets = walkGfmBullets(lines, rlDecl(callerCanonical));   // ANTS-3771
    if (!bullets.isEmpty()) {
        format = QStringLiteral("gfm");
        const quint64 need = rcFnv1a64(rcNormaliseHeadline(locHeadline));
        QVector<int> m;
        if (!locId.isEmpty()) {
            for (int i = 0; i < bullets.size(); ++i)
                if (bullets.at(i).boldId == locId) m.append(i);
        } else if (!locAnchor.isEmpty()) {
            for (int i = 0; i < bullets.size(); ++i)
                if (bullets.at(i).anchor == locAnchor) m.append(i);
        } else {
            for (int i = 0; i < bullets.size(); ++i)
                if (rcGfmHeadlineMatchHashes(bullets.at(i).headline,
                                             bullets.at(i).boldId)
                        .contains(need)) m.append(i);
        }
        if (m.size() == 1) {
            const GfmBullet &t = bullets.at(m.first());
            if (t.insideFenced) {
                return rlErr(QStringLiteral("anchor_unsafe_context"),
                    QStringLiteral("roadmap_log: located bullet is inside a "
                                   "fenced code block — refusing to edit")
                        + rcFenceOpenerHint(t.fenceOpenLine));
            }
            // Anchor the body span at firstLine (the checkbox line), NOT
            // headlineLine: walkGfmBullets advances headlineLine past
            // indented non-metadata continuation lines (they count as
            // "headline content" for anchor placement), but for amend_body
            // those lines ARE part of the searchable body. firstLine+1 is the
            // true body start; the headline line itself is excluded from the
            // search (amendBodyExact starts at bodyAnchorLine+1).
            bodyAnchorLine = t.firstLine;
            reportLine     = t.firstLine + 1;
            matchedId      = t.boldId;
        } else {
            // ANTS-3565 — mixed-format fallback: a GFM-majority roadmap can
            // carry appended ants-v1 emoji bullets (`- 📋 [ID] **…**`) that
            // walkGfmBullets never sees. On a GFM zero-match for an
            // id/headline locator (anchor is GFM-only), try the ants-v1 walker
            // before refusing — mirrors cmdRoadmapLogFlip's step-7 fallback.
            // A GFM ambiguous match (>1) is a real GFM problem — do not fall
            // through.
            QVector<int> vm;
            QVector<AntsV1Bullet> v1;
            if (m.isEmpty() && locAnchor.isEmpty() &&
                markdownBytes > kRoadmapMinParseableSize) {
                v1 = walkAntsV1Bullets(lines);
                if (!locId.isEmpty()) {
                    for (int i = 0; i < v1.size(); ++i)
                        if (v1.at(i).id == locId) vm.append(i);
                } else if (!locHeadline.isEmpty()) {
                    for (int i = 0; i < v1.size(); ++i)
                        if (rcFnv1a64(rcNormaliseHeadline(v1.at(i).headline))
                                == need) vm.append(i);
                }
            }
            if (vm.size() != 1) {
                return rlSugErr(m.isEmpty()
                        ? QStringLiteral("bullet_not_found")
                        : QStringLiteral("bullet_ambiguous"),
                    m.isEmpty()
                        ? QStringLiteral("roadmap_log: locator matched zero "
                                         "GFM bullets")
                        : QStringLiteral("roadmap_log: locator matched %1 GFM "
                                         "bullets — narrow with id/anchor")
                            .arg(m.size()),
                    m.size());
            }
            const AntsV1Bullet &t = v1.at(vm.first());
            if (t.insideFenced) {
                return rlErr(QStringLiteral("anchor_unsafe_context"),
                    QStringLiteral("roadmap_log: located bullet is inside a "
                                   "fenced code block — refusing to edit")
                        + rcFenceOpenerHint(t.fenceOpenLine));
            }
            format         = QStringLiteral("ants-v1");
            bodyAnchorLine = t.firstLine;
            reportLine     = t.firstLine + 1;
            matchedId      = t.id;
        }
    } else if (markdownBytes > kRoadmapMinParseableSize) {
        const QVector<AntsV1Bullet> v1 = walkAntsV1Bullets(lines);
        if (v1.isEmpty()) {
            return rlErr(QStringLiteral("unrecognised_format"),
                QStringLiteral("roadmap_log: \"%1\" parsed zero bullets "
                               "(neither GFM-task-list nor ants-v1 native "
                               "format)").arg(roadmapPath));
        }
        format = QStringLiteral("ants-v1");
        if (!locAnchor.isEmpty()) {
            return rlErr(QStringLiteral("bad_op_combo"),
                QStringLiteral("roadmap_log: anchor locator is not supported "
                               "on ants-v1 native format — use `id` or "
                               "`headline`"));
        }
        QVector<int> m;
        if (!locId.isEmpty()) {
            for (int i = 0; i < v1.size(); ++i)
                if (v1.at(i).id == locId) m.append(i);
        } else {
            const quint64 need = rcFnv1a64(rcNormaliseHeadline(locHeadline));
            for (int i = 0; i < v1.size(); ++i)
                if (rcFnv1a64(rcNormaliseHeadline(v1.at(i).headline)) == need)
                    m.append(i);
        }
        if (m.size() != 1) {
            return rlSugErr(m.isEmpty()
                    ? QStringLiteral("bullet_not_found")
                    : QStringLiteral("bullet_ambiguous"),
                m.isEmpty()
                    ? QStringLiteral("roadmap_log: locator matched zero "
                                     "ants-v1 bullets")
                    : QStringLiteral("roadmap_log: locator matched %1 ants-v1 "
                                     "bullets — narrow with id").arg(m.size()),
                m.size());
        }
        const AntsV1Bullet &t = v1.at(m.first());
        if (t.insideFenced) {
            return rlErr(QStringLiteral("anchor_unsafe_context"),
                QStringLiteral("roadmap_log: located bullet is inside a "
                               "fenced code block — refusing to edit")
                    + rcFenceOpenerHint(t.fenceOpenLine));
        }
        bodyAnchorLine  = t.firstLine;
        reportLine      = t.firstLine + 1;
        matchedId       = t.id;
        matchedHeadline = t.headline;
    } else {
        return rlErr(QStringLiteral("unrecognised_format"),
            QStringLiteral("roadmap_log: \"%1\" parsed zero bullets (neither "
                           "GFM-task-list nor ants-v1 native format)")
                .arg(roadmapPath));
    }

    // ANTS-3809 § 2.2 — the store path, after the locate and before the
    // markdown patch below. ants-v1 only (§ 5), which the format gate above
    // has already decided.
    //
    // The uniqueness guarantee is re-evaluated over DIFFERENT TEXT, and that is
    // a behaviour change worth stating rather than discovering. In markdown the
    // two refusals count over the bullet's continuation lines as they sit in
    // the file; here they count over `item.body`, which ANTS-3808 § 2.1 makes
    // the RESIDUAL — the head line's content stripped off. So an `old_text`
    // that overlapped the headline stops matching, and one whose only second
    // occurrence was in the head line stops being ambiguous. Both refusals stay
    // loud; neither silently edits the wrong text. The residual is also the
    // right target, because it is what the render writes back.
    if (format == QStringLiteral("ants-v1")) {
        RoadmapSource::ReadError why = RoadmapSource::ReadError::None;
        QString seamErr;
        // ANTS-3863 — fromMemory: `markdown` is already read and this op needs
        // all of it regardless of backend.
        auto seamText = RoadmapSource::RoadmapText::fromMemory(markdown);
        const auto target =
            roadmapWriteTarget(callerCanonical, seamText, &why, &seamErr);
        QJsonObject refusal;
        if (rcRoadmapSourceRefused(refusal, why, seamErr))
            return QJsonDocument(refusal);
        if (target) {
            RoadmapStore &store = *target->store;
            const qint64 projectId = target->projectId;

            RoadmapParse::BulletRecord rec;
            rec.id           = matchedId;
            rec.headlineFull = matchedHeadline;
            QString pkCode, pkErr;
            const auto itemPk =
                rlStoreItemPk(store, projectId, rec, &pkCode, &pkErr);
            if (!itemPk)
                return rlErr(pkCode, QStringLiteral("roadmap_log: %1").arg(pkErr));
            const auto before = store.readItem(*itemPk, &seamErr);
            if (!before)
                return rlErr(QStringLiteral("store_failed"), seamErr);

            // ANTS-4668 / ANTS-4683 — headline mode writes the store COLUMN.
            // The refusal this replaces reasoned about markdown: patching a
            // rendered line a migrated project regenerates would be reverted
            // by the next render. A write going through the store is the path
            // op:"flip" and op:"annotate" already take, so that reasoning
            // never reached it — and refusing left a headline immutable for
            // the life of the item, while a headline STATES a finding and a
            // finding can be refuted by later evidence.
            //
            // No counterpart to the markdown path's structural-prefix guard,
            // deliberately: the column holds the headline TEXT alone — the
            // marker, id brackets, status emoji and bold delimiters are
            // composed by the render and are not in the value, so `new_text`
            // has no structure here to damage. Uniqueness IS enforced, for
            // the reason it is on the body: a phrase occurring twice must not
            // be clobbered on a guess.
            if (headlineMode) {
                const QString beforeHeadline = before->headline;
                const int hHits = beforeHeadline.count(oldText);
                if (hHits == 0) {
                    QJsonObject env;
                    env[QStringLiteral("ok")]   = false;
                    env[QStringLiteral("code")] =
                        QStringLiteral("headline_match_not_found");
                    env[QStringLiteral("error")] =
                        QStringLiteral("roadmap_log: `old_text` not found in "
                                       "the headline of the located bullet");
                    if (before->body.contains(oldText)) {
                        env[QStringLiteral("hint")] =
                            QStringLiteral("`old_text` occurs in the bullet's "
                                           "BODY, which amend_headline does "
                                           "not edit — op:\"amend_body\" does");
                    }
                    return QJsonDocument(env);
                }
                if (hHits > 1) {
                    return rlErr(QStringLiteral("headline_match_ambiguous"),
                        QStringLiteral("roadmap_log: `old_text` occurs %1 "
                                       "times in the headline — narrow it to "
                                       "a unique substring").arg(hHits));
                }
                const QString newHeadline =
                    QString(beforeHeadline).replace(oldText, newText);
                // The one structural property the column DOES carry: a bullet
                // with no headline cannot be located by headline again, and
                // the render has nothing to emit for it.
                if (newHeadline.trimmed().isEmpty()) {
                    return rlErr(QStringLiteral("bad_args"),
                        QStringLiteral("roadmap_log: that `new_text` would "
                                       "leave the headline empty — a bullet "
                                       "with no headline cannot be located "
                                       "again"));
                }

                HistoryContext hHist;      // ANTS-3822 § 2.5 — one op, one stamp
                hHist.changedAt = rlHistoryStamp();
                const auto hMutate = [&](QString *err) -> bool {
                    if (!store.setItemField(*itemPk, QStringLiteral("headline"),
                                            newHeadline,
                                            QStringLiteral("asserted"), err))
                        return false;
                    hHist.record(*itemPk, QStringLiteral("headline"),
                                 beforeHeadline, newHeadline);
                    // ANTS-4501 § 2.2 — an amend is a modification. No
                    // `shipped` stamp: amend_headline cannot move the status.
                    if (!rlStampModified(store, *itemPk, err))
                        return false;
                    return rlFlushHistory(store, hHist, err);
                };

                RoadmapRender::Outcome hOutcome;
                QString hWriteErr;
                const auto hr = RoadmapWrite::commitAndRender(
                    store, projectId, callerCanonical, roadmapPath, dryRun,
                    hMutate, &hOutcome, &hWriteErr);
                QJsonObject env;
                if (rcRoadmapWriteRefused(env, hr, hWriteErr, hOutcome))
                    return QJsonDocument(env);
                rlAttachHistoryNote(env, store, hHist);   // ANTS-3822 § 2.3.1

                env[QStringLiteral("ok")]       = true;
                env[QStringLiteral("op")]       = opName;
                env[QStringLiteral("format")]   = QStringLiteral("ants-v1");
                env[QStringLiteral("file")]     = QStringLiteral("ROADMAP.md");
                env[QStringLiteral("amended")]  = true;
                // Echoed so the caller can read the joint result without a
                // re-query — the amend_body path's `body_paragraph` rationale.
                env[QStringLiteral("headline")] = newHeadline;
                if (!matchedId.isEmpty())
                    env[QStringLiteral("id")] = matchedId;
                rcRoadmapWriteFields(env, hOutcome, dryRun);   // ANTS-4463
                if (!newTextScrubbedNames.isEmpty()) {
                    QJsonArray dropped;
                    for (const QString &n : newTextScrubbedNames)
                        dropped.append(n);
                    env[QStringLiteral("note_scrubbed_params")] = dropped;
                }
                if (dryRun)
                    env[QStringLiteral("dry_run")] = true;
                return QJsonDocument(env);
            }

            // ANTS-4550 — the same seam amendBodyExact() runs, so the two
            // paths cannot answer one old_text differently. Indent::None:
            // the store holds the body as an UNINDENTED residual the render
            // indents on the way out, so prefixing here would double it.
            const WrapMatch::Patch patch = WrapMatch::patchOnce(
                before->body, oldText, newText, WrapMatch::Indent::None);
            const int hits = patch.hits;
            if (patch.structuredBlock) return rlWrappedBlockErr();  // ANTS-4612
            if (hits == 0) {
                QJsonObject env;
                env[QStringLiteral("ok")]    = false;
                env[QStringLiteral("code")]  = QStringLiteral("body_match_not_found");
                env[QStringLiteral("error")] =
                    QStringLiteral("roadmap_log: `old_text` not found in the body of "
                                   "the located bullet");
                const QString foldedOld = oldText.simplified();
                // ANTS-4667 — the trap this hint exists for: the trailer lines
                // are COMPOSED at render time from their own columns, and
                // roadmap_query include_body:true returns them INSIDE `body`.
                // So a caller reads one back verbatim, passes it as old_text,
                // and is told it is not there. Name the op that does reach it
                // rather than leaving a dead end.
                static const QStringList kTrailerLabels = {
                    QStringLiteral("Layman:"), QStringLiteral("Kind:"),
                    QStringLiteral("Source:"), QStringLiteral("Lanes:"),
                    QStringLiteral("Evidence:")};
                QString hitLabel;
                for (const QString &lbl : kTrailerLabels)
                    if (oldText.contains(lbl)) { hitLabel = lbl; break; }
                if (!hitLabel.isEmpty()) {
                    QJsonObject tenv;
                    tenv[QStringLiteral("ok")]    = false;
                    tenv[QStringLiteral("code")]  =
                        QStringLiteral("body_match_not_found");
                    tenv[QStringLiteral("error")] = QStringLiteral(
                        "roadmap_log: `old_text` not found in the body of the "
                        "located bullet");
                    tenv[QStringLiteral("hint")] = QStringLiteral(
                        "`old_text` names the trailer key \"%1\", and a trailer "
                        "line is COMPOSED by the render from its own column — it "
                        "is not in the stored body, even though "
                        "roadmap_query include_body:true shows it inside `body`. "
                        "Use op:\"amend_field\" with that column instead of "
                        "declaring the key inside the body, which sets the column "
                        "one-way and cannot be withdrawn.").arg(hitLabel);
                    return QJsonDocument(tenv);
                }
                if (matchedHeadline.contains(oldText)) {
                    // The residual's own failure mode, and the one a caller
                    // coming from the markdown path will hit first.
                    env[QStringLiteral("hint")] =
                        QStringLiteral("`old_text` occurs in the bullet's HEADLINE, "
                                       "which amend_body does not edit — the store "
                                       "holds the body as the residual with the head "
                                       "line stripped off");
                } else if (!foldedOld.isEmpty() &&
                           before->body.simplified().contains(foldedOld)) {
                    // ANTS-4550 — a wrapped phrase now matches, so folded-yet-
                    // absent means the whitespace itself differs from anything
                    // the rule spans (a `>` mid-word, a non-breaking space).
                    env[QStringLiteral("hint")] =
                        QStringLiteral("`old_text` matches the body only with its "
                                       "whitespace folded away entirely — check for "
                                       "a character inside it that is not plain "
                                       "space, tab or newline");
                }
                return QJsonDocument(env);
            }
            if (hits > 1) {
                return rlErr(QStringLiteral("body_match_ambiguous"),
                    QStringLiteral("roadmap_log: `old_text` occurs %1 times in the "
                                   "body — narrow it to a unique substring")
                        .arg(hits));
            }
            const QString newBody = patch.text;
            // ANTS-4097 — echo the paragraph the edit landed in.
            const QString amendedPara =
                rcAmendedParagraph(newBody.split(QChar('\n')), patch.line);

            HistoryContext hist;              // ANTS-3822 § 2.5 — one op, one stamp
            hist.changedAt = rlHistoryStamp();
            QString deriveCode;               // ANTS-4577 — see the flip site
            QStringList keptColumns;

            const auto mutate = [&](QString *err) -> bool {
                if (!store.setItemField(*itemPk, QStringLiteral("body"), newBody,
                                        QStringLiteral("asserted"), err))
                    return false;
                hist.record(*itemPk, QStringLiteral("body"), before->body, newBody);
                // § 2.6 — all five keys, since amend_body supplies none.
                // `kept` (ANTS-4576): amend_body is the only op that can
                // REMOVE a declaration, so it is the only one with a kept
                // column to report.
                if (!rlDeriveTrailerColumns(store, *itemPk, *before, newBody,
                                            {}, &hist, err, &keptColumns,
                                            &deriveCode))
                    return false;
                // ANTS-4501 § 2.2 — a body edit is a modification. No
                // `shipped` stamp: amend_body cannot move the status.
                if (!rlStampModified(store, *itemPk, err))
                    return false;
                return rlFlushHistory(store, hist, err);
            };

            RoadmapRender::Outcome outcome;
            QString writeErr;
            const auto r = RoadmapWrite::commitAndRender(
                store, projectId, callerCanonical, roadmapPath, dryRun, mutate,
                &outcome, &writeErr);
            QJsonObject env;
            if (rcRoadmapWriteRefused(env, r, writeErr, outcome)) {
                if (!deriveCode.isEmpty())     // ANTS-4577
                    env[QStringLiteral("code")] = deriveCode;
                return QJsonDocument(env);
            }
            rlAttachHistoryNote(env, store, hist);   // ANTS-3822 § 2.3.1

            env[QStringLiteral("ok")]      = true;
            env[QStringLiteral("op")]      = opName;
            env[QStringLiteral("format")]  = QStringLiteral("ants-v1");
            env[QStringLiteral("file")]    = QStringLiteral("ROADMAP.md");
            env[QStringLiteral("amended")] = true;
            if (!amendedPara.isEmpty())
                env[QStringLiteral("body_paragraph")] = amendedPara;
            // ANTS-4550 — the match straddled a hard-wrapped line break, so
            // the paragraph re-flowed; say so rather than let the next diff.
            if (patch.wrapped)
                env[QStringLiteral("wrapped_match")] = true;
            // ANTS-4576 — the edit deleted a declaration of a NOT NULL column,
            // whose value therefore SURVIVES and is re-emitted canonically by
            // the render. Reported because it is the one outcome the caller
            // cannot read out of their own diff.
            if (!keptColumns.isEmpty()) {
                QJsonArray kept;
                for (const QString &c : std::as_const(keptColumns)) kept.append(c);
                env[QStringLiteral("trailer_columns_kept")] = kept;
            }
            if (!matchedId.isEmpty())
                env[QStringLiteral("id")] = matchedId;
            // No `line` / `body_line` / `bytes` — a store has no lines
            // (ANTS-3793 INV-2), and `body_line` was an index into the file.
            rcRoadmapWriteFields(env, outcome, dryRun);   // ANTS-4463
            if (!newTextScrubbedNames.isEmpty()) {
                QJsonArray dropped;
                for (const QString &n : newTextScrubbedNames) dropped.append(n);
                env[QStringLiteral("note_scrubbed_params")] = dropped;
            }
            if (dryRun)
                env[QStringLiteral("dry_run")] = true;
            return QJsonDocument(env);
        }
    }

    // 6. Patch. Body span, or — ANTS-4372 — the headline LINE itself.
    int editedLine = -1;
    int hits = 0;
    // ANTS-4550 — set when the match straddled a hard-wrapped line break, so
    // the caller learns the paragraph re-flowed rather than discovering it in
    // the next diff.
    bool wrappedMatch = false;
    if (headlineMode) {
        // The headline is one physical line, so the "spans a line break"
        // failure mode does not exist here; uniqueness is still enforced,
        // for the same reason it is on the body — a phrase occurring twice
        // must not be clobbered on a guess.
        if (bodyAnchorLine >= 0 && bodyAnchorLine < lines.size()) {
            const QString before = lines.at(bodyAnchorLine);
            hits = before.count(oldText);
            if (hits == 1) {
                const QString after = QString(before).replace(oldText, newText);
                // The guard that makes this op SAFER than the hand-edit it
                // replaces: the id, its brackets, the status emoji and the
                // bold delimiters are structure, not prose. Altering them
                // orphans the id or breaks the parse every query verb depends
                // on — and a caller doing this by hand has nothing stopping
                // them.
                static const QRegularExpression prefixRe(
                    QStringLiteral(R"(^(\s*[-*]\s*\S*\s*(?:\[[^\]]+\]\s*)?\*\*))"));
                const auto pb = prefixRe.match(before);
                const auto pa = prefixRe.match(after);
                if (pb.hasMatch() &&
                    (!pa.hasMatch() || pa.captured(1) != pb.captured(1))) {
                    return rlErr(QStringLiteral("bad_args"),
                        QStringLiteral("roadmap_log: that `new_text` would "
                                       "alter the bullet's structural prefix "
                                       "(\"%1\") — the marker, id brackets or "
                                       "bold delimiters. Editing those orphans "
                                       "the id or breaks the parse the query "
                                       "verbs depend on; amend_headline edits "
                                       "the headline TEXT only.")
                            .arg(pb.captured(1).trimmed()));
                }
                lines[bodyAnchorLine] = after;
                editedLine = bodyAnchorLine + 1;   // 1-based
            }
        }
    } else {
        hits = amendBodyExact(lines, bodyAnchorLine, oldText, newText,
                              &editedLine, &wrappedMatch);
    }
    if (hits < 0) return rlWrappedBlockErr();   // ANTS-4612
    if (hits == 0) {
        // ANTS-3467 — distinguish the two common failure modes so the caller
        // self-corrects instead of concluding the text is absent: (a) the
        // phrase exists in ROADMAP.md but outside this bullet's block (wrong
        // bullet targeted); (b) the phrase spans a hard-wrapped line break
        // (bodies wrap at ~70 cols) so no single physical line contains it.
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        // ANTS-4372 — the refusal codes name the surface the caller asked
        // for; `body_match_not_found` from an amend_headline call reads as a
        // different op's error.
        env[QStringLiteral("code")]  =
            headlineMode ? QStringLiteral("headline_match_not_found")
                         : QStringLiteral("body_match_not_found");
        env[QStringLiteral("error")] =
            QStringLiteral("roadmap_log: `old_text` not found in the %1 of "
                           "the located bullet")
                .arg(headlineMode ? QStringLiteral("headline")
                                  : QStringLiteral("body"));
        const QString foldedOld = oldText.simplified();
        if (markdown.contains(oldText)) {
            env[QStringLiteral("hint")] =
                QStringLiteral("`old_text` occurs in ROADMAP.md but outside "
                               "the located bullet's body block (bullet at "
                               "line %1) — verify you targeted the right "
                               "bullet").arg(reportLine);
        } else if (!headlineMode && !foldedOld.isEmpty() &&
                   markdown.simplified().contains(foldedOld)) {
            // ANTS-4550 — a wrapped phrase inside the located bullet now
            // MATCHES, so reaching here with the text present-but-folded
            // means it is present somewhere else: another bullet, or the
            // headline this op does not edit.
            env[QStringLiteral("hint")] =
                QStringLiteral("`old_text` is present in ROADMAP.md only "
                               "with its whitespace folded, and outside this "
                               "bullet's body — check the id, and note that "
                               "amend_body does not edit a headline "
                               "(op:\"amend_headline\" does)");
        }
        return QJsonDocument(env);
    }
    if (hits > 1) {
        return rlErr(headlineMode
                         ? QStringLiteral("headline_match_ambiguous")
                         : QStringLiteral("body_match_ambiguous"),
            QStringLiteral("roadmap_log: `old_text` occurs %1 times in the "
                           "%2 — narrow it to a unique substring")
                .arg(hits)
                .arg(headlineMode ? QStringLiteral("headline")
                                  : QStringLiteral("body")));
    }

    const QString updated = lines.join(QChar('\n'));

    // 7. dry_run preview / atomic write + envelope.
    const qint64 sizeBefore = QFileInfo(roadmapPath).size();   // ANTS-3702
    auto buildEnvelope = [&](bool preview, qint64 byteCount) {
        QJsonObject out;
        out["ok"]        = true;
        out["op"]        = opName;
        out["format"]    = format;
        out["file"]      = QStringLiteral("ROADMAP.md");
        out["line"]      = reportLine;
        out["body_line"] = editedLine + 1;
        out["amended"]   = true;
        // ANTS-4097 — echo the paragraph the edit landed in.
        const QString para = rcAmendedParagraph(lines, editedLine);
        if (!para.isEmpty()) out["body_paragraph"] = para;
        // ANTS-4550 — a wrapped match re-flows the lines it spanned into one.
        if (wrappedMatch) out["wrapped_match"] = true;
        if (preview) { out["dry_run"] = true; out["bytes"] = byteCount; }
        else         { rcSetWriteBytes(out, sizeBefore, byteCount); }
        if (!matchedId.isEmpty()) out["id"] = matchedId;
        if (!newTextScrubbedNames.isEmpty()) {
            QJsonArray dropped;
            for (const QString &n : newTextScrubbedNames) dropped.append(n);
            out["note_scrubbed_params"] = dropped;
        }
        return QJsonDocument(out);
    };

    if (dryRun) {
        return buildEnvelope(true,
            static_cast<qint64>(updated.toUtf8().size()));
    }
    QSaveFile rw(roadmapPath);
    if (!rw.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: could not open \"%1\" for writing")
                .arg(roadmapPath));
    }
    const QByteArray utf8 = updated.toUtf8();
    if (rw.write(utf8) != utf8.size() || !rw.commit()) {
        return rlErr(QStringLiteral("roadmap_write_failed"),
            QStringLiteral("roadmap_log: atomic write of \"%1\" failed")
                .arg(roadmapPath));
    }
    return buildEnvelope(false, static_cast<qint64>(utf8.size()));
}

// ANTS-4667 — op:"amend_field": write one TRAILER COLUMN after creation.
//
// Why it exists. roadmap_log could CREATE a layman / kind / source / lanes /
// evidence at append time and never change one afterwards. amend_body edits
// the stored BODY column, and the trailer lines are COMPOSED at render time
// from their own columns (ANTS-4599), so they are not in the body and
// amend_body cannot reach them. What made a missing feature a TRAP is that
// roadmap_query include_body:true returns those composed lines INSIDE `body` —
// so a caller reads the text back verbatim, passes it as old_text, and is told
// body_match_not_found about a string it just read.
//
// The workaround was one-way: declaring `Layman:` at a line start in the body
// does set the column, last-wins, and cannot be withdrawn, because the write
// path recomputes the column by re-parsing the amended body. It also renders
// plain where a column-sourced one renders bold, so a project that corrected
// one Layman carried two styles it could not reconcile.
//
// Store-only and id-only, both deliberate. The column is the store's, so a
// markdown project has nothing here to edit — there the trailer line IS body
// text and amend_body already reaches it. And an id is the store's own key: a
// headline or anchor locator would re-introduce an ambiguity the key removes,
// on a write that replaces a value outright rather than patching a match.
QJsonDocument RemoteControl::cmdRoadmapLogAmendField(const QJsonObject &req) {
    auto rlErr = [](const QString &code, const QString &message) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        env[QStringLiteral("code")]  = code;
        env[QStringLiteral("error")] = message;
        return QJsonDocument(env);
    };

    const QString id    = req.value(QStringLiteral("id")).toString().trimmed();
    const QString field = req.value(QStringLiteral("field")).toString().trimmed();
    if (id.isEmpty())
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"amend_field\" needs `id` — the "
                           "store's own key, and the only locator it takes"));
    static const QStringList kEditable = {
        QStringLiteral("layman"), QStringLiteral("kind"),
        QStringLiteral("source"), QStringLiteral("lanes"),
        QStringLiteral("evidence")};
    if (!kEditable.contains(field)) {
        return rlErr(QStringLiteral("bad_args"),
            QStringLiteral("roadmap_log: `field` must be one of %1 — the five "
                           "trailer columns. `headline` is op:\"amend_headline\" "
                           "and `status` is op:\"flip\"; body prose is "
                           "op:\"amend_body\".").arg(kEditable.join(QStringLiteral(", "))));
    }
    if (!req.contains(QStringLiteral("value")))
        return rlErr(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: op:\"amend_field\" needs `value` (the "
                           "key must be present; for layman an empty string "
                           "clears the column)"));

    const bool isList = (field == QLatin1String("lanes") ||
                         field == QLatin1String("evidence"));

    // The stored form setItemField wants: canonical JSON text for the two list
    // columns, prose for the other three. An array is the shape a caller
    // reaches for and a comma string is the shape the trailer line uses, so
    // both are accepted rather than making one of them an error.
    QString stored;
    QString display;
    if (isList) {
        QStringList items;
        const QJsonValue v = req.value(QStringLiteral("value"));
        if (v.isArray()) {
            for (const QJsonValue &e : v.toArray()) {
                if (!e.isString())
                    return rlErr(QStringLiteral("bad_args"),
                        QStringLiteral("roadmap_log: `value` for %1 must be an "
                                       "array of strings").arg(field));
                const QString t = e.toString().trimmed();
                if (!t.isEmpty()) items << t;
            }
        } else {
            const QStringList parts = v.toString().split(QChar(','));
            for (const QString &t : parts)
                if (!t.trimmed().isEmpty()) items << t.trimmed();
        }
        QJsonArray arr;
        for (const QString &t : std::as_const(items)) arr.append(t);
        stored  = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        display = items.join(QStringLiteral(", "));
    } else {
        stored  = req.value(QStringLiteral("value")).toString().trimmed();
        display = stored;
        // kind and source are TEXT NOT NULL with no default (ANTS-4576), so
        // "" is not an absent state for them — it is a constraint failure the
        // caller would meet as a raw SQLite string. Refuse it here, named.
        if (stored.isEmpty() && field != QLatin1String("layman"))
            return rlErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: `%1` is NOT NULL and has no absent "
                               "state, so it cannot be set empty. Only `layman` "
                               "is nullable.").arg(field));
        if (field == QLatin1String("kind") &&
            !RoadmapParse::isRecognisedKind(stored)) {
            return rlErr(QStringLiteral("bad_args"),
                QStringLiteral("roadmap_log: \"%1\" is not a recognised Kind — "
                               "the render drops an unrecognised one, so storing "
                               "it would lose the value silently").arg(stored));
        }
    }

    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target = roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) {
        // Remapped for cmdRoadmapLogRender's reason: the shared prologue says
        // `op_unsupported`, and a caller branching on `code` should get one
        // this op's contract promises — with the route that DOES work named,
        // since on a markdown project the trailer line is body text.
        QJsonObject env = refusal.object();
        if (env.value(QStringLiteral("code")).toString()
                == QLatin1String("op_unsupported")) {
            env[QStringLiteral("code")]  = QStringLiteral("unsupported_format");
            env[QStringLiteral("error")] = QStringLiteral(
                "roadmap_log: op:\"amend_field\" writes a STORE column, so it "
                "needs a store-migrated project. On a markdown-backed project "
                "the trailer line is body text — use op:\"amend_body\" on the "
                "`%1:` line itself.").arg(field);
        }
        return QJsonDocument(env);
    }

    RoadmapStore &store = *target->store;
    QString err;
    const auto pk = store.findItem(target->projectId, id, &err);
    if (!pk)
        return rlErr(QStringLiteral("bullet_not_found"),
            QStringLiteral("roadmap_log: no bullet with id \"%1\" in this "
                           "project's store").arg(id));
    const auto before = store.readItem(*pk, &err);
    if (!before)
        return rlErr(QStringLiteral("store_failed"), err);

    // The body wins at render (roadmaprender's shadows(): declared at a line
    // start), AND the next body write recomputes this column by re-parsing the
    // body. So writing the column under a body declaration would be invisible
    // now and reverted later — two ways of being wrong. Refuse, and name the
    // route that works.
    const RoadmapParse::TrailerValues tv =
        RoadmapParse::trailerValuesIn(before->body);
    const RoadmapParse::TrailerMatch &tm =
        field == QLatin1String("layman")   ? tv.layman
      : field == QLatin1String("kind")     ? tv.kind
      : field == QLatin1String("source")   ? tv.source
      : field == QLatin1String("lanes")    ? tv.lanes
                                           : tv.evidence;
    if (tm.offset >= 0 && tm.anchored) {
        return rlErr(QStringLiteral("field_shadowed_by_body"),
            QStringLiteral("roadmap_log: this bullet's BODY declares `%1:` at a "
                           "line start, which wins at render and is re-parsed "
                           "into the column by the next body write — so setting "
                           "the column here would be invisible now and reverted "
                           "later. Edit that declaration with op:\"amend_body\" "
                           "instead.").arg(field));
    }

    const QString oldValue =
        field == QLatin1String("layman")   ? before->layman
      : field == QLatin1String("kind")     ? before->kind
      : field == QLatin1String("source")   ? before->source
      : field == QLatin1String("lanes")    ? before->lanes.join(QStringLiteral(", "))
                                           : before->evidence.join(QStringLiteral(", "));

    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();
    HistoryContext hist;               // ANTS-3822 § 2.5 — one op, one stamp
    hist.changedAt = rlHistoryStamp();
    const auto mutate = [&](QString *e) -> bool {
        if (!store.setItemField(*pk, field, stored,
                                QStringLiteral("asserted"), e))
            return false;
        hist.record(*pk, field, oldValue, display);
        // ANTS-4501 § 2.2 — an amend is a modification, and cannot move status.
        if (!rlStampModified(store, *pk, e))
            return false;
        return rlFlushHistory(store, hist, e);
    };

    RoadmapRender::Outcome outcome;
    QString writeErr;
    const auto r = RoadmapWrite::commitAndRender(
        store, target->projectId, root, roadmapPath, dryRun, mutate,
        &outcome, &writeErr);
    QJsonObject env;
    if (rcRoadmapWriteRefused(env, r, writeErr, outcome))
        return QJsonDocument(env);
    rlAttachHistoryNote(env, store, hist);      // ANTS-3822 § 2.3.1

    env[QStringLiteral("ok")]      = true;
    env[QStringLiteral("op")]      = QStringLiteral("amend_field");
    env[QStringLiteral("id")]      = id;
    env[QStringLiteral("field")]   = field;
    env[QStringLiteral("amended")] = true;
    // Both sides echoed: the caller can see what it replaced without a second
    // query, which is the amend_body path's `body_paragraph` rationale.
    env[QStringLiteral("previous")] = oldValue;
    env[QStringLiteral("value")]    = display;
    rcRoadmapWriteFields(env, outcome, dryRun);  // ANTS-4463
    if (dryRun)
        env[QStringLiteral("dry_run")] = true;
    return QJsonDocument(env);
}

// Store-only and m_main-independent, so the seam is the section ops' shape.
QJsonDocument RemoteControl::cmdRoadmapLogAmendFieldForTest(const QJsonObject &req) {
    return cmdRoadmapLogAmendField(req);
}
