// ANTS-5575 — a scroll area inside a themed dialog shows the theme, not the
// desktop's default background.
//
// Contract:
//   INV-1  With the app stylesheet installed, a QScrollArea's viewport, and
//          the content widget QScrollArea::setWidget gives it, paint nothing
//          of their own: a pixel inside the viewport equals the dialog's
//          themed background just outside the scroll area.
//   INV-2  The same holds for WelcomeDialog, the case that was reported.
//
// Why this exists: QScrollArea::setWidget switches autoFillBackground on for
// the content widget, which then paints the platform palette's Window
// colour. Without a desktop theme that is light grey, under text drawn in
// the Ants theme's pale colour: the welcome dialog was nearly unreadable
// (ANTS-5575). Settings, Audit and the review dialogs share the pattern.

#include "themedstylesheet.h"
#include "themes.h"
#include "welcomedialog.h"

#include <QApplication>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <gtest/gtest.h>

namespace {

struct AppSheet {
    QString previous = qApp->styleSheet();
    AppSheet() {
        qApp->setStyleSheet(themedstylesheet::buildAppStylesheet(
            Themes::byName(QStringLiteral("Dark"))));
    }
    ~AppSheet() { qApp->setStyleSheet(previous); }
};

// The dialog's colour 3px left of the scroll area, and inside its viewport
// 3px in from the top-right corner.
std::pair<QColor, QColor> sample(QDialog &dlg, QScrollArea *scroll) {
    dlg.resize(700, 500);
    dlg.show();
    QApplication::processEvents();
    const QImage img = dlg.grab().toImage();
    const QWidget *vp = scroll->viewport();
    const QPoint outside = scroll->mapTo(&dlg, QPoint(-3, scroll->height() / 2));
    const QPoint inside = vp->mapTo(&dlg, QPoint(vp->width() - 3, 3));
    const qreal dpr = img.devicePixelRatio();
    return {img.pixelColor(outside * dpr), img.pixelColor(inside * dpr)};
}

}  // namespace

TEST(ScrollAreaThemeBackground, Inv1PlainScrollArea) {
    AppSheet sheet;
    QDialog dlg;
    auto *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(12, 12, 12, 12);
    auto *scroll = new QScrollArea(&dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *v = new QVBoxLayout(body);
    v->addWidget(new QLabel(QStringLiteral("text"), body));
    scroll->setWidget(body);
    lay->addWidget(scroll);

    const auto [outside, inside] = sample(dlg, scroll);
    EXPECT_EQ(inside.name(), outside.name())
        << "the scroll area paints its own background over the themed dialog";
}

TEST(ScrollAreaThemeBackground, Inv2WelcomeDialog) {
    AppSheet sheet;
    WelcomeDialog::Options opts;
    opts.claudeDetected = false;
    WelcomeDialog dlg(QStringLiteral("Dark"), opts);
    auto *scroll = dlg.findChild<QScrollArea *>();
    ASSERT_NE(scroll, nullptr);

    const auto [outside, inside] = sample(dlg, scroll);
    EXPECT_EQ(inside.name(), outside.name())
        << "the welcome dialog's body paints its own background";
}
