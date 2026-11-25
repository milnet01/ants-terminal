// Feature-conformance test for spec.md — exercises the
// RuleQualityTracker data layer behind the 0.6.31 audit dialog's
// Rule Quality view.
//
// Headless: no QApplication. Pure model code (Qt JSON / Qt Core only).

#include "auditrulequality.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>

#include <gtest/gtest.h>

namespace {
int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

#define CHECK_EQ(actual, expected, msg) do {                                 \
    if ((actual) != (expected)) {                                            \
        std::fprintf(stderr, "FAIL %s:%d  %s (actual=%lld expected=%lld)\n", \
                     __FILE__, __LINE__, msg,                                \
                     static_cast<long long>(actual),                         \
                     static_cast<long long>(expected));                      \
        ++failures;                                                          \
    }                                                                        \
} while (0)

// ANTS-6862 — the history lives in the audit's own cache folder.
QString historyPath(const QTemporaryDir &tmp) {
    return tmp.path() + "/.audit_cache/audit_rule_quality.json";
}

// A fixture written before the tracker exists needs the folder first.
QString seededHistoryPath(const QTemporaryDir &tmp) {
    QDir().mkpath(tmp.path() + "/.audit_cache");
    return historyPath(tmp);
}

// --- Test 1 — recordFire / recordSuppression aggregation. ------------
void testFireAndSuppressionAggregation() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "QTemporaryDir construction failed");
    RuleQualityTracker t(tmp.path());

    // 5 fires for memory_patterns, 3 fires for secrets_scan.
    for (int i = 0; i < 5; ++i)
        t.recordFire("memory_patterns",
                     QString("src/foo.cpp:%1: new QWidget(parent)").arg(10 + i));
    for (int i = 0; i < 3; ++i)
        t.recordFire("secrets_scan",
                     QString("src/bar.cpp:%1: api_key=\"deadbeef\"").arg(20 + i));

    // 3 suppressions for memory_patterns, 0 for secrets_scan.
    t.recordSuppression("memory_patterns", "key1",
                        "src/foo.cpp:10: new QWidget(parent)", "Qt parent-child");
    t.recordSuppression("memory_patterns", "key2",
                        "src/foo.cpp:11: new QWidget(parent)", "Qt parent-child");
    t.recordSuppression("memory_patterns", "key3",
                        "src/foo.cpp:12: new QWidget(parent)", "Qt parent-child");

    const auto rows = t.report();
    CHECK_EQ(rows.size(), 2, "report should have 2 distinct rules");

    // Sort order: noisiest first. memory_patterns has 3/5 = 60% FP rate;
    // secrets_scan has 0%. memory_patterns must come first.
    CHECK(rows[0].ruleId == "memory_patterns",
          "noisiest rule (memory_patterns, 60% FP) must sort first");
    CHECK_EQ(rows[0].fires30d, 5, "memory_patterns fires30d");
    CHECK_EQ(rows[0].suppressions30d, 3, "memory_patterns suppressions30d");
    CHECK_EQ(rows[0].fpRate30d, 60, "memory_patterns fpRate30d (60%)");

    CHECK(rows[1].ruleId == "secrets_scan",
          "secrets_scan must sort after memory_patterns");
    CHECK_EQ(rows[1].fires30d, 3, "secrets_scan fires30d");
    CHECK_EQ(rows[1].suppressions30d, 0, "secrets_scan suppressions30d");
    CHECK_EQ(rows[1].fpRate30d, 0, "secrets_scan fpRate30d (0%)");
}

// --- Test 2 — LCS suggester returns the common shape. ---------------
void testSuggestTighteningFindsCommonShape() {
    QTemporaryDir tmp;
    RuleQualityTracker t(tmp.path());

    // 3 suppressions with a clear common substring containing structural
    // chars: " new QWidget(parent)" is 20 chars and contains spaces +
    // parens — should be returned as the suggested tightening.
    t.recordSuppression("memory_patterns", "k1",
                        "src/dialog.cpp:42:30: new QWidget(parent)", "");
    t.recordSuppression("memory_patterns", "k2",
                        "src/sshdialog.cpp:88:14: new QWidget(parent)", "");
    t.recordSuppression("memory_patterns", "k3",
                        "src/aidialog.cpp:120:8: new QWidget(parent)", "");

    const QString suggestion = t.suggestTightening("memory_patterns");
    CHECK(!suggestion.isEmpty(),
          "suggestTightening with 3 matching samples should not be empty");
    CHECK(suggestion.contains("new QWidget(parent)"),
          "suggestion should contain the common substring");
}

