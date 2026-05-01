// Feature-conformance test for spec.md — asserts the two invariants
// that make the Review Changes button produce a visible dialog:
//
//   (A) A QDialog shown on a frameless QMainWindow parent needs
//       explicit raise() + activateWindow() to become the active
//       window. show() alone leaves the stacking order at the WM's
//       discretion — fine on GNOME, flaky on KWin with a frameless
//       parent.
//
//   (B) The focus-redirect guard: if code queues a deferred
//       setFocus() on a background widget (e.g. the terminal) and
//       later spawns a dialog before the deferred call fires, the
//       re-evaluation at firing time must see the dialog is now
//       active and skip the refocus. Without this, the queued
//       refocus steals focus from the dialog, re-activating its
//       parent and (on KWin) obscuring the dialog.
//
// These are the two regression vectors for the user report
// "Review Changes button does nothing when I click it" — see
// tests/features/review_changes_click/spec.md for the full contract.
//
// The test does NOT build a full MainWindow — that would require
// linking terminalwidget + claudeintegration + half the project.
// Instead it reproduces the exact widget topology (frameless
// QMainWindow + QStatusBar + QDialog child) and the exact Qt API
// calls (show / raise / activateWindow / QTimer::singleShot +
// QApplication::activeWindow re-check). Both invariants are API-
// level, so a topology-faithful harness is sufficient.
//
// Runs under QT_QPA_PLATFORM=offscreen; QApplication::activeWindow
// is reliable under offscreen (it tracks the most-recently-activated
// top-level QWidget regardless of real WM).
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QString>
#include <QTimer>

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

// Invariant A: show() + raise() + activateWindow() on a frameless
// parent produces an active dialog. This is the direct counterpart
// of the showDiffViewer fix — we're asserting the API contract the
// fix relies on.
void testDialogBecomesActiveAfterRaiseActivate() {
    QMainWindow win;
    // Match the MainWindow flag at mainwindow.cpp:74 — Qt::FramelessWindowHint
    // is the environmental condition that makes raise/activate necessary
    // in the first place.
    win.setWindowFlag(Qt::FramelessWindowHint);
    win.resize(800, 600);
    win.show();
    QApplication::processEvents();

    // Sanity: with only the main window shown, it's the active window.
    CHECK(QApplication::activeWindow() == &win,
          "main window not active after show (harness precondition)");

    // Simulate showDiffViewer's dialog creation path.
    auto *dialog = new QDialog(&win);
    dialog->setWindowTitle("Review Changes");
    dialog->resize(400, 300);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // --- The actual invariant under test: ---
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    QApplication::processEvents();

    CHECK(dialog->isVisible(),
          "dialog not visible after show() (regression)");
    CHECK(QApplication::activeWindow() == dialog,
          "dialog did not become active window after raise()+activateWindow() "
          "(regression: showDiffViewer would leave user seeing 'button did nothing')");

    dialog->close();
    QApplication::processEvents();
}

// Invariant B — fire-time re-evaluation. Reproduces the exact lambda
// shape in mainwindow.cpp's focusChanged handler: a QTimer::singleShot(0)
// captures a pointer to the background widget and, at firing time,
// must consult QApplication::activeWindow() — if it's a QDialog other
// than the background widget's own window, skip the refocus.
void testQueuedRefocusSkippedWhenDialogActive() {
    QMainWindow win;
    win.setWindowFlag(Qt::FramelessWindowHint);
    win.resize(800, 600);

    // "Terminal" stand-in — the background widget the lambda would
    // refocus. Using a plain QWidget is enough; the invariant is
    // about the guard, not the target widget type.
    auto *bgTarget = new QWidget(&win);
    bgTarget->setFocusPolicy(Qt::StrongFocus);
    win.setCentralWidget(bgTarget);

    // "Status bar button" stand-in.
    auto *statusBtn = new QPushButton("Review Changes", &win);
    win.statusBar()->addPermanentWidget(statusBtn);

    win.show();
    QApplication::processEvents();
    bgTarget->setFocus();
    QApplication::processEvents();

    // Track whether the queued refocus actually fired (vs. was skipped
    // by the guard). We observe this by checking bgTarget's focus state
    // after the event loop drains.
    bool refocusFired = false;

    // Simulate: status bar button took focus (mouse press). The
    // production lambda at this point queues a singleShot that
    // refocuses bgTarget IF no dialog has been shown by fire time.
    // We replicate the *exact* guard structure: re-check
    // QApplication::activeWindow() at the moment of the fire, not at
    // the moment of the queue.
    QPointer<QWidget> guard(bgTarget);
    QTimer::singleShot(0, &win, [guard, &refocusFired]() {
        if (!guard) return;
        // Mirror the exact guard from mainwindow.cpp's focusChanged
        // lambda: walk top-level widgets, skip if any visible QDialog
        // is live. Visibility is synchronous (set by QWidget::show()
        // before it returns), so this works regardless of WM/platform
        // activateWindow timing — unlike QApplication::activeWindow()
        // which lags by one event-loop iteration on offscreen.
        const QWidget *mainWin = guard->window();
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w == mainWin) continue;
            if (!w->isVisible()) continue;
            if (w->inherits("QDialog")) {
                return;
            }
        }
        refocusFired = true;
        guard->setFocus(Qt::OtherFocusReason);
    });

    // Before the singleShot fires, simulate showDiffViewer opening a
    // dialog (the thing the button click handler would do). The dialog
    // raises + activates just like the fix in mainwindow.cpp.
    auto *dialog = new QDialog(&win);
    dialog->setWindowTitle("Review Changes");
    dialog->resize(400, 300);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Precondition: the dialog must be visible before the queued
    // singleShot fires (visibility is synchronous, so no processEvents
    // required to observe it).
    CHECK(dialog->isVisible(),
          "dialog not visible after show() (test precondition violated)");

    // Drain the event queue so the singleShot fires.
    QApplication::processEvents();
    QApplication::processEvents();  // second pump for zero-timer dispatch

    // The guard should have skipped — refocusFired must stay false.
    CHECK(!refocusFired,
          "queued refocus fired despite dialog being visible "
          "(regression: terminal-refocus would steal focus from the dialog, "
          "re-activating the main window over it on KWin)");

    dialog->close();
    QApplication::processEvents();
}

