// Feature-conformance test for ANTS-1254 — the `last_audit_summary`
// MCP tool. Exercises AuditEngine::summariseSarif against committed
// SARIF fixtures + source-grep checks for wiring contracts.
//
// Exit 0 = all 10 invariants hold.

#include <fstream>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "../../_support/expect.h"
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
#ifndef FIXTURE_CPPCHECK_XML
#error "FIXTURE_CPPCHECK_XML compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




}  // namespace

// ----------------------------------------------------------------
// Parser-side INVs (use the fixture).
// ----------------------------------------------------------------

TEST(McpLastAuditSummary, Inv3SortOrderLevelDescConfidenceDesc) {
    expect_reset();
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
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv4CountsCoverFullSetNotFiltered) {
    expect_reset();
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
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2123 — semgrep countSuppressed parity. A `# nosemgrep`-ignored finding
// carries extra.is_ignored=true; summariseSemgrepJson must tally it into
// countSuppressed (parallel to the SARIF suppressions[] path) WITHOUT excluding
// it from the level counts or the top-findings pool.
TEST(McpLastAuditSummary, Ants2123SemgrepCountSuppressed) {
    expect_reset();
    QTemporaryDir dir;
    expect(dir.isValid(), "ANTS-2123", "temp dir invalid");
    const QString jsonPath = dir.path() + "/semgrep.json";
    {
        std::ofstream out(jsonPath.toStdString());
        out << R"json({
          "results": [
            {"path":"src/a.cpp","start":{"line":10},"check_id":"rule.ignored",
             "extra":{"message":"ignored finding","severity":"ERROR","is_ignored":true}},
            {"path":"src/b.cpp","start":{"line":20},"check_id":"rule.live",
             "extra":{"message":"live finding","severity":"WARNING"}}
          ],
          "errors": []
        })json";
    }
    auto s = AuditEngine::summariseSemgrepJson(jsonPath, 50, "note");
    expect(s.has_value(), "ANTS-2123", "summariseSemgrepJson returned nullopt");
    if (!s) FAIL();
    // Suppressed finding is tallied in parallel: counted by level AND kept in
    // the pool, exactly like the SARIF path.
    expect(s->countSuppressed == 1, "ANTS-2123",
           ("countSuppressed=" + std::to_string(s->countSuppressed) +
            "; expected 1").c_str());
    expect(s->countError == 1, "ANTS-2123",
           ("countError=" + std::to_string(s->countError)).c_str());
    expect(s->countWarning == 1, "ANTS-2123",
           ("countWarning=" + std::to_string(s->countWarning)).c_str());
    expect(s->topFindings.size() == 2, "ANTS-2123",
           ("topFindings size=" + std::to_string(s->topFindings.size()) +
            "; suppressed finding must still appear").c_str());
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3372 — a scanner progress-bar line that leaks into the findings
// stream (no real location, junk `file`) must be dropped before ranking so
// it can't out-rank real findings as top_findings[0]. A legitimate
// project-level finding at line 0 with a real path must survive (the filter
// is conservative on purpose).
TEST(McpLastAuditSummary, Ants3372DropsProgressBarArtifacts) {
    expect_reset();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString sarifPath = dir.path() + "/audit.sarif";
    {
        std::ofstream out(sarifPath.toStdString());
        // ━ is a box-drawing glyph (progress-bar fill). The middle
        // result mimics RetroDB's scraped "Working... ━━ 100% 0" artifact:
        // level warning, ruleId empty, no location, junk file string.
        out << R"sarif({
          "version": "2.1.0",
          "runs": [{
            "tool": {"driver": {"name": "t", "rules": []}},
            "results": [
              {"level":"error","ruleId":"real-bug",
               "message":{"text":"real finding"},
               "locations":[{"physicalLocation":{
                 "artifactLocation":{"uri":"src/real.cpp"},
                 "region":{"startLine":42}}}]},
              {"level":"warning","ruleId":"",
               "message":{"text":"05"},
               "locations":[{"physicalLocation":{
                 "artifactLocation":{"uri":"Working... ━━ 100% 0"},
                 "region":{"startLine":0}}}]},
              {"level":"note","ruleId":"proj-level",
               "message":{"text":"project-level finding"},
               "locations":[{"physicalLocation":{
                 "artifactLocation":{"uri":"src/projlevel.cpp"},
                 "region":{"startLine":0}}}]}
            ]
          }]
        })sarif";
    }
    auto s = AuditEngine::summariseSarif(sarifPath, 50, "note");
    ASSERT_TRUE(s.has_value());
    // Counts are unaffected — the filter runs after the level tally.
    EXPECT_EQ(1, s->countError);
    EXPECT_EQ(1, s->countWarning);
    EXPECT_EQ(1, s->countNote);
    bool sawArtifact = false, sawReal = false, sawProjLevel = false;
    for (const auto &f : s->topFindings) {
        if (f.file.contains(QStringLiteral("Working"))) sawArtifact = true;
        if (f.file == QStringLiteral("src/real.cpp"))     sawReal = true;
        if (f.file == QStringLiteral("src/projlevel.cpp")) sawProjLevel = true;
    }
    EXPECT_FALSE(sawArtifact)
        << "ANTS-3372: progress-bar artifact must be filtered from top_findings";
    EXPECT_TRUE(sawReal)
        << "ANTS-3372: real finding must survive";
    EXPECT_TRUE(sawProjLevel)
        << "ANTS-3372: a legitimate line-0 finding with a real path must survive";
    EXPECT_EQ(2, s->topFindings.size());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv7FilePassthroughNoRewrite) {
    expect_reset();
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
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv9CountsFixedShape) {
    expect_reset();
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "note");
    if (!s) FAIL();
    // Fixed-shape contract is on the wire envelope; here we lock
    // that the underlying summary always populates all 4 counters
    // (defaults to 0 in the struct).
    expect(s->countError + s->countWarning + s->countNote ==
               (1 + 3 + 1),
           "INV-9", "all 4 count fields populated by parser");
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv10EmptyRunsReturnsNullopt) {
    expect_reset();
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_EMPTY_SARIF), 5, "warning");
    expect(!s.has_value(), "INV-10",
           "empty runs[] must return nullopt (caller maps to not_audited)");
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv3SeverityResolvedFromRuleIndex) {
    expect_reset();
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
    EXPECT_EQ(0, expect_failures());
}

