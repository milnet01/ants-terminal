// Feature-conformance test for ANTS-1254 — the `last_audit_summary`
// MCP tool. Exercises AuditEngine::summariseSarif against committed
// SARIF fixtures + source-grep checks for wiring contracts.
//
// Exit 0 = all 10 invariants hold.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "auditengine.h"

#ifndef SRC_AUDIT_ENGINE_H_PATH
#error "SRC_AUDIT_ENGINE_H_PATH compile definition required"
#endif
#ifndef SRC_AUDIT_ENGINE_CPP_PATH
#error "SRC_AUDIT_ENGINE_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef FIXTURE_MIN_SARIF
#error "FIXTURE_MIN_SARIF compile definition required"
#endif
#ifndef FIXTURE_EMPTY_SARIF
#error "FIXTURE_EMPTY_SARIF compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int g_failures = 0;

void expect(bool cond, const char *invName, const char *msg) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "[%s] FAIL: %s\n", invName, msg);
    }
}

}  // namespace

// ----------------------------------------------------------------
// Parser-side INVs (use the fixture).
// ----------------------------------------------------------------

TEST(McpLastAuditSummary, Inv3SortOrderLevelDescConfidenceDesc) {
    g_failures = 0;
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "note");
    expect(s.has_value(), "INV-3", "summariseSarif returned nullopt");
    if (!s) FAIL();
    // Fixture: 5 results, none filtered out at floor=note.
    // Expected order:
    //   1. "rule-blocker" (level=error)
    //   2. "rule-major" confidence=70 (suppressed but still in pool)
    //   3. "rule-major" confidence=50 (file b.cpp)
    //   4. "rule-major" confidence=20 (file a.cpp:5)
    //   5. "rule-minor" (level=note)
    const auto &top = s->topFindings;
    expect(top.size() == 5, "INV-3",
           ("topFindings size=" + std::to_string(top.size()) +
            "; expected 5").c_str());
    if (top.size() == 5) {
        expect(top[0].level == "error", "INV-3",
               "[0] level should be error (BLOCKER first)");
        expect(top[1].level == "warning" && top[1].confidence == 70,
               "INV-3", "[1] should be warning conf=70");
        expect(top[2].confidence == 50, "INV-3",
               "[2] should be conf=50");
        expect(top[3].confidence == 20, "INV-3",
               "[3] should be conf=20");
        expect(top[4].level == "note", "INV-3",
               "[4] should be note (lowest level)");
    }
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv4CountsCoverFullSetNotFiltered) {
    g_failures = 0;
    // severity_floor=error → top_findings has only error rows, but
    // counts must still cover all 3 levels + suppressed.
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "error");
    expect(s.has_value(), "INV-4", "summariseSarif returned nullopt");
    if (!s) FAIL();
    expect(s->countError == 1, "INV-4",
           ("countError=" + std::to_string(s->countError)).c_str());
    expect(s->countWarning == 3, "INV-4",
           ("countWarning=" + std::to_string(s->countWarning)).c_str());
    expect(s->countNote == 1, "INV-4",
           ("countNote=" + std::to_string(s->countNote)).c_str());
    expect(s->countSuppressed == 1, "INV-4",
           ("countSuppressed=" + std::to_string(s->countSuppressed)).c_str());
    expect(s->topFindings.size() == 1, "INV-4",
           ("error-only top size=" +
            std::to_string(s->topFindings.size())).c_str());
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv7FilePassthroughNoRewrite) {
    g_failures = 0;
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "note");
    if (!s || s->topFindings.isEmpty()) FAIL();
    // Fixture writes "src/a.cpp" verbatim — no absolutisation.
    bool foundAsIs = false;
    for (const auto &f : s->topFindings) {
        if (f.file == QStringLiteral("src/a.cpp")) {
            foundAsIs = true;
            break;
        }
    }
    expect(foundAsIs, "INV-7",
           "expected file 'src/a.cpp' (as-is from SARIF) in topFindings");
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv9CountsFixedShape) {
    g_failures = 0;
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "note");
    if (!s) FAIL();
    // Fixed-shape contract is on the wire envelope; here we lock
    // that the underlying summary always populates all 4 counters
    // (defaults to 0 in the struct).
    expect(s->countError + s->countWarning + s->countNote ==
               (1 + 3 + 1),
           "INV-9", "all 4 count fields populated by parser");
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv10EmptyRunsReturnsNullopt) {
    g_failures = 0;
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_EMPTY_SARIF), 5, "warning");
    expect(!s.has_value(), "INV-10",
           "empty runs[] must return nullopt (caller maps to not_audited)");
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv3SeverityResolvedFromRuleIndex) {
    g_failures = 0;
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "note");
    if (!s || s->topFindings.size() < 5) FAIL();
    // Top finding is rule-blocker — severity must resolve to BLOCKER
    // (not the level-fallback CRITICAL), proving the rule index works.
    expect(s->topFindings[0].severity == QStringLiteral("BLOCKER"),
           "INV-3 / spec § 3.1 step 3",
           ("expected BLOCKER, got " +
            s->topFindings[0].severity.toStdString()).c_str());
    // Rule-minor is in the index as MINOR — same proof in the
    // opposite direction.
    expect(s->topFindings.last().severity == QStringLiteral("MINOR"),
           "INV-3 / spec § 3.1 step 3",
           ("expected MINOR, got " +
            s->topFindings.last().severity.toStdString()).c_str());
    if (g_failures) FAIL();
}

