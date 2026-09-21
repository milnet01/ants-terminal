// ANTS-4614 — TU 16/17 — roadmap_log op:"render": publish the store to the
// file on demand, with no semantic change.
// Contract: tests/features/roadmap_write_half/spec.md (Ants4614* cases)
//
// Its own TU for remotecontrol_roadmap_repair.cpp's reason: it is a member that
// never existed in the pre-split remotecontrol_roadmap_log.cpp, so appending it
// last cannot violate any pre-split relative order. That is the whole reason —
// an earlier version of this comment also claimed the log TU sat AT ANTS-3833
// INV-6's line cap, which was never true of it and is not true now (ANTS-4688).
//
// Why the op exists. roadmap_migrate honestly reports markdown_rewritten:false
// (ANTS-4482 shipped the saying-so half) and nothing owned the DOING half: the
// canonical re-render only landed on the next semantic write. On LottoTracker
// that was not cosmetic — the file carried two id dialects the store would
// normalise, so a real, wanted normalisation sat undelivered with no way to
// publish it. The only route was to invent a semantic write purely as a render
// trigger, which pollutes the roadmap with a bullet nobody wanted. And it made
// the migration unverifiable from the repo side: a clean `git status` after
// migrating is indistinguishable from the migration never having run.
//
// It is a WRITE and lives here rather than as a roadmap_query mode, for
// backfill_dates' reason: INV-10 forbids the report from writing at all, so
// putting a writing operation behind the read verb would contradict the
// invariant one section along.

#include "remotecontrol.h"
#include "remotecontrol_internal.h"

#include "roadmapmigrateverb.h"
#include "roadmapparse.h"
#include "roadmapstore.h"
#include "roadmapwrite.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>

using namespace rcdetail;

QJsonDocument RemoteControl::cmdRoadmapLogRender(const QJsonObject &req) {
    QString root, roadmapPath;
    QJsonDocument refusal;
    const auto target = roadmapSectionOpTarget(req, &root, &roadmapPath, &refusal);
    if (!target) {
        // Remapped for ANTS-4501's reason: the shared prologue calls this
        // `op_unsupported`, and a caller branching on `code` should get the one
        // this op's contract promises. A markdown-backed project has nothing to
        // publish FROM — its file already is the source of truth — so the
        // refusal has to say that rather than look like a transient failure.
        QJsonObject env = refusal.object();
        if (env.value(QStringLiteral("code")).toString()
                == QLatin1String("op_unsupported")) {
            // ANTS-4802 — ASK the store before saying what it holds. Only the
            // ants-v1 dialect is store-served (RoadmapSource::migratedProject)
            // while roadmap_migrate reads all three, so a project can be
            // registered, carry hundreds of rows, and still land here. Told
            // "the store holds no row", the reader re-runs the migration — the
            // one action that cannot help, and the one that writes a second
            // snapshot nothing reads.
            const QString subject = root.isEmpty() ? roadmapPath : root;
            QString storeErr;
            RoadmapStore *store = roadmapStoreOrNull(nullptr, &storeErr);
            std::optional<RoadmapStore::ProjectRow> row;
            if (store && !root.isEmpty()) {
                const QString canonical = QFileInfo(root).canonicalFilePath();
                if (!canonical.isEmpty())
                    row = store->readProjectByRoot(canonical, &storeErr);
            }
            env[QStringLiteral("code")] = QStringLiteral("project_not_registered");
            if (row) {
                env[QStringLiteral("code")] = QStringLiteral("unsupported_format");
                env[QStringLiteral("store_row_present")] = true;
                env[QStringLiteral("store_source_format")] = row->sourceFormat;
                env[QStringLiteral("error")] = QStringLiteral(
                    "roadmap_log: render publishes the STORE to the file. “%1” "
                    "IS registered — the store records format “%2” — but only "
                    "the ants-v1 dialect is served from the store, so there is "
                    "no store-side record to publish and its roadmap is read "
                    "and written as markdown. Re-running roadmap_migrate will "
                    "not change this; it writes rows no read path consults.")
                        .arg(subject, row->sourceFormat);
            } else {
                env[QStringLiteral("error")] = QStringLiteral(
                    "roadmap_log: render publishes the STORE to the file, so it "
                    "needs a store-migrated project — the store holds no row for "
                    "\"%1\". Run roadmap_migrate first; on a markdown-backed "
                    "project the file is already the source of truth and there is "
                    "nothing to publish.")
                        .arg(subject);
            }
            return QJsonDocument(env);
        }
        return refusal;
    }

    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // The whole op: the shared write sequence with a mutate that does nothing.
    // No locator, no arguments to validate, no store rows touched — which is
    // the point, and is what makes it safe to reach for when the only thing
    // wanted is the canonical file. Every gate the eight semantic ops run
    // (INV-5's Layman gate, ANTS-4141's divergence guard) runs here too,
    // because they live in commitAndRender rather than in any one op.
    QString err;
    RoadmapRender::Outcome outcome;
    const auto rc = RoadmapWrite::commitAndRender(
        *target->store, target->projectId, root, roadmapPath, dryRun,
        [](QString *) { return true; }, &outcome, &err);

    QJsonObject env;
    if (rcRoadmapWriteRefused(env, rc, err, outcome))
        return QJsonDocument(env);

    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("op")] = QStringLiteral("render");
    env[QStringLiteral("project_root")] = root;
    // files_written / items_rendered / the drift breakdown, in the tense
    // ANTS-4463 requires — the same helper every other write op uses, so a
    // caller reads one envelope shape rather than this op's dialect of it.
    rcRoadmapWriteFields(env, outcome, dryRun);

    // The reviewable artefact ANTS-4614 asks for. `discarded_edit_lines` (and
    // ANTS-4615's breakdown beside it) already say HOW FAR the file had drifted
    // from the store; this says how big the published result is, so a caller
    // can see the render landed without re-reading the file. Absent on a dry
    // run: no bytes were written, and a past-tense name is an assertion.
    if (!dryRun) {
        qint64 bytes = 0;
        for (const QString &f : outcome.filesWritten)
            bytes += QFileInfo(f).size();
        env[QStringLiteral("bytes_written")] = double(bytes);
    }
    return QJsonDocument(env);
}

