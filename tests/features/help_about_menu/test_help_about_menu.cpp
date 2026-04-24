// Feature-conformance test for spec.md —
//
// Invariant 1 — Help menu exists and is last on the menubar.
// Invariant 2 — Help menu contains "About Ants Terminal..." action.
// Invariant 3 — About handler body references ANTS_VERSION.
// Invariant 4 — About dialog is rich-text + TextBrowserInteraction.
// Invariant 5 — "About Qt..." action routes to QMessageBox::aboutQt.
// Invariant 6 — no hardcoded "0.7." literal inside the About handler.
//
// Source-grep only. MainWindow is too heavy to instantiate in a
// feature test; the wiring of a menu item is structurally obvious
// from the source, and the grep catches the three regression shapes
// (menu dropped, version literal hardcoded, rich-text disabled).

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

    const QString path = QStringLiteral(SRC_MAINWINDOW_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] source-open: cannot read %s\n",
                     qUtf8Printable(path));
        return 1;
    }
    const QString src = QString::fromUtf8(f.readAll());
    f.close();

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

    // Invariant 5 — About Qt action.
    expect(src.contains(QStringLiteral("addAction(\"About &Qt...\")")),
           "I5a/about-qt-action-present");
    expect(src.contains(QStringLiteral("QMessageBox::aboutQt(this")),
           "I5b/about-qt-routes-to-qt-helper");

    // Scope subsequent greps to the About-action handler lambda. The
    // handler body runs from the first "About Ants Terminal..." mention
    // until the closing `});` of the enclosing connect lambda. We pull
    // a generous window (2 000 chars) which is plenty for the handler
    // without overlapping the aboutQt lambda below it.
    const int aboutStart = src.indexOf(
        QStringLiteral("&About Ants Terminal..."));
    const int aboutEnd = src.indexOf(QStringLiteral("About &Qt..."),
                                     aboutStart);
    expect(aboutStart > 0 && aboutEnd > aboutStart,
           "I2b/handler-block-located");
    const QString aboutBlock = src.mid(aboutStart, aboutEnd - aboutStart);

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

    // Invariant 4 — rich-text + browser interaction.
    expect(aboutBlock.contains(QStringLiteral("setTextFormat(Qt::RichText)")),
           "I4a/rich-text-enabled");
    expect(aboutBlock.contains(
               QStringLiteral("setTextInteractionFlags(Qt::TextBrowserInteraction)")),
           "I4b/browser-interaction-enabled",
           QStringLiteral("URLs in the About dialog must be clickable; "
                          "that requires TextBrowserInteraction"));

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
