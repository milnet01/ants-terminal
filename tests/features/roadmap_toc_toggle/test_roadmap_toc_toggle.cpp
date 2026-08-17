// ANTS-4415 — the roadmap dialog's contents pane collapses, remembers, and
// costs nothing while hidden. Contract: spec.md beside this file.
//
// The last invariant is the one worth the test. Hiding a widget and leaving
// the code that fills it running looks identical from outside — same pixels,
// same behaviour, just slower — so nothing but an assertion on the work itself
// can tell the two apart.

#include "config.h"
#include "roadmapdialog.h"

#include "../../_support/xdg_guard.h"

#include <QCheckBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QSplitter>
#include <QString>
#include <QTemporaryDir>
#include <QToolButton>

#include <gtest/gtest.h>

namespace {

const char *kFixture =
    "# ROADMAP\n"
    "\n"
    "## Alpha section\n"
    "\n"
    "- ✅ [ANTS-0001] **A shipped thing.**\n"
    "  Kind: fix.\n"
    "\n"
    "## Beta section\n"
    "\n"
    "- 📋 [ANTS-0002] **A planned thing.**\n"
    "  Kind: implement.\n"
    "\n"
    "## Gamma section\n"
    "\n"
    "- 📋 [ANTS-0003] **Another planned thing.**\n"
    "  Kind: doc.\n";

struct Harness {
    ants_test::XdgGuard guard;
    QTemporaryDir       dir;
    QString             path;

    Harness() {
        // Deliberate and load-bearing — see spec.md § Test shape. Test mode
        // makes QStandardPaths ignore XDG_CONFIG_HOME, so a sibling test's
        // persisted Config arrives as this dialog's "defaults".
        guard.setTestMode(false);
        guard.setEnv("XDG_CONFIG_HOME", dir.path().toUtf8());
        path = dir.filePath(QStringLiteral("ROADMAP.md"));
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(kFixture);
    }
};

QToolButton *toggleOf(RoadmapDialog &d) {
    return d.findChild<QToolButton *>(
        QStringLiteral("roadmap-toc-toggle-button"));
}
QListWidget *tocOf(RoadmapDialog &d) {
    return d.findChild<QListWidget *>(QStringLiteral("roadmap-toc"));
}

// rebuild() is a private slot, so moc has registered it and a test can drive it
// by name. Invoking it beats waiting on scheduleRebuild()'s debounce timer: the
// question here is what one render does, not when it happens.
void forceRebuild(RoadmapDialog &d) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&d, "rebuild", Qt::DirectConnection))
        << "rebuild() is no longer an invokable slot — this test drives it by "
           "name and cannot see the render otherwise";
}

}  // namespace

TEST(RoadmapTocToggle, ToggleExistsAndIsReachable) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    QToolButton *btn = toggleOf(dlg);
    ASSERT_NE(btn, nullptr) << "the contents toggle is missing entirely";
    EXPECT_TRUE(btn->isCheckable())
        << "a non-checkable button cannot show which state the pane is in";
    EXPECT_EQ(btn->focusPolicy(), Qt::StrongFocus)
        << "the toggle must be tab-reachable";
    EXPECT_FALSE(btn->accessibleName().isEmpty());
    EXPECT_EQ(btn->menu(), nullptr)
        << "this is a toggle, not one of ANTS-4412's popup filter buttons";
}

TEST(RoadmapTocToggle, CheckedStateTracksPaneVisibility) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    dlg.show();

    QToolButton *btn = toggleOf(dlg);
    QListWidget *toc = tocOf(dlg);
    ASSERT_NE(btn, nullptr);
    ASSERT_NE(toc, nullptr);

    EXPECT_TRUE(btn->isChecked()) << "the pane defaults to shown";
    EXPECT_TRUE(toc->isVisible());

    btn->setChecked(false);
    EXPECT_FALSE(toc->isVisible()) << "unchecking must hide the pane";

    btn->setChecked(true);
    EXPECT_TRUE(toc->isVisible()) << "re-checking must bring it back";
}

// INV-4 — the one that cannot be seen from outside.
TEST(RoadmapTocToggle, HiddenPaneCostsNothingAndRefillsOnReturn) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    dlg.show();

    QToolButton *btn = toggleOf(dlg);
    QListWidget *toc = tocOf(dlg);
    ASSERT_NE(btn, nullptr);
    ASSERT_NE(toc, nullptr);

    const int populated = toc->count();
    ASSERT_GT(populated, 0)
        << "fixture has three headings; the visible pane should list them";

    btn->setChecked(false);
    toc->clear();   // prove the render does not refill it while hidden

    // Drive a real render the way the user would: change a filter. rebuild()
    // runs, and with the pane hidden it must not touch the list.
    if (auto *cb = dlg.findChild<QCheckBox *>(
            QStringLiteral("roadmap-filter-done")))
        cb->setChecked(false);
    forceRebuild(dlg);
    EXPECT_EQ(toc->count(), 0)
        << "a hidden pane was repopulated — the render is doing TOC work with "
           "no observer, which is the whole cost this toggle removes";

    // …and coming back must refill it, precisely BECAUSE the render skipped it.
    btn->setChecked(true);
    forceRebuild(dlg);
    EXPECT_GT(toc->count(), 0)
        << "the pane came back empty: showing it must trigger the render that "
           "hiding it suppressed";
}

TEST(RoadmapTocToggle, ChoicePersistsThroughConfig) {
    Harness h;
    Config cfg;

    EXPECT_TRUE(cfg.roadmapTocVisible())
        << "an unset key must read as shown — nobody has chosen yet";

    {
        RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
        QToolButton *btn = toggleOf(dlg);
        ASSERT_NE(btn, nullptr);
        btn->setChecked(false);
    }
    EXPECT_FALSE(cfg.roadmapTocVisible())
        << "hiding the pane must be written back to Config";

    // A second dialog over the same Config opens hidden.
    RoadmapDialog reopened(h.path, QStringLiteral("light"), nullptr, &cfg);
    QListWidget *toc = tocOf(reopened);
    QToolButton *btn = toggleOf(reopened);
    ASSERT_NE(toc, nullptr);
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isChecked())
        << "the reopened dialog forgot the user's choice";
}

TEST(RoadmapTocToggle, SplitterCanCollapseThePaneByDrag) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    auto *sp = dlg.findChild<QSplitter *>(QStringLiteral("roadmap-splitter"));
    ASSERT_NE(sp, nullptr);
    EXPECT_TRUE(sp->isCollapsible(0))
        << "m_toc has a 180px minimum width, so without setCollapsible(0) a "
           "drag stops short of zero and the handle contradicts the button";
}
