// ANTS-4614 — TU 15/16 — roadmap_log op:"render": publish the store to the
// file on demand, with no semantic change.
// Contract: tests/features/roadmap_write_half/spec.md (Ants4614* cases)
//
// Its own TU for remotecontrol_roadmap_repair.cpp's reason: it is a member that
// never existed in the pre-split remotecontrol_roadmap_log.cpp, so appending it
// last cannot violate any pre-split relative order. (Nor would there be room —
// that TU sits AT ANTS-3833 INV-6's 6000-line cap; see ANTS-4620.)
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

#include "roadmapstore.h"
#include "roadmapwrite.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

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
            env[QStringLiteral("code")]  = QStringLiteral("project_not_registered");
            env[QStringLiteral("error")] = QStringLiteral(
                "roadmap_log: render publishes the STORE to the file, so it "
                "needs a store-migrated project — the store holds no row for "
                "\"%1\". Run roadmap_migrate first; on a markdown-backed "
                "project the file is already the source of truth and there is "
                "nothing to publish.")
                    .arg(root.isEmpty() ? roadmapPath : root);
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