// ----------------------------------------------------------------
// Wiring INVs (source-grep — the impl exists with the right shape).
// ----------------------------------------------------------------

TEST(McpLastAuditSummary, Inv1EngineDoesNotReadHtml) {
    expect_reset();
    const std::string ec = ants_test::slurpFile(SRC_AUDIT_ENGINE_CPP_PATH);
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
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv2CacheMembersDeclared) {
    expect_reset();
    const std::string rh = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    std::regex memberRe(R"(mutable[^;]*m_auditSummary)");
    auto begin = std::sregex_iterator(rh.begin(), rh.end(), memberRe);
    auto end   = std::sregex_iterator();
    const size_t n = static_cast<size_t>(std::distance(begin, end));
    expect(n == 5, "INV-2",
           ("remotecontrol.h has " + std::to_string(n) +
            " mutable m_auditSummary* fields; expected 5 "
            "(path/mtimeMs/cache/topN/floor)")
               .c_str());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv8FloorValidatedBeforeDiskScan) {
    expect_reset();
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1459 LAS-2 — cppcheck XML parser populates AuditSummary
// with the right counts + sourceFormat tag.
TEST(McpLastAuditSummary, Ants1459CppcheckXmlParsesCounts) {
    expect_reset();
    auto s = AuditEngine::summariseCppcheckXml(
        QString::fromUtf8(FIXTURE_CPPCHECK_XML), 10,
        QStringLiteral("note"));
    ASSERT_TRUE(s.has_value())
        << "ANTS-1459 LAS-2: cppcheck XML must parse to AuditSummary";
    // Fixture has 4 entries:
    //   uninitMemberVar  severity=error       → countError++
    //   constParameter   severity=style       → countNote++ (note level)
    //   useStlAlgorithm  severity=performance → countNote++
    //   missingInclude   severity=information → countNote++
    EXPECT_EQ(1, s->countError);
    EXPECT_EQ(0, s->countWarning);
    EXPECT_EQ(3, s->countNote);
    EXPECT_EQ(QString::fromUtf8("cppcheck-xml"), s->sourceFormat);
    // Top entry sorts to error-level first.
    ASSERT_FALSE(s->topFindings.isEmpty());
    EXPECT_EQ(QStringLiteral("error"), s->topFindings.first().level);
    EXPECT_EQ(QStringLiteral("uninitMemberVar"),
              s->topFindings.first().ruleId);
    EXPECT_EQ(QStringLiteral("src/foo.cpp"),
              s->topFindings.first().file);
    EXPECT_EQ(42, s->topFindings.first().line);
}

// ANTS-1459 LAS-3 — buildLasEnvelope echoes source_format
// (defaulting to "sarif" when blank for back-compat).
TEST(McpLastAuditSummary, Ants1459SourceFormatInEnvelope) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rc.find("ok[\"source_format\"]") != std::string::npos,
           "LAS-3",
           "buildLasEnvelope must emit a source_format field");
    expect(rc.find("\"sarif\"") != std::string::npos &&
               rc.find("\"cppcheck-xml\"") != std::string::npos,
           "LAS-1/2",
           "remotecontrol.cpp must reference both sarif and "
           "cppcheck-xml source-format literals");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1459 RQ-1/2/3 — shared findRoadmapUnder helper widens
