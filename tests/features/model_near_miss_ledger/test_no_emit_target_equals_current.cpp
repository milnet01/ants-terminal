// ANTS-1946 INV-14 — maybeEmitNearMiss must NOT emit when blockedBy
// contains "target_equals_current" (clamped recommendation == current tier;
// no switch was ever possible). The throttle state must also remain
// untouched so a subsequent real near-miss can still fire.
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
    g.composerEmpty = true;
    g.focusedState  = ClaudeState::Idle;
    return g;
}

}  // namespace

// Sole blocker is target_equals_current — zero records.
TEST(ModelNearMissLedger, Inv14TargetEqualsCurrentSoleBlockerNoEmit) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/proj");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    ctrl.maybeEmitNearMiss(
        blockedDecisionWith({QStringLiteral("target_equals_current")}),
        sampleGate(), proj, 0, path);

    EXPECT_EQ(NM::readRecords(path).size(), 0)
        << "target_equals_current sole-blocker must not emit a near-miss";
}

// target_equals_current co-fires with ticks_target_stable_insufficient
// (the historically dominant co-fire pattern) — still zero records.
TEST(ModelNearMissLedger, Inv14TargetEqualsCurrentCoFireNoEmit) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/proj");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    ctrl.maybeEmitNearMiss(
        blockedDecisionWith({QStringLiteral("target_equals_current"),
                             QStringLiteral("ticks_target_stable_insufficient")}),
        sampleGate(), proj, 0, path);

    EXPECT_EQ(NM::readRecords(path).size(), 0)
        << "target_equals_current co-fire must not emit a near-miss";
}

// After suppressed target_equals_current decisions the throttle maps must
// remain empty so a real near-miss can still fire without a 5 s wait.
TEST(ModelNearMissLedger, Inv14SuppressedDecisionDoesNotPoisonThrottle) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/proj");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    // Two suppressed target_equals_current ticks at t=0 and t=1 s.
    ctrl.maybeEmitNearMiss(
        blockedDecisionWith({QStringLiteral("target_equals_current")}),
        sampleGate(), proj, 0, path);
    ctrl.maybeEmitNearMiss(
        blockedDecisionWith({QStringLiteral("target_equals_current"),
                             QStringLiteral("ticks_target_stable_insufficient")}),
        sampleGate(), proj, 1000, path);

    ASSERT_EQ(NM::readRecords(path).size(), 0);

    // Real near-miss at t=2 s — no floor has been started, so it emits.
    ctrl.maybeEmitNearMiss(
        blockedDecisionWith({QStringLiteral("composer_not_empty")}),
        sampleGate(), proj, 2000, path);

    EXPECT_EQ(NM::readRecords(path).size(), 1)
        << "real near-miss after suppressed decisions must emit (throttle was not poisoned)";
}
