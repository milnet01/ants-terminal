// Feature-conformance test for ANTS-1723 — workflow_state MCP tool.
// See tests/features/mcp_workflow_state/spec.md.

#include <gtest/gtest.h>
#include <QFile>

#include "../../_support/srcgrep.h"

#include <string>

// INV-6: CallerCwdContract::Required registered
TEST(McpWorkflowState, Inv6CallerCwdRequired) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    // ANTS-2067 — match the actual contract row (proximity), not the two
    // tokens "workflow_state" and "Required" appearing anywhere in the
    // (large) file. Whitespace-normalised to survive `return` realignment.
    const std::string txt =
        ants_test::squashWhitespace(f.readAll().toStdString());
    EXPECT_NE(txt.find("if (toolName == QStringLiteral(\"workflow_state\")) "
                       "return C::Required;"),
              std::string::npos)
        << "CallerCwdContract::Required must be registered for workflow_state";
}

// INV-1: "found" field emitted in cmdWorkflowState
TEST(McpWorkflowState, Inv1FoundFieldPresent) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    // ANTS-2067 — scope to the cmdWorkflowState body so an unrelated
    // "found" elsewhere in remotecontrol.cpp can't satisfy this.
    const std::string body = ants_test::slurpFunctionBody(
        f.readAll().toStdString(), "RemoteControl::cmdWorkflowState");
    EXPECT_NE(body.find("\"found\""), std::string::npos)
        << "found field missing in cmdWorkflowState";
}

// INV-4: 72h TTL constant present
TEST(McpWorkflowState, Inv4TtlLogicPresent) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("kTtlMs") || txt.contains("259200000"))
        << "72h TTL constant (kTtlMs) missing in cmdWorkflowState";
    EXPECT_TRUE(txt.contains("updated_at_ms"))
        << "updated_at_ms field missing";
}

// INV-7: skill name regex present
TEST(McpWorkflowState, Inv7SkillNameRegex) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("A-Za-z0-9_-") && txt.contains("32"))
        << "skill name regex ^[A-Za-z0-9_-]{1,32}$ missing";
}

// INV-11 (ANTS-3511): arg-validation messages name all missing required
// args and distinguish an absent skill from a malformed one. Scoped to the
// cmdWorkflowState body so an unrelated match elsewhere can't satisfy it.
TEST(McpWorkflowState, Inv11ArgValidationMessages) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const std::string body = ants_test::slurpFunctionBody(
        f.readAll().toStdString(), "RemoteControl::cmdWorkflowState");
    // Absent op names BOTH required args in one refusal.
    EXPECT_NE(body.find("op and skill are required"), std::string::npos)
        << "absent op must name `skill` as also-required";
    // Absent skill reads as required, not \"invalid\".
    EXPECT_NE(body.find("skill is required"), std::string::npos)
        << "empty/absent skill must refuse with \"skill is required\", "
           "not the regex message";
    // Regex message retained for a present-but-non-conforming skill (INV-7).
    EXPECT_NE(body.find("invalid skill name"), std::string::npos)
        << "present-but-malformed skill keeps the regex message";
}

// INV-9: 4 KiB payload cap
TEST(McpWorkflowState, Inv9PayloadCap) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("payload_too_large"))
        << "payload_too_large refusal code missing";
}

// INV-10: wf. dot prefix used (not slash)
TEST(McpWorkflowState, Inv10DotPrefixNotSlash) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_FALSE(txt.contains("\"wf/\""))
        << "slash separator must not be used (not valid in key charset)";
    EXPECT_TRUE(txt.contains("\"wf.\"") || txt.contains("wf."))
        << "dot separator wf.<skill> must be used";
}
