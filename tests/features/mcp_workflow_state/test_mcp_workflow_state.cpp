// Feature-conformance test for ANTS-1723 — workflow_state MCP tool.
// See tests/features/mcp_workflow_state/spec.md.

#include <gtest/gtest.h>
#include <QFile>

// INV-6: CallerCwdContract::Required registered
TEST(McpWorkflowState, Inv6CallerCwdRequired) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("workflow_state") && txt.contains("Required"))
        << "CallerCwdContract::Required must be registered for workflow_state";
}

// INV-1: "found" field emitted in cmdWorkflowState
TEST(McpWorkflowState, Inv1FoundFieldPresent) {
    QFile f(QString::fromUtf8(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("workflow_state") && txt.contains("\"found\""))
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