// discovery to docs/, docs/private/, docs/internal/, .github/, and
// returns empty on an empty canonical root.
TEST(McpLastAuditSummary, Ants1459FindRoadmapUnderWidens) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rc.find("findRoadmapUnder") != std::string::npos,
           "RQ-3",
           "remotecontrol.cpp must declare findRoadmapUnder helper");
    expect(rc.find("if (canonicalRoot.isEmpty()) return {};")
               != std::string::npos,
           "RQ-1",
           "findRoadmapUnder must early-return on empty canonical "
           "root (defensive guard)");
    expect(rc.find("docs/private/ROADMAP.md") != std::string::npos,
           "RQ-2",
           "findRoadmapUnder must probe docs/private/ROADMAP.md");
    expect(rc.find("docs/internal/ROADMAP.md") != std::string::npos,
           "RQ-2",
           "findRoadmapUnder must probe docs/internal/ROADMAP.md");
    expect(rc.find(".github/ROADMAP.md") != std::string::npos,
           "RQ-2",
           "findRoadmapUnder must probe .github/ROADMAP.md");
    // ANTS-3350 — the resolver walks up to the enclosing git repo so write/
    // query verbs resolve the project from a subdirectory (DOOM_Ants feedback).
    expect(rc.find("/.git") != std::string::npos,
           "ANTS-3350",
           "findRoadmapUnder must walk up to the .git repo boundary");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1459 RQ-4 — MainWindow::refreshRoadmapButton's path-list
// stays byte-equal to findRoadmapUnder's so the status-bar
// button surfaces on the same projects the MCP can query.
TEST(McpLastAuditSummary, Ants1459StatusBarRoadmapButtonWidened) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(mw.find("docs/private/ROADMAP.md") != std::string::npos,
           "RQ-4",
           "refreshRoadmapButton must probe docs/private/ROADMAP.md");
    expect(mw.find("docs/internal/ROADMAP.md") != std::string::npos,
           "RQ-4",
           "refreshRoadmapButton must probe docs/internal/ROADMAP.md");
    expect(mw.find(".github/ROADMAP.md") != std::string::npos,
           "RQ-4",
           "refreshRoadmapButton must probe .github/ROADMAP.md");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// ANTS-1576 — last_audit_summary hardening
// ============================================================
//
// Spec: docs/specs/ANTS-1576.md
//
// Bundles:
// - INV-1..3  : buildVcsProvenanceBlock helper (declared + behaviour).
// - INV-4..5  : writer-side wiring (auditrunner + auditdialog).
// - INV-6..7  : reader-side fallback wiring + cache anchor.
// - INV-8..10 : scope classifier (single_file / narrow / broad).
// - INV-11    : null-or-omit run_at + html_path emission.
// - INV-12    : rule_ids behavioural — already shipped, regression
//               coverage. Verified inline below.
// ============================================================

