// Feature test for the ColoredTabBar storage contract (spec.md §1-5).
// Exercises the round-trip behaviour of setTabColor / tabColor across
// insert, move (drag-reorder), and remove — the three operations
// where an index-keyed MainWindow-side map would go stale but the
// QTabBar::tabData backing survives correctly. §5 covers cross-session
// persistence (colour survives app restart) via Config's tab_groups
// key, keyed by per-tab UUID.
//
// Needs QApplication because QTabBar internally instantiates widgets.
// Runs under the "offscreen" QPA platform so no display server is
// required on CI. Uses QStandardPaths::setTestModeEnabled so Config's
// writes land in ~/.qttest/ rather than the real user config.

#include "coloredtabbar.h"
#include "config.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>
#include <QWidget>

#include <cstdio>

namespace {

const QColor kRed(0xF3, 0x8B, 0xA8);
const QColor kGreen(0xA6, 0xE3, 0xA1);
const QColor kBlue(0x89, 0xB4, 0xFA);

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL [%s]: %s (line %d)\n",                  \
                         __FUNCTION__, msg, __LINE__);                         \
            ++failures;                                                        \
        }                                                                     \
    } while (0)

// spec §4: setTabColor / tabColor round-trip.
int runRoundTrip() {
    int failures = 0;
    ColoredTabBar bar;
    bar.addTab("A");
    bar.addTab("B");
    bar.addTab("C");

    CHECK(!bar.tabColor(0).isValid(), "new tab starts with no colour");
    CHECK(!bar.tabColor(1).isValid(), "new tab starts with no colour");

    bar.setTabColor(0, kRed);
    bar.setTabColor(1, kGreen);
    CHECK(bar.tabColor(0) == kRed,   "tab 0 colour round-trips");
    CHECK(bar.tabColor(1) == kGreen, "tab 1 colour round-trips");
    CHECK(!bar.tabColor(2).isValid(), "tab 2 remains uncoloured");

    // Clear via invalid colour.
    bar.setTabColor(0, QColor());
    CHECK(!bar.tabColor(0).isValid(), "invalid QColor clears the slot");
    CHECK(bar.tabColor(1) == kGreen,  "clearing tab 0 doesn't touch tab 1");

    // Out-of-range is a no-op, not a crash.
    bar.setTabColor(99, kBlue);
    bar.setTabColor(-1, kBlue);
    CHECK(!bar.tabColor(99).isValid(), "out-of-range get returns invalid");
    CHECK(!bar.tabColor(-1).isValid(), "out-of-range get returns invalid");

    return failures;
}

// spec §2: colour follows the tab through drag-reorder (moveTab).
int runReorderSurvives() {
    int failures = 0;
    ColoredTabBar bar;
    bar.addTab("A");
    bar.addTab("B");
    bar.addTab("C");
    bar.setTabColor(0, kRed);
    bar.setTabColor(1, kGreen);
    bar.setTabColor(2, kBlue);

    // Move tab 0 to position 2 (simulates user dragging A past B and C).
    // QTabBar handles its per-tab data automatically; the colour should
    // end up at the new position, not at the old one.
    bar.moveTab(0, 2);

    CHECK(bar.tabText(0) == "B",     "post-move: position 0 holds B");
    CHECK(bar.tabText(1) == "C",     "post-move: position 1 holds C");
    CHECK(bar.tabText(2) == "A",     "post-move: position 2 holds A");
    CHECK(bar.tabColor(0) == kGreen, "colour follows B to position 0");
    CHECK(bar.tabColor(1) == kBlue,  "colour follows C to position 1");
    CHECK(bar.tabColor(2) == kRed,   "colour follows A to position 2");

    return failures;
}

// spec §3: colour metadata auto-drops on removeTab.
int runRemoveCleansUp() {
    int failures = 0;
    ColoredTabBar bar;
    bar.addTab("A");
    bar.addTab("B");
    bar.addTab("C");
    bar.setTabColor(0, kRed);
    bar.setTabColor(1, kGreen);
    bar.setTabColor(2, kBlue);

    bar.removeTab(1);  // close the middle tab

    CHECK(bar.count() == 2,          "count decremented");
    CHECK(bar.tabText(0) == "A",     "remaining: position 0 is A");
    CHECK(bar.tabText(1) == "C",     "remaining: position 1 is C");
    CHECK(bar.tabColor(0) == kRed,   "A keeps its colour");
    CHECK(bar.tabColor(1) == kBlue,  "C shifts down with its colour");

    // Insert a new tab at the former B slot — must NOT inherit B's
    // stale colour (proves the old index-keyed map would leak here).
    bar.insertTab(1, "B'");
    CHECK(bar.count() == 3,           "insert grew count");
    CHECK(bar.tabText(1) == "B'",     "new tab at position 1");
    CHECK(!bar.tabColor(1).isValid(), "new tab starts uncoloured "
                                      "(no leak from removed B)");
    CHECK(bar.tabColor(0) == kRed,    "A's colour still intact");
    CHECK(bar.tabColor(2) == kBlue,   "C's colour still intact");

    return failures;
}

// spec §5: colour persists across "app restart" via Config::tabGroups().
//
// The user-visible bug the round-trip guards: "I set a tab's colour,
// closed ants-terminal, re-opened, and the colour was gone." Storage
// was previously only in QTabBar::tabData (alive for the process
// lifetime, zero-initialised on start). The fix keys a JSON map by the
// tab's UUID (m_tabSessionIds) and persists it under the `tab_groups`
// config key, which this test drives directly without needing a live
// MainWindow.
int runPersistenceRoundTrip() {
    int failures = 0;

    // A fresh UUID per run so we don't collide with any leftover state
    // from previous test runs in the same ~/.qttest/ directory.
    const QString tabIdA = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString tabIdB = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // --- "Session 1": set colours, persist them via Config ---
    {
        Config cfg;
        QJsonObject groups = cfg.tabGroups();
        groups[tabIdA] = kRed.name(QColor::HexArgb);
        groups[tabIdB] = kGreen.name(QColor::HexArgb);
        cfg.setTabGroups(groups);
    }

    // --- "Session 2": brand-new Config instance reads from disk ---
    {
        Config cfg2;
        const QJsonObject groups = cfg2.tabGroups();
        CHECK(groups.contains(tabIdA),
              "UUID A present after Config reload");
        CHECK(groups.contains(tabIdB),
              "UUID B present after Config reload");

        const QColor restoredA(groups.value(tabIdA).toString());
        const QColor restoredB(groups.value(tabIdB).toString());
        CHECK(restoredA == kRed,   "tab A colour round-trips through Config");
        CHECK(restoredB == kGreen, "tab B colour round-trips through Config");

        // Drive the restore path: mirror what
        // MainWindow::applyPersistedTabColor does — read hex, parse,
        // apply to the bar. Proves the persistence format is directly
        // consumable by the in-memory tab bar.
        ColoredTabBar bar;
        bar.addTab("A");
        bar.addTab("B");
        bar.setTabColor(0, QColor(groups.value(tabIdA).toString()));
        bar.setTabColor(1, QColor(groups.value(tabIdB).toString()));
        CHECK(bar.tabColor(0) == kRed,
              "bar receives persisted A colour after reload");
        CHECK(bar.tabColor(1) == kGreen,
              "bar receives persisted B colour after reload");
    }

    // --- Clearing a colour removes the entry (no orphan accumulation) ---
    {
        Config cfg3;
        QJsonObject groups = cfg3.tabGroups();
        groups.remove(tabIdA);
        cfg3.setTabGroups(groups);
    }
    {
        Config cfg4;
        const QJsonObject groups = cfg4.tabGroups();
        CHECK(!groups.contains(tabIdA),
              "cleared UUID A is absent, not empty-string");
        CHECK(groups.contains(tabIdB),
              "clearing A does not touch B");
    }

    // Cleanup so ~/.qttest state doesn't grow unbounded across runs.
    {
        Config cfg5;
        QJsonObject groups = cfg5.tabGroups();
        groups.remove(tabIdB);
        cfg5.setTabGroups(groups);
    }

    return failures;
}

}  // namespace

int main(int argc, char **argv) {
    // Force offscreen QPA so the test runs on CI without a display.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Redirect Config's QStandardPaths reads/writes to ~/.qttest/ so
    // the real user config is never touched. Must be called BEFORE any
    // Config instance is constructed (Config's ctor calls load()).
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    int failures = 0;
    failures += runRoundTrip();
    failures += runReorderSurvives();
    failures += runRemoveCleansUp();
    failures += runPersistenceRoundTrip();

    if (failures == 0) {
        std::printf("tab_color: round-trip + reorder + remove + persist pass\n");
        return 0;
    }
    std::fprintf(stderr, "tab_color: %d failure(s)\n", failures);
    return 1;
}
