// Feature-conformance test for spec.md —
//
// Invariant 1 — Help menu exists and is last on the menubar.
// Invariant 2 — Help menu contains "About Ants Terminal..." action.
// Invariant 3 — About handler body references ANTS_VERSION.
// Invariant 4 — About dialog is a custom QDialog with a plain
//               QPushButton (NOT QDialogButtonBox) whose clicked() is
//               wired to QDialog::close; the body QLabel uses
//               Qt::RichText + LinksAccessibleByMouse +
//               setOpenExternalLinks(true), and does NOT enable
//               TextSelectableByMouse / TextBrowserInteraction (the
//               regression shape that silently dropped the OK click on
//               KDE/KWin + Qt 6.11 — see spec.md Regression history
//               2026-04-25). 0.7.50 retired QDialogButtonBox after
//               three failed fix attempts: per QTBUG-79126 the role-
//               dispatch path is an aggravator on Wayland.
// Invariant 5 — "About Qt..." action exists and uses the same
//               heap+show pattern as About Ants Terminal — NOT
//               QMessageBox::aboutQt (whose internal exec() blocked OK
//               clicks on KDE/KWin + Qt 6.11 with a frameless
//               translucent parent — see spec.md regression 2026-04-27).
// Invariant 6 — no hardcoded "0.7." literal inside the About handler.
// Invariant 7 — Both About dialogs use heap allocation +
//               WA_DeleteOnClose + show()/raise()/activateWindow()
//               rather than stack + exec(), and are NON-MODAL (no
//               setModal(true) call). 0.7.49's setModal(true) was
//               retired in 0.7.50 — Wayland has no equivalent of
//               Qt::ApplicationModal (QTBUG-79126), and the modal
//               grab caused KWin/Wayland to route the OK click into
//               the grab handler instead of the button.
//
// Source-grep only. MainWindow is too heavy to instantiate in a
// feature test; the wiring of a menu item is structurally obvious
// from the source, and the grep catches the regression shapes
// (menu dropped, version literal hardcoded, rich-text disabled,
// QMessageBox + TextBrowserInteraction reintroduced).

