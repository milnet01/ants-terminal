// ANTS-1891 — Trust-signal v2 behavioural tests.
//
// Covers the 13 invariants in docs/specs/ANTS-1891.md:
//   INV-1   regret_count folds in under-route
//   INV-2   measured/inconclusive bucketing
//   INV-3   regret_rate denominator semantics
//   INV-4   sessionCleanlyEndedOnNewTier predicate
//   INV-5   weighted_avoided = opus_turns_avoided + 0.5*clean_end_count
//   INV-6   headline floor / "insufficient data" form
//   INV-7   headline full form (at/above floor)
//   INV-8   headline empty-records form (scope-phrased)
//   INV-9   envelope shape — 4 new fields always present on ON
//   INV-10  legacy record JSON round-trip (missing field → false)
//   INV-11  quiet-window inclusive boundary edge
//   INV-12  changed flag detects clean-end-only flip
//   INV-13  quiet-window settlement (additive to the three pre-existing)

#include <gtest/gtest.h>
#include <QJsonDocument>
#include <QJsonObject>
#include "modelswitchledger.h"

namespace L = ModelSwitchLedger;

namespace {

L::Record makeDowngrade(int turnsOnToTier,
                        bool override_, bool correction, bool underRoute,
                        bool cleanEnd, bool pending,
                        const QString &from = QStringLiteral("opus"),
                        const QString &to   = QStringLiteral("haiku"))
{
    L::Record r;
    r.ts          = QStringLiteral("2026-05-26T10:00:00Z");
    r.project     = QStringLiteral("/p");
    r.fromTier    = from;
    r.toTier      = to;
    r.scoreReason = QStringLiteral("mechanical");
    r.trigger     = QStringLiteral("auto");
    r.outcome.turnsOnToTier              = turnsOnToTier;
    r.outcome.userOverrideWithin5        = override_;
    r.outcome.correctionSignalWithin5    = correction;
    r.outcome.underRouteSignalWithin5    = underRoute;
    r.outcome.sessionCleanlyEndedOnNewTier = cleanEnd;
    r.outcome.pending                    = pending;
    return r;
}

L::StatsConfig enabledCfg(const QString &scope = QStringLiteral("project")) {
    L::StatsConfig cfg;
    cfg.switchEnabled = true;
    cfg.floorTier     = QStringLiteral("haiku");
    cfg.minDwellSec   = 90;
    cfg.scope         = scope;
    return cfg;
}

L::TranscriptTurn asstTurn(qint64 tsMs, const QString &modelId) {
    L::TranscriptTurn t;
    t.tsMs        = tsMs;
    t.isAssistant = true;
    t.modelId     = modelId;
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// INV-1 — regret_count includes under-route.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV1, RegretFoldsInUnderRoute)
{
    QList<L::Record> recs;
    // (a) under-route only → counts in regret (INV-1).
    recs << makeDowngrade(0, false, false, true,  false, /*pending=*/false);
    // (b) user-override only → counts in regret.
    recs << makeDowngrade(0, true,  false, false, false, false);
    // (c) both → counts in regret once.
    recs << makeDowngrade(0, true,  false, true,  false, false);

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_EQ(env.value(QStringLiteral("regret_count")).toInt(), 3);

    // Re-run with under-route stripped: only the user-override pair count.
    QList<L::Record> recs2;
    recs2 << makeDowngrade(0, false, false, false, false, false);  // inconclusive
    recs2 << makeDowngrade(0, true,  false, false, false, false);  // override only
    recs2 << makeDowngrade(0, true,  false, false, false, false);  // override only
    QJsonObject env2 = L::statsEnvelope(recs2, enabledCfg());
    EXPECT_EQ(env2.value(QStringLiteral("regret_count")).toInt(), 2);
}

// ---------------------------------------------------------------------------
// INV-2 — measured vs inconclusive bucketing.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV2, InconclusiveBucketExcludesFromMeasured)
{
    QList<L::Record> recs;
    // Two inconclusive (0-turn, no signal, no clean-end).
    recs << makeDowngrade(0, false, false, false, false, false);
    recs << makeDowngrade(0, false, false, false, false, false);

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_EQ(env.value(QStringLiteral("inconclusive_count")).toInt(), 2);
    EXPECT_EQ(env.value(QStringLiteral("measured_downgrades")).toInt(), 0);

    // Add a positive-turns record → measured = 1, inconclusive still = 2.
    recs << makeDowngrade(3, false, false, false, false, false);
    env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_EQ(env.value(QStringLiteral("measured_downgrades")).toInt(), 1);
    EXPECT_EQ(env.value(QStringLiteral("inconclusive_count")).toInt(), 2);
}

// ---------------------------------------------------------------------------
// INV-3 — regret_rate denominator excludes inconclusive.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV3, RegretRateDenominatorTightened)
{
    QList<L::Record> recs;
    // 1 user-override (0-turn) — measured + regret.
    recs << makeDowngrade(0, true,  false, false, false, false);
    // 4 inconclusive — neither measured nor regret.
    for (int i = 0; i < 4; ++i)
        recs << makeDowngrade(0, false, false, false, false, false);

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_EQ(env.value(QStringLiteral("measured_downgrades")).toInt(), 1);
    EXPECT_EQ(env.value(QStringLiteral("regret_count")).toInt(), 1);
    EXPECT_DOUBLE_EQ(env.value(QStringLiteral("regret_rate")).toDouble(), 100.0);
    // Pre-1891 would have been 1/5 = 20%.
}

// ---------------------------------------------------------------------------
// INV-4 — sessionCleanlyEndedOnNewTier predicate (5 cases).
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV4, SessionCleanlyEndedPredicate)
{
    const qint64 switchMs = 100'000;
    const qint64 nowMs    = switchMs + L::kCleanEndQuietMs + 1000;

    // (a) turnsOnToTier > 0, no signals → true.
    //     Use ≥kOutcomeWindowTurns (5) on-tier assistant turns so the
    //     fullWindowObserved settlement path fires (the simplest way to
    //     reach the "settled downgrade" precondition for INV-4 with turns).
    {
        QList<L::TranscriptTurn> turns;
        for (int i = 1; i <= L::kOutcomeWindowTurns; ++i)
            turns << asstTurn(switchMs + i * 1000,
                              QStringLiteral("claude-haiku-x"));
        auto fill = L::computeOutcome("opus", "haiku", switchMs, turns,
                                      {}, {}, {}, nowMs);
        EXPECT_FALSE(fill.outcome.pending);
        EXPECT_TRUE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
    // (b) turnsOnToTier=0, no signals, transcript silent past quiet window → true.
    {
        auto fill = L::computeOutcome("opus", "haiku", switchMs,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_TRUE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
    // (c) like (b) but with a user /model override → no clean-end.
    {
        L::TranscriptTurn ut;
        ut.tsMs     = switchMs + 100;
        ut.isUser   = true;
        ut.userText = QStringLiteral("/model opus");
        QList<L::TranscriptTurn> turns;
        turns << ut;
        auto fill = L::computeOutcome("opus", "haiku", switchMs, turns,
                                      {}, {}, {}, nowMs);
        EXPECT_TRUE(fill.outcome.userOverrideWithin5);
        EXPECT_FALSE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
    // (d) upgrade case → never set.
    {
        auto fill = L::computeOutcome("haiku", "opus", switchMs,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_FALSE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
    // (e) empty postSwitchTurns + nowMs ≥ switchMs + K → true.
    //     (Already covered in (b); re-asserted here for documentation.)
    {
        auto fill = L::computeOutcome("opus", "sonnet", switchMs,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_TRUE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
}

// ---------------------------------------------------------------------------
// INV-5 — weighted_avoided = opus_turns_avoided + 0.5 * clean_end_count.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV5, WeightedAvoidedFormula)
{
    QList<L::Record> recs;
    // 2 records with turnsOnToTier=3 AND clean-end=true.
    recs << makeDowngrade(3, false, false, false, true, false);
    recs << makeDowngrade(3, false, false, false, true, false);
    // 4 records with turnsOnToTier=0 AND clean-end=true.
    for (int i = 0; i < 4; ++i)
        recs << makeDowngrade(0, false, false, false, true, false);

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_EQ(env.value(QStringLiteral("opus_turns_avoided")).toInt(), 6);
    EXPECT_EQ(env.value(QStringLiteral("clean_end_count")).toInt(), 6);
    EXPECT_DOUBLE_EQ(env.value(QStringLiteral("weighted_avoided")).toDouble(),
                     9.0);
}

// ---------------------------------------------------------------------------
// INV-6 — headline "calibrating" form below floor (ANTS-1909 renamed the
// pre-floor phrase from "insufficient data" → "calibrating" and added the
// `dwell=Ns` parenthetical, so the floor parenthetical is no longer a
// closed `(floor=haiku)` but the multi-key form `(floor=haiku, dwell=…)`).
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV6, HeadlineFloorCalibrating)
{
    QList<L::Record> recs;
    for (int i = 0; i < 3; ++i)
        recs << makeDowngrade(2, false, false, false, false, false);
    // 3 measured (turnsOnToTier > 0), under the floor (10).

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    const QString headline = env.value(QStringLiteral("headline")).toString();
    const QString want = QStringLiteral("%1/%2 measured")
        .arg(3).arg(L::kHeadlineFloorMeasured);
    EXPECT_TRUE(headline.contains(QStringLiteral("floor=haiku")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("dwell=")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("calibrating")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(want)) << headline.toStdString();
}

// ---------------------------------------------------------------------------
// INV-7 — headline full form at/above floor.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV7, HeadlineFullForm)
{
    QList<L::Record> recs;
    // 5 with turns=1, no signals, no clean-end → measured=5, opus_avoided=5.
    for (int i = 0; i < 5; ++i)
        recs << makeDowngrade(1, false, false, false, false, false);
    // 4 with turns=0 + clean-end=true → measured=4, clean_end=4, no opus_avoided.
    for (int i = 0; i < 4; ++i)
        recs << makeDowngrade(0, false, false, false, true,  false);
    // 1 with turns=0 + user-override → measured=1, regret=1.
    recs << makeDowngrade(0, true, false, false, false, false);
    // Totals: measured=10 (== floor), regret=1, regret_rate=10.0%,
    //         opus_avoided=5, clean_end_count=4.

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_EQ(env.value(QStringLiteral("measured_downgrades")).toInt(),
              L::kHeadlineFloorMeasured);
    EXPECT_EQ(env.value(QStringLiteral("opus_turns_avoided")).toInt(), 5);
    EXPECT_EQ(env.value(QStringLiteral("clean_end_count")).toInt(), 4);
    EXPECT_EQ(env.value(QStringLiteral("regret_count")).toInt(), 1);

    const QString headline = env.value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("floor=haiku")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("dwell=")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("avoided 5 Opus turns")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("(+4 clean end)")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("1 regretted")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("regret 10.0%")))
        << headline.toStdString();
}

// ---------------------------------------------------------------------------
// INV-8 — headline empty form carries floor + scope phrase.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV8, HeadlineEmptyScope)
{
    QJsonObject envP = L::statsEnvelope({}, enabledCfg(QStringLiteral("project")));
    const QString headP = envP.value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headP.contains(QStringLiteral("no switches yet")));
    EXPECT_TRUE(headP.contains(QStringLiteral("in this project")));
    EXPECT_TRUE(headP.contains(QStringLiteral("floor=haiku")));
    EXPECT_TRUE(headP.contains(QStringLiteral("dwell=")));

    QJsonObject envG = L::statsEnvelope({}, enabledCfg(QStringLiteral("global")));
    const QString headG = envG.value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headG.contains(QStringLiteral("no switches yet")));
    EXPECT_TRUE(headG.contains(QStringLiteral("globally")));
    EXPECT_TRUE(headG.contains(QStringLiteral("floor=haiku")));
    EXPECT_TRUE(headG.contains(QStringLiteral("dwell=")));
}

// ---------------------------------------------------------------------------
// ANTS-1909 — near-miss enrichment: when the StatsConfig carries a 24 h
// near-miss count and dominant_blocker, both the "no switches yet" and
// "calibrating" headlines surface "N near-misses in 24 h blocked by <blocker>"
// so the trust signal reads as "evaluating but blocked" rather than
// "feature did nothing".
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_ANTS1909, HeadlineSurfacesNearMissWhenNoSwitches)
{
    L::StatsConfig cfg = enabledCfg();
    cfg.nearMissTotal24h        = 53;
    cfg.nearMissDominantBlocker = QStringLiteral("composer_not_empty");

    const QString headline =
        L::statsEnvelope({}, cfg).value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("no switches yet")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("53 near-misses")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("composer_not_empty")))
        << headline.toStdString();
}

TEST(ModelSwitchStatsV2_ANTS1909, HeadlineSurfacesNearMissWhileCalibrating)
{
    L::StatsConfig cfg = enabledCfg();
    cfg.nearMissTotal24h        = 12;
    cfg.nearMissDominantBlocker = QStringLiteral("target_equals_current");
    QList<L::Record> recs;
    for (int i = 0; i < 3; ++i)
        recs << makeDowngrade(2, false, false, false, false, false);

    const QString headline =
        L::statsEnvelope(recs, cfg).value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("calibrating")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("12 near-misses")))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("target_equals_current")))
        << headline.toStdString();
}

