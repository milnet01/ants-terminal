// ANTS-1894 INV-5 — maybeEmitNearMiss writes iff the post-sort blocked_by
// signature differs from the last emitted signature for this project.
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
    d.act             = false;
    d.blockedBy       = blockers;
    d.currentTier     = ModelRecommender::Tier::Opus;
    d.recommendedTier = ModelRecommender::Tier::Haiku;
    return d;
}

MAS::Gate sampleGate() {
    MAS::Gate g;
    g.composerEmpty   = false;
    g.focusedState    = ClaudeState::Thinking;
    g.ticksTargetStable = 1;
    g.msSinceLastSwitch = 45000;
    return g;
}

}  // namespace

TEST(ModelNearMissLedger, Inv5EmitOnSignatureChange) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/projA");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    // Tick 1: signature [composer_not_empty] @ now=0 — first emit always lands.
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty")}),
                           sampleGate(), proj, 0, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Tick 2: same signature @ +6 s (past floor) — should NOT emit (INV-5).
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty")}),
                           sampleGate(), proj, 6000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Tick 3: different signature @ +12 s — should emit (INV-5 + INV-6 both ok).
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty"),
                                                QStringLiteral("dwell_time_insufficient")}),
                           sampleGate(), proj, 12000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 2);
}

TEST(ModelNearMissLedger, Inv5SignatureIsOrderIndependent) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/projB");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    // Tick 1: [composer, dwell] @ 0 — emit.
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("composer_not_empty"),
                                                QStringLiteral("dwell_time_insufficient")}),
                           sampleGate(), proj, 0, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);

    // Tick 2: same blockers, reversed order @ +6 s — should NOT emit (sort-equal).
    ctrl.maybeEmitNearMiss(blockedDecisionWith({QStringLiteral("dwell_time_insufficient"),
                                                QStringLiteral("composer_not_empty")}),
                           sampleGate(), proj, 6000, path);
    EXPECT_EQ(NM::readRecords(path).size(), 1);
}

TEST(ModelNearMissLedger, Inv5RecordPreservesEvaluationOrder) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    const QString proj = QStringLiteral("/projC");

    QStatusBar bar;
    ClaudeStatusBarController ctrl(&bar, nullptr);

    // Emit with a deliberately-non-sorted blockedBy (this is what decide()
    // returns — evaluation order). The recorded blocked_by must match.
    const QStringList eval = {QStringLiteral("composer_not_empty"),
                              QStringLiteral("target_equals_current"),
                              QStringLiteral("override_cooldown_active")};
    ctrl.maybeEmitNearMiss(blockedDecisionWith(eval), sampleGate(), proj, 0, path);
    const QList<NM::Record> recs = NM::readRecords(path);
    ASSERT_EQ(recs.size(), 1);
    EXPECT_EQ(recs[0].blockedBy, eval);
}
