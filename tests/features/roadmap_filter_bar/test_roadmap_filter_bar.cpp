// ANTS-4412 — the roadmap dialog's collapsed filter chrome. Contract: spec.md
// beside this file.
//
// Sixteen always-visible checkboxes across two rows became two summarising
// buttons plus a reset. The checkboxes are the SAME widgets, re-parented into
// the buttons' popup menus, so the first row here is the one that matters: if
// a re-parent had turned into a rewrite, every rule ANTS-1106 / ANTS-1150 /
// ANTS-1238 lock would be quietly gone and their tests would not notice,
// because they scrape source strings rather than the live tree.

#include "config.h"
#include "roadmapdialog.h"

#include "../../_support/xdg_guard.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QString>
#include <QTemporaryDir>
#include <QToolButton>

#include <gtest/gtest.h>

namespace {

const char *kFixture =
    "# ROADMAP\n"
    "\n"
    "## Now\n"
    "\n"
    "- ✅ [ANTS-0001] **A shipped thing.**\n"
    "  Kind: fix.\n"
    "- 📋 [ANTS-0002] **A planned thing.**\n"
    "  Kind: implement.\n";

// One dialog over one throwaway roadmap. Config is real but XDG-guarded, so a
// filter toggle's persistence write cannot reach the user's own settings.
struct Harness {
    ants_test::XdgGuard guard;
    QTemporaryDir       dir;
    QString             path;

    Harness() {
        // setTestMode(false) is deliberate and load-bearing. Test mode makes
        // QStandardPaths ignore XDG_CONFIG_HOME in favour of one shared
        // per-binary location — so with it ON, a sibling test in this bundle
        // that persists a kind filter (ANTS-1150 writes on every toggle) is
        // restored into THIS dialog, and the defaults asserted below come back
        // as whatever ran before. Measured: with test mode on,
        // roadmap-filter-done arrived unchecked and two kind boxes checked.
        // It is set explicitly rather than left alone because a sibling may
        // have turned it on without a guard.
        guard.setTestMode(false);
        guard.setEnv("XDG_CONFIG_HOME", dir.path().toUtf8());
        path = dir.filePath(QStringLiteral("ROADMAP.md"));
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(kFixture);
    }
};

QToolButton *btn(RoadmapDialog &d, const char *name) {
    return d.findChild<QToolButton *>(QString::fromLatin1(name));
}
QCheckBox *box(RoadmapDialog &d, const char *name) {
    return d.findChild<QCheckBox *>(QString::fromLatin1(name));
}

}  // namespace

// The load-bearing row. A QMenu owned by the dialog is still in its object
// tree, so `findChild` reaches the boxes — and every existing test, signal and
// config write reaches them by exactly that route.
TEST(RoadmapFilterBar, CheckboxesSurviveTheReparent) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    for (const char *n : {"roadmap-filter-done", "roadmap-filter-planned",
                          "roadmap-filter-in-progress",
                          "roadmap-filter-considered",
                          "roadmap-filter-current"}) {
        QCheckBox *cb = box(dlg, n);
        ASSERT_NE(cb, nullptr) << n << " is no longer reachable by objectName";
        EXPECT_TRUE(cb->isChecked())
            << n << " must still default to on — the status set counts DOWN "
                    "from all-on and a flipped default silently hides items";
        EXPECT_FALSE(cb->accessibleName().isEmpty())
            << n << " lost its accessibleName in the re-parent";
    }

    // The kind boxes default OFF: an empty kind set means no narrowing
    // (ANTS-1106), the opposite convention to the status set above.
    for (const char *n : {"roadmap-filter-kind-implement",
                          "roadmap-filter-kind-fix",
                          "roadmap-filter-kind-doc"}) {
        QCheckBox *cb = box(dlg, n);
        ASSERT_NE(cb, nullptr) << n << " is no longer reachable by objectName";
        EXPECT_FALSE(cb->isChecked()) << n << " must still default to off";
    }
}