TEST(ModelSwitchStatsV2_ANTS1909, HeadlineOmitsNearMissWhenBlockerEmpty)
{
    // Empty dominantBlocker → no enrichment, no near-miss suffix.
    L::StatsConfig cfg = enabledCfg();
    cfg.nearMissTotal24h        = 7;
    cfg.nearMissDominantBlocker = QString();

    const QString headline =
        L::statsEnvelope({}, cfg).value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("no switches yet")));
    EXPECT_FALSE(headline.contains(QStringLiteral("near-miss")))
        << "near-miss suffix should be hidden when dominant_blocker is empty";
}

// ---------------------------------------------------------------------------
// INV-9 — envelope carries the 4 new fields on every ON response.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV9, EnvelopeFieldsPresent)
{
    QList<L::Record> recs;
    recs << makeDowngrade(1, false, false, false, false, false);

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    EXPECT_TRUE(env.contains(QStringLiteral("inconclusive_count")));
    EXPECT_TRUE(env.contains(QStringLiteral("clean_end_count")));
    EXPECT_TRUE(env.contains(QStringLiteral("weighted_avoided")));
    EXPECT_TRUE(env.contains(QStringLiteral("headline_floor")));
    EXPECT_EQ(env.value(QStringLiteral("headline_floor")).toInt(),
              L::kHeadlineFloorMeasured);
}

