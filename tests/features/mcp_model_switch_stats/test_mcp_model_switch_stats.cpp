// Feature-conformance test for ANTS-1735 — the model_switch_stats MCP verb.
// See tests/features/mcp_model_switch_stats/spec.md and docs/specs/ANTS-1735.md
// §2.5 (INV-13). Two halves:
//   (1) behavioural — the pure aggregation (statsEnvelope/statsForProject),
//   (2) wiring contract — source-grep that the MCP dispatch is registered
//       (RemoteControl delegate, mainwindow registration, tools/list descriptor,
//       Required caller_cwd contract, ETag opt-in).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <string>

#include "modelswitchledger.h"

namespace L = ModelSwitchLedger;

namespace {

L::Record rec(const QString &from, const QString &to, bool pending, int turns,
              bool override_ = false, bool underRoute = false,
              const QString &project = QStringLiteral("/projA")) {
    L::Record r;
    // Relative-recent so the record stays inside statsForProject's default
    // 30-day recency window regardless of when the suite runs. (A fixed
    // 2026-05-25 stamp aged out of the window on 2026-06-24T14:00 and
    // started failing GlobalScope/ProjectScoped — a test time-bomb, not a
    // behaviour change.)
    r.ts = QDateTime::currentDateTimeUtc().addDays(-1).toString(Qt::ISODate);
    r.project = project;
    r.fromTier = from;
    r.toTier = to;
    r.trigger = QStringLiteral("auto");
    r.outcome.pending = pending;
    r.outcome.turnsOnToTier = turns;
    r.outcome.userOverrideWithin5    = override_;
    r.outcome.overrideUndidDowngrade = override_;  // ANTS-1957: regret reads this
    r.outcome.underRouteSignalWithin5 = underRoute;
    return r;
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-13: an absent ledger is not an error — ok:true, switches:0.
TEST(McpModelSwitchStats, AbsentLedgerOkZero) {
    const QJsonObject env =
        L::statsForProject(QStringLiteral("/no/such/ledger.jsonl"),
                           QStringLiteral("/projA"));
    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("switches")).toInt(), 0);
}

// INV-13: aggregation counts downgrades/upgrades, sums avoided/routed Opus
// turns, and reports a regret ratio.
TEST(McpModelSwitchStats, EnvelopeAggregates) {
    const QList<L::Record> recs = {
        rec("opus", "haiku", /*pending*/false, /*turns*/6),                 // clean downgrade
        rec("opus", "haiku", false, 4, /*override*/true),                   // regretted downgrade
        rec("haiku", "opus", false, 3),                                     // upgrade to opus
    };
    const QJsonObject env = L::statsEnvelope(recs);
    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("switches")).toInt(), 3);
    EXPECT_EQ(env.value(QStringLiteral("downgrades")).toInt(), 2);
    EXPECT_EQ(env.value(QStringLiteral("upgrades")).toInt(), 1);
    EXPECT_EQ(env.value(QStringLiteral("opus_turns_avoided")).toInt(), 10); // 6 + 4
    EXPECT_EQ(env.value(QStringLiteral("opus_turns_routed_in")).toInt(), 3);
    EXPECT_EQ(env.value(QStringLiteral("regret_count")).toInt(), 1);
    // regret_rate = 1 of 2 measured downgrades = 50%.
    EXPECT_DOUBLE_EQ(env.value(QStringLiteral("regret_rate")).toDouble(), 50.0);
    EXPECT_EQ(env.value(QStringLiteral("pending_count")).toInt(), 0);
}

// INV-13 (ANTS-1889): the headline is an avoided/regret ratio string when
// the switcher is enabled and has measured outcomes at/above the floor.
// ANTS-1891 — the floor (kHeadlineFloorMeasured = 10) means a single record
// shows "insufficient data" instead of the ratio; this test feeds enough
// measured downgrades to exceed the floor.
TEST(McpModelSwitchStats, HeadlineIsRatio) {
    L::StatsConfig cfg;
    cfg.switchEnabled = true;
    QList<L::Record> recs;
    for (int i = 0; i < L::kHeadlineFloorMeasured; ++i)
        recs << rec("opus", "haiku", false, 6);
    const QString headline =
        L::statsEnvelope(recs, cfg).value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("avoided"))) << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("regret"))) << headline.toStdString();
}