// Store-only and m_main-independent, so the seam is the section ops' shape.
QJsonDocument RemoteControl::cmdRoadmapLogRenderForTest(const QJsonObject &req) {
    return cmdRoadmapLogRender(req);
}

// ---------------------------------------------------------------------------
// ANTS-4491 — op:"convert": re-import a github-task-list roadmap and republish
// it as canonical ants-v1. Contract: docs/specs/ANTS-4491-dialect-convert.md,
// tests/features/roadmap_convert/spec.md.
//
// HERE rather than in a TU of its own, and the reason is not thematic tidiness
// alone: rc_tu_split INV-3 pins a `TU N/M` head marker on every remotecontrol
// TU, so a new one renumbers all of them. It also belongs here — convert IS a
// publish, in a dialect the store did not previously record.
// ---------------------------------------------------------------------------

namespace {

// The cap on the echoed id list. A convert over a large roadmap allocates
// hundreds; `ids_allocated` carries the true total either way, so the list is an
// aid for review rather than an accounting array. Same reasoning as
// commitAndRender's touchedBullets cap.
constexpr int kMaxEchoedIds = 200;

// The section ops' refusal envelope. A local copy rather than an export of
// remotecontrol_roadmap_log_batch.cpp's `rcSectionOpErr`: it is file-local
// there, and widening a TU's internal linkage to share five lines would couple
// two TUs that are otherwise independent.
QJsonDocument refuseWith(const QString &code, const QString &message) {
    QJsonObject env;
    env[QStringLiteral("ok")]    = false;
    env[QStringLiteral("code")]  = code;
    env[QStringLiteral("error")] = message;
    return QJsonDocument(env);
}

}  // namespace