#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const QString &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label,
                     qUtf8Printable(detail));
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    auto loadFile = [](const QString &path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr,
                         "[FAIL] source-open: cannot read %s\n",
                         qUtf8Printable(path));
            return {};
        }
        const QString s = QString::fromUtf8(f.readAll());
        f.close();
        return s;
    };

    // Menu-structure invariants (I1, I2, I5) live in mainwindow.cpp's
    // setupHelpMenu. Dialog-construction invariants (I3, I4, I6, I7)
    // moved to src/aboutdialogs.cpp post-ANTS-1181 — both files are
    // checked, and the dialog-body grep is scoped to aboutdialogs.cpp.
    const QString src = loadFile(QStringLiteral(SRC_MAINWINDOW_PATH));
    if (src.isEmpty()) return 1;

    const QString aboutSrc = loadFile(QStringLiteral(SRC_ABOUTDIALOGS_PATH));
    if (aboutSrc.isEmpty()) return 1;

    // Invariant 1 — Help menu present, and it appears AFTER every other
    // m_menuBar->addMenu(...) call (so Qt places it last).
    const int helpIdx = src.indexOf(
        QStringLiteral("m_menuBar->addMenu(\"&Help\")"));
    expect(helpIdx > 0, "I1/help-menu-added");

    // Collect every addMenu call index and assert the Help one is last.
    int lastOtherAddMenu = -1;
    int scan = 0;
    while (true) {
        const int next = src.indexOf(
            QStringLiteral("m_menuBar->addMenu("), scan);
        if (next < 0) break;
        if (next != helpIdx && next > lastOtherAddMenu) lastOtherAddMenu = next;
        scan = next + 1;
    }
    expect(helpIdx > lastOtherAddMenu,
           "I1/help-menu-is-last",
           QStringLiteral("Help menu must be added last on m_menuBar so "
                          "it sits at the rightmost position; helpIdx=%1 "
                          "lastOther=%2").arg(helpIdx).arg(lastOtherAddMenu));

    // Invariant 2 — About Ants Terminal action.
    expect(src.contains(
               QStringLiteral("addAction(\"&About Ants Terminal...\")")),
           "I2/about-ants-action-present");

    // Invariant 5 — About Qt action present, but no longer routes to
    // QMessageBox::aboutQt (its internal exec() blocked clicks under our
    // frameless+translucent parent on KDE/KWin + Qt 6.11 — 2026-04-27).
    expect(src.contains(QStringLiteral("addAction(\"About &Qt...\")")),
           "I5a/about-qt-action-present");
    expect(!src.contains(QStringLiteral("QMessageBox::aboutQt(")),
           "I5b/about-qt-no-longer-uses-aboutQt-helper",
           QStringLiteral("QMessageBox::aboutQt(...) opens a nested "
                          "event loop via exec() which on KDE/KWin + "
                          "Qt 6.11 with a frameless+translucent "
                          "MainWindow does not surface the dialog as "
                          "the active input window — OK clicks no-op. "
                          "Use a custom QDialog with show()+raise()+"
                          "activateWindow() instead, mirroring the "
                          "About Ants pattern."));

    // ANTS-1181: dialog construction lives in aboutdialogs.cpp; the
    // showAboutAnts() / showAboutQt() function bodies hold what used to
    // be inline lambdas. Both delegate to a file-local makeAboutDialog
    // helper for the heap+show+raise+activate scaffolding, so the I7
    // patterns appear ONCE in the helper rather than duplicated per
    // dialog. We scope `aboutBlock` to showAboutAnts (text + ANTS_VERSION
    // + body invariants) and `aboutQtBlock` to showAboutQt (text + Qt-
    // body invariants); shared scaffolding is verified via `aboutSrc`.
    auto extractFunctionBody = [](const QString &source,
                                  const QString &signature) -> QString {
        const int sigIdx = source.indexOf(signature);
        if (sigIdx < 0) return {};
        // Walk forward to the opening `{`, then balance braces to the
        // matching `}`.
        int braceIdx = source.indexOf(QChar('{'), sigIdx);
        if (braceIdx < 0) return {};
        int depth = 1;
        int i = braceIdx + 1;
        while (i < source.size() && depth > 0) {
            const QChar c = source.at(i);
            if (c == QChar('{')) ++depth;
            else if (c == QChar('}')) --depth;
            ++i;
        }
        return source.mid(braceIdx, i - braceIdx);
    };

    const QString aboutBlock = extractFunctionBody(
        aboutSrc, QStringLiteral("void showAboutAnts(QWidget *parent)"));
    expect(!aboutBlock.isEmpty(), "I2b/handler-block-located",
           QStringLiteral("Could not locate showAboutAnts() body in "
                          "aboutdialogs.cpp — has the function been "
                          "renamed or removed?"));

    const QString aboutQtBlock = extractFunctionBody(
        aboutSrc, QStringLiteral("void showAboutQt(QWidget *parent)"));
    expect(!aboutQtBlock.isEmpty(), "I5c/aboutqt-handler-block-located",
           QStringLiteral("Could not locate showAboutQt() body in "
                          "aboutdialogs.cpp — has the function been "
                          "renamed or removed?"));

    // Invariant 3 — ANTS_VERSION used, not a literal.
    expect(aboutBlock.contains(QStringLiteral("ANTS_VERSION")),
           "I3/body-uses-ANTS_VERSION",
           QStringLiteral("About handler must read the ANTS_VERSION "
                          "macro — CMakeLists.txt is the single source "
                          "of truth; hardcoded literals drift on every "
                          "bump"));

    // Invariant 6 — no hardcoded 0.7. version literal inside the handler.
    //   The only "0." literals allowed near this code are in adjacent
    //   comments or separate menus; scoping to aboutBlock catches a
    //   regression where someone pastes the current version inline.
    expect(!aboutBlock.contains(QStringLiteral("\"0.7.")) &&
               !aboutBlock.contains(QStringLiteral("Version: 0.")),
           "I6/no-hardcoded-version-literal-in-handler",
           QStringLiteral("About handler must not embed a version "
                          "string literal like \"0.7.x\" — use "
                          "ANTS_VERSION so the dialog stays accurate "
                          "across releases"));

    // Invariant 4 — rich-text + clickable links via the custom QDialog
    // path (see spec.md, regression 2026-04-25). ANTS-1181: the QLabel
    // setup lives in the file-local makeAboutDialog() helper in
    // aboutdialogs.cpp, so I4a-c + I4h scope to `aboutSrc` (file-wide)
    // rather than per-handler — the helper applies the contract
    // identically to both About dialogs.
    expect(aboutSrc.contains(QStringLiteral("setTextFormat(Qt::RichText)")),
           "I4a/rich-text-enabled");
    expect(aboutSrc.contains(QStringLiteral("Qt::LinksAccessibleByMouse")),
           "I4b/links-clickable",
           QStringLiteral("Body QLabel must enable Qt::LinksAccessibleByMouse "
                          "so the GitHub URL is clickable."));
    expect(aboutSrc.contains(QStringLiteral("setOpenExternalLinks(true)")),
           "I4c/open-external-links",
           QStringLiteral("Body QLabel must call setOpenExternalLinks(true); "
                          "without it, link clicks silently no-op even when "
                          "LinksAccessibleByMouse is set."));
    // Negative: the QMessageBox + TextBrowserInteraction shape that
    // caused the 2026-04-25 OK-button regression must stay out of the
    // dialog factory file. TextSelectableByMouse on the label is part
    // of the regression — we only need links clickable, not selection.
    // Match the call shape, not the bare token, so prose comments
    // explaining the regression don't false-positive on the grep.
    expect(!aboutSrc.contains(
               QStringLiteral("setTextInteractionFlags(Qt::TextBrowserInteraction)")),
           "I4d/no-textbrowserinteraction-regression",
           QStringLiteral("setTextInteractionFlags(Qt::TextBrowserInteraction) "
                          "is the regression shape that silently dropped the "
                          "OK click under our frameless + WA_TranslucentBackground "
                          "MainWindow on KDE/KWin + Qt 6.11. Use "
                          "Qt::LinksAccessibleByMouse + setOpenExternalLinks(true) "
                          "instead — same user-visible behaviour, no click steal."));
    expect(!aboutSrc.contains(
               QStringLiteral("setTextInteractionFlags(Qt::TextSelectableByMouse")),
           "I4e/no-text-selection-on-body",
           QStringLiteral("TextSelectableByMouse on the About body is part of "
                          "the regression shape — there's nothing in the body "
                          "the user needs to select."));
    expect(!aboutSrc.contains(QStringLiteral("QMessageBox mb(")),
           "I4f/no-qmessagebox-construction",
           QStringLiteral("The About handler must use QDialog + "
                          "plain QPushButton so we control the OK wiring "
                          "explicitly. QMessageBox::Ok is what regressed."));
    // 0.7.50 retired QDialogButtonBox in this dialog after three
    // failed fix attempts; per QTBUG-79126 its role-dispatch path is
    // a click-drop aggravator on KWin/Wayland. Match the constructor
    // call shape (`new QDialogButtonBox`) so explanatory comments
    // mentioning the type don't false-positive the grep.
    expect(!aboutSrc.contains(QStringLiteral("new QDialogButtonBox")),
           "I4g/no-qdialogbuttonbox",
           QStringLiteral("The About handler must NOT construct a "
                          "QDialogButtonBox — 0.7.50 reverted to a plain "
                          "QPushButton because the role-dispatch path "
                          "interacts badly with KWin/Wayland (QTBUG-79126). "
                          "Use a plain QPushButton labelled \"OK\" wired to "
                          "QDialog::close."));
    // The OK button must be wired to dlg->close() — the missing wire is
    // exactly what makes a button "do nothing." Match against the
    // plain QPushButton::clicked → QDialog::close shape.
    expect(aboutSrc.contains(
               QStringLiteral("&QPushButton::clicked")) &&
           aboutSrc.contains(QStringLiteral("&QDialog::close")),
           "I4h/ok-connected-to-close",
           QStringLiteral("Plain QPushButton::clicked must be connected to "
                          "QDialog::close so clicking OK closes the dialog."));

    // Invariant 7 — heap allocation + show()/raise()/activateWindow()
    // pattern. A stack-allocated `QDialog dlg(this)` followed by
    // `dlg.exec()` opens a nested event loop that on KDE/KWin +
    // Qt 6.11 + our frameless translucent MainWindow does not promote
    // the dialog to active input window, so OK clicks no-op
    // (regression 2026-04-27). Post-ANTS-1181 the heap+WA_DeleteOnClose
    // scaffolding lives in a file-local makeAboutDialog() helper, so the
    // alloc invariants are checked against `aboutSrc` (file-wide) while
    // the show/raise/activate sequence stays per-handler (each
    // showAboutAnts/showAboutQt body issues its own show+raise+activate).

    // I7a — heap allocation must use `new QDialog(parent)` (post-1181)
    // or `new QDialog(this)` (pre-1181 inline shape). Either is fine
    // for the contract being tested.
    expect(aboutSrc.contains(QStringLiteral("new QDialog(parent)")) ||
               aboutSrc.contains(QStringLiteral("new QDialog(this)")),
           "I7a/heap-allocated",
           QStringLiteral("Dialog must be heap-allocated via "
                          "`new QDialog(parent)` (or `(this)` if inline) "
                          "so it survives the function's return; the "
                          "show()-pattern doesn't block the caller."));

    // I7b — WA_DeleteOnClose set in the helper (or inline body).
    expect(aboutSrc.contains(QStringLiteral(
               "setAttribute(Qt::WA_DeleteOnClose)")),
           "I7b/wa-delete-on-close",
           QStringLiteral("Heap-allocated dialog must set "
                          "Qt::WA_DeleteOnClose so it deletes itself "
                          "on dismissal — otherwise the QDialog "
                          "leaks each time the menu item is invoked."));

    struct HandlerCheck {
        const QString *block;
        const char *name;
    };
    const HandlerCheck handlers[] = {
        {&aboutBlock,   "About-Ants"},
        {&aboutQtBlock, "About-Qt"},
    };
    for (const auto &h : handlers) {
        const QString &blk = *h.block;
        const char *name = h.name;
        QString tag;

        tag = QStringLiteral("I7c/%1/uses-show-not-exec").arg(name);
        expect(blk.contains(QStringLiteral("dlg->show()")),
               qUtf8Printable(tag),
               QStringLiteral("Dialog must be displayed via show(), not "
                              "exec(). exec() opens a nested QEventLoop "
                              "that doesn't surface the dialog as the "
                              "active input window on KDE/KWin + Qt 6.11."));

        tag = QStringLiteral("I7d/%1/raise-and-activate").arg(name);
        expect(blk.contains(QStringLiteral("dlg->raise()")) &&
                   blk.contains(QStringLiteral("dlg->activateWindow()")),
               qUtf8Printable(tag),
               QStringLiteral("After show(), the dialog must call "
                              "raise() + activateWindow() so KWin "
                              "promotes it to the active input window. "
                              "Without these, the dialog appears but the "
                              "OK button never receives focus."));

        tag = QStringLiteral("I7e/%1/no-stack-exec").arg(name);
        // Reject the pre-fix shape: `QDialog dlg(this);` … `dlg.exec();`
        expect(!blk.contains(QStringLiteral("QDialog dlg(this)")) &&
                   !blk.contains(QStringLiteral("dlg.exec()")),
               qUtf8Printable(tag),
               QStringLiteral("The stack-allocated `QDialog dlg(this)` + "
                              "`dlg.exec()` pattern is exactly what "
                              "regressed on KDE/KWin + Qt 6.11 — clicking "
                              "OK no-opped because exec()'s nested event "
                              "loop did not promote the dialog to the "
                              "active input window."));

        tag = QStringLiteral("I7f/%1/non-modal").arg(name);
        // 0.7.49 added setModal(true) on the theory it would help; the
        // actual effect on Wayland (QTBUG-79126) was the opposite —
        // the modal-grab routes the OK click into the grab handler
        // instead of the button. 0.7.50 dropped it.
        expect(!blk.contains(QStringLiteral("dlg->setModal(")),
               qUtf8Printable(tag),
               QStringLiteral("Dialog must NOT call setModal(true) — Wayland "
                              "has no Qt::ApplicationModal equivalent "
                              "(QTBUG-79126, QTBUG-90005), and the modal-"
                              "grab side effect on KWin/Wayland routes the "
                              "OK click into the grab handler, dropping it. "
                              "0.7.50 reverted to non-modal."));
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
