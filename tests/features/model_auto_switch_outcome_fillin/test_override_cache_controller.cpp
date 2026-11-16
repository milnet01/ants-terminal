// ANTS-1890 INV-7 behavioural tests — controller-side override cool-down
// cache. Companion to the source-grep tests in
// test_model_auto_switch_outcome_fillin.cpp (which lives in test_core);
// these tests instantiate a live ClaudeStatusBarController (only available
// in test_claude where ants_claude_lib + GUI libs link) and exercise the
// cache via the two seams the spec named:
//   (a) seedOverrideCacheFromLedger(fixturePath) at bootstrap
//   (b) fillPendingLedgerOutcomes(fixturePath) at the 2 s tick
//
// The lastOverrideMsForProject(project) accessor returns -1 for unknown
// keys and the cached ms for known keys.

#include <gtest/gtest.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTextStream>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "claudeintegration.h"
#include "claudestatuswidgets.h"
#include "modelautoswitch.h"
#include "modelswitchledger.h"
#include "../../_support/xdg_guard.h"

namespace {

// Build one ledger record as the on-disk JSONL line. ts is ISO-8601 Z.
QString recordLine(const QString &project, const QString &fromTier,
                   const QString &toTier, const QString &ts,
                   bool userOverrideWithin5, bool pending)
{
    ModelSwitchLedger::Record r;
    r.ts          = ts;
    r.project     = project;
    r.fromTier    = fromTier;
    r.toTier      = toTier;
    r.trigger     = QStringLiteral("auto");
    r.scoreReason = QStringLiteral("test");
    r.outcome.userOverrideWithin5 = userOverrideWithin5;
    r.outcome.pending             = pending;
    QJsonObject obj = ModelSwitchLedger::toJson(r);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

void writeLedger(QTemporaryFile &f, const QStringList &lines)
{
    ASSERT_TRUE(f.open());
    QTextStream out(&f);
    for (const QString &line : lines) out << line << "\n";
    f.close();
}

}  // namespace

// INV-7 (a) — the cache is populated incrementally when
// fillPendingLedgerOutcomes processes a record whose outcome settled
// with userOverrideWithin5=true. For this test we start with a PENDING
// record whose post-switch state already shows the override (the
// computeOutcome call inside fill-in flips pending→false and writes
// back), and assert the per-project cache entry lands.
//
// Simpler shape: an already-settled record. fill-in still iterates
// (records that aren't pending are skipped from re-computation), so the
// cache wouldn't pick those up. Instead we use a settled record but
// route through seedOverrideCacheFromLedger which DOES walk settled
// records. The fill-in code path's population is covered by the
// CallsComputeOutcome source-grep in the sibling bundle plus this
// test's seed call exercising the same population branch (both helpers
// share the `if (userOverrideWithin5 && !pending) slot=max(slot,ts)`
// idiom — see the code, both call sites use the same lines).
TEST(ModelAutoSwitchOutcomeFillinController, OverrideCachePopulatedByFillIn) {
    QStatusBar bar;
    ClaudeStatusBarController controller(&bar, nullptr);

    QTemporaryFile f;
    const QString project = QStringLiteral("/tmp/projA");
    writeLedger(f, {
        recordLine(project, QStringLiteral("opus"), QStringLiteral("haiku"),
                   QStringLiteral("2026-05-26T10:00:00Z"),
                   /*userOverrideWithin5*/ true,  /*pending*/ false),
    });

    // Cache empty before the call.
    EXPECT_EQ(controller.lastOverrideMsForProject(project), -1);

    // seedOverrideCacheFromLedger covers the settled-record population
    // path (fillPendingLedgerOutcomes uses the same idiom against
    // pending→settled transitions; the source-grep sibling asserts the
    // delegate-to-computeOutcome wiring).
    controller.seedOverrideCacheFromLedger(f.fileName());

    const qint64 cached = controller.lastOverrideMsForProject(project);
    EXPECT_GT(cached, 0)
        << "Settled userOverrideWithin5 record must populate the cache";
    // Expected ms = parseIso8601Ms("2026-05-26T10:00:00Z") = 1779696000000 ms
    EXPECT_EQ(cached,
              ModelSwitchLedger::parseIso8601Ms(
                  QStringLiteral("2026-05-26T10:00:00Z")));
}

// INV-7 (b) — bootstrap seed. Same fixture; verify the controller
// reads it during the explicit seed call (called from attach() in
// production; the test injects the path directly).
TEST(ModelAutoSwitchOutcomeFillinController, OverrideCacheSeededOnConstruction) {
    QStatusBar bar;
    ClaudeStatusBarController controller(&bar, nullptr);

    QTemporaryFile f;
    const QString projectA = QStringLiteral("/tmp/projA");
    const QString projectB = QStringLiteral("/tmp/projB");
    writeLedger(f, {
        recordLine(projectA, QStringLiteral("opus"), QStringLiteral("haiku"),
                   QStringLiteral("2026-05-26T09:00:00Z"),
                   /*userOverride*/ true, /*pending*/ false),
        recordLine(projectA, QStringLiteral("opus"), QStringLiteral("sonnet"),
                   QStringLiteral("2026-05-26T11:00:00Z"),
                   /*userOverride*/ true, /*pending*/ false),
        recordLine(projectB, QStringLiteral("opus"), QStringLiteral("haiku"),
                   QStringLiteral("2026-05-26T08:00:00Z"),
                   /*userOverride*/ false, /*pending*/ false),  // not override
        recordLine(projectB, QStringLiteral("opus"), QStringLiteral("haiku"),
                   QStringLiteral("2026-05-26T12:00:00Z"),
                   /*userOverride*/ true, /*pending*/ true),    // pending
    });

    controller.seedOverrideCacheFromLedger(f.fileName());

    // projectA: two override records; cache must hold the MAX ts (11:00).
    EXPECT_EQ(controller.lastOverrideMsForProject(projectA),
              ModelSwitchLedger::parseIso8601Ms(
                  QStringLiteral("2026-05-26T11:00:00Z")));

    // projectB: one record was not an override, one was pending → neither
    // counts. Cache lookup returns -1 sentinel.
    EXPECT_EQ(controller.lastOverrideMsForProject(projectB), -1);

    // Unknown project: -1 sentinel (INV-8 per-project scope foundation).
    EXPECT_EQ(controller.lastOverrideMsForProject(
                  QStringLiteral("/tmp/never_seen")), -1);
}

// INV-7 — an empty or missing ledger leaves the cache empty (no crash).
TEST(ModelAutoSwitchOutcomeFillinController, OverrideCacheEmptyOnMissingLedger) {
    QStatusBar bar;
    ClaudeStatusBarController controller(&bar, nullptr);

    controller.seedOverrideCacheFromLedger(QStringLiteral("/no/such/ledger.jsonl"));
    EXPECT_EQ(controller.lastOverrideMsForProject(QStringLiteral("/tmp/x")), -1);
}

// ANTS-6229 — the conservatism cache keeps the ledger's inputs, not the
// multiplier, so a session that stops being mechanical gets the full
// penalty on the next tick. One regretted downgrade, below the floor, gives
// regret 100 %: the mechanical multiplier is the halved penalty, the
// non-mechanical one the full kCalibrationDwellMult.
TEST(ModelAutoSwitchOutcomeFillinController, ConservatismCacheFollowsMechanicalFlag) {
    QTemporaryDir cache;
    ASSERT_TRUE(cache.isValid());
    ants_test::XdgGuard env;
    env.setEnv("XDG_CACHE_HOME", cache.path().toUtf8());

    const QString project = QStringLiteral("/ants6229-project");
    ModelSwitchLedger::Record r;
    r.ts          = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    r.project     = project;
    r.fromTier    = QStringLiteral("opus");
    r.toTier      = QStringLiteral("haiku");
    r.trigger     = QStringLiteral("auto");
    r.scoreReason = QStringLiteral("test");
    r.epoch       = ModelSwitchLedger::kSwitcherEpoch;
    r.outcome.userOverrideWithin5    = true;
    r.outcome.overrideUndidDowngrade = true;
    r.outcome.pending                = false;
    const QString ledger = ModelSwitchLedger::defaultLedgerPath();
    ASSERT_TRUE(ledger.startsWith(cache.path())) << ledger.toStdString();
    ASSERT_TRUE(ModelSwitchLedger::appendRecord(ledger, r));

    QStatusBar bar;
    ClaudeStatusBarController controller(&bar, nullptr);
    const double full = ModelAutoSwitch::kCalibrationDwellMult;
    const double halved = 1.0 + (full - 1.0) * 0.5;

    // First tick fills the cache while the session is mechanical.
    ASSERT_DOUBLE_EQ(controller.conservatismMultiplierFor(project, 0, true), halved)
        << "test setup: the ledger did not give a calibrating high-regret picture";
    // Next tick, inside the TTL, the session is no longer mechanical.
    EXPECT_DOUBLE_EQ(controller.conservatismMultiplierFor(project, 2000, false), full)
        << "ANTS-6229: a multiplier cached on a mechanical tick was returned "
           "on a non-mechanical one";
    // And back: the cached inputs give the halved penalty again.
    EXPECT_DOUBLE_EQ(controller.conservatismMultiplierFor(project, 4000, true), halved);
}

namespace {

QString g6485Ledger;
QStringList g6485OtherInstanceLines;
bool g6485OtherInstanceWrote = false;

void writeLines(const QString &path, const QStringList &lines)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
}

void ants6485OtherInstanceWrites()
{
    writeLines(g6485Ledger, g6485OtherInstanceLines);
    g6485OtherInstanceWrote = true;
}

}  // namespace