// ---------------------------------------------------------------------------
// ANTS-1934 — by_trigger split. Upgrades/downgrades are partitioned by
// `trigger` so a reader can isolate the autonomous switcher's behaviour.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_ANTS1934, ByTriggerSplit)
{
    QList<L::Record> recs;
    // 2 auto downgrades (opus→haiku) + 1 auto upgrade (haiku→opus).
    recs << makeDowngrade(1, false, false, false, false, false);   // auto down
    recs << makeDowngrade(1, false, false, false, false, false);   // auto down
    {
        L::Record up = makeDowngrade(0, false, false, false, false, false,
                                     QStringLiteral("haiku"),
                                     QStringLiteral("opus"));       // auto up
        recs << up;
    }
    // 1 MANUAL upgrade (sonnet→opus) — should land in the manual bucket only.
    {
        L::Record m = makeDowngrade(0, false, false, false, false, false,
                                    QStringLiteral("sonnet"),
                                    QStringLiteral("opus"));
        m.trigger = QStringLiteral("chip");   // non-"auto" → manual
        recs << m;
    }

    QJsonObject env = L::statsEnvelope(recs, enabledCfg());
    // Flat totals (back-compat) blend both triggers.
    EXPECT_EQ(env.value(QStringLiteral("upgrades")).toInt(), 2);
    EXPECT_EQ(env.value(QStringLiteral("downgrades")).toInt(), 2);

    const QJsonObject bt = env.value(QStringLiteral("by_trigger")).toObject();
    const QJsonObject a  = bt.value(QStringLiteral("auto")).toObject();
    const QJsonObject m  = bt.value(QStringLiteral("manual")).toObject();
    EXPECT_EQ(a.value(QStringLiteral("upgrades")).toInt(),   1);
    EXPECT_EQ(a.value(QStringLiteral("downgrades")).toInt(), 2);
    EXPECT_EQ(m.value(QStringLiteral("upgrades")).toInt(),   1);
    EXPECT_EQ(m.value(QStringLiteral("downgrades")).toInt(), 0);
}

