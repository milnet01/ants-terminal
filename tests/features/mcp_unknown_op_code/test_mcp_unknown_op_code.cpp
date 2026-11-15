// ANTS-6771 — every multi-op verb refuses an unknown op with bad_mode.
// Contract: spec.md here.

#include "../standalone_mcp_server/mcpd_session.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

TEST(McpUnknownOpCode, Inv1EveryMultiOpVerbAnswersBadMode) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ants_test::McpdSession session(root, tmp.filePath(QStringLiteral("unknown-op.sock")));
    ASSERT_TRUE(session.started());

    const QStringList verbs{
        QStringLiteral("build_status"),    QStringLiteral("changelog_log"),
        QStringLiteral("ci_report"),       QStringLiteral("feedback_log"),
        QStringLiteral("git_state"),       QStringLiteral("know_how"),
        QStringLiteral("project_settings"), QStringLiteral("roadmap_log"),
        QStringLiteral("roadmap_migrate"), QStringLiteral("run_trace"),
        QStringLiteral("session_memory"),  QStringLiteral("session_message"),
        QStringLiteral("spec_log"),        QStringLiteral("subsystem"),
        QStringLiteral("test_results"),    QStringLiteral("workflow_state"),
    };
    for (const QString &verb : verbs) {
        const QJsonObject r = session.call(
            verb, QJsonObject{{"caller_cwd", root}, {"op", "no_such_op"}});
        ASSERT_FALSE(r.value(QStringLiteral("test_timeout")).toBool())
            << verb.toStdString() << " timed out";
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool(true)) << verb.toStdString();
        EXPECT_EQ(r.value(QStringLiteral("code")).toString().toStdString(), "bad_mode")
            << "INV-1: " << verb.toStdString() << " answered "
            << QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString();
    }
}