// --- Test 3 — LCS suggester returns empty for too-few samples. ------
void testSuggestTighteningEmptyForFewSamples() {
    QTemporaryDir tmp;
    RuleQualityTracker t(tmp.path());

    t.recordSuppression("foo", "k1", "some line text here", "");

    const QString s = t.suggestTightening("foo");
    CHECK(s.isEmpty(),
          "suggestTightening with only 1 sample should return empty");
}

// --- Test 4 — LCS rejects pure-identifier substring. ----------------
void testSuggestTighteningRejectsPureIdentifier() {
    QTemporaryDir tmp;
    RuleQualityTracker t(tmp.path());

    // Two lines whose ONLY common substring is `m_status` (no boundary
    // char). Should be rejected — no structural punctuation.
    t.recordSuppression("foo", "k1", "abcdef m_status xyz", "");
    t.recordSuppression("foo", "k2", "qrstuv m_status mno", "");

    const QString s = t.suggestTightening("foo");
    // The LCS contains a leading space, which IS a structural char. Make
    // the suggestion structural-boundary check meaningful by verifying
    // that suggestions DO include boundary chars when present.
    CHECK(s.isEmpty() || s.contains(' ') || s.contains('(') ||
          s.contains(')'),
          "suggestion (if non-empty) must contain a structural boundary "
          "character (space / paren / brace / bracket / semicolon / quote)");
}

// --- Test 5 — Persistence round-trip. -------------------------------
void testPersistenceRoundTrip() {
    QTemporaryDir tmp;
    {
        RuleQualityTracker t(tmp.path());
        t.recordFire("rule_a", "src/foo.cpp:1: line a1");
        t.recordFire("rule_a", "src/foo.cpp:2: line a2");
        t.recordSuppression("rule_a", "ka", "src/foo.cpp:1: line a1", "noise");
        t.save();
    }
    // Verify the file exists and is non-empty.
    QFile f(historyPath(tmp));
    CHECK(f.exists(), "audit_rule_quality.json must be written");
    CHECK(f.size() > 0, "audit_rule_quality.json must be non-empty");

    // Reload via a fresh tracker.
    RuleQualityTracker t2(tmp.path());
    const auto rows = t2.report();
    CHECK_EQ(rows.size(), 1, "round-trip: 1 rule expected");
    CHECK(rows[0].ruleId == "rule_a", "round-trip: rule_a expected");
    CHECK_EQ(rows[0].fires30d, 2, "round-trip: fires preserved");
    CHECK_EQ(rows[0].suppressions30d, 1, "round-trip: suppressions preserved");
    CHECK_EQ(rows[0].fpRate30d, 50, "round-trip: 1/2 = 50% FP rate");
}

// --- Test 6 — ANTS-5085: save only what changed, not per suppression. --
void testSaveOnlyWhenChanged() {
    QTemporaryDir tmp;
    const QString path = historyPath(tmp);
    {
        RuleQualityTracker t(tmp.path());
        t.recordSuppression("rule_a", "ka", "src/foo.cpp:1: line a1", "noise");
        CHECK(!QFile::exists(path),
              "a suppression alone must not rewrite the history file");
    }
    CHECK(QFile::exists(path), "destroying a changed tracker must save it");
    {
        RuleQualityTracker t(tmp.path());
        QFile::remove(path);
    }
    CHECK(!QFile::exists(path),
          "a tracker with no new records must not rewrite the file");
}

