// ANTS-5092 — the Projects dialog reads a lazily loaded session summary
// once. See tests/features/claude_projects_summary_cache/spec.md.

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <cstdlib>
#include <utime.h>

#include "claudeintegration.h"
#include "claudeprojects.h"
#include "config.h"

namespace {

bool writeSession(const QString &path, const QString &cwd, const QString &text,
                  qint64 mtimeSec) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QStringLiteral(
        R"({"type":"user","cwd":"%1","message":{"role":"user","content":"%2"}})"
        "\n").arg(cwd, text).toUtf8());
    f.close();
    struct utimbuf t{};
    t.actime = t.modtime = static_cast<time_t>(mtimeSec);
    return ::utime(QFile::encodeName(path).constData(), &t) == 0;
}

void selectOnly(QTreeWidget *tree, QTreeWidgetItem *item) {
    tree->clearSelection();
    item->setSelected(true);
}

}  // namespace

// INV-1
TEST(ClaudeProjectsSummaryCache, Inv1LazySummaryIsKept) {
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const bool hadHome = qEnvironmentVariableIsSet("HOME");
    const QByteArray priorHome = qgetenv("HOME");
    auto restore = qScopeGuard([&] {
        if (hadHome) qputenv("HOME", priorHome);
        else qunsetenv("HOME");
    });
    ::setenv("HOME", home.path().toLocal8Bit().constData(), 1);

    const QString cwd = home.path() + QStringLiteral("/proj");
    ASSERT_TRUE(QDir().mkpath(cwd));
    const QString dir = home.path() + QStringLiteral("/.claude/projects/") +
                        ClaudeIntegration::encodeProjectPath(cwd);
    ASSERT_TRUE(QDir().mkpath(dir));
    // Newest first: session 1 is the newest, session 6 the oldest, so
    // discoverProjects preloads 1..5 and leaves 6 to the dialog.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QString sixth;
    for (int i = 1; i <= 6; ++i) {
        const QString path = dir + QStringLiteral("/s%1.jsonl").arg(i);
        ASSERT_TRUE(writeSession(path, cwd, QStringLiteral("hello %1").arg(i),
                                 now - i * 60));
        if (i == 6) sixth = path;
    }

    ClaudeIntegration ci;
    Config cfg;
    ClaudeProjectsDialog dlg(&ci, &cfg);
    // The trees are told apart by their first header label.
    QTreeWidget *projects = nullptr;
    QTreeWidget *sessions = nullptr;
    for (QTreeWidget *t : dlg.findChildren<QTreeWidget *>()) {
        const QString head = t->headerItem()->text(0);
        if (head == QStringLiteral("Project")) projects = t;
        if (head == QStringLiteral("Summary")) sessions = t;
    }
    ASSERT_TRUE(projects && sessions);
    ASSERT_EQ(projects->topLevelItemCount(), 1);

    selectOnly(projects, projects->topLevelItem(0));
    ASSERT_EQ(sessions->topLevelItemCount(), 6);
    ASSERT_EQ(sessions->topLevelItem(5)->text(0), QStringLiteral("hello 6"));

    ASSERT_TRUE(QFile::remove(sixth));
    projects->clearSelection();
    selectOnly(projects, projects->topLevelItem(0));
    ASSERT_EQ(sessions->topLevelItemCount(), 6);
    EXPECT_EQ(sessions->topLevelItem(5)->text(0), QStringLiteral("hello 6"))
        << "INV-1: the summary was re-read from disk";
}