// INV-13 (ANTS-1889): the envelope carries the live switcher configuration —
// auto_model_switch_enabled / floor_tier / min_dwell_sec — so a caller can
// distinguish "feature off" from "feature on, no candidate turns yet".
TEST(McpModelSwitchStats, EnvelopeCarriesConfigTriple) {
    L::StatsConfig cfg;
    cfg.switchEnabled = true;
    cfg.floorTier = QStringLiteral("sonnet");
    cfg.minDwellSec = 120;
    const QJsonObject env = L::statsEnvelope({}, cfg);
    EXPECT_TRUE(env.value(QStringLiteral("auto_model_switch_enabled")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("floor_tier")).toString(),
              QStringLiteral("sonnet"));
    EXPECT_EQ(env.value(QStringLiteral("min_dwell_sec")).toInt(), 120);
    EXPECT_EQ(env.value(QStringLiteral("scope")).toString(),
              QStringLiteral("project"));   // default
}

// INV-13 (ANTS-1889): when the switcher is disabled, headline says "OFF"
// regardless of record counts — the dormant-vs-quiet ambiguity closes.
TEST(McpModelSwitchStats, HeadlineWhenDisabled) {
    L::StatsConfig cfg;   // switchEnabled defaults to false
    const QString headline =
        L::statsEnvelope({}, cfg).value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("OFF"), Qt::CaseInsensitive))
        << headline.toStdString();
}

// INV-13 (ANTS-1889): when enabled with zero records in scope, headline
// signals "ON ... (no switches yet)" — distinct from "OFF" and from the
// avoided/regret form.
TEST(McpModelSwitchStats, HeadlineWhenEnabledNoSwitches) {
    L::StatsConfig cfg;
    cfg.switchEnabled = true;
    const QString headline =
        L::statsEnvelope({}, cfg).value(QStringLiteral("headline")).toString();
    EXPECT_TRUE(headline.contains(QStringLiteral("ON"), Qt::CaseSensitive))
        << headline.toStdString();
    EXPECT_TRUE(headline.contains(QStringLiteral("no switches"),
                                  Qt::CaseInsensitive))
        << headline.toStdString();
}

// INV-13 (ANTS-1889): scope:"global" echoes through the envelope so the
// caller knows which aggregation it got.
TEST(McpModelSwitchStats, GlobalScopeEcho) {
    L::StatsConfig cfg;
    cfg.scope = QStringLiteral("global");
    EXPECT_EQ(L::statsEnvelope({}, cfg).value(QStringLiteral("scope")).toString(),
              QStringLiteral("global"));
}

// INV-13 (ANTS-1889): statsForScope with "global" aggregates across all
// projects in the ledger, ignoring the projectRoot filter.
TEST(McpModelSwitchStats, GlobalScopeAggregatesAcrossProjects) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    ASSERT_TRUE(L::appendRecord(path, rec("opus","haiku",false,6,false,false,"/projA")));
    ASSERT_TRUE(L::appendRecord(path, rec("opus","haiku",false,4,false,false,"/projB")));
    ASSERT_TRUE(L::appendRecord(path, rec("opus","haiku",false,5,false,false,"/projC")));

    L::StatsConfig cfg;
    cfg.switchEnabled = true;
    cfg.scope = QStringLiteral("global");
    const QJsonObject env =
        L::statsForScope(path, QStringLiteral("/projA"), cfg);
    EXPECT_EQ(env.value(QStringLiteral("switches")).toInt(), 3);
    EXPECT_EQ(env.value(QStringLiteral("opus_turns_avoided")).toInt(), 15);  // 6+4+5
    EXPECT_EQ(env.value(QStringLiteral("scope")).toString(),
              QStringLiteral("global"));
}