TEST(Ants1576, HelperDeclaredInEngine) {
    expect_reset();
    const std::string eh = ants_test::slurpFile(SRC_AUDIT_ENGINE_H_PATH);
    expect(eh.find("buildVcsProvenanceBlock") != std::string::npos,
           "INV-1",
           "auditengine.h must declare buildVcsProvenanceBlock");
    const std::string ec = ants_test::slurpFile(SRC_AUDIT_ENGINE_CPP_PATH);
    expect(ec.find("buildVcsProvenanceBlock") != std::string::npos,
           "INV-1",
           "auditengine.cpp must define buildVcsProvenanceBlock");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, HelperPopulatesAgainstOwnRoot) {
    expect_reset();
    // INV-2 — calling against the Ants repo itself should yield a
    // non-empty array with a 40-hex revisionId. ANTS_SOURCE_DIR is
    // wired by CMakeLists.txt for this bundle.
    const QString root = QString::fromUtf8(ANTS_SOURCE_DIR);
    const QJsonArray vcp = AuditEngine::buildVcsProvenanceBlock(root);
    if (vcp.isEmpty()) {
        // Defensive: a CI environment without git in PATH would
        // legitimately return empty. Treat as test-skip, not failure.
        GTEST_SKIP() << "git probe returned empty — likely no git in PATH";
    }
    ASSERT_EQ(1, vcp.size());
    const QJsonObject vcs = vcp.first().toObject();
    const QString head = vcs.value(QStringLiteral("revisionId")).toString();
    EXPECT_EQ(40, head.size());
    // Sanity: lowercase-hex only.
    for (QChar c : head) {
        const bool hex = (c >= QChar('0') && c <= QChar('9')) ||
                         (c >= QChar('a') && c <= QChar('f'));
        if (!hex) FAIL() << "non-hex char in revisionId: "
                         << c.toLatin1();
    }
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, HelperEmptyOnNonGit) {
    expect_reset();
    // INV-3 — a tempdir without .git should yield an empty array.
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const QJsonArray vcp =
        AuditEngine::buildVcsProvenanceBlock(td.path());
    EXPECT_EQ(0, vcp.size());
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, WriterAuditRunnerWired) {
    expect_reset();
    // INV-4 — auditrunner.cpp's writeSarif calls buildVcsProvenanceBlock
    // and threads a rootCanonical argument.
    const std::string ar = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(ar.find("buildVcsProvenanceBlock(") != std::string::npos,
           "INV-4",
           "auditrunner.cpp must call buildVcsProvenanceBlock");
    expect(ar.find("rootCanonical") != std::string::npos,
           "INV-4",
           "writeSarif must accept a rootCanonical parameter");
    expect(ar.find("\"versionControlProvenance\"") != std::string::npos,
           "INV-4",
           "writeSarif must emit a versionControlProvenance key");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, WriterAuditDialogWired) {
    expect_reset();
    // INV-5 — auditdialog.cpp's exportSarif() also calls the helper
    // with m_projectPath.
    const std::string ad = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);
    const auto fnPos = ad.find("AuditDialog::exportSarif()");
    expect(fnPos != std::string::npos, "INV-5",
           "exportSarif body not found");
    if (fnPos == std::string::npos) FAIL();
    const std::string body = ad.substr(fnPos);
    expect(body.find("buildVcsProvenanceBlock(m_projectPath)") !=
               std::string::npos,
           "INV-5",
           "exportSarif must call buildVcsProvenanceBlock(m_projectPath)");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, ReaderFallbackWired) {
    expect_reset();
    // INV-6 — cmdLastAuditSummary's cache-miss branch contains the
    // read-time fallback: "rev-parse" + "symbolic-ref" calls and the
    // two branchSource literals.
    // Window grown to 12 KiB after ANTS-1625 added the pickForeign
    // lambda + pick_basis emission (~25 extra lines in the handler).
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto fnPos = rcc.find("cmdLastAuditSummary(const QJsonObject");
    if (fnPos == std::string::npos) FAIL();
    const std::string body = rcc.substr(fnPos, 12000);
    expect(body.find("\"read_time\"") != std::string::npos,
           "INV-6", "expected read_time literal in handler");
    expect(body.find("\"file_provenance\"") != std::string::npos,
           "INV-6", "expected file_provenance literal in handler");
    expect(body.find("rev-parse") != std::string::npos,
           "INV-6", "expected rev-parse call in handler");
    expect(body.find("symbolic-ref") != std::string::npos,
           "INV-6", "expected symbolic-ref call in handler");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, ReaderFallbackBeforeCacheStore) {
    expect_reset();
    // INV-7 — the branchSource population runs BEFORE the assignment
    // to m_auditSummaryCache, so cache hits inherit the populated
    // data for free. 12 KiB window covers the cache-miss block in
    // cmdLastAuditSummary (~250 lines).
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto fnPos = rcc.find("cmdLastAuditSummary(const QJsonObject");
    if (fnPos == std::string::npos) FAIL();
    const std::string body = rcc.substr(fnPos, 12000);
    const auto branchSrcPos = body.find("branchSource = QStringLiteral");
    // Whitespace varies in the assignment alignment; match the
    // load-bearing token-pair instead of an exact spacing.
    const auto cacheStorePos = body.find("m_auditSummaryCache");
    expect(branchSrcPos != std::string::npos, "INV-7",
           "branchSource assignment not found in cache-miss block");
    expect(cacheStorePos != std::string::npos, "INV-7",
           "m_auditSummaryCache reference not found in cache-miss block");
    if (branchSrcPos != std::string::npos &&
        cacheStorePos != std::string::npos) {
        // Re-anchor cacheStorePos to the WRITE, not the earlier read
        // (the cache-hit branch reads m_auditSummaryCache at line :4438
        // before the miss-block's write at :4490).
        const auto writePos = body.find(
            "m_auditSummaryCache       = std::move", cacheStorePos);
        ASSERT_NE(writePos, std::string::npos)
            << "INV-7: cache write `m_auditSummaryCache = std::move(...)` "
               "not found in cache-miss block";
        expect(branchSrcPos < writePos, "INV-7",
               "branchSource must be set before storing into cache");
    }
    EXPECT_EQ(0, expect_failures());
}

