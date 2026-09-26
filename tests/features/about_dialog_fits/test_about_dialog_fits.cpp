// ANTS-5390 — the About dialog never clips its body. See spec.md.

#include "aboutdialogs.h"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPointer>

#include <gtest/gtest.h>

namespace {

QDialog *findAboutDialog() {
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->objectName() == QLatin1String("aboutAntsDialog"))
            return qobject_cast<QDialog *>(w);
    return nullptr;
}

}  // namespace

TEST(AboutDialogFits, BodyIsNeverClipped) {
    AboutDialogs::showAboutAnts(nullptr, {});
    QPointer<QDialog> dlg = findAboutDialog();
    ASSERT_TRUE(dlg) << "showAboutAnts opened no dialog named aboutAntsDialog";

    // A size far below the text, as a saved size from an older build was.
    dlg->resize(200, 120);
    QApplication::processEvents();

    EXPECT_GE(dlg->width(), 560) << "INV-1: the minimum width was not held";
    const auto *label = dlg->findChild<QLabel *>(QStringLiteral("aboutAntsBody"));
    ASSERT_TRUE(label);
    EXPECT_GE(label->height(), label->heightForWidth(label->width()))
        << "INV-2: the body label is shorter than its wrapped text at width "
        << label->width();

    dlg->close();
    QApplication::processEvents();
}