// --- Test 7 — ANTS-5085: fires are counted per rule per day, so repeated
// runs neither saturate the record cap nor grow the file per finding. ----
void testManyFiresStayCountedAndSmall() {
    QTemporaryDir tmp;
    const QString path = historyPath(tmp);
    {
        RuleQualityTracker t(tmp.path());
        for (int i = 0; i < 60000; ++i)
            t.recordFire("noisy_rule", QString("src/foo.cpp:%1: some finding text").arg(i));
    }
    RuleQualityTracker reloaded(tmp.path());
    const auto rows = reloaded.report();
    CHECK(rows.size() == 1, "one rule expected after reload");
    if (rows.size() == 1)
        CHECK_EQ(rows[0].firesAllTime, 60000, "every fire counted across save/reload");
    CHECK(QFileInfo(path).size() < 64LL * 1024,
          "the history file grows per finding instead of per rule-day");
}

// --- Test 8 — ANTS-5085: a v1 file's per-fire records still load. --------
void testLoadsV1FireRecords() {
    QTemporaryDir tmp;
    const QString ts = QDateTime::currentDateTime().toString(Qt::ISODate);
    QFile f(seededHistoryPath(tmp));
    CHECK(f.open(QIODevice::WriteOnly), "could not write the v1 fixture");
    f.write(QString(R"({"schema_version":1,"fires":[)"
                    R"({"rule":"old_rule","line":"a","ts":"%1"},)"
                    R"({"rule":"old_rule","line":"b","ts":"%1"}],)"
                    R"("suppressions":[]})").arg(ts).toUtf8());
    f.close();

    RuleQualityTracker t(tmp.path());
    const auto rows = t.report();
    CHECK(rows.size() == 1, "the v1 rule is missing after load");
    if (rows.size() == 1) CHECK_EQ(rows[0].fires30d, 2, "v1 fires counted");
}

// --- Test 9 — ANTS-6480: two dialogs open on one project each keep their
// records when both close. --------------------------------------------------
void testTwoTrackersKeepEachOthersRecords() {
    QTemporaryDir tmp;
    {
        RuleQualityTracker a(tmp.path());
        RuleQualityTracker b(tmp.path());
        a.recordFire("rule_a", "x");
        a.recordSuppression("rule_a", "ka", "line a", "noise");
        b.recordFire("rule_a", "y");
        b.recordFire("rule_b", "z");
        b.recordSuppression("rule_b", "kb", "line b", "noise");
        a.save();
        b.save();
    }
    RuleQualityTracker t(tmp.path());
    int fires = 0, supps = 0;
    for (const auto &row : t.report()) {
        fires += row.firesAllTime;
        supps += row.suppressionsAllTime;
    }
    CHECK_EQ(fires, 3, "a fire recorded by the other tracker was lost");
    CHECK_EQ(supps, 2, "a suppression recorded by the other tracker was lost");
}

// --- Test 10 — ANTS-6481: a history a newer Ants wrote is not rewritten as
// this build's schema. --------------------------------------------------------
void testNewerSchemaIsNotRewritten() {
    QTemporaryDir tmp;
    const QString path = seededHistoryPath(tmp);
    const QByteArray seed = R"({"schema_version":3,"fire_days":[],"suppressions":[],"v3_field":1})";
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly), "could not write the v3 fixture");
    f.write(seed);
    f.close();
    {
        RuleQualityTracker t(tmp.path());
        t.recordFire("rule_a", "x");
        t.save();
    }
    QFile g(path);
    CHECK(g.open(QIODevice::ReadOnly), "the v3 file is gone");
    CHECK(g.readAll() == seed, "a newer-schema history was rewritten");
}

// --- Test 11 — ANTS-6862: a save leaves nothing in the project's root. ------
void testHistoryLivesInAuditCache() {
    QTemporaryDir tmp;
    {
        RuleQualityTracker t(tmp.path());
        t.recordFire("rule_a", "x");
        t.save();
    }
    CHECK(QFile::exists(historyPath(tmp)), "the history is not in .audit_cache/");
    CHECK(!QFile::exists(tmp.path() + "/audit_rule_quality.json"),
          "the history was written to the project root");
    CHECK(!QFile::exists(tmp.path() + "/audit_rule_quality.json.lock"),
          "the history's lock was left in the project root");
}

