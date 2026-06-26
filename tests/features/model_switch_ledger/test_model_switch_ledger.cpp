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
    r.outcome.overrideUndidDowngrade = true;   // ANTS-1957
    r.outcome.underRouteSignalWithin5 = true;
    const L::Record back = L::fromJson(L::toJson(r));
    EXPECT_EQ(back.fromTier, r.fromTier);
    EXPECT_EQ(back.toTier, r.toTier);
    EXPECT_EQ(back.outcome.turnsOnToTier, 6);
    EXPECT_TRUE(back.outcome.userOverrideWithin5);
    EXPECT_TRUE(back.outcome.overrideUndidDowngrade);   // ANTS-1957
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
    // ANTS-1891 — bumped 700→800: the new session_cleanly_ended_on_new_tier
    // field grew each record from ~280B to ~362B.
    // ANTS-1957 — bumped 800→900: the new override_undid_downgrade field grew
    // each record to ~406B; 2 records (PIN + NEW) now total ~813B, so the cap
    // must clear that for both mandatory records to survive.
    const qint64 cap = 900;
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

// ANTS-2196: pending records are soft-pinned, but a run of never-settling
// pending outcomes (a vanished project dir → fillPendingLedgerOutcomes can never
// clear them) would grow the file unbounded under the soft cap alone. The hard
// secondary ceiling (kEvictHardCeilingMult× the soft cap) drops the OLDEST line
// regardless of pending state, so the ledger stays bounded; the newest survives.
TEST(ModelSwitchLedger, Ants2196PendingHardCeiling) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    const qint64 cap = 900;
    // 30 all-pending records: every one is soft-pinned (~406B each ⇒ ~12 KiB),
    // so only the hard ceiling (4×900 = 3600B) can bound the file.
    for (int i = 0; i < 30; ++i)
        ASSERT_TRUE(L::appendRecord(
            path, makeRecord("opus","haiku",true,QStringLiteral("p%1").arg(i)), cap));

    EXPECT_LE(QFile(path).size(), cap * L::kEvictHardCeilingMult)
        << "all-pending ledger grew past the hard ceiling";

    const QList<L::Record> recs = L::readRecords(path);
    ASSERT_FALSE(recs.isEmpty());
    EXPECT_EQ(recs.last().sessionId, QStringLiteral("p29"))
        << "newest record must always survive";
    bool hasOldest = false;
    for (const L::Record &r : recs)
        if (r.sessionId == QStringLiteral("p0")) hasOldest = true;
    EXPECT_FALSE(hasOldest)
        << "oldest pending record should be evicted at the hard ceiling";
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

