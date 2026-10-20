// ANTS-5787 TU 23/23 — the readability verbs' handlers: function_sizes and
// source_scrapes (ANTS-5602). Contracts: tests/features/mcp_function_sizes/
// spec.md and tests/features/mcp_source_scrapes/spec.md.
//
// Thin by design, like quotation_check: each seam (outside
// ANTS_RC_SOURCES_REL) does everything below the root; what lives here is
// resolving caller_cwd and forwarding the declared arguments.
//
// Every argument this file reads via req.value() MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp (ANTS-4621): the schema sets
// additionalProperties:false, so an undeclared argument is refused by a strict
// client before the handler runs.

#include "remotecontrol.h"

#include "functionsizes.h"
#include "resolvedroot.h"
#include "sourcescrapes.h"

namespace {

// The resolved root, or a no_project refusal in `*refusal`.
QString resolveRoot(const ants::RootProvider *roots, const QJsonObject &req,
                    const QString &verb, QJsonObject *refusal) {
    // caller_cwd absent is the dispatcher's refusal (CallerCwdContract::Required).
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const ants::ResolvedRoot rr = ants::resolveCallerCwdRoot(roots, callerRaw);
    if (rr.source == ants::ResolvedRoot::Source::Unresolvable || rr.cwd.isEmpty()) {
        (*refusal)[QStringLiteral("ok")] = false;
        (*refusal)[QStringLiteral("code")] = QStringLiteral("no_project");
        (*refusal)[QStringLiteral("error")] =
            QStringLiteral("%1: caller_cwd \"%2\" does not resolve to a directory")
                .arg(verb, callerRaw);
        return QString();
    }
    return rr.cwd;
}

// Copies the named arguments that are present.
QJsonObject forward(const QJsonObject &req, std::initializer_list<const char *> names) {
    QJsonObject args;
    for (const char *n : names) {
        const QString key = QString::fromLatin1(n);
        if (req.contains(key)) args[key] = req.value(key);
    }
    return args;
}

} // namespace

QJsonDocument RemoteControl::cmdFunctionSizes(const QJsonObject &req) {
    QJsonObject refusal;
    const QString root =
        resolveRoot(m_roots, req, QStringLiteral("function_sizes"), &refusal);
    if (root.isEmpty()) return QJsonDocument(refusal);
    return QJsonDocument(FunctionSizes::run(
        root, forward(req, {"paths", "top", "include_tests", "include_generated"})));
}

QJsonDocument RemoteControl::cmdSourceScrapes(const QJsonObject &req) {
    QJsonObject refusal;
    const QString root =
        resolveRoot(m_roots, req, QStringLiteral("source_scrapes"), &refusal);
    if (root.isEmpty()) return QJsonDocument(refusal);
    return QJsonDocument(SourceScrapes::run(root, forward(req, {"path", "paths", "limit"})));
}
