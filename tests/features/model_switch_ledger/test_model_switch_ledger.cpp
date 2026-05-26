// Feature-conformance test for ANTS-1735 — effectiveness ledger.
// See tests/features/model_switch_ledger/spec.md and docs/specs/ANTS-1735.md
// §2.5. Covers INV-10 (append + byte-cap eviction with pending-pinning),
// INV-11 (override detection), INV-12 (under-route detection), the correction
// soft signal, and the outcome fill-in lifecycle.

#include <gtest/gtest.h>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "modelswitchledger.h"

namespace L = ModelSwitchLedger;

namespace {

L::Record makeRecord(const QString &from, const QString &to, bool pending,
                     const QString &session = QStringLiteral("s1")) {
    L::Record r;
    r.ts          = QStringLiteral("2026-05-25T14:02:11Z");
    r.sessionId   = session;
    r.project     = QStringLiteral("/mnt/proj");
    r.fromTier    = from;
    r.toTier      = to;
    r.scoreReason = QStringLiteral("mechanical");
    r.trigger     = QStringLiteral("auto");
    r.outcome.pending = pending;
    return r;
}

int fileLineCount(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray all = f.readAll();
    int n = 0;
    for (const QByteArray &ln : all.split('\n'))
        if (!ln.trimmed().isEmpty()) ++n;
    return n;
}

}  // namespace

// Round-trip: append one record, read it back identical.
TEST(ModelSwitchLedger, AppendAndReadRoundTrip) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus", "haiku", true)));
    const QList<L::Record> recs = L::readRecords(path);
    ASSERT_EQ(recs.size(), 1);
    EXPECT_EQ(recs[0].fromTier, QStringLiteral("opus"));
    EXPECT_EQ(recs[0].toTier, QStringLiteral("haiku"));
    EXPECT_EQ(recs[0].scoreReason, QStringLiteral("mechanical"));
    EXPECT_EQ(recs[0].trigger, QStringLiteral("auto"));
    EXPECT_TRUE(recs[0].outcome.pending);
}

// toJson/fromJson round-trip preserves every field.
TEST(ModelSwitchLedger, JsonRoundTrip) {
    L::Record r = makeRecord("sonnet", "opus", false);
    r.outcome.turnsOnToTier = 6;
    r.outcome.userOverrideWithin5 = true;
    r.outcome.underRouteSignalWithin5 = true;
    const L::Record back = L::fromJson(L::toJson(r));
    EXPECT_EQ(back.fromTier, r.fromTier);
    EXPECT_EQ(back.toTier, r.toTier);
    EXPECT_EQ(back.outcome.turnsOnToTier, 6);
    EXPECT_TRUE(back.outcome.userOverrideWithin5);
    EXPECT_TRUE(back.outcome.underRouteSignalWithin5);
    EXPECT_FALSE(back.outcome.pending);
}

// INV-10: the file mode is owner-only 0600 after append.
TEST(ModelSwitchLedger, Inv10OwnerOnlyPerms) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus", "haiku", false)));
    const QFileDevice::Permissions p = QFile(path).permissions();
    EXPECT_TRUE(p.testFlag(QFileDevice::ReadOwner));
    EXPECT_TRUE(p.testFlag(QFileDevice::WriteOwner));
    EXPECT_FALSE(p.testFlag(QFileDevice::ReadGroup));
    EXPECT_FALSE(p.testFlag(QFileDevice::ReadOther));
    EXPECT_FALSE(p.testFlag(QFileDevice::WriteGroup));
    EXPECT_FALSE(p.testFlag(QFileDevice::WriteOther));
}

// INV-10: writing past the cap evicts oldest lines; size stays ≤ cap, the
// newest record survives, and every surviving line is whole (valid JSON).
TEST(ModelSwitchLedger, Inv10EvictionUnderCap) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    const qint64 cap = 700;
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(L::appendRecord(
            path, makeRecord("opus", "haiku", false,
                             QStringLiteral("s%1").arg(i)), cap));
    EXPECT_LE(QFile(path).size(), cap);

    const QList<L::Record> recs = L::readRecords(path);
    ASSERT_FALSE(recs.isEmpty());
    EXPECT_EQ(recs.last().sessionId, QStringLiteral("s7"));   // newest intact
    // No partial line: line count equals parsed-record count.
    EXPECT_EQ(fileLineCount(path), recs.size());
}