// INV-13 (ANTS-1957): detectCorrectiveOverride only flags a non-auto /model that
// moves to a tier ABOVE toTier (the user undid the downgrade by moving up).
TEST(ModelSwitchLedger, Inv13CorrectiveOverrideDirectionAware) {
    // opus→haiku downgrade. A user /model up (sonnet or opus) undoes it.
    EXPECT_TRUE(L::detectCorrectiveOverride({{"sonnet", 5000}}, {}, "haiku"));
    EXPECT_TRUE(L::detectCorrectiveOverride({{"opus",   5000}}, {}, "haiku"));
    // opus→sonnet downgrade. A user /model haiku is a FURTHER downgrade (agreed,
    // not a regret); /model sonnet is the same tier (no-op). Neither is corrective.
    EXPECT_FALSE(L::detectCorrectiveOverride({{"haiku",  5000}}, {}, "sonnet"));
    EXPECT_FALSE(L::detectCorrectiveOverride({{"sonnet", 5000}}, {}, "sonnet"));
    // An auto-authored upward move is the controller's own switch, not the user.
    EXPECT_FALSE(L::detectCorrectiveOverride(
        {{"opus", 5000}}, {{"opus", 5002}}, "haiku"));
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

// INV-13 (ANTS-1957): computeOutcome populates overrideUndidDowngrade only when
// the user's manual switch moves above toTier; a lateral/further-down manual
// switch leaves userOverrideWithin5=true but overrideUndidDowngrade=false.
TEST(ModelSwitchLedger, Inv13ComputeOutcomeCorrectiveVsNeutralOverride) {
    // opus→haiku, then user /model sonnet → upward → corrective (regret-worthy).
    const QList<L::TranscriptTurn> up = {
        assistantTurn(2000, "claude-haiku-4-5"),
        userTurn(2500, "/model sonnet"),
        assistantTurn(3000, "claude-sonnet-4-6"),
    };
    auto resUp = L::computeOutcome("opus", "haiku", 1000, up, {}, {});
    EXPECT_TRUE(resUp.outcome.userOverrideWithin5);
    EXPECT_TRUE(resUp.outcome.overrideUndidDowngrade);

    // opus→sonnet, then user /model haiku → further DOWN → not corrective.
    const QList<L::TranscriptTurn> down = {
        assistantTurn(2000, "claude-sonnet-4-6"),
        userTurn(2500, "/model haiku"),
        assistantTurn(3000, "claude-haiku-4-5"),
    };
    auto resDown = L::computeOutcome("opus", "sonnet", 1000, down, {}, {});
    EXPECT_TRUE(resDown.outcome.userOverrideWithin5);    // still a manual switch
    EXPECT_FALSE(resDown.outcome.overrideUndidDowngrade); // but not a regret
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

// ANTS-1936 — recency window filters stale records out of the aggregation.
// A ledger with one ancient record (2020) and one fresh (now): a 30-day
// window excludes the ancient one and surfaces excluded_stale_count + the
// window_days echo; windowDays=0 (all-time) keeps everything.
TEST(ModelSwitchLedger, Ants1936RecencyWindowExcludesStale) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));

    L::Record old = makeRecord("opus", "haiku", false);
    old.ts = QStringLiteral("2020-01-01T00:00:00Z");   // far past
    L::Record fresh = makeRecord("opus", "haiku", false);
    fresh.ts = L::nowIso8601();                          // now
    ASSERT_TRUE(L::appendRecord(path, old));
    ASSERT_TRUE(L::appendRecord(path, fresh));

    // 30-day window: ancient record excluded.
    L::StatsConfig windowed;
    windowed.scope      = QStringLiteral("project");
    windowed.windowDays = 30;
    const QJsonObject envW = L::statsForScope(path, QStringLiteral("/mnt/proj"), windowed);
    EXPECT_EQ(envW.value("window_days").toInt(), 30);
    EXPECT_EQ(envW.value("excluded_stale_count").toInt(), 1);
    const int windowedSwitches = envW.value("switches").toInt();

    // All-time (windowDays=0): both records counted, no window fields.
    L::StatsConfig allTime;
    allTime.scope      = QStringLiteral("project");
    allTime.windowDays = 0;
    const QJsonObject envA = L::statsForScope(path, QStringLiteral("/mnt/proj"), allTime);
    EXPECT_FALSE(envA.contains("window_days"));
    EXPECT_FALSE(envA.contains("excluded_stale_count"));
    EXPECT_GT(envA.value("switches").toInt(), windowedSwitches);
}

// ===== ANTS-1941 — behaviour-epoch filter =====

// INV-1 / INV-2 — epoch round-trips through toJson/fromJson; a legacy object
// with no `epoch` key reads back as 0 (pre-epoch); a non-zero epoch survives.
TEST(ModelSwitchLedger, Ants1941EpochJsonRoundTrip) {
    L::Record r = makeRecord("opus", "sonnet", false);
    r.epoch = L::kSwitcherEpoch;
    EXPECT_EQ(L::fromJson(L::toJson(r)).epoch, L::kSwitcherEpoch);   // INV-1

    r.epoch = 2;                                                      // INV-2 (non-zero)
    EXPECT_EQ(L::fromJson(L::toJson(r)).epoch, 2);

    QJsonObject legacy = L::toJson(r);                                // INV-2 (missing key)
    legacy.remove(QStringLiteral("epoch"));
    EXPECT_EQ(L::fromJson(legacy).epoch, 0);
}

// INV-3 — minEpoch>0 excludes epoch<minEpoch records (counted in
// excluded_pre_epoch_count); a record with epoch>minEpoch stays in-scope (strict
// `<`, not `!=`); and a surviving current-epoch settled downgrade IS counted in
// measured_downgrades (the positive path, not just stale-exclusion).
TEST(ModelSwitchLedger, Ants1941EpochFilterExcludesPreEpoch) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));

    L::Record pre = makeRecord("opus", "haiku", false);   // epoch 0 — excluded
    L::Record cur = makeRecord("opus", "haiku", false);   // epoch 1 — settled downgrade
    cur.epoch = 1;
    cur.outcome.turnsOnToTier = 6;                          // positive evidence → measured
    L::Record next = makeRecord("opus", "haiku", false);  // epoch 2 — newer generation
    next.epoch = 2;
    next.outcome.turnsOnToTier = 4;
    ASSERT_TRUE(L::appendRecord(path, pre));
    ASSERT_TRUE(L::appendRecord(path, cur));
    ASSERT_TRUE(L::appendRecord(path, next));

    L::StatsConfig cfg;
    cfg.scope      = QStringLiteral("project");
    cfg.windowDays = 0;          // isolate the epoch filter from the age window
    cfg.minEpoch   = 1;
    const QJsonObject env = L::statsForScope(path, QStringLiteral("/mnt/proj"), cfg);

    EXPECT_EQ(env.value("min_epoch").toInt(), 1);
    EXPECT_EQ(env.value("excluded_pre_epoch_count").toInt(), 1);   // the epoch-0 record
    EXPECT_EQ(env.value("switches").toInt(), 2);                   // epoch-1 AND epoch-2
    EXPECT_EQ(env.value("measured_downgrades").toInt(), 2);        // both settled downgrades
}

