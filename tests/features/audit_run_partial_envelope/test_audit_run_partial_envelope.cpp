// ANTS-2032 — feature-conformance test for audit_run's partiality
// surface (incompleteToolNames helper + partial flag derivation) and
// the SARIF-before-reply / partial-envelope source guarantees.

#include "auditrunner.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <QHash>
#include <QString>
#include <QStringList>

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

AuditRunner::ToolResult tr(const QString &tool, const QString &status) {
    AuditRunner::ToolResult t;
    t.tool   = tool;
    t.status = status;
    return t;
}

}  // namespace

// INV-1 — incompleteToolNames lists non-ok tools, sorted ascending.
TEST(AuditRunPartialEnvelope, Inv1IncompleteNamesSorted) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    byTool["semgrep"]  = tr("semgrep", "timed_out");
    byTool["ruff"]     = tr("ruff", "ok");
    byTool["cppcheck"] = tr("cppcheck", "crashed");
    byTool["bandit"]   = tr("bandit", "ok");
    const QStringList inc = AuditRunner::internal::incompleteToolNames(byTool);
    ASSERT_EQ(inc.size(), 2);
    EXPECT_EQ(inc[0], QStringLiteral("cppcheck"));  // sorted
    EXPECT_EQ(inc[1], QStringLiteral("semgrep"));
}

// INV-1b — an all-ok map yields an empty list.
TEST(AuditRunPartialEnvelope, Inv1AllOkEmpty) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    byTool["ruff"]   = tr("ruff", "ok");
    byTool["bandit"] = tr("bandit", "ok");
    EXPECT_TRUE(AuditRunner::internal::incompleteToolNames(byTool).isEmpty());
}

// INV-2 — partial ⟺ a tool did not finish ok (the derivation the runner
// applies: partial = !incompleteTools.isEmpty()).
TEST(AuditRunPartialEnvelope, Inv2PartialDerivation) {
    QHash<QString, AuditRunner::ToolResult> ok;
    ok["ruff"] = tr("ruff", "ok");
    EXPECT_FALSE(!AuditRunner::internal::incompleteToolNames(ok).isEmpty());

    QHash<QString, AuditRunner::ToolResult> mixed;
    mixed["ruff"]    = tr("ruff", "ok");
    mixed["semgrep"] = tr("semgrep", "timed_out");
    EXPECT_TRUE(!AuditRunner::internal::incompleteToolNames(mixed).isEmpty());
}

// INV-3 — the audit_run provider serialises the partial surface, and the
// descriptor advertises it.
TEST(AuditRunPartialEnvelope, Inv3EnvelopeSerialisesPartial) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    EXPECT_TRUE(contains(mw, "env[\"partial\"]"))
        << "INV-3: provider writes the partial flag";
    EXPECT_TRUE(contains(mw, "incomplete_tools"))
        << "INV-3: provider writes incomplete_tools[]";
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_TRUE(contains(ci, "incomplete_tools"))
        << "INV-3: audit_run descriptor advertises incomplete_tools";
}

// INV-4 — SARIF is written inside runAudit before it returns (the caller
// serialises the reply only after runAudit returns), so a too-big/failed
// reply still leaves a recoverable artifact. Source-order guarantee.
TEST(AuditRunPartialEnvelope, Inv4SarifWrittenBeforeReturn) {
    const std::string rn = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    const auto writePos = rn.find("writeSarif(cacheSarifAbs");
    ASSERT_NE(writePos, std::string::npos)
        << "INV-4: writeSarif call present in runAudit";
    // The final `return r;` of runAudit must come AFTER the SARIF write.
    const auto retPos = rn.rfind("return r;");
    ASSERT_NE(retPos, std::string::npos);
    EXPECT_LT(writePos, retPos)
        << "INV-4: SARIF is written before runAudit returns (artifact "
           "exists before the caller serialises the reply)";
}
