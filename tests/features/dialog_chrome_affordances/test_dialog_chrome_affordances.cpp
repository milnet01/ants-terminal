// ANTS-1842 — DialogChrome D2–D4 affordances (GUI bundle, offscreen).
// See tests/features/dialog_chrome_affordances/spec.md.

#include "config.h"
#include "dialogchrome.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QShowEvent>
#include <QSize>
#include <QSizeGrip>

#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

namespace {

// Restores the process-global Config registration to nullptr on scope exit
// so a test that registers a temp Config can't dangle it for the next
// bundle-sibling dialog test.
struct ConfigGuard {
    explicit ConfigGuard(Config *c) { DialogChrome::setConfig(c); }
    ~ConfigGuard() { DialogChrome::setConfig(nullptr); }
};

}  // namespace

// INV-1 — resizable install adds a QSizeGrip (D2).
TEST(DialogChromeAffordances, INV1_ResizableAddsSizeGrip) {
    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("K1"));
    EXPECT_NE(dlg.findChild<QSizeGrip *>(), nullptr)
        << "resizable dialog must carry a QSizeGrip (frameless = no OS edge)";
}

// INV-2 — default (D1-only) install adds NO QSizeGrip.
TEST(DialogChromeAffordances, INV2_DefaultHasNoGrip) {
    QDialog dlg;
    DialogChrome::install(&dlg);  // resizable defaults false
    EXPECT_EQ(dlg.findChild<QSizeGrip *>(), nullptr)
        << "a non-opted-in dialog must stay D1-only (no grip)";
}

// INV-3 — closing a resizable+keyed dialog persists its size (D3 save).
TEST(DialogChromeAffordances, INV3_CloseSavesSize) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // isolate config.json; restored on scope exit
    Config cfg;
    ConfigGuard cg(&cfg);

    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("SaveMe"));
    dlg.resize(640, 480);
    QCloseEvent ce;
    QApplication::sendEvent(&dlg, &ce);

    EXPECT_EQ(cfg.dialogSize(QStringLiteral("SaveMe")), QSize(640, 480))
        << "close must persist the chosen size under the key";
}

// INV-4 — a keyed dialog restores its saved size on first show (D3 restore).
TEST(DialogChromeAffordances, INV4_ShowRestoresSize) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    Config cfg;
    cfg.setDialogSize(QStringLiteral("RestoreMe"), QSize(720, 510));
    ConfigGuard cg(&cfg);

    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("RestoreMe"));
    QShowEvent se;
    QApplication::sendEvent(&dlg, &se);

    EXPECT_EQ(dlg.size(), QSize(720, 510))
        << "first show must restore the persisted size";
}

// INV-5 — Config size map round-trips a QSize; missing key → invalid.
TEST(DialogChromeAffordances, INV5_ConfigSizeRoundTrip) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    Config cfg;
    EXPECT_FALSE(cfg.dialogSize(QStringLiteral("none")).isValid());
    cfg.setDialogSize(QStringLiteral("d"), QSize(1024, 768));
    EXPECT_EQ(cfg.dialogSize(QStringLiteral("d")), QSize(1024, 768));
    // Invalid / empty sizes are not stored.
    cfg.setDialogSize(QStringLiteral("e"), QSize());
    EXPECT_FALSE(cfg.dialogSize(QStringLiteral("e")).isValid());
}

// INV-6 — releasing a Config falls back to the one registered before it
// (ANTS-5036). A second window closing must not leave D3 on its Config.
TEST(DialogChromeAffordances, INV6_ReleaseFallsBackToEarlierConfig) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    Config first;
    first.setDialogSize(QStringLiteral("Fallback"), QSize(700, 500));
    ConfigGuard cg(&first);
    Config second;
    second.setDialogSize(QStringLiteral("Fallback"), QSize(800, 600));
    DialogChrome::setConfig(&second);
    DialogChrome::releaseConfig(&second);

    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("Fallback"));
    QShowEvent se;
    QApplication::sendEvent(&dlg, &se);

    EXPECT_EQ(dlg.size(), QSize(700, 500))
        << "after releasing the second Config, D3 must use the first";
}

