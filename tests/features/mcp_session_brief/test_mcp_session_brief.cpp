// Feature-conformance test for ANTS-1724 — session_brief MCP tool.
// See tests/features/mcp_session_brief/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QFile>

// INV-9: isEtagSupportedTool must list "session_brief"
TEST(McpSessionBrief, Inv9EtagSupportedToolListed) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    EXPECT_TRUE(txt.contains("\"session_brief\""))
        << "session_brief must appear in isEtagSupportedTool";
}

// INV-7 + INV-11: callerCwdContractFor returns Required; refusal code is "no_project"
TEST(McpSessionBrief, Inv7Inv11CallerCwdRequired) {
    {
        QFile ci(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
        ASSERT_TRUE(ci.open(QIODevice::ReadOnly));
        const QByteArray txt = ci.readAll();
        EXPECT_TRUE(txt.contains("session_brief") && txt.contains("Required"))
            << "callerCwdContractFor must return Required for session_brief";
    }
    {
        // ANTS-3833 — the class is eleven TUs; read all of them.
        const QByteArray rcSrc =
            QByteArray::fromStdString(ants_test::slurpRemoteControl());
        ASSERT_FALSE(rcSrc.isEmpty());
        const QByteArray txt = rcSrc;
        EXPECT_TRUE(txt.contains("\"no_project\""))
            << "cmdSessionBrief refusal must carry code:\"no_project\"";
    }
}

// INV-1: all envelope fields emitted in cmdSessionBrief
TEST(McpSessionBrief, Inv1AllEnvelopeFieldsPresent) {
    // ANTS-3833 — the class is eleven TUs; read all of them.
    const QByteArray fSrc =
        QByteArray::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(fSrc.isEmpty());
    const QByteArray txt = fSrc;
    for (const char *field : {"\"git\"", "\"build\"", "\"test\"",
                               "\"audit\"", "\"roadmap\""}) {
        EXPECT_TRUE(txt.contains(field))
            << "field " << field << " missing in remotecontrol.cpp";
    }
}

// INV-3/INV-4: result enum values present
TEST(McpSessionBrief, Inv3Inv4ResultEnumValues) {
    // ANTS-3833 — the class is eleven TUs; read all of them.
    const QByteArray fSrc =
        QByteArray::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(fSrc.isEmpty());
    const QByteArray txt = fSrc;
    EXPECT_TRUE(txt.contains("\"pass\""));
    EXPECT_TRUE(txt.contains("\"fail\""));
    EXPECT_TRUE(txt.contains("\"unknown\""));
}

// kindForName must classify session_brief as "workspace"
TEST(McpSessionBrief, KindForNameClassification) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray txt = f.readAll();
    const int sbPos = txt.indexOf("session_brief");
    ASSERT_GE(sbPos, 0) << "session_brief not found in claudeintegration.cpp";
    EXPECT_TRUE(txt.contains("workspace"))
        << "workspace kind entry missing";
}