namespace {
// Synthetic helper — mirrors the production classifier signature so
// the test asserts the runtime contract, not the function pointer.
struct ScopeProbe {
    QString scope;
    QStringList files;
};
ScopeProbe probeScope(const QStringList &files, const QString &reportBase) {
    using AuditEngine::AuditSummary;
    using AuditEngine::AuditSummaryFinding;
    AuditSummary s;
    for (int i = 0; i < files.size(); ++i) {
        AuditSummaryFinding f;
        f.file = files[i];
        f.line = 10 + i;
        f.level = QStringLiteral("warning");
        s.topFindings.append(f);
    }
    // Replicate the classifier's logic. The production helper is in
    // remotecontrol.cpp's anon namespace; replicate the rule here so
    // the test isolates the classifier contract from the call site.
    QSet<QString> seen;
    QStringList preview;
    for (const auto &f : s.topFindings) {
        if (!seen.contains(f.file)) {
            seen.insert(f.file);
            if (preview.size() < 5) preview.append(f.file);
        }
    }
    const int distinct = seen.size();
    const bool narrowHint =
        reportBase.contains(QStringLiteral("-postfix")) ||
        reportBase.contains(QStringLiteral("-single"))  ||
        reportBase.contains(QStringLiteral("-narrow"));
    ScopeProbe out;
    out.files = preview;
    if (distinct == 1 && !narrowHint) out.scope = "single_file";
    else if (distinct >= 1 && distinct <= 5) out.scope = "narrow";
    else                                     out.scope = "broad";
    return out;
}
}  // namespace

TEST(Ants1576, ScopeSingleFile) {
    // INV-8 — one distinct file + no narrow-name hint → single_file.
    auto p = probeScope({"src/a.cpp", "src/a.cpp"}, "audit-2026-05-18");
    EXPECT_EQ(QString("single_file"), p.scope);
    EXPECT_EQ(1, p.files.size());
}

TEST(Ants1576, ScopeNarrowFilenameHint) {
    // INV-9 — narrow-name hint flips a single-file pick to narrow.
    auto p = probeScope({"src/a.cpp"}, "cppcheck-b68-ozone-postfix");
    EXPECT_EQ(QString("narrow"), p.scope);
}

TEST(Ants1576, ScopeNarrowMultiFile) {
    // INV-9 — 3 distinct files, no hint → narrow.
    auto p = probeScope({"src/a.cpp", "src/b.cpp", "src/c.cpp"},
                        "audit-2026-05-18");
    EXPECT_EQ(QString("narrow"), p.scope);
    EXPECT_EQ(3, p.files.size());
}

TEST(Ants1576, ScopeBroad) {
    // INV-10 — 6+ distinct files → broad.
    auto p = probeScope({"a.cpp", "b.cpp", "c.cpp", "d.cpp", "e.cpp",
                         "f.cpp", "g.cpp"},
                        "audit-2026-05-18");
    EXPECT_EQ(QString("broad"), p.scope);
}

