// ANTS-5391 — the themed title bar follows a dialog's title. See spec.md.

#include "dialogchrome.h"
#include "elidedlabel.h"
#include "titlebar.h"

#include <QDialog>
#include <QLabel>

#include <gtest/gtest.h>

TEST(DialogChromeTitleFollows, RetitleReachesTheBar) {
    QDialog dlg;
    dlg.setWindowTitle(QStringLiteral("Roadmap — ROADMAP.md"));
    const auto chrome = DialogChrome::install(&dlg);
    ASSERT_TRUE(chrome.titleBar);
    const ElidedLabel *label = nullptr;
    for (QLabel *l : chrome.titleBar->findChildren<QLabel *>())
        if ((label = dynamic_cast<const ElidedLabel *>(l))) break;
    ASSERT_TRUE(label) << "the title bar carries no ElidedLabel";
    ASSERT_EQ(label->fullText(), QStringLiteral("Roadmap — ROADMAP.md"));

    dlg.setWindowTitle(QStringLiteral("Roadmap — from the roadmap store"));
    EXPECT_EQ(label->fullText(), QStringLiteral("Roadmap — from the roadmap store"))
        << "INV-1: the bar kept the title it copied at install";
}