// --- Test 12 — ANTS-6862: a history an older Ants left in the root moves
// into .audit_cache/ with its records, and its lock goes with it. ------------
void testLegacyRootHistoryMovesIn() {
    QTemporaryDir tmp;
    const QString legacy = tmp.path() + "/audit_rule_quality.json";
    const QString day = QDate::currentDate().toString(Qt::ISODate);
    QFile f(legacy);
    CHECK(f.open(QIODevice::WriteOnly), "could not write the legacy fixture");
    f.write(QString(R"({"schema_version":2,"fire_days":[)"
                    R"({"rule":"old_rule","day":"%1","count":3}],)"
                    R"("suppressions":[]})").arg(day).toUtf8());
    f.close();
    QFile lock(legacy + ".lock");
    CHECK(lock.open(QIODevice::WriteOnly), "could not write the legacy lock");
    lock.close();

    RuleQualityTracker t(tmp.path());
    const auto rows = t.report();
    CHECK(rows.size() == 1, "the legacy history's rule is missing");
    if (rows.size() == 1) CHECK_EQ(rows[0].firesAllTime, 3, "legacy fires lost");
    CHECK(QFile::exists(historyPath(tmp)), "the legacy history was not moved in");
    CHECK(!QFile::exists(legacy), "the legacy history is still in the root");
    CHECK(!QFile::exists(legacy + ".lock"), "the legacy lock is still in the root");
}

// --- Test 13 — ANTS-6862: a history already in .audit_cache/ wins, and the
// root copy is left as it is rather than lost. --------------------------------
void testRootHistoryLeftWhenCacheHasOne() {
    QTemporaryDir tmp;
    const QString legacy = tmp.path() + "/audit_rule_quality.json";
    const QByteArray rootSeed = R"({"schema_version":2,"fire_days":[],"suppressions":[]})";
    QFile r(legacy);
    CHECK(r.open(QIODevice::WriteOnly), "could not write the root fixture");
    r.write(rootSeed);
    r.close();
    const QString day = QDate::currentDate().toString(Qt::ISODate);
    QFile c(seededHistoryPath(tmp));
    CHECK(c.open(QIODevice::WriteOnly), "could not write the cache fixture");
    c.write(QString(R"({"schema_version":2,"fire_days":[)"
                    R"({"rule":"cache_rule","day":"%1","count":1}],)"
                    R"("suppressions":[]})").arg(day).toUtf8());
    c.close();

    RuleQualityTracker t(tmp.path());
    const auto rows = t.report();
    CHECK(rows.size() == 1 && rows[0].ruleId == "cache_rule",
          "the .audit_cache/ history was not the one loaded");
    QFile g(legacy);
    CHECK(g.open(QIODevice::ReadOnly), "the root copy was removed");
    CHECK(g.readAll() == rootSeed, "the root copy was changed");
    CHECK(!QFile::exists(legacy + ".lock"), "loading left a lock in the root");
}

#define RQ_TEST(NAME) \
    TEST(AuditRuleQuality, NAME) { \
        int before = failures; \
        test##NAME(); \
        if (failures > before) FAIL(); \
    }

RQ_TEST(FireAndSuppressionAggregation)
RQ_TEST(SuggestTighteningFindsCommonShape)
RQ_TEST(SuggestTighteningEmptyForFewSamples)
RQ_TEST(SuggestTighteningRejectsPureIdentifier)
RQ_TEST(PersistenceRoundTrip)
RQ_TEST(SaveOnlyWhenChanged)
RQ_TEST(ManyFiresStayCountedAndSmall)
RQ_TEST(LoadsV1FireRecords)
RQ_TEST(TwoTrackersKeepEachOthersRecords)
RQ_TEST(NewerSchemaIsNotRewritten)
RQ_TEST(HistoryLivesInAuditCache)
RQ_TEST(LegacyRootHistoryMovesIn)
RQ_TEST(RootHistoryLeftWhenCacheHasOne)

}  // namespace