// INV-10: a pending record is pinned — never evicted even when it is the oldest
// eviction candidate; the newest stays too.
TEST(ModelSwitchLedger, Inv10PendingRecordPinned) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    const qint64 cap = 700;
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus","haiku",false,"A"), cap));
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus","haiku",false,"B"), cap));
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus","haiku",true ,"PIN"), cap));
    for (const char *s : {"C","D","E"})
        ASSERT_TRUE(L::appendRecord(
            path, makeRecord("opus","haiku",false,QString::fromLatin1(s)), cap));
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus","haiku",false,"NEW"), cap));

    EXPECT_LE(QFile(path).size(), cap);
    const QList<L::Record> recs = L::readRecords(path);
    bool hasPin = false, hasNew = false;
    for (const L::Record &r : recs) {
        if (r.sessionId == QStringLiteral("PIN")) hasPin = true;
        if (r.sessionId == QStringLiteral("NEW")) hasNew = true;
    }
    EXPECT_TRUE(hasPin) << "pending record was evicted";
    EXPECT_TRUE(hasNew) << "newest record was evicted";
}

// INV-10: the production cap is exactly 256 KiB.
TEST(ModelSwitchLedger, Inv10MaxBytesConstant) {
    EXPECT_EQ(L::kMaxLedgerBytes, 256 * 1024);
}

// INV-11: two auto-switches in the window, each transcript /model matching its
// own auto record by tier+time → NOT an override.
TEST(ModelSwitchLedger, Inv11AutoThenAutoNotOverride) {
    const QList<L::ModelEvent> events = {{"haiku", 1000}, {"opus", 9000}};
    const QList<L::AutoSwitch> autos  = {{"haiku", 1005}, {"opus", 9002}};
    EXPECT_FALSE(L::detectUserOverride(events, autos));
}

// INV-11: a transcript /model with no matching auto record (wrong time, or none)
// → an override.
TEST(ModelSwitchLedger, Inv11AutoThenUserIsOverride) {
    // Same tier but Δts = 49 s ≫ 10 s window → not auto-authored.
    const QList<L::ModelEvent> mistimed = {{"haiku", 1000}};
    const QList<L::AutoSwitch> autos    = {{"haiku", 50000}};
    EXPECT_TRUE(L::detectUserOverride(mistimed, autos));
    // No auto records at all → user typed it.
    EXPECT_TRUE(L::detectUserOverride({{"sonnet", 5000}}, {}));
}

// INV-12: a higher tier re-recommended within the window after a downgrade.
TEST(ModelSwitchLedger, Inv12UnderRouteOnHigherRec) {
    EXPECT_EQ(L::detectUnderRoute("haiku", {"opus"}), L::UnderRoute::Yes);
    EXPECT_EQ(L::detectUnderRoute("sonnet", {"haiku", "opus"}), L::UnderRoute::Yes);
}

// INV-12: a downgrade at the last turn (no following turns) stays pending.
TEST(ModelSwitchLedger, Inv12LastTurnDowngradePending) {
    EXPECT_EQ(L::detectUnderRoute("haiku", {}), L::UnderRoute::Pending);
}

// INV-12: following turns that never re-recommend higher → not under-routed.
TEST(ModelSwitchLedger, Inv12NoHigherRecIsNo) {
    EXPECT_EQ(L::detectUnderRoute("haiku", {"haiku", "haiku"}), L::UnderRoute::No);
}

// MEDIUM-2: the correction regex fires on the listed cues (and the documented
// prose false-positive) but not on neutral text.
TEST(ModelSwitchLedger, CorrectionRegexSoftSignal) {
    EXPECT_TRUE(L::detectCorrection(QStringLiteral("no, that's not right")));
    EXPECT_TRUE(L::detectCorrection(QStringLiteral("undo that")));
    EXPECT_TRUE(L::detectCorrection(QStringLiteral("revert please")));
    EXPECT_TRUE(L::detectCorrection(QStringLiteral("try again")));
    EXPECT_TRUE(L::detectCorrection(QStringLiteral("that's wrong")));
    EXPECT_TRUE(L::detectCorrection(QStringLiteral("no problem")));  // documented false-fire
    EXPECT_FALSE(L::detectCorrection(QStringLiteral("looks good, continue")));
    EXPECT_FALSE(L::detectCorrection(QStringLiteral("thanks")));
}

// Outcome fill-in lifecycle: a pending record is read, filled, and rewritten.
TEST(ModelSwitchLedger, OutcomeFillInLifecycle) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    ASSERT_TRUE(L::appendRecord(path, makeRecord("opus","haiku",true,"fill")));
    QList<L::Record> recs = L::readRecords(path);
    ASSERT_EQ(recs.size(), 1);
    ASSERT_TRUE(recs[0].outcome.pending);

    recs[0].outcome.turnsOnToTier = 6;
    recs[0].outcome.userOverrideWithin5 = false;
    recs[0].outcome.pending = false;
    ASSERT_TRUE(L::writeRecords(path, recs));

    const QList<L::Record> after = L::readRecords(path);
    ASSERT_EQ(after.size(), 1);
    EXPECT_FALSE(after[0].outcome.pending);
    EXPECT_EQ(after[0].outcome.turnsOnToTier, 6);
}