// Invariant A — production-binding check. The API-level test above
// proves show+raise+activateWindow produces an active dialog, but a
// harness-only test doesn't catch a regression that reverts the
// raise/activateWindow calls inside the actual showDiffViewer().
// This test reads mainwindow.cpp at runtime and asserts the two
// required API calls appear *within* the showDiffViewer function
// body. A simple string search is sufficient — any formatting-style
// change that preserves the semantics keeps the calls; any revert
// that drops them fails.
//
// Why a source-grep rather than linking mainwindow.cpp: the
// showDiffViewer function has a deep dependency chain (TerminalWidget
// → PTY → VtParser, ClaudeIntegration, most of the project's srcs)
// and spinning up an entire MainWindow for a dialog visibility check
// is disproportionate. The grep binds the test to the fix site
// directly with minimal friction.
void testShowDiffViewerCallsRaiseAndActivate() {
    // ANTS-1145 (0.7.73): the dialog construction body lives in
    // src/diffviewer.cpp's `diffviewer::show(QWidget *, const
    // QString &, const QString &)` after the carve-out. The CMake
    // define `SRC_MAINWINDOW_PATH` is repointed at diffviewer.cpp
    // — name preserved so the wiring noise stays small.
    const QString path = QStringLiteral(SRC_MAINWINDOW_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "FAIL: cannot open %s — test harness wiring broken\n",
                     qUtf8Printable(path));
        ++failures;
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());

    // Find the dialog construction function body. Search for the
    // post-carve-out signature and then balanced-brace-scan from
    // its opening brace to the matching close.
    const int start = src.indexOf(QStringLiteral("QDialog *show("));
    if (start < 0) {
        std::fprintf(stderr,
                     "FAIL: diffviewer::show not found in %s — function renamed "
                     "or extraction reverted?\n",
                     qUtf8Printable(path));
        ++failures;
        return;
    }
    int braceStart = src.indexOf(QChar('{'), start);
    if (braceStart < 0) { ++failures; return; }
    int depth = 1;
    int i = braceStart + 1;
    while (i < src.size() && depth > 0) {
        QChar c = src.at(i);
        if (c == QChar('{')) ++depth;
        else if (c == QChar('}')) --depth;
        ++i;
    }
    const QString body = src.mid(braceStart, i - braceStart);

    // Invariant A production binding: showDiffViewer MUST call raise()
    // and activateWindow() on its dialog after show(). The user-
    // reported "button does nothing" regression was show() alone.
    CHECK(body.contains(QStringLiteral("dialog->raise()")) ||
          body.contains(QStringLiteral("->raise();")),
          "showDiffViewer does not call raise() on its dialog "
          "(regression: 'Review Changes does nothing' — dialog opens behind main window)");

    CHECK(body.contains(QStringLiteral("dialog->activateWindow()")) ||
          body.contains(QStringLiteral("->activateWindow();")),
          "showDiffViewer does not call activateWindow() on its dialog "
          "(regression: 'Review Changes does nothing' — dialog lacks input focus, "
          "queued terminal-refocus steals it away)");
}

// Invariant B, inverse direction: when NO dialog has been shown, the
// queued refocus MUST still fire — we haven't broken the original
// 0.6.26 behavior that returns focus to the terminal after a chrome
// click.
void testQueuedRefocusFiresWhenNoDialog() {
    QMainWindow win;
    win.setWindowFlag(Qt::FramelessWindowHint);
    win.resize(800, 600);

    auto *bgTarget = new QWidget(&win);
    bgTarget->setFocusPolicy(Qt::StrongFocus);
    win.setCentralWidget(bgTarget);

    win.show();
    QApplication::processEvents();

    bool refocusFired = false;

    QPointer<QWidget> guard(bgTarget);
    QTimer::singleShot(0, &win, [guard, &refocusFired]() {
        if (!guard) return;
        const QWidget *mainWin = guard->window();
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w == mainWin) continue;
            if (!w->isVisible()) continue;
            if (w->inherits("QDialog")) {
                return;
            }
        }
        refocusFired = true;
        guard->setFocus(Qt::OtherFocusReason);
    });

    // No dialog shown — just drain the event loop.
    QApplication::processEvents();
    QApplication::processEvents();

    CHECK(refocusFired,
          "queued refocus did NOT fire when no dialog was visible "
          "(regression: breaks the 0.6.26 status-bar chrome auto-return-to-terminal)");
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    testDialogBecomesActiveAfterRaiseActivate();
    testShowDiffViewerCallsRaiseAndActivate();
    testQueuedRefocusSkippedWhenDialogActive();
    testQueuedRefocusFiresWhenNoDialog();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All Review Changes click invariants hold.\n");
    return 0;
}