// ---------------------------------------------------------------------------
// INV-10 — legacy record JSON round-trip (missing field → false).
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV10, LegacyRecordRoundTrip)
{
    // Hand-craft pre-1891 JSON with no session_cleanly_ended_on_new_tier.
    QJsonObject oc;
    oc[QStringLiteral("turns_on_to_tier")]              = 0;
    oc[QStringLiteral("user_override_within_5_turns")]  = false;
    oc[QStringLiteral("correction_signal_within_5_turns")] = false;
    oc[QStringLiteral("under_route_signal_within_5_turns")] = true;
    oc[QStringLiteral("pending")] = false;

    QJsonObject obj;
    obj[QStringLiteral("ts")]        = QStringLiteral("2026-05-25T10:00:00Z");
    obj[QStringLiteral("project")]   = QStringLiteral("/p");
    obj[QStringLiteral("from_tier")] = QStringLiteral("opus");
    obj[QStringLiteral("to_tier")]   = QStringLiteral("haiku");
    obj[QStringLiteral("outcome")]   = oc;

    L::Record r = L::fromJson(obj);
    EXPECT_FALSE(r.outcome.sessionCleanlyEndedOnNewTier);  // default
    EXPECT_TRUE(r.outcome.underRouteSignalWithin5);        // round-tripped
    EXPECT_FALSE(r.outcome.pending);

    // Re-serialise and round-trip again — every other field stable.
    QJsonObject obj2 = L::toJson(r);
    L::Record r2 = L::fromJson(obj2);
    EXPECT_EQ(r.outcome.turnsOnToTier, r2.outcome.turnsOnToTier);
    EXPECT_EQ(r.outcome.userOverrideWithin5, r2.outcome.userOverrideWithin5);
    EXPECT_EQ(r.outcome.correctionSignalWithin5,
              r2.outcome.correctionSignalWithin5);
    EXPECT_EQ(r.outcome.underRouteSignalWithin5,
              r2.outcome.underRouteSignalWithin5);
    EXPECT_EQ(r.outcome.sessionCleanlyEndedOnNewTier,
              r2.outcome.sessionCleanlyEndedOnNewTier);
    EXPECT_EQ(r.outcome.pending, r2.outcome.pending);
}

