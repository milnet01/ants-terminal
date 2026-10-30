// ANTS-6310 TU 24/24 — the `know_how` handler: the machine-global store of
// how-to facts. Contract: docs/specs/ANTS-6310-know-how-store.md.
//
// Thin by design, like its session_message sibling. Every op, refusal and reply
// is KnowHowStore's, which `test_core` links without RemoteControl. What lives
// here is what cannot: resolving caller_cwd and naming the writing project.
//
// Every argument this file or KnowHowStore reads MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp. test_know_how_verb.cpp scrapes
// both sources and holds the schema to them.

#include "remotecontrol.h"

#include "knowhowstore.h"
#include "projectsettings.h"
#include "resolvedroot.h"
#include "roadmapstore.h"

namespace {

QJsonDocument refuse(const char *code, const QString &message) {
    QJsonObject e;
    e[QStringLiteral("ok")] = false;
    e[QStringLiteral("code")] = QString::fromLatin1(code);
    e[QStringLiteral("error")] = QStringLiteral("know_how: ") + message;
    return QJsonDocument(e);
}

}  // namespace

QJsonDocument RemoteControl::cmdKnowHow(const QJsonObject &req) {
    QJsonObject refusal;
    const QString cwd =
        ants::requiredCallerCwd(m_roots, req, QStringLiteral("know_how"), &refusal);
    if (cwd.isEmpty())
        return QJsonDocument(refusal);

    const QString op = req.value(QStringLiteral("op")).toString(QStringLiteral("find"));
    if (!KnowHowStore::isOp(op))
        return refuse("bad_args",
                      QStringLiteral("unknown op \"%1\": expected find, get, list, add, update, "
                                     "retire or purge").arg(op));

    // § 4.3 — reads open no roadmap store. Deliberately NOT registerProject(),
    // which would register any root it is handed.
    QString author;
    if (KnowHowStore::isWriteOp(op)) {
        RoadmapStore store(RoadmapStore::defaultPath());
        QString err;
        if (!store.open(&err))
            return refuse("io_error",
                          QStringLiteral("cannot open the roadmap store, so the author is "
                                         "unknown: %1").arg(err));
        const auto project = store.projectIdContaining(ProjectSettings::repoRootOf(cwd), &err);
        const auto row = project ? store.readProject(*project, &err) : std::nullopt;
        if (!row)
            return refuse("no_project",
                          QStringLiteral("\"%1\" is not inside a project registered in the "
                                         "roadmap store, so it cannot write. Run roadmap_migrate "
                                         "first.").arg(cwd));
        author = row->exportSlug;
    }
    return QJsonDocument(KnowHowStore(KnowHowStore::defaultPath()).run(op, req, author));
}