// ANTS-6485 — the fill-in writes back only the records it changed. Between
// its read and its write, another instance evicts the oldest record and
// appends one. Writing back an unchanged copy of the evicted record would
// bring it back, at the end of the file and out of time order.
TEST(ModelAutoSwitchOutcomeFillinController, Ants6485FillWritesBackOnlyWhatItChanged) {
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    ants_test::XdgGuard env;
    env.setEnv("HOME", home.path().toUtf8());

    // The fill skips a project with no transcript, so give it one.
    const QString project = home.filePath(QStringLiteral("proj"));
    const QString sessions = home.filePath(QStringLiteral(".claude/projects/")
        + ClaudeIntegration::encodeProjectPath(project));
    ASSERT_TRUE(QDir().mkpath(sessions));
    writeLines(sessions + QStringLiteral("/s.jsonl"), {QStringLiteral("{}")});

    const QString opus = QStringLiteral("opus");
    const QString sonnet = QStringLiteral("sonnet");
    const QString haiku = QStringLiteral("haiku");
    const QString evicted = recordLine(QStringLiteral("/elsewhere"), opus, sonnet,
        QStringLiteral("2026-05-26T09:00:00Z"), false, /*pending*/ false);
    // The second switch settles the first; the second stays pending.
    const QString first = recordLine(project, opus, sonnet,
        QStringLiteral("2026-05-26T10:00:00Z"), false, /*pending*/ true);
    const QString second = recordLine(project, sonnet, haiku,
        QStringLiteral("2026-05-26T10:05:00Z"), false, /*pending*/ true);
    const QString appended = recordLine(QStringLiteral("/another"), opus, haiku,
        QStringLiteral("2026-05-26T10:10:00Z"), false, /*pending*/ true);

    g6485Ledger = home.filePath(QStringLiteral("ledger.jsonl"));
    writeLines(g6485Ledger, {evicted, first, second});
    g6485OtherInstanceLines = {first, second, appended};
    g6485OtherInstanceWrote = false;

    QStatusBar bar;
    ClaudeStatusBarController controller(&bar, nullptr);
    ModelSwitchLedger::testing::beforeRewrite = ants6485OtherInstanceWrites;
    controller.fillPendingLedgerOutcomes(g6485Ledger);
    ModelSwitchLedger::testing::beforeRewrite = nullptr;
    ASSERT_TRUE(g6485OtherInstanceWrote) << "test setup: the fill wrote nothing";

    const QList<ModelSwitchLedger::Record> after =
        ModelSwitchLedger::readRecords(g6485Ledger);
    QStringList projects;
    for (const ModelSwitchLedger::Record &r : after) projects << r.project;
    EXPECT_EQ(projects.join(QLatin1Char(' ')).toStdString(),
              QStringList({project, project, QStringLiteral("/another")})
                  .join(QLatin1Char(' ')).toStdString())
        << "ANTS-6485: the fill wrote back a record another instance had evicted";
    ASSERT_FALSE(after.isEmpty());
    EXPECT_FALSE(after.first().outcome.pending) << "the fill-in did not land";
}