// ---------------------------------------------------------------------------
// INV-11 — quiet-window inclusive boundary edge.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV11, QuietWindowBoundary)
{
    const qint64 switchMs = 100'000;
    const qint64 nowMs    = switchMs + L::kCleanEndQuietMs + 1000;

    // Last assistant turn 1 ms past the inclusive boundary (delta ≥ K → true).
    {
        QList<L::TranscriptTurn> turns;
        turns << asstTurn(nowMs - L::kCleanEndQuietMs - 1,
                          QStringLiteral("claude-haiku-x"));
        auto fill = L::computeOutcome("opus", "haiku", switchMs, turns,
                                      {}, {}, {}, nowMs);
        EXPECT_TRUE(fill.outcome.sessionCleanlyEndedOnNewTier);
        EXPECT_FALSE(fill.outcome.pending);
    }
    // Last assistant turn 1 s short of the boundary (delta < K → false).
    {
        QList<L::TranscriptTurn> turns;
        turns << asstTurn(nowMs - L::kCleanEndQuietMs + 1000,
                          QStringLiteral("claude-haiku-x"));
        auto fill = L::computeOutcome("opus", "haiku", switchMs, turns,
                                      {}, {}, {}, nowMs);
        EXPECT_FALSE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
}

// ---------------------------------------------------------------------------
// INV-12 — changed flag detects clean-end-only flip.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV12, ChangedFlagOnCleanEndFlip)
{
    const qint64 switchMs = 100'000;
    const qint64 nowMs    = switchMs + L::kCleanEndQuietMs + 1000;

    // inputBefore.sessionCleanlyEnded=false, conditions flip it to true.
    L::Outcome before;
    before.sessionCleanlyEndedOnNewTier = false;
    auto fill = L::computeOutcome("opus", "haiku", switchMs,
                                  QList<L::TranscriptTurn>{}, {}, {}, before,
                                  nowMs);
    EXPECT_TRUE(fill.outcome.sessionCleanlyEndedOnNewTier);
    EXPECT_TRUE(fill.changed);

    // Pre-stamp clean-end=true; same conditions → no change in that field
    // (other fields still control changed; if all match, changed=false).
    before.sessionCleanlyEndedOnNewTier = true;
    before.pending = false;
    auto fill2 = L::computeOutcome("opus", "haiku", switchMs,
                                   QList<L::TranscriptTurn>{}, {}, {}, before,
                                   nowMs);
    EXPECT_TRUE(fill2.outcome.sessionCleanlyEndedOnNewTier);
    // changed=false in the clean-end dimension; the pending flip might still
    // not change since we set before.pending=false. Verify no spurious change.
    EXPECT_FALSE(fill2.changed);
}

// ---------------------------------------------------------------------------
// INV-13 — quiet-window settlement, additive to the three pre-existing.
// ---------------------------------------------------------------------------
TEST(ModelSwitchStatsV2_INV13, QuietWindowSettlement)
{
    const qint64 switchMs = 100'000;

    // (a) Just past quiet boundary → pending=false via the new clause.
    {
        const qint64 nowMs = switchMs + L::kCleanEndQuietMs + 1000;
        auto fill = L::computeOutcome("opus", "haiku", switchMs,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_FALSE(fill.outcome.pending);
    }
    // (b) Just short of boundary → still pending.
    {
        const qint64 nowMs = switchMs + L::kCleanEndQuietMs - 1000;
        auto fill = L::computeOutcome("opus", "haiku", switchMs,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_TRUE(fill.outcome.pending);
    }
    // (c) Upgrade variant — the new clause must NOT fire for upgrades; the
    //     three pre-existing clauses govern. No turns + no next-auto + no
    //     divergence → still pending regardless of nowMs.
    {
        const qint64 nowMs = switchMs + L::kCleanEndQuietMs + 10'000;
        auto fill = L::computeOutcome("haiku", "opus", switchMs,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_TRUE(fill.outcome.pending);
    }
    // (d) Malformed switchTsMs (0) → never settles via the new clause.
    {
        const qint64 nowMs = L::kCleanEndQuietMs * 2;
        auto fill = L::computeOutcome("opus", "haiku", /*switchTsMs=*/0,
                                      QList<L::TranscriptTurn>{}, {}, {}, {},
                                      nowMs);
        EXPECT_TRUE(fill.outcome.pending);
        EXPECT_FALSE(fill.outcome.sessionCleanlyEndedOnNewTier);
    }
}
