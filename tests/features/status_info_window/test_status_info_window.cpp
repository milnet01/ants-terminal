// Feature-conformance test for spec.md — ANTS-5759: a click on a status-bar
// pill with no click action opens a window showing its tooltip, kept up to
// date while open. statusinfowindow.h is header-only, so the test drives it
// on a bare QLabel.

#include "statusinfowindow.h"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QStatusBar>

#include <gtest/gtest.h>

namespace {

void click(QWidget *w) {
    const QPointF pos(w->width() / 2.0, w->height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, pos, w->mapToGlobal(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, w->mapToGlobal(pos),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QApplication::sendEvent(w, &release);
    QApplication::processEvents();
}

// InfoWindow carries no Q_OBJECT, so it is found by its object name.
QList<QDialog *> windowsOf(QWidget *top) {
    QList<QDialog *> out;
    for (QDialog *d : top->findChildren<QDialog *>())
        if (d->objectName() == QStringLiteral("statusInfoWindow")) out << d;
    return out;
}

QString shownText(QDialog *win) {
    const auto labels = win->findChildren<QLabel *>();
    for (QLabel *l : labels)
        if (l->textInteractionFlags() & Qt::TextSelectableByMouse) return l->text();
    return {};
}

}  // namespace

// A click opens one window holding the tooltip; a second click raises the
// same window rather than opening another; a tooltip change updates it.
TEST(StatusInfoWindow, ClickShowsTheTooltipAndFollowsIt) {
    QMainWindow mw;
    auto *pill = new QLabel(QStringLiteral("42%"), &mw);
    pill->setToolTip(QStringLiteral("Context: 42%\nline two"));
    mw.statusBar()->addPermanentWidget(pill);
    statusinfo::attach(pill, QStringLiteral("Context"));
    mw.show();
    QApplication::processEvents();

    EXPECT_EQ(pill->cursor().shape(), Qt::PointingHandCursor);
    ASSERT_TRUE(windowsOf(&mw).isEmpty());

    click(pill);
    auto wins = windowsOf(&mw);
    ASSERT_EQ(wins.size(), 1);
    EXPECT_EQ(wins.first()->windowTitle(), QStringLiteral("Context"));
    EXPECT_EQ(shownText(wins.first()), QStringLiteral("Context: 42%\nline two"));
    EXPECT_TRUE(wins.first()->isVisible());

    click(pill);
    EXPECT_EQ(windowsOf(&mw).size(), 1);

    pill->setToolTip(QStringLiteral("Context: 43%"));
    QApplication::processEvents();
    EXPECT_EQ(shownText(windowsOf(&mw).first()), QStringLiteral("Context: 43%"));

    // A tooltip emptied while open closes the window.
    pill->setToolTip(QString());
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(windowsOf(&mw).isEmpty());
}

// A pill with no tooltip opens nothing; a right click opens nothing.
TEST(StatusInfoWindow, EmptyTooltipAndRightClickOpenNothing) {
    QMainWindow mw;
    auto *pill = new QLabel(QStringLiteral("idle"), &mw);
    mw.statusBar()->addPermanentWidget(pill);
    statusinfo::attach(pill, QStringLiteral("Claude status"));
    mw.show();
    QApplication::processEvents();

    click(pill);
    EXPECT_TRUE(windowsOf(&mw).isEmpty());

    pill->setToolTip(QStringLiteral("Claude: idle"));
    const QPointF pos(1, 1);
    QMouseEvent rightUp(QEvent::MouseButtonRelease, pos, pill->mapToGlobal(pos),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(pill, &rightUp);
    QApplication::processEvents();
    EXPECT_TRUE(windowsOf(&mw).isEmpty());
}

// The window is sized to show all of a long tooltip without scrolling.
TEST(StatusInfoWindow, WindowFitsTheText) {
    QMainWindow mw;
    auto *pill = new QLabel(QStringLiteral("saved"), &mw);
    QStringList lines;
    for (int i = 0; i < 12; ++i)
        lines << QStringLiteral("Line %1: tokens saved this month by project").arg(i);
    pill->setToolTip(lines.join(QChar(u'\n')));
    mw.statusBar()->addPermanentWidget(pill);
    statusinfo::attach(pill, QStringLiteral("Tokens saved"));
    mw.show();
    QApplication::processEvents();

    click(pill);
    const auto wins = windowsOf(&mw);
    ASSERT_EQ(wins.size(), 1);
    QApplication::processEvents();
    const auto areas = wins.first()->findChildren<QScrollArea *>();
    ASSERT_EQ(areas.size(), 1);
    QWidget *text = areas.first()->widget();
    EXPECT_GE(areas.first()->viewport()->width(),  text->sizeHint().width());
    EXPECT_GE(areas.first()->viewport()->height(), text->sizeHint().height());
}
