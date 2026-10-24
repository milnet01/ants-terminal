// ANTS-5698 TU 21/23 — the `session_cost` handler: a session's context rent.
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

QJsonDocument RemoteControl::cmdSessionCost(const QJsonObject &req) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::Required).
    QJsonObject refusal;
    const QString cwd = ants::requiredCallerCwd(m_roots, req, QStringLiteral("session_cost"), &refusal);
    if (cwd.isEmpty())
        return QJsonDocument(refusal);

    QJsonObject args;
    for (const char *k : {"session_id", "top"}) {
        const QString key = QLatin1String(k);
        if (req.contains(key))
            args[key] = req.value(key);
    }
    return QJsonDocument(SessionCost::run(SessionCost::projectsDirs(), cwd, args,
                                          QDateTime::currentMSecsSinceEpoch()));
}
