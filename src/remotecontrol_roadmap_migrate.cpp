// ANTS-3833 TU 13/17 — ANTS-3855's `roadmap_migrate` handler.
// Contract: docs/specs/ANTS-3855-roadmap-migrate-verb.md § 2.1.1.
//
// Deliberately thin, and this is the whole of it: resolve the caller's root,
// derive the two defaults, read the clock ONCE, name the store. Everything that
// happens to a store is RoadmapMigrateVerb::run(), in its own translation unit
// (src/roadmapmigrateverb.cpp) so a test can link the seam without dragging
// RemoteControl and MainWindow in behind it — see roadmapmigrateverb.h for why
// that is a link-time requirement and not a preference.
//
// Joins ANTS_RC_SOURCES beside its two roadmap siblings. Nothing here is
// carved out of the pre-split remotecontrol.cpp, so it ADDS a member rather
// than moving one, and the slice order that file's scrape windows depend on is
// unchanged wherever this TU sits in the list.
//
// remotecontrol_internal.h IS included, for `findRoadmapUnder` alone. ANTS-3833
// INV-5 is a subset check, so a TU needing one of the promoted rcdetail helpers
// includes the header; this TU needed none until ANTS-4740 added op:"init",
// which must know whether a roadmap already exists before it writes one.

#include "remotecontrol.h"
#include "remotecontrol_internal.h"

#include "resolvedroot.h"
#include "roadmapmigrateverb.h"
#include "roadmapstore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>