TEST(RoadmapFilterBar, SummariesTrackTheCheckboxes) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    QToolButton *status = btn(dlg, "roadmap-filter-status-button");
    QToolButton *kind   = btn(dlg, "roadmap-filter-kind-button");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(kind, nullptr);

    EXPECT_TRUE(status->text().contains(QStringLiteral("all")))
        << "at rest every status shows: " << status->text().toStdString();
    EXPECT_TRUE(kind->text().contains(QStringLiteral("all")))
        << "at rest every kind shows: " << kind->text().toStdString();

    box(dlg, "roadmap-filter-done")->setChecked(false);
    EXPECT_TRUE(status->text().contains(QStringLiteral("4 of 5")))
        << "one status off must read 4 of 5, not 'all': "
        << status->text().toStdString();

    box(dlg, "roadmap-filter-kind-fix")->setChecked(true);
    box(dlg, "roadmap-filter-kind-doc")->setChecked(true);
    EXPECT_TRUE(kind->text().contains(QStringLiteral("2 of ")))
        << "two kinds on must read 2 of N: " << kind->text().toStdString();
    // …and the two summaries do not derive from one another's shape: status
    // counts what is SHOWN, kind counts what is SELECTED.
    EXPECT_TRUE(status->text().contains(QStringLiteral("4 of 5")));
}

TEST(RoadmapFilterBar, ResetIsEnabledExactlyWhenNarrowed) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    QToolButton *reset = btn(dlg, "roadmap-filter-reset-button");
    ASSERT_NE(reset, nullptr);
    EXPECT_FALSE(reset->isEnabled())
        << "nothing is filtered at rest, so the control must read as inert";

    box(dlg, "roadmap-filter-done")->setChecked(false);
    EXPECT_TRUE(reset->isEnabled()) << "a hidden status is a narrowed list";
    box(dlg, "roadmap-filter-done")->setChecked(true);
    EXPECT_FALSE(reset->isEnabled());

    box(dlg, "roadmap-filter-kind-fix")->setChecked(true);
    EXPECT_TRUE(reset->isEnabled()) << "a selected kind is a narrowed list";
    box(dlg, "roadmap-filter-kind-fix")->setChecked(false);
    EXPECT_FALSE(reset->isEnabled());

    // Search counts. It narrows as hard as any checkbox and the reset clears
    // it, so a reset that looked inert while a search was active would be
    // lying about the only control most sessions actually use.
    QLineEdit *search =
        dlg.findChild<QLineEdit *>(QStringLiteral("roadmap-search-box"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("zzz"));
    EXPECT_TRUE(reset->isEnabled()) << "search text narrows the list too";
}

TEST(RoadmapFilterBar, ResetRestoresEveryControl) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    box(dlg, "roadmap-filter-done")->setChecked(false);
    box(dlg, "roadmap-filter-considered")->setChecked(false);
    box(dlg, "roadmap-filter-kind-fix")->setChecked(true);
    QLineEdit *search =
        dlg.findChild<QLineEdit *>(QStringLiteral("roadmap-search-box"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("something"));

    QToolButton *reset = btn(dlg, "roadmap-filter-reset-button");
    ASSERT_NE(reset, nullptr);
    ASSERT_TRUE(reset->isEnabled());
    reset->click();

    EXPECT_TRUE(box(dlg, "roadmap-filter-done")->isChecked());
    EXPECT_TRUE(box(dlg, "roadmap-filter-considered")->isChecked());
    EXPECT_FALSE(box(dlg, "roadmap-filter-kind-fix")->isChecked());
    EXPECT_TRUE(search->text().isEmpty());
    EXPECT_FALSE(reset->isEnabled())
        << "after a reset there is nothing left to reset";
}

TEST(RoadmapFilterBar, ControlsAreKeyboardReachable) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    // The row this replaced was sixteen tab stops. Collapsing it must not cost
    // keyboard access — the buttons take focus and the two filter ones open a
    // menu whose checkboxes are reachable with the arrow keys.
    for (const char *n : {"roadmap-filter-status-button",
                          "roadmap-filter-kind-button",
                          "roadmap-filter-reset-button"}) {
        QToolButton *b = btn(dlg, n);
        ASSERT_NE(b, nullptr) << n;
        EXPECT_EQ(b->focusPolicy(), Qt::StrongFocus)
            << n << " must be tab-reachable";
    }
    EXPECT_NE(btn(dlg, "roadmap-filter-status-button")->menu(), nullptr);
    EXPECT_NE(btn(dlg, "roadmap-filter-kind-button")->menu(), nullptr);
    EXPECT_EQ(btn(dlg, "roadmap-filter-reset-button")->menu(), nullptr)
        << "reset is an action, not a popup";
}