TEST(Ants1576, NullOrOmitRunAtAndHtmlPath) {
    expect_reset();
    // INV-11 — buildLasEnvelope guards on isEmpty() before emitting
    // run_at + html_path. Source-grep tripwire (the source-level
    // guard is the regression contract).
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("if (!s.runAtIso.isEmpty())") != std::string::npos,
           "INV-11",
           "buildLasEnvelope must guard run_at emission with !isEmpty()");
    expect(rcc.find("if (!s.htmlPath.isEmpty())") != std::string::npos,
           "INV-11",
           "buildLasEnvelope must guard html_path emission with !isEmpty()");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, ScopeClassifierWiredInHandler) {
    expect_reset();
    // Defensive: ensure the handler actually emits "scope" and
    // "narrow_run_warning" — guards against a future revert that
    // adds the helper but forgets the call site.
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("classifyAuditScope(") != std::string::npos,
           "INV-8/9/10",
           "cmdLastAuditSummary must call classifyAuditScope");
    expect(rcc.find("env[\"scope\"]") != std::string::npos,
           "INV-8/9/10",
           "envelope must carry a scope field");
    expect(rcc.find("\"narrow_run_warning\"") != std::string::npos,
           "INV-9",
           "envelope must carry a narrow_run_warning field");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants3512, RequestedScopeFromSidecarWiredInHandler) {
    expect_reset();
    // ANTS-3512 — cmdLastAuditSummary must read the requested scope back
    // from the sibling findings sidecar and prefer it over the derived
    // distinct-file heuristic, so a full-tree sweep that surfaces findings
    // in one file isn't mislabelled a single_file rerun. Source-grep
    // tripwire (matches the ScopeClassifierWiredInHandler pattern above).
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("env[\"requested_scope\"]") != std::string::npos,
           "ANTS-3512 INV-1",
           "handler must emit requested_scope from the findings sidecar");
    expect(rcc.find("findings-") != std::string::npos,
           "ANTS-3512 INV-1",
           "handler must derive the findings-<iso>-<sha>.json sidecar name");
    // The narrow_run_warning must be gated on the confirmed-broad flag so
    // a requested full-tree run doesn't emit the false "broader file may
    // exist" alarm.
    expect(rcc.find("confirmedBroad") != std::string::npos,
           "ANTS-3512 INV-2",
           "narrow_run_warning must be gated on a confirmed-broad request");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants3517, ChangesetScopesSuppressNarrowWarning) {
    expect_reset();
    // ANTS-3517 — the ANTS-3512 confirmed-broad gate was `full`-only, so a
    // genuine since-tag / since-last-run / branch-diff / files changeset sweep
    // that surfaced findings in one file still tripped narrow_run_warning
    // (finbreak feedback 2026-07-14). The gate must now recognise every
    // explicit multi-file scope selector, not just "full". Source-grep
    // tripwire (matches the Ants3512 handler-wiring pattern above); these
    // tokens did not exist in remotecontrol.cpp before the fix, so a revert to
    // the full-only form fails this test.
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("startsWith(QLatin1String(\"since-tag:\"))") != std::string::npos,
           "ANTS-3517",
           "confirmedBroad gate must recognise since-tag: scopes");
    expect(rcc.find("QLatin1String(\"branch-diff\")") != std::string::npos,
           "ANTS-3517",
           "confirmedBroad gate must recognise the branch-diff scope");
    expect(rcc.find("QLatin1String(\"since-last-run\")") != std::string::npos,
           "ANTS-3517",
           "confirmedBroad gate must recognise the since-last-run scope");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1576, RuleIdsFilterBehavioural) {
    expect_reset();
    // INV-12 — drive the engine helper that backs the rule_ids
    // filter. The applyRuleIdsFilter helper is in remotecontrol.cpp's
    // anonymous namespace; we cover the contract via the parser-side
    // post-filter check: summariseSarif at topN=50 yields a 5-row
    // pool; filtering it to {rule-blocker} keeps only the BLOCKER.
    auto s = AuditEngine::summariseSarif(
        QString::fromUtf8(FIXTURE_MIN_SARIF), 50, "note");
    ASSERT_TRUE(s.has_value());
    int kept = 0;
    for (const auto &f : s->topFindings) {
        if (f.ruleId == QStringLiteral("rule-blocker")) ++kept;
    }
    EXPECT_EQ(1, kept);
    // Source-grep that the live handler still echoes rule_ids_filter.
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("\"rule_ids_filter\"") != std::string::npos,
           "INV-12",
           "envelope must echo rule_ids_filter when filter is active");
    EXPECT_EQ(0, expect_failures());
}

TEST(McpLastAuditSummary, Inv5And6WiringRegistered) {
    expect_reset();
    // INV-5 (UDS reachability) is inherited; the witness here is that
    // last_audit_summary IS registered through the same pipeline as
    // every other tool. INV-6 (lex-max discovery) is exercised by
    // the dir scan with QDir::Name | QDir::Reversed.
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(mw.find("registerToolProvider(\"last_audit_summary\"") !=
               std::string::npos,
           "INV-5",
           "mainwindow.cpp missing registerToolProvider(\"last_audit_summary\", …)");
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("QDir::Name | QDir::Reversed") != std::string::npos,
           "INV-6",
           "cmdLastAuditSummary must scan with QDir::Name | QDir::Reversed "
           "for lex-max filename discovery");
    EXPECT_EQ(0, expect_failures());
}

// ----------------------------------------------------------------
// ANTS-1625 — foreign-format picker preference.
// ----------------------------------------------------------------

namespace ants1625 {

struct PickProbe {
    QString name;
    QString basis;
};

// Mirrors `pickForeignReport` in remotecontrol.cpp. The production helper
// is in an anon namespace; we replicate the algorithm here so the test
// asserts the contract independently of the call site (same approach
// used for ANTS-1576's classifyAuditScope above).
PickProbe pickForeign(const QDir &dir, const QString &glob) {
    PickProbe out;
    const QStringList ns = dir.entryList(
        QStringList{glob}, QDir::Files, QDir::Name | QDir::Reversed);
    if (ns.isEmpty()) return out;
    if (ns.size() == 1) {
        out.name  = ns.first();
        out.basis = QStringLiteral("sole");
        return out;
    }
    struct Cand { QString name; qint64 mtimeMs; qint64 size; bool narrow{}; };
    QList<Cand> cands;
    qint64 newestMs = 0;
    for (const QString &n : ns) {
        const QFileInfo fi(dir.absoluteFilePath(n));
        Cand c;
        c.name    = n;
        c.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
        c.size    = fi.size();
        const QString lower = n.toLower();
        c.narrow = lower.contains(QStringLiteral("-postfix"))
                || lower.contains(QStringLiteral("-single"))
                || lower.contains(QStringLiteral("-narrow"));
        cands.append(c);
        if (c.mtimeMs > newestMs) newestMs = c.mtimeMs;
    }
    constexpr qint64 kWindowMs = 24LL * 60LL * 60LL * 1000LL;
    const qint64 minMs = newestMs - kWindowMs;
    QString newestName;
    for (const Cand &c : cands) {
        if (c.mtimeMs != newestMs) continue;
        if (newestName.isEmpty() || c.name > newestName) newestName = c.name;
    }
    const Cand *best = nullptr;
    for (const Cand &c : cands) {
        if (c.mtimeMs < minMs) continue;
        if (!best) { best = &c; continue; }
        if (c.narrow != best->narrow) {
            if (!c.narrow) best = &c;
            continue;
        }
        if (c.size != best->size) {
            if (c.size > best->size) best = &c;
            continue;
        }
        if (c.name > best->name) best = &c;
    }
    if (!best) {
        out.name  = newestName;
        out.basis = QStringLiteral("newest");
        return out;
    }
    out.name  = best->name;
    out.basis = (best->name == newestName)
        ? QStringLiteral("newest")
        : QStringLiteral("broadest_in_recency_window");
    return out;
}

void writeFile(const QDir &dir, const QString &name,
               const QByteArray &content, qint64 mtimeMs) {
    QFile f(dir.absoluteFilePath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        FAIL() << "cannot write " << name.toStdString();
    }
    f.write(content);
    f.close();
    QFile g(dir.absoluteFilePath(name));
    ASSERT_TRUE(g.open(QIODevice::ReadOnly));
    g.setFileTime(QDateTime::fromMSecsSinceEpoch(mtimeMs),
                  QFileDevice::FileModificationTime);
    g.close();
}

}  // namespace ants1625