// --- §2.5 outcome fill-in: pure computeOutcome helper ---

namespace {

L::TranscriptTurn assistantTurn(qint64 tsMs, const char *modelId) {
    L::TranscriptTurn t;
    t.tsMs        = tsMs;
    t.isAssistant = true;
    t.modelId     = QString::fromLatin1(modelId);
    return t;
}

L::TranscriptTurn userTurn(qint64 tsMs, const char *text) {
    L::TranscriptTurn t;
    t.tsMs     = tsMs;
    t.isUser   = true;
    t.userText = QString::fromLatin1(text);
    return t;
}

}  // namespace

// aliasFromModelId — pure mapping, no claude_lib dep.
TEST(ModelSwitchLedger, AliasFromModelId) {
    EXPECT_EQ(L::aliasFromModelId("claude-haiku-4-5"), "haiku");
    EXPECT_EQ(L::aliasFromModelId("claude-sonnet-4-6"), "sonnet");
    EXPECT_EQ(L::aliasFromModelId("claude-opus-4-7"), "opus");
    // Unknown model id → "sonnet" (default — matches ModelRecommender).
    EXPECT_EQ(L::aliasFromModelId("claude-some-future-id"), "sonnet");
    EXPECT_EQ(L::aliasFromModelId(""), "");
}

// Outcome fill-in: zero post-switch turns → pending, defaults.
TEST(ModelSwitchLedger, ComputeOutcomeNoTurnsStaysPending) {
    auto res = L::computeOutcome("opus", "haiku", 1000, {}, {}, {});
    EXPECT_TRUE(res.outcome.pending);
    EXPECT_EQ(res.outcome.turnsOnToTier, 0);
}

// 5 assistant turns on the new tier → settled, turns counted.
TEST(ModelSwitchLedger, ComputeOutcome5AssistantTurnsSettled) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-haiku-4-5"),
        assistantTurn(3000, "claude-haiku-4-5"),
        assistantTurn(4000, "claude-haiku-4-5"),
        assistantTurn(5000, "claude-haiku-4-5"),
        assistantTurn(6000, "claude-haiku-4-5"),
    };
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, {}, {"haiku"});
    EXPECT_FALSE(res.outcome.pending);
    EXPECT_EQ(res.outcome.turnsOnToTier, 5);
}

// Subsequent auto-switch ends the dwell early; turns_on_to_tier is the count up
// to that switch.
TEST(ModelSwitchLedger, ComputeOutcomeSubsequentAutoSettlesEarly) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-haiku-4-5"),
        assistantTurn(3000, "claude-haiku-4-5"),
    };
    const QList<L::AutoSwitch> nextAutos = {{"sonnet", 4000}};
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, nextAutos, {});
    EXPECT_FALSE(res.outcome.pending);
    EXPECT_EQ(res.outcome.turnsOnToTier, 2);
}

// turns_on_to_tier stops counting at the first divergent assistant model id
// (e.g. a user typed /model and the next assistant turn shows the new model).
TEST(ModelSwitchLedger, ComputeOutcomeStopsAtModelDivergence) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-haiku-4-5"),
        assistantTurn(3000, "claude-haiku-4-5"),
        userTurn(3500, "/model sonnet"),
        assistantTurn(4000, "claude-sonnet-4-6"),
        assistantTurn(5000, "claude-sonnet-4-6"),
    };
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, {}, {"sonnet"});
    EXPECT_EQ(res.outcome.turnsOnToTier, 2)
        << "must stop at first non-to_tier assistant turn";
}

// INV-12: downgrade then higher recommendation → under_route signal.
TEST(ModelSwitchLedger, ComputeOutcomeUnderRouteOnHigherRec) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-haiku-4-5"),
        assistantTurn(3000, "claude-haiku-4-5"),
        assistantTurn(4000, "claude-haiku-4-5"),
        assistantTurn(5000, "claude-haiku-4-5"),
        assistantTurn(6000, "claude-haiku-4-5"),
    };
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, {}, {"opus"});
    EXPECT_FALSE(res.outcome.pending);
    EXPECT_TRUE(res.outcome.underRouteSignalWithin5);
}