// INV-4 — a record both pre-epoch AND older than the window lands ONLY in
// excluded_pre_epoch_count (the epoch check precedes the age check).
TEST(ModelSwitchLedger, Ants1941EpochCheckPrecedesAgeCheck) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));

    L::Record ancientPre = makeRecord("opus", "haiku", false);   // epoch 0 AND stale
    ancientPre.ts = QStringLiteral("2020-01-01T00:00:00Z");
    L::Record cur = makeRecord("opus", "haiku", false);
    cur.epoch = 1;
    cur.ts = L::nowIso8601();
    ASSERT_TRUE(L::appendRecord(path, ancientPre));
    ASSERT_TRUE(L::appendRecord(path, cur));

    L::StatsConfig cfg;
    cfg.scope      = QStringLiteral("project");
    cfg.windowDays = 30;
    cfg.minEpoch   = 1;
    const QJsonObject env = L::statsForScope(path, QStringLiteral("/mnt/proj"), cfg);

    EXPECT_EQ(env.value("excluded_pre_epoch_count").toInt(), 1);
    EXPECT_EQ(env.value("excluded_stale_count").toInt(), 0);   // NOT double-counted
}

// INV-5 / INV-6 — minEpoch=0 (default) counts every epoch and emits neither
// epoch key; the two keys are additive (appear only when minEpoch>0).
TEST(ModelSwitchLedger, Ants1941NoEpochFilterByDefault) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));

    L::Record pre = makeRecord("opus", "haiku", false);   // epoch 0
    L::Record cur = makeRecord("opus", "haiku", false);
    cur.epoch = 1;
    ASSERT_TRUE(L::appendRecord(path, pre));
    ASSERT_TRUE(L::appendRecord(path, cur));

    L::StatsConfig allEpochs;
    allEpochs.scope      = QStringLiteral("project");
    allEpochs.windowDays = 0;
    allEpochs.minEpoch   = 0;
    const QJsonObject envAll = L::statsForScope(path, QStringLiteral("/mnt/proj"), allEpochs);
    EXPECT_EQ(envAll.value("switches").toInt(), 2);            // both counted (INV-5)
    EXPECT_FALSE(envAll.contains("min_epoch"));
    EXPECT_FALSE(envAll.contains("excluded_pre_epoch_count"));

    L::StatsConfig filtered = allEpochs;
    filtered.minEpoch = 1;
    const QJsonObject envF = L::statsForScope(path, QStringLiteral("/mnt/proj"), filtered);
    EXPECT_TRUE(envF.contains("min_epoch"));                   // additive (INV-6)
    EXPECT_TRUE(envF.contains("excluded_pre_epoch_count"));
}

// INV-8 — an all-pre-epoch ledger filtered with minEpoch=1 yields
// measured_downgrades=0 and regret_rate=0.0 (the empty current-epoch set the
// caution-dial chain relies on); excluded_pre_epoch_count == record count. The
// emit-when-count-0 case: even though one record would have been a regret, it is
// filtered out, so the dial reads neutral.
TEST(ModelSwitchLedger, Ants1941AllPreEpochYieldsNeutralStats) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));

    L::Record a = makeRecord("opus", "haiku", false);
    a.outcome.turnsOnToTier = 5;
    L::Record b = makeRecord("opus", "haiku", false);
    b.outcome.overrideUndidDowngrade = true;    // ANTS-1957 — regret if counted
    L::Record c = makeRecord("opus", "haiku", false);
    c.outcome.turnsOnToTier = 3;
    ASSERT_TRUE(L::appendRecord(path, a));
    ASSERT_TRUE(L::appendRecord(path, b));
    ASSERT_TRUE(L::appendRecord(path, c));

    L::StatsConfig cfg;
    cfg.scope      = QStringLiteral("project");
    cfg.windowDays = 0;
    cfg.minEpoch   = 1;
    const QJsonObject env = L::statsForScope(path, QStringLiteral("/mnt/proj"), cfg);

    EXPECT_EQ(env.value("excluded_pre_epoch_count").toInt(), 3);
    EXPECT_EQ(env.value("measured_downgrades").toInt(), 0);
    EXPECT_DOUBLE_EQ(env.value("regret_rate").toDouble(), 0.0);
}
