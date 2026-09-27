// ANTS-5479 — the verify.json trust dialog never clips a button label.
// See spec.md.

#include "themedstylesheet.h"
#include "themes.h"
#include "verifytrustmodal.h"

#include <QApplication>
#include <QByteArray>
#include <QMessageBox>
#include <QPushButton>

#include <gtest/gtest.h>

TEST(VerifyTrustModalButtonsFit, NoButtonIsNarrowerThanItsLabel) {
    // The app-wide sheet the running terminal installs; its button padding
    // is part of each size hint.
    const QString previousSheet = qApp->styleSheet();
    qApp->setStyleSheet(
        themedstylesheet::buildAppStylesheet(Themes::defaultTheme()));

    QMessageBox box;
    VerifyTrust::buildPromptBox(
        box, QStringLiteral("/mnt/Games/Scripts/Linux/Ants_Terminal"),
        QString(64, QLatin1Char('a')),
        QByteArrayLiteral(R"({"build":{"command":"true"}})"));
    box.show();
    QApplication::processEvents();

    int checked = 0;
    for (const QPushButton *b : box.findChildren<QPushButton *>()) {
        if (!b->isVisible()) continue;
        ++checked;
        EXPECT_GE(b->width(), b->sizeHint().width())
            << "INV-1: \"" << b->text().toStdString() << "\" is "
            << b->width() << " px wide but needs " << b->sizeHint().width();
    }
    EXPECT_GE(checked, 5) << "expected Show Details plus four choice buttons";

    box.close();
    qApp->setStyleSheet(previousSheet);
    QApplication::processEvents();
}