QJsonDocument RemoteControl::cmdRoadmapLogConvert(const QJsonObject &req) {
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    if (callerRaw.isEmpty())
        return refuseWith(QStringLiteral("missing_field"),
            QStringLiteral("roadmap_log: caller_cwd is required"));
    const QString callerCanonical = QFileInfo(callerRaw).canonicalFilePath();
    if (callerCanonical.isEmpty())
        return refuseWith(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: caller_cwd \"%1\" does not canonicalise "
                           "to an existing directory").arg(callerRaw));

    const QString roadmapPath = findRoadmapUnder(callerCanonical);
    if (roadmapPath.isEmpty())
        return refuseWith(QStringLiteral("no_roadmap"),
            QStringLiteral("roadmap_log: no ROADMAP.md under \"%1\"")
                .arg(callerCanonical));
    const QString root = rcProjectRootFor(callerCanonical);

    QFile rf(roadmapPath);
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text))
        return refuseWith(QStringLiteral("roadmap_read_failed"),
            QStringLiteral("roadmap_log: could not read \"%1\"").arg(roadmapPath));
    const QString markdown = QString::fromUtf8(rf.readAll());
    rf.close();

    // § 4.4 — the accepted set, split by what detectRoadmapFormat() returns. It
    // answers exactly one of three, and its no-signal default is ants-v1.
    //
    // `ants-v1` is ACCEPTED rather than refused, and that is not an oversight:
    // after a successful run the file IS ants-v1, so refusing that dialect would
    // make the op refuse its own output and INV-3 — converting twice produces
    // the same file — would be unsatisfiable. On an already-ants-v1 source the
    // convert is a plain render plus a re-import.
    bool sawSignal = false;
    const QString detected =
        RoadmapParse::detectRoadmapFormat(markdown.split(QLatin1Char('\n')), &sawSignal);
    if (!sawSignal) {
        // No format signal at all. detectRoadmapFormat()'s no-signal DEFAULT is
        // `ants-v1`, so branching on the returned name alone would read an
        // empty or mangled file as the accepted dialect and convert it. This is
        // the branch § 4.4 assigns to migratedProject()'s
        // ReadError::SourceUnrecognised; the code is that refusal's, so a
        // caller sees one answer for one condition however it arrived.
        return refuseWith(QStringLiteral("unrecognised_format"),
            QStringLiteral("roadmap_log: \"%1\" carries no roadmap dialect signal "
                           "— no task-list bullets, no status emoji and no Pass "
                           "headings. There is nothing to convert and nothing "
                           "was written.").arg(roadmapPath));
    }
    if (detected != QLatin1String("ants-v1")
        && detected != QLatin1String("github-task-list")) {
        // A RECOGNISED third dialect. migratedProject() does not refuse this,
        // which is why the op needs a code of its own; a source with no format
        // signal at all is a different branch and never reaches here, because
        // detectRoadmapFormat() answers ants-v1 for it.
        return refuseWith(QStringLiteral("dialect_out_of_scope"),
            QStringLiteral("roadmap_log: convert takes a github-task-list or "
                           "ants-v1 roadmap; \"%1\" reads as %2. Converting that "
                           "dialect is out of scope and nothing was written.")
                .arg(roadmapPath, detected));
    }

    // This op's OWN connection, on Access::Bulk, for the duration of one call —
    // NOT RemoteControl's process-owned Interactive one. `RoadmapMigrateLoad`
    // refuses anything but Bulk (ANTS-3765 INV-12), and the load and
    // commitAndRender must share ONE connection or they do not share the
    // transaction. The same pattern, for the same reason, as
    // RoadmapMigrateVerb::run() step 5; two live connections in one process are
    // safe by construction, since RoadmapStore names each from an atomic
    // counter and the store runs in WAL.
    QString storeErr;
    RoadmapStore bulk(RoadmapStore::defaultPath(),
                      m_roadmapHistoryCap < 0 ? RoadmapStore::kDefaultHistoryCapBytes
                                              : m_roadmapHistoryCap,
                      RoadmapStore::Access::Bulk);
    if (!bulk.open(&storeErr))
        return refuseWith(QStringLiteral("store_failed"),
            storeErr.isEmpty() ? QStringLiteral("the roadmap store is unavailable")
                               : storeErr);
    RoadmapStore *store = &bulk;

    const auto row = store->readProjectByRoot(root, &storeErr);
    if (!row) {
        // § 6 — the convert refuses rather than migrating implicitly. Migration
        // is a separate operation with its own guards, and running one as a side
        // effect of a format change would register a project nobody asked to
        // register.
        return refuseWith(QStringLiteral("project_not_registered"),
            QStringLiteral("roadmap_log: convert rewrites a project the store "
                           "already holds — the store has no row for \"%1\". Run "
                           "roadmap_migrate first; nothing was written.")
                .arg(root));
    }
    const qint64 projectId = row->projectId;
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // Filled inside mutate(), read after. The load's Outcome does not survive
    // the lambda, and the envelope needs what it allocated — which is the one
    // thing a reviewer of a one-way bulk rewrite has to see BEFORE it lands.
    qint64      idsAllocated = 0;
    QStringList allocatedIds;
    int         bulletsTotal = 0;
    int         idsParsed    = 0;
    QString     loadError;
    // ANTS-5252 — the per-bullet id report Vestige asked for.
    QVector<RoadmapMigrateVerb::PlannedId> plannedIds;

    // § 4.3 — what mutate() does, in order. A mutate that only wrote the column
    // would be a different operation: it would leave the store holding whatever
    // an earlier migration left and publish THAT, which is not a conversion of
    // the file in front of it.
    const auto mutate = [&](QString *err) -> bool {
        // Through the migrate SEAM, not by open-coding findRoadmaps/planFrom/
        // load here. Those three have exactly one production call site under
        // src/ by contract (roadmap_migrate_verb INV-1), and a second
        // open-coded copy is the migration-logic drift that invariant exists to
        // prevent — the same class as the ci-parity parallel implementation.
        //
        // The project's OWN name and slug, not fresh ones: registerProject() is
        // get-or-create and keys on root, but export_slug is UNIQUE across the
        // whole store, so inventing one would collide with this project's own
        // row.
        const auto loaded = RoadmapMigrateVerb::loadInOpenTransaction(
            *store, root, row->name, row->exportSlug, rlHistoryStamp(),
            kMaxEchoedIds);
        if (!loaded.ok) {
            loadError = loaded.error;
            if (err) *err = loaded.error;
            return false;
        }
        idsAllocated = loaded.idsAllocated;
        allocatedIds = loaded.allocatedIds;
        bulletsTotal = loaded.bulletsTotal;
        idsParsed    = loaded.idsParsed;
        plannedIds   = loaded.plannedIds;

        // Step 4. Last, so a load failure above leaves the column alone — though
        // the rollback would undo it anyway; the ordering is for the reader.
        return store->setProjectSourceFormat(projectId, QStringLiteral("ants-v1"), err);
    };

    QString err;
    RoadmapRender::Outcome outcome;
    // ANTS-5256 — LaymanGate::Exempt, and this is the only call site in the
    // tree that passes it. A convert is a MIGRATION: the same bullets change
    // representation and no new claim enters the project, so INV-5's rule about
    // authoring does not apply to it. It also cannot be satisfied — Layman is
    // an ants-v1 rule, and the source dialect is one where roadmap-format.md
    // makes it optional, so enforcing it here demands the destination
    // dialect's rule of items that exist only in the source dialect, as a
    // precondition of the call that would make that rule apply.
    //
    // Measured (Vestige, 2026-09-21): 460 open items, so the op refused
    // outright on the one project it was built for. ANTS-4628 narrowed the gate
    // to touched items specifically to unblock conversion, and a convert
    // touches everything by construction, so that narrowing cannot reach this.
    //
    // The exemption ends here. Every ordinary write afterwards judges these
    // items normally, so the first edit to one still owes its summary.
    const auto rc = RoadmapWrite::commitAndRender(
        *store, projectId, root, roadmapPath, dryRun, mutate, &outcome, &err,
        RoadmapWrite::LaymanGate::Exempt);

    QJsonObject env;
    if (rcRoadmapWriteRefused(env, rc, err, outcome)) {
        if (!loadError.isEmpty())
            env[QStringLiteral("load_error")] = loadError;
        return QJsonDocument(env);
    }

    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("op")] = QStringLiteral("convert");
    env[QStringLiteral("project_root")]    = root;
    env[QStringLiteral("source_dialect")]  = detected;
    env[QStringLiteral("target_dialect")]  = QStringLiteral("ants-v1");
    rcRoadmapWriteFields(env, outcome, dryRun);

    // The id report. Asked for by the blocked consumer (Vestige, 2026-09-21)
    // and kept because the op is a one-way bulk rewrite of a version-controlled
    // file that moves a counter other documents cite: without it the first
    // observable state is the rewritten roadmap. On a dry run this IS the
    // deliverable — the whole point is to see the assignment before it lands.
    QJsonObject ids;
    ids[QStringLiteral("allocated")]     = double(idsAllocated);
    ids[QStringLiteral("parsed")]        = idsParsed;
    ids[QStringLiteral("bullets_total")] = bulletsTotal;
    QJsonArray echoed;
    for (const QString &id : allocatedIds)
        echoed.append(id);
    ids[QStringLiteral("allocated_ids")] = echoed;
    if (idsAllocated > allocatedIds.size()) {
        ids[QStringLiteral("allocated_ids_truncated")] = true;
        // ANTS-5256 — say how many were withheld, not merely that some were.
        // Vestige: "a capped list that does not announce its cap is the
        // 'partial result indistinguishable from a complete one' problem"
        // (ANTS-5257) in a second place. `allocated` above is the true total,
        // so the arithmetic is available — but a reader should not have to do
        // it to learn the list is short.
        ids[QStringLiteral("allocated_ids_shown")] = allocatedIds.size();
    }

    // ANTS-5252 — the per-bullet report, beside the aggregate rather than
    // instead of it. One table keyed by id, so a reviewer reads ONE thing:
    // Vestige's objection to two capped lists was that a caller reading one
    // and not the other will misread one of them.
    //
    // On a dry run this IS the deliverable. The op is a one-way bulk rewrite
    // of a version-controlled public file that moves a counter other documents
    // cite, and the dry run is the only moment the assignment can be checked
    // before it becomes permanent. `origin` and `in_file` are the two columns
    // to scan: a newly assigned id is visible and fixable, a CHANGED one is
    // invisible and permanent.
    //
    // `origin` reports what the FILE says. "absent" means the bullet carries
    // no id there — the load will either issue one or match an existing store
    // item by headline, and `ids.allocated_ids[]` names the ids actually
    // issued. The row does not claim an allocation it cannot know happened.
    QJsonArray planned;
    for (const auto &row : plannedIds) {
        QJsonObject o;
        o[QStringLiteral("id")]      = row.id;
        o[QStringLiteral("origin")]  = row.origin;
        o[QStringLiteral("in_file")] = row.inFile;
        o[QStringLiteral("line")]    = row.firstLine;
        // Gated to the true arm, like id_inferred is everywhere else: a key
        // present on every row saying `false` is a key nobody reads.
        if (row.inferred)
            o[QStringLiteral("id_inferred")] = true;
        if (!row.hasLayman)
            o[QStringLiteral("layman_missing")] = true;
        // ANTS-5258 — what the load DID with it. `origin` says what the file
        // holds; this says what became of it, and the matched arm is the one
        // to review: a fresh id collides with nothing, a wrong match writes an
        // EXISTING id into the file and every prior citation of that id then
        // resolves to the wrong work. Gated to the true arm like the flags
        // above — `matched` absent means a fresh id.
        if (row.matched) {
            o[QStringLiteral("matched")]          = true;
            o[QStringLiteral("matched_id")]       = row.matchedId;
            o[QStringLiteral("matched_headline")] = row.matchedHeadline;
        }
        // The pairing rested on ORDER alone — several stored rows satisfied
        // the key. Reproducible, and still the one arm a human should check.
        if (row.ambiguous)
            o[QStringLiteral("ambiguous_rematch")] = true;
        planned.append(o);
    }
    // ANTS-5258 — the two counts a reviewer triages on, computed over EVERY
    // row rather than the capped echo, so a truncated list still reports the
    // true size of each arm. `ambiguous` is the one that should be zero.
    int matchedCount = 0, ambiguousCount = 0;
    for (const auto &row : plannedIds) {
        if (row.matched)   ++matchedCount;
        if (row.ambiguous) ++ambiguousCount;
    }
    ids[QStringLiteral("matched")]           = matchedCount;
    ids[QStringLiteral("ambiguous_rematch")] = ambiguousCount;
    ids[QStringLiteral("planned")] = planned;
    if (bulletsTotal > planned.size()) {
        ids[QStringLiteral("planned_truncated")] = true;
        ids[QStringLiteral("planned_shown")]     = planned.size();
    }
    env[QStringLiteral("ids")] = ids;

    // ANTS-5256 — what the exempted gate let through. The exemption is a
    // relaxation of a rule, so it reports rather than passing silently: these
    // items landed with `layman` NULL and the next write touching any of them
    // will be refused until it carries one.
    //
    // Emitted on BOTH arms, empty count included. A caller cannot tell an
    // exemption that found nothing from one that did not run if the key is
    // simply absent, and on a dry run this is part of the review material.
    {
        QJsonObject missing;
        missing[QStringLiteral("count")] = outcome.laymanMissing.size();
        QJsonArray shown;
        for (const QString &id : outcome.laymanMissing) {
            if (shown.size() >= kMaxEchoedIds)
                break;
            shown.append(id);
        }
        missing[QStringLiteral("ids")] = shown;
        if (outcome.laymanMissing.size() > shown.size()) {
            missing[QStringLiteral("truncated")] = true;
            missing[QStringLiteral("shown")]     = shown.size();
        }
        env[QStringLiteral("layman_missing")] = missing;
    }
    return QJsonDocument(env);
}

// Store-only and m_main-independent, so the seam is the section ops' shape.
QJsonDocument RemoteControl::cmdRoadmapLogConvertForTest(const QJsonObject &req) {
    return cmdRoadmapLogConvert(req);
}