// INV-13: pending records are counted separately — never folded into the
// regret/under-route outcome stats.
TEST(McpModelSwitchStats, PendingCountedSeparately) {
    const QList<L::Record> recs = {
        rec("opus", "haiku", /*pending*/true, 0, /*override*/true, /*underRoute*/true),
        rec("opus", "haiku", false, 5),
    };
    const QJsonObject env = L::statsEnvelope(recs);
    EXPECT_EQ(env.value(QStringLiteral("pending_count")).toInt(), 1);
    // The pending record's override/under-route flags must NOT be counted.
    EXPECT_EQ(env.value(QStringLiteral("regret_count")).toInt(), 0);
    EXPECT_EQ(env.value(QStringLiteral("under_route_count")).toInt(), 0);
    // regret_rate denominator excludes the pending downgrade (0 of 1 measured).
    EXPECT_DOUBLE_EQ(env.value(QStringLiteral("regret_rate")).toDouble(), 0.0);
}

// INV-13: read-only — aggregating leaves the ledger file byte-for-byte intact.
TEST(McpModelSwitchStats, ReadOnly) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    ASSERT_TRUE(L::appendRecord(path, rec("opus", "haiku", false, 6)));
    QFile f(path); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray before = f.readAll(); f.close();

    (void)L::statsForProject(path, QStringLiteral("/projA"));

    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray after = f.readAll(); f.close();
    EXPECT_EQ(before, after);
}

// INV-13: stats are scoped to the caller's project — other projects' records
// are excluded.
TEST(McpModelSwitchStats, ProjectScoped) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ledger.jsonl"));
    ASSERT_TRUE(L::appendRecord(path, rec("opus","haiku",false,6,false,false,"/projA")));
    ASSERT_TRUE(L::appendRecord(path, rec("opus","haiku",false,6,false,false,"/projB")));
    ASSERT_TRUE(L::appendRecord(path, rec("opus","haiku",false,6,false,false,"/projB")));
    const QJsonObject env = L::statsForProject(path, QStringLiteral("/projA"));
    EXPECT_EQ(env.value(QStringLiteral("switches")).toInt(), 1);
}

// Wiring contract — the MCP dispatch must be registered end to end.
TEST(McpModelSwitchStats, WiringContract) {
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_RC_CPP);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    EXPECT_TRUE(has(rcHdr, "cmdModelSwitchStats(const QJsonObject"))
        << "decl missing from remotecontrol.h";
    EXPECT_TRUE(has(rcCpp, "RemoteControl::cmdModelSwitchStats("))
        << "definition missing from remotecontrol.cpp";
    EXPECT_TRUE(has(rcCpp, "ANTS-1735")) << "remotecontrol.cpp must anchor ANTS-1735";
    EXPECT_TRUE(has(rcCpp, "statsForScope"))
        << "cmdModelSwitchStats must delegate to ModelSwitchLedger::statsForScope (ANTS-1889)";
    EXPECT_TRUE(has(mwCpp, "registerToolProvider(\"model_switch_stats\""))
        << "mainwindow.cpp must register model_switch_stats";
    EXPECT_TRUE(has(mwCpp, "cmdModelSwitchStats"))
        << "mainwindow registration must delegate to cmdModelSwitchStats";
    EXPECT_TRUE(has(ciCpp, "t[\"name\"] = \"model_switch_stats\""))
        << "tools/list must carry a model_switch_stats descriptor";
    EXPECT_TRUE(has(ciCpp, "\"model_switch_stats\""))
        << "callerCwdContractFor must classify model_switch_stats";

    // ANTS-1889 — the verb must read the live switcher config and surface
    // it in the envelope; the tool descriptor must document the `scope` arg.
    EXPECT_TRUE(has(rcCpp, "claudeAutoModel"))
        << "cmdModelSwitchStats must read Config::claudeAutoModel for INV-13";
    EXPECT_TRUE(has(rcCpp, "ANTS-1889"))
        << "cmdModelSwitchStats must anchor ANTS-1889";
    EXPECT_TRUE(has(ciCpp, "auto_model_switch_enabled"))
        << "tool descriptor must mention the new envelope field";
    EXPECT_TRUE(has(ciCpp, "\"scope\""))
        << "tool descriptor must declare a scope property";
}
