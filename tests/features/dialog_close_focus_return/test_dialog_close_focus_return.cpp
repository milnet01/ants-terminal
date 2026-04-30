// Feature-conformance test for tests/features/dialog_close_focus_return/spec.md.
//
// Locks ANTS-1050 — auto-return focus to terminal when any dialog
// closes. Drives dialogfocus::shouldRefocusOnDialogClose (pure static
// helper) on synthetic QDialog/QWidget instances + source-greps for
// the qApp filter installation, the eventFilter dispatch, the
// deferred-dispatch primitive, and the null-guard.
//
// Exit 0 = all invariants hold.

#include "dialogfocus.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QEvent>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const std::string source = slurp(MAINWINDOW_CPP);
    const std::string focusHdr = slurp(DIALOGFOCUS_H);
    if (source.empty()) return fail("INV-1", "mainwindow.cpp not readable");
    if (focusHdr.empty()) return fail("INV-2", "dialogfocus.h not readable");

    // INV-1: qApp->installEventFilter(this) in MainWindow's
    // constructor. We confirm the call exists; parsing the
    // constructor body precisely is brittle, so source-grep for
    // the literal call form.
    if (!contains(source, "qApp->installEventFilter(this)") &&
        !contains(source, "QApplication::instance()->installEventFilter(this)"))
        return fail("INV-1", "qApp->installEventFilter(this) not found");

    // INV-2: pure-logic helper signature exists in dialogfocus.h.
    if (!contains(focusHdr, "namespace dialogfocus") ||
        !contains(focusHdr,
            "shouldRefocusOnDialogClose(QObject *watched, QEvent *event)"))
        return fail("INV-2",
            "shouldRefocusOnDialogClose free function missing in dialogfocus.h");

    // INV-2a: positive case — Close on a visible QDialog with no
    // other dialog visible.
    {
        auto *dlg = new QDialog;
        dlg->show();
        // Make sure the dialog had a paint cycle so isVisible() is true.
        QApplication::processEvents();
        QCloseEvent ev;
        const bool got = dialogfocus::shouldRefocusOnDialogClose(dlg, &ev);
        delete dlg;
        if (!got)
            return fail("INV-2a",
                "Close on lone visible QDialog should refocus");
    }

    // INV-2b: non-QDialog widget — Close should NOT refocus.
    {
        auto *plain = new QWidget;
        plain->show();
        QApplication::processEvents();
        QCloseEvent ev;
        const bool got = dialogfocus::shouldRefocusOnDialogClose(plain, &ev);
        delete plain;
        if (got)
            return fail("INV-2b",
                "Close on non-QDialog widget must NOT refocus");
    }

    // INV-2c: stacked-dialog case — another QDialog still visible.
    {
        auto *a = new QDialog;
        auto *b = new QDialog;
        a->show();
        b->show();
        QApplication::processEvents();
        QCloseEvent ev;
        const bool got = dialogfocus::shouldRefocusOnDialogClose(b, &ev);
        delete a;
        delete b;
        if (got)
            return fail("INV-2c",
                "Close on B while A still visible must NOT refocus");
    }

    // INV-2d: non-Close events on a QDialog return false.
    {
        auto *dlg = new QDialog;
        dlg->show();
        QApplication::processEvents();
        const QEvent::Type sampleTypes[] = {
            QEvent::Show,
            QEvent::Hide,
            QEvent::MouseButtonPress,
            QEvent::KeyPress,
            QEvent::FocusIn,
        };
        for (auto t : sampleTypes) {
            QEvent ev(t);
            if (dialogfocus::shouldRefocusOnDialogClose(dlg, &ev)) {
                std::fprintf(stderr,
                    "[INV-2d] FAIL: event type %d should not refocus\n",
                    static_cast<int>(t));
                delete dlg;
                return 1;
            }
        }
        delete dlg;
    }

    // INV-2e: defensive null checks.
    {
        QCloseEvent ev;
        if (dialogfocus::shouldRefocusOnDialogClose(nullptr, &ev))
            return fail("INV-2", "nullptr watched should not refocus");
        auto *dlg = new QDialog;
        dlg->show();
        QApplication::processEvents();
        if (dialogfocus::shouldRefocusOnDialogClose(dlg, nullptr)) {
            delete dlg;
            return fail("INV-2", "nullptr event should not refocus");
        }
        delete dlg;
    }

    // INV-3: deferred-dispatch primitive used inside eventFilter.
    if (!contains(source, "QTimer::singleShot(0") &&
        !contains(source, "QMetaObject::invokeMethod") )
        return fail("INV-3",
            "no deferred-dispatch primitive (singleShot(0) or invokeMethod)");

    // INV-4: focusedTerminal() invoked inside the deferred lambda.
    // We don't precisely scope to the lambda; source-grep that the
    // eventFilter body references both shouldRefocusOnDialogClose
    // (the gate) and focusedTerminal() (the resolution).
    if (!contains(source, "shouldRefocusOnDialogClose"))
        return fail("INV-4",
            "eventFilter body does not invoke shouldRefocusOnDialogClose");
    if (!contains(source, "focusedTerminal()"))
        return fail("INV-4",
            "eventFilter body does not resolve focusedTerminal()");

    // INV-5: null-guard on the resolved terminal pointer. The
    // pattern is `if (auto *t = self->focusedTerminal()) ...` or
    // similar — assert the function body contains both
    // focusedTerminal() AND a `setFocus` call gated on a non-null
    // pointer. We approximate this by requiring `if` and
    // `focusedTerminal()` and `setFocus` all appear inside the
    // eventFilter body region.
    {
        const size_t fnStart = source.find(
            "MainWindow::eventFilter(QObject *watched, QEvent *event)");
        const size_t fnEndCandidate = source.find(
            "void MainWindow::closeEvent(", fnStart);
        if (fnStart == std::string::npos || fnEndCandidate == std::string::npos)
            return fail("INV-5",
                "could not locate eventFilter body bounds");
        const std::string body = source.substr(fnStart, fnEndCandidate - fnStart);
        if (!contains(body, "focusedTerminal()") ||
            !contains(body, "setFocus") ||
            !contains(body, "if "))
            return fail("INV-5",
                "eventFilter body missing null-guarded setFocus pattern");
    }

    // INV-6: shared eventFilter override (current implementation +
    // future ANTS-1051 path). We assert the close branch exists —
    // either by direct `QEvent::Close` reference or via the
    // `shouldRefocusOnDialogClose` helper (which encapsulates the
    // type check). The latter form is preferred since it's the
    // current implementation; tightening to a specific form happens
    // once ANTS-1051 lands.
    if (!contains(source, "QEvent::Close") &&
        !contains(source, "shouldRefocusOnDialogClose"))
        return fail("INV-6",
            "eventFilter body missing Close-event branch");

    std::fprintf(stderr, "OK — dialog close focus-return INVs hold.\n");
    return 0;
}