QJsonDocument RemoteControl::cmdRoadmapMigrate(const QJsonObject &req) {
    // caller_cwd absent or empty is NOT this verb's refusal to make:
    // CallerCwdContract::Required has the dispatcher refuse it with
    // `caller_cwd_required` before this handler is entered (ANTS-1404). So the
    // one case here is Unresolvable — the path was given and does not exist.
    // NoMatch (it resolves, but no open tab sits there) is not a refusal:
    // migrating a project you have no terminal open in is legitimate.
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr = ants::resolveCallerCwdRoot(m_main, callerRaw);
    if (rr.source == ants::ResolvedRoot::Source::Unresolvable) {
        QJsonObject e;
        e[QStringLiteral("ok")]    = false;
        e[QStringLiteral("code")]  = QStringLiteral("no_project");
        e[QStringLiteral("error")] =
            QStringLiteral("roadmap_migrate: caller_cwd \"%1\" does not resolve "
                           "to a directory").arg(callerRaw);
        return QJsonDocument(e);
    }

    // rr.cwd is canonical, and three later steps rest on that: it is the same
    // QFileInfo::canonicalFilePath() form registerProject() stores and
    // RoadmapSource::migratedProject() later looks up by. Were the forms to
    // diverge, step 6's re-run detection would miss, every re-run would attempt
    // a new project row, and INV-7's idempotency would fail. One canonicaliser,
    // named — run() takes an already-canonical root and does not redo it.
    const QString root = rr.cwd;
    const QString leaf = QDir(root).dirName();

    // 0b — ANTS-4600. The store is MACHINE-GLOBAL, so a root registered from
    // here outlives the session that asked, and every machine-wide surface
    // (`roadmap_query mode:"report" scope:"all"`) counts it forever. A session
    // scratchpad under the temp dir is exactly that: it existed when it was
    // migrated — so registerProject()'s INV-8 canonicalisation guard passed —
    // and was gone minutes later, leaving 33 duplicate items behind under a
    // path nothing can render to. See docs/standards/mcp-error-codes.md § 1;
    // `bad_path` is not it, this path is fine and is the wrong KIND of place.
    //
    // The guard is HERE and not in run(): run() takes an arbitrary storePath,
    // and its own fixtures legitimately migrate temp roots into temp stores.
    // What is refused is registering one into the shared store, which is this
    // handler's decision alone — the same reason INV-4 puts `no_project` here.
    if (RoadmapMigrateVerb::isTransientRoot(rr.cwd)) {
        QJsonObject e;
        e[QStringLiteral("ok")]    = false;
        e[QStringLiteral("code")]  = QStringLiteral("transient_root");
        e[QStringLiteral("error")] =
            QStringLiteral("roadmap_migrate: \"%1\" is under the temporary "
                           "directory \"%2\" — a session scratchpad, not a "
                           "durable project. Migrate the real project root "
                           "instead.")
                .arg(rr.cwd, QDir::tempPath());
        return QJsonDocument(e);
    }

    // ANTS-4617 — the inverse. Branches BEFORE the migrate request is built,
    // because deregister shares only `caller_cwd` with it: no project_name, no
    // export_slug default, no clock. It also deliberately sits AFTER the
    // ANTS-4600 transient-root guard above, so a scratchpad under the temp dir
    // is refused by the same rule on both verbs rather than being registrable
    // one way and prunable the other.
    if (req.value(QStringLiteral("op")).toString() == QLatin1String("deregister")) {
        RoadmapMigrateVerb::DeregisterRequest d;
        d.projectRoot = root;
        d.exportSlug  = req.value(QStringLiteral("export_slug")).toString();
        d.confirm     = req.value(QStringLiteral("confirm")).toBool(false);
        d.dryRun      = req.value(QStringLiteral("dry_run")).toBool(false);
        return QJsonDocument(
            RoadmapMigrateVerb::deregister(RoadmapStore::defaultPath(), d));
    }

    // ANTS-4740 — op:"init", the bootstrap.
    //
    // The store is the source of truth and the file is its render, yet every
    // route INTO the store required a parseable file to already exist:
    // roadmap_migrate refused no_roadmap and so did roadmap_log op:append. So a
    // greenfield project had to hand-author a conforming ants-v1 file first —
    // markdown became primary again, briefly, at exactly the moment
    // adopt-project and start-project run.
    //
    // And the hand-authored step was the error-prone one: a mis-cased trailer
    // label is silently invisible per roadmap-format.md, so a bootstrap file
    // could migrate cleanly while dropping fields. That is the worst failure
    // available at the start of a project's life.
    //
    // It writes the skeleton and then falls through to the ordinary migrate, so
    // registration, the id_prefix row and idempotency are the SAME code path a
    // migrated project takes. Nothing here duplicates run().
    bool initialised = false;
    if (req.value(QStringLiteral("op")).toString() == QLatin1String("init")) {
        const QString existing = rcdetail::findRoadmapUnder(root);
        if (!existing.isEmpty()) {
            // Not an error the caller should paper over: op:"init" creates, and
            // a project that already has a roadmap wants the plain migrate.
            QJsonObject e;
            e[QStringLiteral("ok")]    = false;
            e[QStringLiteral("code")]  = QStringLiteral("roadmap_exists");
            e[QStringLiteral("error")] =
                QStringLiteral("roadmap_migrate op:\"init\": \"%1\" already has "
                               "a roadmap. Run roadmap_migrate with no op to "
                               "register it; init only creates one from nothing.")
                    .arg(existing);
            e[QStringLiteral("path")]  = existing;
            return QJsonDocument(e);
        }

        const QString name =
            req.value(QStringLiteral("project_name")).toString().isEmpty()
                ? leaf
                : req.value(QStringLiteral("project_name")).toString();
        const QString target = root + QStringLiteral("/ROADMAP.md");
        const QString skeleton = RoadmapMigrateVerb::initSkeleton(name);

        if (req.value(QStringLiteral("dry_run")).toBool(false)) {
            QJsonObject o;
            o[QStringLiteral("ok")]      = true;
            o[QStringLiteral("dry_run")] = true;
            o[QStringLiteral("op")]      = QStringLiteral("init");
            o[QStringLiteral("would_write")] = target;
            o[QStringLiteral("section")] = QStringLiteral("backlog");
            o[QStringLiteral("skeleton")] = skeleton;
            return QJsonDocument(o);
        }

        QSaveFile w(target);
        if (!w.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            w.write(skeleton.toUtf8()) != skeleton.toUtf8().size() ||
            !w.commit()) {
            QJsonObject e;
            e[QStringLiteral("ok")]    = false;
            e[QStringLiteral("code")]  = QStringLiteral("write_failed");
            e[QStringLiteral("error")] =
                QStringLiteral("roadmap_migrate op:\"init\": could not write "
                               "\"%1\"").arg(target);
            return QJsonDocument(e);
        }
        // Falls through to the migrate below, which registers the project.
        initialised = true;
    }

    RoadmapMigrateVerb::Request r;
    r.projectRoot = root;
    const QString nameArg = req.value(QStringLiteral("project_name")).toString();
    const QString slugArg = req.value(QStringLiteral("export_slug")).toString();
    r.projectName = nameArg.isEmpty() ? leaf : nameArg;
    r.exportSlug  = slugArg.isEmpty() ? RoadmapMigrateVerb::defaultExportSlug(leaf)
                                      : slugArg;
    // ONE clock read per call, here. run() never reads a clock — that is what
    // makes it reproducible, and it is ANTS-3765 § 2.1's "the clock is a
    // PARAMETER, not a call" held one layer further out. Qt::ISODate on a UTC
    // QDateTime yields exactly the shape history.changed_at CHECKs
    // (…THH:MM:SSZ); Qt::ISODateWithMs would add milliseconds it rejects.
    r.changedAt   = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    r.dryRun      = req.value(QStringLiteral("dry_run")).toBool(false);

    QJsonObject out = RoadmapMigrateVerb::run(RoadmapStore::defaultPath(), r);
    // ANTS-4740 — say the file was created. Without it an init reply is
    // byte-identical to a migrate of a roadmap that happened to be there, and a
    // caller cannot tell whether it bootstrapped or adopted.
    if (initialised) {
        out[QStringLiteral("initialised")] = true;
        out[QStringLiteral("created_path")] = root + QStringLiteral("/ROADMAP.md");
        out[QStringLiteral("section")]      = QStringLiteral("backlog");
        out[QStringLiteral("next_step")] = QStringLiteral(
            "append with roadmap_log op:\"append\" section:\"backlog\". The id "
            "prefix is not stored anywhere: pass `id_prefix` on that first "
            "append to pin it, or it is derived from the directory name.");
    }
    return QJsonDocument(out);
}
