// Feature-conformance test for tests/features/dialog_pseudo_modal/spec.md.
//
// Locks ANTS-1051 — pseudo-modal blocking via the qApp-level
// eventFilter. Drives dialogfocus::shouldSuppressEventForDialog on
// synthetic widget hierarchies, source-greps the eventFilter
// dispatch + the shared-with-1050 contract.
//
// Exit 0 = all invariants hold.

#include "dialogfocus.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

void fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
}

// Build a synthetic event of the requested type using the smallest
// constructor that produces a valid event for filter testing.
// Returns a heap-allocated event the caller deletes.
QEvent *makeEvent(QEvent::Type t) {
    switch (t) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        return new QMouseEvent(t, QPointF(0, 0), QPointF(0, 0),
                               Qt::LeftButton, Qt::LeftButton,
                               Qt::NoModifier);
    case QEvent::Wheel:
        return new QWheelEvent(QPointF(0, 0), QPointF(0, 0),
                               QPoint(), QPoint(0, 120),
                               Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false);
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        return new QKeyEvent(t, Qt::Key_A, Qt::NoModifier);
    default:
        return new QEvent(t);
    }
}

}  // namespace

TEST(DialogPseudoModal, Main) {    const std::string focusHdr = ants_test::slurpFile(DIALOGFOCUS_H);
    const std::string source = ants_test::slurpFile(MAINWINDOW_CPP);
    if (focusHdr.empty() || source.empty())
        fail("INV-1", "source files not readable");

    // INV-1: helper signature exists in dialogfocus.h.
    if (!contains(focusHdr, "namespace dialogfocus") ||
        !contains(focusHdr,
            "shouldSuppressEventForDialog(QObject *watched, QEvent *event)"))
        fail("INV-1",
            "shouldSuppressEventForDialog signature missing in dialogfocus.h");

    using dialogfocus::shouldSuppressEventForDialog;

    // INV-2a: positive case for ALL six suppressed event types.
    {
        auto *dlg = new QDialog;
        dlg->show();
        QApplication::processEvents();
        auto *outside = new QWidget;
        outside->show();
        QApplication::processEvents();

        const QEvent::Type suppressed[] = {
            QEvent::MouseButtonPress,
            QEvent::MouseButtonRelease,
            QEvent::MouseButtonDblClick,
            QEvent::Wheel,
            QEvent::KeyPress,
            QEvent::KeyRelease,
        };
        for (QEvent::Type t : suppressed) {
            QEvent *ev = makeEvent(t);
            const bool got = shouldSuppressEventForDialog(outside, ev);
            delete ev;
            if (!got) {
                std::fprintf(stderr,
                    "[INV-2a] FAIL: event type %d on outside widget should suppress\n",
                    static_cast<int>(t));
                delete dlg;
                delete outside;
                FAIL();
            }
        }
        delete outside;
        delete dlg;
    }

    // INV-2b: negation — watched is the dialog or a child. With the
    // dialog visible, suppressing must be false for both.
    {
        auto *dlg = new QDialog;
        auto *child = new QPushButton("ok", dlg);
        dlg->show();
        QApplication::processEvents();
        QEvent *ev = makeEvent(QEvent::MouseButtonPress);
        const bool gotDlg = shouldSuppressEventForDialog(dlg, ev);
        const bool gotChild = shouldSuppressEventForDialog(child, ev);
        delete ev;
        delete dlg;  // also deletes child via QObject parent
        if (gotDlg)
            fail("INV-2h",
                "watched IS the dialog should NOT suppress");
        if (gotChild)
            fail("INV-2b",
                "watched is dialog's child should NOT suppress");
    }

    // INV-2c: no visible dialog → suppress false.
    {
        auto *outside = new QWidget;
        outside->show();
        QApplication::processEvents();
        QEvent *ev = makeEvent(QEvent::MouseButtonPress);
        const bool got = shouldSuppressEventForDialog(outside, ev);
        delete ev;
        delete outside;
        if (got)
            fail("INV-2c",
                "no visible dialog should not suppress anything");
    }

    // INV-2d: visible dialog + non-mouse/key event → suppress false.
    {
        auto *dlg = new QDialog;
        dlg->show();
        QApplication::processEvents();
        auto *outside = new QWidget;
        outside->show();
        QApplication::processEvents();
        const QEvent::Type allowed[] = {
            QEvent::Show, QEvent::Hide, QEvent::Paint,
            QEvent::FocusIn, QEvent::Close, QEvent::Resize,
            QEvent::Move, QEvent::Timer,
        };
        for (QEvent::Type t : allowed) {
            QEvent *ev = makeEvent(t);
            const bool got = shouldSuppressEventForDialog(outside, ev);
            delete ev;
            if (got) {
                std::fprintf(stderr,
                    "[INV-2d] FAIL: non-mouse/key event type %d should NOT suppress\n",
                    static_cast<int>(t));
                delete dlg;
                delete outside;
                FAIL();
            }
        }
        delete outside;
        delete dlg;
    }

    // INV-2e: stacked dialogs A and B both visible. Click on A
    // (or A's child) → allowed.
    // INV-2f: stacked dialogs both visible, click outside both →
    // suppressed.
    {
        auto *a = new QDialog;
        auto *b = new QDialog;
        auto *aChild = new QPushButton("a", a);
        auto *outside = new QWidget;
        a->show();
        b->show();
        outside->show();
        QApplication::processEvents();
        QEvent *ev = makeEvent(QEvent::MouseButtonPress);
        const bool onA      = shouldSuppressEventForDialog(a, ev);
        const bool onAChild = shouldSuppressEventForDialog(aChild, ev);
        const bool onOutside= shouldSuppressEventForDialog(outside, ev);
        delete ev;
        delete a;  // owns aChild
        delete b;
        delete outside;
        if (onA)
            fail("INV-2e",
                "stacked: click on dialog A itself should be allowed");
        if (onAChild)
            fail("INV-2e",
                "stacked: click on dialog A's child should be allowed");
        if (!onOutside)
            fail("INV-2f",
                "stacked: click outside both dialogs should suppress");
    }

    // INV-2i: structural — the activePopupWidget() short-circuit
    // must exist in dialogfocus.h so combo dropdowns / menus opened
    // from inside a dialog aren't eaten by the pseudo-modal blocker.
    // Behavioural drive isn't viable: the offscreen QPA platform
    // doesn't promote Qt::Popup widgets (bare or via QMenu::popup())
    // to QApplication::activePopupWidget(), so a positive behavioural
    // assertion would silently no-op on CI. Source-grep matches the
    // pattern INV-1 uses for the helper signature.
    if (!contains(focusHdr, "activePopupWidget"))
        fail("INV-2i",
            "dialogfocus.h missing QApplication::activePopupWidget() "
            "short-circuit — combo dropdown clicks will be eaten by "
            "the pseudo-modal blocker (regression of the 0.7.85 "
            "RoadmapDialog density combo bug)");
    if (!contains(focusHdr, "popup->isAncestorOf(target)") &&
        !contains(focusHdr, "popup->isAncestorOf("))
        fail("INV-2i",
            "dialogfocus.h has activePopupWidget() reference but no "
            "ancestor check on the popup tree — popup descendants "
            "(QListView items inside a combo dropdown) won't be "
            "matched without the isAncestorOf walk");

    // INV-2g: defensive null checks.
    {
        QEvent *ev = makeEvent(QEvent::MouseButtonPress);
        if (shouldSuppressEventForDialog(nullptr, ev)) {
            delete ev;
            fail("INV-2g", "nullptr watched should not suppress");
        }
        auto *w = new QWidget;
        w->show();
        QApplication::processEvents();
        if (shouldSuppressEventForDialog(w, nullptr)) {
            delete ev;
            delete w;
            fail("INV-2g", "nullptr event should not suppress");
        }
        delete ev;
        delete w;
    }

    // INV-3: mouse/key suppression wired through eventFilter.
    if (!contains(source, "shouldSuppressEventForDialog"))
        fail("INV-3",
            "eventFilter does not invoke shouldSuppressEventForDialog");
    if (!contains(source, "return true;"))
        fail("INV-3",
            "eventFilter has no `return true` to swallow events");

    // INV-4: shared eventFilter — both helpers referenced in the
    // same TU.
    if (!contains(source, "shouldRefocusOnDialogClose"))
        fail("INV-4",
            "eventFilter does not invoke shouldRefocusOnDialogClose (ANTS-1050)");

    // INV-5: mutual exclusivity is structural — shouldSuppressEventFor
    // Dialog returns false on Close (covered by INV-2d). No additional
    // assertion needed; this is documented for the spec reader.

    std::fprintf(stderr, "OK — pseudo-modal INVs hold.\n");
    return;
}

