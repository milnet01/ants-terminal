// ANTS-5787 TU 23/23 — the readability verbs' handlers: function_sizes,
// source_scrapes (ANTS-5602), comment_claims (ANTS-5786) and duplicate_blocks
// (ANTS-5785). Contracts: each
// verb's tests/features/mcp_<verb>/spec.md.
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

#include "commentclaims.h"
#include "duplicateblocks.h"
#include "functionsizes.h"
#include "resolvedroot.h"
#include "sourcescrapes.h"

namespace {

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
        ants::requiredCallerCwd(m_roots, req, QStringLiteral("function_sizes"), &refusal);
    if (root.isEmpty()) return QJsonDocument(refusal);
    return QJsonDocument(FunctionSizes::run(
        root, forward(req, {"paths", "top", "include_tests", "include_generated"})));
}

QJsonDocument RemoteControl::cmdSourceScrapes(const QJsonObject &req) {
    QJsonObject refusal;
    const QString root =
        ants::requiredCallerCwd(m_roots, req, QStringLiteral("source_scrapes"), &refusal);
    if (root.isEmpty()) return QJsonDocument(refusal);
    return QJsonDocument(SourceScrapes::run(root, forward(req, {"path", "paths", "limit"})));
}

QJsonDocument RemoteControl::cmdCommentClaims(const QJsonObject &req) {
    QJsonObject refusal;
    const QString root =
        ants::requiredCallerCwd(m_roots, req, QStringLiteral("comment_claims"), &refusal);
    if (root.isEmpty()) return QJsonDocument(refusal);
    return QJsonDocument(CommentClaims::run(
        root, forward(req, {"paths", "limit", "include_tests", "include_generated"})));
}

QJsonDocument RemoteControl::cmdDuplicateBlocks(const QJsonObject &req) {
    QJsonObject refusal;
    const QString root =
        ants::requiredCallerCwd(m_roots, req, QStringLiteral("duplicate_blocks"), &refusal);
    if (root.isEmpty()) return QJsonDocument(refusal);
    return QJsonDocument(DuplicateBlocks::run(
        root, forward(req, {"paths", "min_lines", "similarity", "normalise_names", "top",
                            "include_tests", "include_generated"})));
}
