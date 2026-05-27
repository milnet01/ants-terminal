// ANTS-1894 INV-6 — maybeEmitNearMiss skips emission inside the 5 s
// per-project floor; skipped attempts do NOT update the throttle state
// (so a transition past the floor still fires).
#include <gtest/gtest.h>
#include <QStatusBar>
#include <QTemporaryDir>
#include "claudestatuswidgets.h"
#include "modelautoswitch.h"
#include "modelnearmissledger.h"

namespace MAS = ModelAutoSwitch;
namespace NM  = ModelNearMissLedger;

namespace {

MAS::Decision blockedDecisionWith(const QStringList &blockers) {
    MAS::Decision d;
    d.act       = false;
    d.blockedBy = blockers;
    return d;
}

MAS::Gate sampleGate() {
    MAS::Gate g;
    g.composerEmpty   = false;
    g.focusedState    = ClaudeState::Thinking;
    return g;
}

}  // namespace

TEST(ModelNearMissLedger, Inv6FiveSecondFloorSkipsRapidTransitions) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/projA");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    // Tick 1 @ 0 ms: sig [composer] — emits.
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty")}),
                           sampleGate(), proj, 0, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Tick 2 @ +2 s: sig [composer, dwell] — DIFFERENT signature but inside
    // the 5 s floor. Should NOT emit.
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty"),
                                                QStringLiteral("dwell_time_insufficient")}),
                           sampleGate(), proj, 2000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Tick 3 @ +4 s: sig [override] — yet another transition, still inside floor.
    // Should NOT emit. Crucially the throttle state was NOT updated by the
    // dropped tick-2, so the floor is still measured from tick 1 (@ 0 ms).
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("override_cooldown_active")}),
                           sampleGate(), proj, 4000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Tick 4 @ +6 s: sig [override] — same as tick 3, but now past the
    // 5 s floor measured from tick 1. Should emit (the transition that was
    // skipped at tick 3 now lands at tick 4).
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("override_cooldown_active")}),
                           sampleGate(), proj, 6000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 2);
}

TEST(ModelNearMissLedger, Inv6PerProjectFloorIsIndependent) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    // Project A @ 0 ms: emit.
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty")}),
                           sampleGate(), QStringLiteral("/projA"), 0, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Project B @ 1 s (well inside projA's floor) — different project, so the
    // throttle is independent. Should emit.
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty")}),
                           sampleGate(), QStringLiteral("/projB"), 1000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 2);
}