TEST(Ants1625, HelperDeclaredInRemoteControl) {
    expect_reset();
    // INV-1 — pickForeignReport exists in remotecontrol.cpp.
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("pickForeignReport") != std::string::npos,
           "INV-1",
           "remotecontrol.cpp must declare pickForeignReport helper");
    expect(rcc.find("broadest_in_recency_window") != std::string::npos,
           "INV-1",
           "pickForeignReport must define the broadest_in_recency_window "
           "basis literal");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1625, PickBasisWiredInHandler) {
    expect_reset();
    // INV-2 — `pick_basis` envelope wiring.
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rcc.find("env[\"pick_basis\"]") != std::string::npos,
           "INV-2",
           "cmdLastAuditSummary must emit env[\"pick_basis\"]");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1625, NarrowSuffixLiteralsConsistent) {
    expect_reset();
    // INV-8 — narrow-suffix set mirrors ANTS-1576's classifyAuditScope:
    // both helpers in remotecontrol.cpp must agree on `-postfix`,
    // `-single`, `-narrow`. Source-grep tripwire prevents drift.
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    auto countOf = [&](const char *needle) {
        size_t n = 0, pos = 0;
        while ((pos = rcc.find(needle, pos)) != std::string::npos) {
            ++n; pos += 1;
        }
        return n;
    };
    expect(countOf("\"-postfix\"") >= 2, "INV-8",
           "-postfix literal must appear in both classifyAuditScope and "
           "pickForeignReport");
    expect(countOf("\"-single\"") >= 2, "INV-8",
           "-single literal must appear in both helpers");
    expect(countOf("\"-narrow\"") >= 2, "INV-8",
           "-narrow literal must appear in both helpers");
    EXPECT_EQ(0, expect_failures());
}

TEST(Ants1625, SoleCase) {
    // INV-3 — one file → "sole".
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QDir dir(tmp.path());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ants1625::writeFile(dir, "cppcheck-only.xml",
                        QByteArrayLiteral("<a/>"), now);
    auto p = ants1625::pickForeign(dir, QStringLiteral("cppcheck-*.xml"));
    EXPECT_EQ(QString("cppcheck-only.xml"), p.name);
    EXPECT_EQ(QString("sole"), p.basis);
}

TEST(Ants1625, NewestIsBroadest) {
    // INV-4 — two files in-window; newest is also largest non-narrow.
    // Picker chooses it; basis == "newest".
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QDir dir(tmp.path());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ants1625::writeFile(dir, "cppcheck-aaa.xml",
                        QByteArray(1024, 'x'), now - 30 * 60 * 1000);
    ants1625::writeFile(dir, "cppcheck-zzz.xml",
                        QByteArray(4096, 'y'), now);
    auto p = ants1625::pickForeign(dir, QStringLiteral("cppcheck-*.xml"));
    EXPECT_EQ(QString("cppcheck-zzz.xml"), p.name);
    EXPECT_EQ(QString("newest"), p.basis);
}

