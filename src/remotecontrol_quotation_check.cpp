// ANTS-5502 TU 20/23 — the `quotation_check` handler: many quotations checked in one call.
// Contract: docs/specs/ANTS-5502-quotation-check.md.
//
// Thin by design, like its run_trace and session_message siblings. The seam
// (src/quotationcheckverb.cpp, outside ANTS_RC_SOURCES_REL) does everything
// below the root; what lives here is resolving caller_cwd.
//
// Every argument this file reads via req.value() MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp (ANTS-4621): the schema sets
// additionalProperties:false, so an undeclared argument is refused by a strict
// client before the handler runs.

#include "remotecontrol.h"

#include "quotationcheckverb.h"
#include "resolvedroot.h"

QJsonDocument RemoteControl::cmdQuotationCheck(const QJsonObject &req) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::Required).
    QJsonObject refusal;
    const QString cwd = ants::requiredCallerCwd(m_roots, req, QStringLiteral("quotation_check"), &refusal);
    if (cwd.isEmpty())
        return QJsonDocument(refusal);

    // Forwarded only when present: the seam tells an absent `allowed` from an
    // empty one.
    QJsonObject args;
    if (req.contains(QStringLiteral("items")))
        args[QStringLiteral("items")] = req.value(QStringLiteral("items"));
    if (req.contains(QStringLiteral("allowed")))
        args[QStringLiteral("allowed")] = req.value(QStringLiteral("allowed"));
    if (req.contains(QStringLiteral("max_bytes")))
        args[QStringLiteral("max_bytes")] = req.value(QStringLiteral("max_bytes"));
    if (req.contains(QStringLiteral("tolerance")))  // ANTS-5732
        args[QStringLiteral("tolerance")] = req.value(QStringLiteral("tolerance"));
    return QJsonDocument(QuotationCheck::run(cwd, args));
}
