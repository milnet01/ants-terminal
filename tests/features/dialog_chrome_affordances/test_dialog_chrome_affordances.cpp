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
