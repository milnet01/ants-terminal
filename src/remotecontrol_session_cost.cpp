// ANTS-5698 TU 21/21 — the `session_cost` handler: a session's context rent.
// Contract: tests/features/session_cost/spec.md.
//
// Thin by design, like its quotation_check and run_trace siblings. The seam
// (src/sessioncostverb.cpp) reads the transcript; what lives here is
// resolving caller_cwd, naming the transcript folders and reading the clock.
//
// Every argument this file reads via req.value() MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp (ANTS-4621): the schema sets
// additionalProperties:false, so an undeclared argument is refused by a strict
// client before the handler runs.

#include "remotecontrol.h"

#include "resolvedroot.h"
#include "sessioncostverb.h"

#include <QDateTime>
#include <QDir>

QJsonDocument RemoteControl::cmdSessionCost(const QJsonObject &req) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::Required).
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr = ants::resolveCallerCwdRoot(m_roots, callerRaw);
    if (rr.source == ants::ResolvedRoot::Source::Unresolvable || rr.cwd.isEmpty()) {
        QJsonObject e;
        e[QStringLiteral("ok")]    = false;
        e[QStringLiteral("code")]  = QStringLiteral("no_project");
        e[QStringLiteral("error")] =
            QStringLiteral("session_cost: caller_cwd \"%1\" does not resolve to a "
                           "directory").arg(callerRaw);
        return QJsonDocument(e);
    }

    // Where the harness keeps transcripts; CLAUDE_CONFIG_DIR moves it.
    QStringList projectsDirs;
    const QString cfg = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    if (!cfg.isEmpty())
        projectsDirs << QDir(cfg).filePath(QStringLiteral("projects"));
    projectsDirs << QDir::home().filePath(QStringLiteral(".claude/projects"));

    QJsonObject args;
    for (const char *k : {"session_id", "top"}) {
        const QString key = QLatin1String(k);
        if (req.contains(key))
            args[key] = req.value(key);
    }
    return QJsonDocument(SessionCost::run(projectsDirs, rr.cwd, args,
                                          QDateTime::currentMSecsSinceEpoch()));
}