// INV-7 — releasing the only Config leaves D3 inert (ANTS-5036).
TEST(DialogChromeAffordances, INV7_ReleasingLastConfigLeavesD3Inert) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    Config only;
    only.setDialogSize(QStringLiteral("Inert"), QSize(700, 500));
    ConfigGuard cg(&only);
    DialogChrome::releaseConfig(&only);

    QDialog dlg;
    dlg.resize(300, 200);
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("Inert"));
    QShowEvent se;
    QApplication::sendEvent(&dlg, &se);

    EXPECT_EQ(dlg.size(), QSize(300, 200))
        << "a released Config must not be read on show";
}

// INV-8 — ~MainWindow releases its own Config (ANTS-5036). Source scrape:
// a MainWindow cannot be built headless in this bundle.
TEST(DialogChromeAffordances, INV8_MainWindowDtorReleasesConfig) {
    const std::string body = ants_test::squashWhitespace(
        ants_test::slurpFunctionBody(SRC_MAINWINDOW_CPP_PATH,
                                     "MainWindow::~MainWindow()"));
    ASSERT_FALSE(body.empty()) << "~MainWindow not found";
    EXPECT_NE(body.find("DialogChrome::releaseConfig(&m_config);"),
              std::string::npos)
        << "~MainWindow must release the Config it registered";
}

// Why this exists: ANTS-5037 — ChromeGuard only saves on QEvent::Close.
// The chrome's title-bar close button and Esc call QDialog::reject(), which
// (per dialogs.md D3's own claim that reject() "routes through" close/done)
// is supposed to save too. INV-9/INV-10 lock reject()/accept(); INV-11 is a
// same-shape regression guard that the pre-existing close() path still
// saves after any fix.

// INV-9 — reject() (title-bar close button / Esc) persists the size (D3).
TEST(DialogChromeAffordances, INV9_RejectSavesSize) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // isolate config.json; restored on scope exit
    Config cfg;
    ConfigGuard cg(&cfg);

    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("RejectMe"));
    dlg.show();
    QApplication::processEvents();
    const QSize expected(650, 490);
    dlg.resize(expected);
    QApplication::processEvents();

    dlg.reject();
    QApplication::processEvents();

    const QSize saved = cfg.dialogSize(QStringLiteral("RejectMe"));
    EXPECT_EQ(saved, expected)
        << "reject() (chrome close button / Esc) must persist the chosen "
           "size under the key — saved "
        << saved.width() << "x" << saved.height() << ", expected "
        << expected.width() << "x" << expected.height();
}

// INV-10 — accept() persists the size (D3), same defect class as reject().
TEST(DialogChromeAffordances, INV10_AcceptSavesSize) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    Config cfg;
    ConfigGuard cg(&cfg);

    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("AcceptMe"));
    dlg.show();
    QApplication::processEvents();
    const QSize expected(700, 520);
    dlg.resize(expected);
    QApplication::processEvents();

    dlg.accept();
    QApplication::processEvents();

    const QSize saved = cfg.dialogSize(QStringLiteral("AcceptMe"));
    EXPECT_EQ(saved, expected)
        << "accept() must persist the chosen size under the key — saved "
        << saved.width() << "x" << saved.height() << ", expected "
        << expected.width() << "x" << expected.height();
}

// INV-11 — regression guard: an explicit close() (via QWidget::close(), not
// a synthetic QCloseEvent) must still persist the size after any INV-9/
// INV-10 fix. Same shape as INV-9/INV-10 (show, resize, process events,
// act, process events) so it exercises the same code path they do.
TEST(DialogChromeAffordances, INV11_CloseStillSavesSize) {
    ants_test::XdgGuard xdg;
    xdg.setTestMode(true);  // restored on scope exit
    Config cfg;
    ConfigGuard cg(&cfg);

    QDialog dlg;
    DialogChrome::install(&dlg, QString(), /*resizable=*/true,
                          QStringLiteral("CloseStill"));
    dlg.show();
    QApplication::processEvents();
    const QSize expected(660, 500);
    dlg.resize(expected);
    QApplication::processEvents();

    dlg.close();
    QApplication::processEvents();

    const QSize saved = cfg.dialogSize(QStringLiteral("CloseStill"));
    EXPECT_EQ(saved, expected)
        << "close() must still persist the chosen size under the key — "
           "saved "
        << saved.width() << "x" << saved.height() << ", expected "
        << expected.width() << "x" << expected.height();
}
