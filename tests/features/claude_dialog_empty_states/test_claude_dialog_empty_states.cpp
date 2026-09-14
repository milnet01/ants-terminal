// Claude dialogs explain an empty view — see spec.md. ANTS-5092.

#include "claudeallowlist.h"

#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

// INV-1
TEST(ClaudeDialogEmptyStates, UnreadableAllowlistIsReported) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{ \"permissions\": { \"allow\": [ ");   // truncated JSON
    }
    ClaudeAllowlistDialog dlg;
    dlg.setSettingsPath(path);
    bool reported = false;
    for (const QLabel *label : dlg.findChildren<QLabel *>(QString())) {
        if (label->text().contains(path) && label->text().contains(QStringLiteral("could not be read")))
            reported = true;
    }
    EXPECT_TRUE(reported) << "a corrupt settings file shows empty lists with no message";
}

// INV-2
TEST(ClaudeDialogEmptyStates, ToolResultOnlyUserEntryIsNotBlank) {
    QFile src(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
              + QStringLiteral("/../../../src/claudetranscript.cpp"));
    ASSERT_TRUE(src.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(src.readAll());
    const int fn = text.indexOf(QStringLiteral("QString ClaudeTranscriptDialog::formatEntry("));
    ASSERT_GE(fn, 0);
    const int user = text.indexOf(QStringLiteral("User:</b>"), fn);
    const int guard = text.indexOf(QStringLiteral("content.isEmpty() && !contentArr.isEmpty()"), fn);
    ASSERT_GE(user, 0);
    ASSERT_GE(guard, 0) << "a tool-result-only user entry renders an empty User line";
    EXPECT_LT(guard, user);
}