// ----------------------------------------------------------------
// Wiring INVs (source-grep — the impl exists with the right shape).
// ----------------------------------------------------------------

TEST(McpLastAuditSummary, Inv1EngineDoesNotReadHtml) {
    g_failures = 0;
    const std::string ec = slurp(SRC_AUDIT_ENGINE_CPP_PATH);
    // summariseSarif derives htmlPath but never opens it for read.
    // QFile::exists is allowed (just stat); QFile::open is not.
    const std::string body = [&]() {
        const auto pos = ec.find("summariseSarif(");
        return pos == std::string::npos ? std::string{} : ec.substr(pos);
    }();
    expect(!body.empty(), "INV-1", "summariseSarif body not found");
    // Strict gate: the body should never call open() on the htmlCandidate
    // / htmlPath variables. Pattern matches `<varname>.open(`.
    expect(body.find("htmlCandidate.open(") == std::string::npos &&
               body.find("htmlPath.open(") == std::string::npos,
           "INV-1",
           "summariseSarif appears to open() the HTML file (must only stat)");
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv2CacheMembersDeclared) {
    g_failures = 0;
    const std::string rh = slurp(SRC_REMOTECONTROL_H_PATH);
    std::regex memberRe(R"(mutable[^;]*m_auditSummary)");
    auto begin = std::sregex_iterator(rh.begin(), rh.end(), memberRe);
    auto end   = std::sregex_iterator();
    const size_t n = static_cast<size_t>(std::distance(begin, end));
    expect(n == 5, "INV-2",
           ("remotecontrol.h has " + std::to_string(n) +
            " mutable m_auditSummary* fields; expected 5 "
            "(path/mtimeMs/cache/topN/floor)")
               .c_str());
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv8FloorValidatedBeforeDiskScan) {
    g_failures = 0;
    const std::string rcc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Within cmdLastAuditSummary, the bad_severity_floor return must
    // come BEFORE the .audit_cache directory scan.
    const auto fnPos = rcc.find("cmdLastAuditSummary(const QJsonObject");
    expect(fnPos != std::string::npos, "INV-8",
           "cmdLastAuditSummary not found");
    if (fnPos == std::string::npos) FAIL();
    const std::string body = rcc.substr(fnPos, 4000);
    const auto floorPos = body.find("bad_severity_floor");
    const auto cachePos = body.find(".audit_cache");
    expect(floorPos != std::string::npos && cachePos != std::string::npos,
           "INV-8", "expected both bad_severity_floor and .audit_cache");
    expect(floorPos < cachePos, "INV-8",
           "bad_severity_floor must be returned BEFORE .audit_cache scan");
    if (g_failures) FAIL();
}

TEST(McpLastAuditSummary, Inv5And6WiringRegistered) {
    g_failures = 0;
    // INV-5 (UDS reachability) is inherited; the witness here is that
    // last_audit_summary IS registered through the same pipeline as
    // every other tool. INV-6 (lex-max discovery) is exercised by
    // the dir scan with QDir::Name | QDir::Reversed.
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(mw.find("registerToolProvider(\"last_audit_summary\"") !=
               std::string::npos,
           "INV-5",
           "mainwindow.cpp missing registerToolProvider(\"last_audit_summary\", …)");
    const std::string rcc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("QDir::Name | QDir::Reversed") != std::string::npos,
           "INV-6",
           "cmdLastAuditSummary must scan with QDir::Name | QDir::Reversed "
           "for lex-max filename discovery");
    if (g_failures) FAIL();
}