// Under_route only fires on a downgrade, never on an upgrade.
TEST(ModelSwitchLedger, ComputeOutcomeUnderRouteNotOnUpgrade) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-opus-4-7"),
        assistantTurn(3000, "claude-opus-4-7"),
        assistantTurn(4000, "claude-opus-4-7"),
        assistantTurn(5000, "claude-opus-4-7"),
        assistantTurn(6000, "claude-opus-4-7"),
    };
    // From haiku → opus is an upgrade; even if scorer later wants opus (same
    // tier), under_route stays false because it only meaningfully applies to
    // a downgrade per spec §2.5.
    auto res = L::computeOutcome("haiku", "opus", 1000, turns, {}, {"opus"});
    EXPECT_FALSE(res.outcome.underRouteSignalWithin5);
}

// Correction signal — first post-switch user turn matches the regex (downgrade).
TEST(ModelSwitchLedger, ComputeOutcomeCorrectionFirstUserTurn) {
    const QList<L::TranscriptTurn> turns = {
        userTurn(1500, "no, that was wrong"),
        assistantTurn(2000, "claude-haiku-4-5"),
        assistantTurn(3000, "claude-haiku-4-5"),
        assistantTurn(4000, "claude-haiku-4-5"),
        assistantTurn(5000, "claude-haiku-4-5"),
        assistantTurn(6000, "claude-haiku-4-5"),
    };
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, {}, {"haiku"});
    EXPECT_FALSE(res.outcome.pending);
    EXPECT_TRUE(res.outcome.correctionSignalWithin5);
}

// Correction signal does NOT fire on an upgrade (per §2.5 — soft signal is
// "user pushed back on the cheaper tier", not relevant on upgrades).
TEST(ModelSwitchLedger, ComputeOutcomeCorrectionNotOnUpgrade) {
    const QList<L::TranscriptTurn> turns = {
        userTurn(1500, "no, that was wrong"),
        assistantTurn(2000, "claude-opus-4-7"),
        assistantTurn(3000, "claude-opus-4-7"),
        assistantTurn(4000, "claude-opus-4-7"),
        assistantTurn(5000, "claude-opus-4-7"),
        assistantTurn(6000, "claude-opus-4-7"),
    };
    auto res = L::computeOutcome("haiku", "opus", 1000, turns, {}, {"opus"});
    EXPECT_FALSE(res.outcome.correctionSignalWithin5);
}

// User override via inline `/model X` text — no matching auto record → override.
TEST(ModelSwitchLedger, ComputeOutcomeUserOverrideViaTextCommand) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-haiku-4-5"),
        userTurn(2500, "/model sonnet"),
        assistantTurn(3000, "claude-sonnet-4-6"),
        assistantTurn(4000, "claude-sonnet-4-6"),
        assistantTurn(5000, "claude-sonnet-4-6"),
        assistantTurn(6000, "claude-sonnet-4-6"),
    };
    // No auto record matches the sonnet switch → user typed it → override.
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, {}, {"sonnet"});
    EXPECT_TRUE(res.outcome.userOverrideWithin5);
}

// `/model X` matched by a *subsequent auto* record within the author window
// → NOT an override (INV-11 author-correlation).
TEST(ModelSwitchLedger, ComputeOutcomeAutoSwitchIsNotOverride) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(2000, "claude-haiku-4-5"),
        userTurn(2500, "/model sonnet"),
        assistantTurn(3000, "claude-sonnet-4-6"),
    };
    // The subsequent ledger auto-record matches by tier + |Δts| ≤ 10s.
    const QList<L::AutoSwitch> nextAutos = {{"sonnet", 2502}};
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, nextAutos, {});
    EXPECT_FALSE(res.outcome.userOverrideWithin5);
}

// Pre-switch turns (tsMs < switchTsMs) are ignored.
TEST(ModelSwitchLedger, ComputeOutcomePreSwitchTurnsIgnored) {
    const QList<L::TranscriptTurn> turns = {
        assistantTurn(500, "claude-opus-4-7"),     // before switch
        userTurn(600, "no good"),                   // before switch
        assistantTurn(2000, "claude-haiku-4-5"),    // after switch
    };
    auto res = L::computeOutcome("opus", "haiku", 1000, turns, {}, {});
    // Only 1 post-switch assistant turn — not yet settled.
    EXPECT_TRUE(res.outcome.pending);
    EXPECT_EQ(res.outcome.turnsOnToTier, 1);
    // Pre-switch "no good" should NOT count as a correction; the helper
    // examines only post-switch user turns.
    EXPECT_FALSE(res.outcome.correctionSignalWithin5);
}

// Tier ranking underpins the higher/lower comparisons.
TEST(ModelSwitchLedger, TierRankOrdering) {
    EXPECT_EQ(L::tierRank("haiku"), 0);
    EXPECT_EQ(L::tierRank("sonnet"), 1);
    EXPECT_EQ(L::tierRank("opus"), 2);
    EXPECT_LT(L::tierRank("haiku"), L::tierRank("opus"));
    EXPECT_EQ(L::tierRank("bogus"), -1);
}