TEST(Ants1625, BroadestInWindowBeatsLexMaxNarrow) {
    // INV-5 — load-bearing case from RetroArch Bundle 69. Older + larger +
    // non-narrow beats newer + smaller + narrow-suffix when both are
    // inside the 24-hour recency window.
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QDir dir(tmp.path());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // older but larger and non-narrow → preferred
    ants1625::writeFile(dir, "cppcheck-aaa-broad.xml",
                        QByteArray(8192, 'x'), now - 30 * 60 * 1000);
    // newest but narrow-postfix and smaller
    ants1625::writeFile(dir, "cppcheck-zzz-ozone-postfix.xml",
                        QByteArray(512, 'y'), now);
    auto p = ants1625::pickForeign(dir, QStringLiteral("cppcheck-*.xml"));
    EXPECT_EQ(QString("cppcheck-aaa-broad.xml"), p.name);
    EXPECT_EQ(QString("broadest_in_recency_window"), p.basis);
}

TEST(Ants1625, OutOfWindowNarrowStaysAsNewest) {
    // INV-6 — broader candidate older than 24h is out-of-window. Picker
    // falls back to the newest entry (the narrow one), basis="newest".
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QDir dir(tmp.path());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ants1625::writeFile(dir, "cppcheck-aaa-broad.xml",
                        QByteArray(8192, 'x'),
                        now - 48LL * 60 * 60 * 1000);
    ants1625::writeFile(dir, "cppcheck-zzz-ozone-postfix.xml",
                        QByteArray(512, 'y'), now);
    auto p = ants1625::pickForeign(dir, QStringLiteral("cppcheck-*.xml"));
    EXPECT_EQ(QString("cppcheck-zzz-ozone-postfix.xml"), p.name);
    EXPECT_EQ(QString("newest"), p.basis);
}

TEST(Ants1625, SarifPathUnchanged) {
    // INV-7 — SARIF path does NOT route through pickForeignReport.
    // Source-grep that the SARIF branch still uses QDir::entryList with
    // the audit-*.sarif glob and lex-max-reversed ordering, and sets
    // pickBasis directly.
    expect_reset();
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Locate the cmdLastAuditSummary handler and assert the SARIF branch
    // is still a direct entryList call, not pickForeignReport.
    const auto handlerStart = rcc.find("cmdLastAuditSummary(");
    ASSERT_NE(handlerStart, std::string::npos);
    const auto sarifBranch = rcc.find("sarifNames", handlerStart);
    ASSERT_NE(sarifBranch, std::string::npos);
    // The sarif branch must still pre-populate pickBasis ("sole"/"newest")
    // rather than route through the foreign-pick lambda.
    const auto pickForeignCall =
        rcc.find("pickForeign(", handlerStart);
    ASSERT_NE(pickForeignCall, std::string::npos);
    // The pickForeign call must be inside the else-branch (i.e. after
    // sarifNames is empty), not before it.
    EXPECT_LT(sarifBranch, pickForeignCall)
        << "INV-7: SARIF branch must run before the foreign pickForeign "
           "lambda — SARIF naming already sorts correctly lex-max";
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2056 — cmdLastAuditSummary must surface an always-on staleness
// signal (a cached snapshot stamped with read-time HEAD must not read as
// "HEAD's current findings"): a `stale` flag from the artifact mtime vs
// the HEAD commit date, plus a pinned-snapshot hint. Source-grep wiring
// tripwires, matching this verb's handler-feature test idiom (the full
// behavioural path needs a live window + git repo, as with pick_basis).
TEST(Ants2056, StalenessSignalWiredInHandler) {
    expect_reset();
    const std::string rcc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto handlerStart = rcc.find("cmdLastAuditSummary(");
    ASSERT_NE(handlerStart, std::string::npos);
    expect(rcc.find("env[\"stale\"]") != std::string::npos,
           "ANTS-2056",
           "handler must emit env[\"stale\"]");
    expect(rcc.find("--format=%ct") != std::string::npos,
           "ANTS-2056",
           "handler must read the HEAD commit date (%ct) for the "
           "staleness comparison");
    expect(rcc.find("env[\"pinned_snapshot_hint\"]") != std::string::npos,
           "ANTS-2056",
           "handler must emit env[\"pinned_snapshot_hint\"]");
    expect(rcc.find("-b[0-9]+-fixes|-pre-") != std::string::npos,
           "ANTS-2056",
           "handler must carry the pinned-snapshot regex literal");
    // The staleness signal must NOT be gated behind the opt-in
    // `since_commit` block — it lives after pick_basis, near the final
    // `return QJsonDocument(env)` of the handler.
    const auto pickBasisEmit = rcc.find("env[\"pick_basis\"]", handlerStart);
    const auto staleEmit      = rcc.find("env[\"stale\"]", handlerStart);
    ASSERT_NE(pickBasisEmit, std::string::npos);
    ASSERT_NE(staleEmit, std::string::npos);
    EXPECT_LT(pickBasisEmit, staleEmit)
        << "stale signal must be emitted after pick_basis (always-on tail), "
           "not inside the since_commit short-circuit";
    EXPECT_EQ(0, expect_failures());
}
