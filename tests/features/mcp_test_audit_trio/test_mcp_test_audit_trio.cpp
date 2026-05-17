// ANTS-1397 v1 — source-scrape regression test for the
// test_audit_* MCP trio.

#include "../../_support/expect.h"
#include "testauditengine.h"

#include <gtest/gtest.h>

#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — partition is pure data, no QProcess.
TEST(mcp_test_audit_trio, Inv1PartitionNoShell) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(!contains(cpp, "QProcess"),
           "INV-1: testauditengine.cpp must not spawn QProcess");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — fold-in delegates to RoadmapFoldIn (single batched).
TEST(mcp_test_audit_trio, Inv3FoldInBatchedDelegate) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(contains(cpp, "RoadmapFoldIn::allocateIds(canon, n)"),
           "INV-3: single batched allocateIds(canon, n) call");
    expect(contains(cpp, "RoadmapFoldIn::insertBlock(canon"),
           "INV-3: insertBlock delegated call");
    expect(!contains(cpp, "QSaveFile"),
           "INV-3: engine must not re-implement QSaveFile write");
    expect(!contains(cpp, ".roadmap-counter"),
           "INV-3: engine must not touch .roadmap-counter directly");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — partition_token via qHash (NOT SHA-256).
TEST(mcp_test_audit_trio, Inv4TokenViaQHash) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(contains(cpp, "qHash(callerCwd)"),
           "INV-4: token mixes qHash(callerCwd)");
    expect(contains(cpp, "qHash(scope)"),
           "INV-4: token mixes qHash(scope)");
    expect(!contains(cpp, "QCryptographicHash::Sha256"),
           "INV-4: token MUST NOT use SHA-256 (cargo-cult per "
           "user decision 2026-05-17)");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — all four verbs Optional (none in Required branch).
TEST(mcp_test_audit_trio, Inv5OptionalContract) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Locate the Required block (between "Required — refuse" and
    // "TabSpecific —" comments).
    const auto reqStart = ci.find("Required — refuse with caller_cwd_required");
    const auto reqEnd   = ci.find("TabSpecific —", reqStart);
    ASSERT_NE(reqStart, std::string::npos);
    ASSERT_NE(reqEnd,   std::string::npos);
    const std::string region = ci.substr(reqStart, reqEnd - reqStart);
    expect(!contains(region, "\"test_audit_partition\""),
           "INV-5: test_audit_partition must NOT be Required");
    expect(!contains(region, "\"test_audit_brief\""),
           "INV-5: test_audit_brief must NOT be Required");
    expect(!contains(region, "\"test_audit_synthesis_prompt\""),
           "INV-5: test_audit_synthesis_prompt must NOT be Required");
    expect(!contains(region, "\"test_audit_fold_in\""),
           "INV-5: test_audit_fold_in must NOT be Required");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — dimension list canonical (kDimensions).
TEST(mcp_test_audit_trio, Inv6KDimensions) {
    expect_reset();
    expect(!TestAuditEngine::kDimensions().isEmpty(),
           "INV-6: kDimensions exposes non-empty canonical list");
    expect(TestAuditEngine::kDimensions().size() == 18,
           "INV-6: 18 dimensions per spec");
    expect(TestAuditEngine::kDimensions().contains(
        QStringLiteral("flakiness")),
        "INV-6: 'flakiness' in canonical list");
    expect(TestAuditEngine::kDimensions().contains(
        QStringLiteral("security")),
        "INV-6: 'security' in canonical list");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — pre-pass per-chunk cap 20.
TEST(mcp_test_audit_trio, Inv7PrePassCap) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(contains(cpp, "kPrePassPerChunkCap = 20"),
           "INV-7: cap = 20 per chunk");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — synth fences per-chunk reports.
TEST(mcp_test_audit_trio, Inv8SynthFence) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(contains(cpp, "<chunk_report file="),
           "INV-8: fence opening tag present in synth output");
    expect(contains(cpp, "</chunk_report>"),
           "INV-8: fence closing tag present");
    expect(contains(cpp, "&lt;/chunk_report&gt;"),
           "INV-8: nested fence markers escaped");
    EXPECT_EQ(0, expect_failures());
}

// INV-9 — chunk-size clamp constants.
TEST(mcp_test_audit_trio, Inv9ChunkSizeClamp) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(contains(cpp, "kChunkSizeMin = 4"),
           "INV-9: min = 4");
    expect(contains(cpp, "kChunkSizeMax = 30"),
           "INV-9: max = 30");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — pagination fields in envelope.
TEST(mcp_test_audit_trio, Inv10PaginationFields) {
    expect_reset();
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "env[\"offset\"]    = r.offset"),
           "INV-10: offset echoed in envelope");
    expect(contains(mw, "env[\"truncated\"] = r.truncated"),
           "INV-10: truncated flag");
    expect(contains(mw, "env[\"next_offset\"]"),
           "INV-10: next_offset emitted when truncated");
    EXPECT_EQ(0, expect_failures());
}

// INV-13 — pre_pass_findings JSON shape carries no matched text.
TEST(mcp_test_audit_trio, Inv13PrePassNoMatchedText) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    // Scan the prePassFile body for any "matched" / "text" /
    // "line_content" field assignment. The struct only sets file,
    // line, pattern_id, dimension.
    const auto fnStart = cpp.find("QJsonArray prePassFile(");
    const auto fnEnd   = cpp.find("\n}\n", fnStart);
    ASSERT_NE(fnStart, std::string::npos);
    const std::string body = cpp.substr(fnStart, fnEnd - fnStart);
    expect(contains(body, "o[\"file\"]"),
           "INV-13: file field set");
    expect(contains(body, "o[\"line\"]"),
           "INV-13: line field set");
    expect(contains(body, "o[\"pattern_id\"]"),
           "INV-13: pattern_id field set");
    expect(contains(body, "o[\"dimension\"]"),
           "INV-13: dimension field set");
    expect(!contains(body, "o[\"matched\""),
           "INV-13: NO matched text emission");
    expect(!contains(body, "o[\"line_content\""),
           "INV-13: NO line_content emission");
    EXPECT_EQ(0, expect_failures());
}

// INV-15 — mtime recheck rate-limit constant.
TEST(mcp_test_audit_trio, Inv15RecheckRateLimit) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    expect(contains(cpp, "kMtimeRecheckRateLimitMs = 5'000"),
           "INV-15: 5 s recheck rate-limit constant");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1461 — file_index aggregation dedups by basename so a chunk
// that cites the same file inconsistently (e.g. `test_X.py` and
// `tests/test_X.py`) is counted once, not twice. Source-grep on
// the two-pass merge in testauditengine.cpp::synthesisPrompt.
TEST(mcp_test_audit_trio, Ants1461FileIndexBasenameDedup) {
    expect_reset();
    const std::string cpp = slurp(SRC_TESTAUDITENGINE_CPP_PATH);
    // The merge keeps the longest path per basename (the
    // directory-prefixed form is the canonical display key) and
    // sums counts under that key.
    expect(contains(cpp, "canonicalByBase"),
           "ANTS-1461: file_index aggregation must build a "
           "canonicalByBase map keyed on basename");
    expect(contains(cpp, "mergedRefCount"),
           "ANTS-1461: counts must be summed into a mergedRefCount "
           "table keyed on the canonical (longest) path");
    expect(contains(cpp,
               ".section('/', -1)"),
           "ANTS-1461: basename extraction must use "
           "QString::section('/', -1)");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1461 + ANTS-1487 — test_audit_partition descriptor names what
// pre_pass_dimensions[] actually reflects (pre-pass regex hits, not
// finding density). The field was renamed from dimension_hints to
// pre_pass_dimensions in ANTS-1487 to make the source explicit on
// the name itself. Source-grep on the description string in
// claudeintegration.cpp.
TEST(mcp_test_audit_trio, Ants1461DimensionHintsClarified) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Locate the test_audit_partition descriptor region and
    // confirm the clarifier sentence is present.
    const auto pos = ci.find("t[\"name\"] = \"test_audit_partition\"");
    ASSERT_NE(pos, std::string::npos)
        << "test_audit_partition descriptor missing";
    // Description string is set immediately after the name.
    const std::string region = ci.substr(pos, 4000);
    // The clarifier sentence is wrapped across two source lines —
    // C++ concatenates "NOT a " + "finding-density predictor", but
    // grep sees them with the wrap. Search for the intact tail.
    expect(contains(region, "finding-density predictor"),
           "ANTS-1461: descriptor must warn that pre_pass_dimensions "
           "reflects pre-pass regex hits, NOT finding density");
    expect(contains(region, "pre_pass_dimensions"),
           "ANTS-1487: descriptor must name the pre_pass_dimensions "
           "output field (renamed from dimension_hints)");
    EXPECT_EQ(0, expect_failures());
}

// Schema/dispatch — all four verbs registered.
TEST(mcp_test_audit_trio, AllFourVerbsRegistered) {
    expect_reset();
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "registerToolProvider(\"test_audit_partition\""),
           "dispatch: test_audit_partition registered");
    expect(contains(mw, "registerToolProvider(\"test_audit_brief\""),
           "dispatch: test_audit_brief registered");
    expect(contains(mw, "registerToolProvider(\"test_audit_synthesis_prompt\""),
           "dispatch: test_audit_synthesis_prompt registered");
    expect(contains(mw, "registerToolProvider(\"test_audit_fold_in\""),
           "dispatch: test_audit_fold_in registered");
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "t[\"name\"] = \"test_audit_partition\""),
           "schema: test_audit_partition descriptor");
    expect(contains(ci, "t[\"name\"] = \"test_audit_brief\""),
           "schema: test_audit_brief descriptor");
    expect(contains(ci, "t[\"name\"] = \"test_audit_synthesis_prompt\""),
           "schema: test_audit_synthesis_prompt descriptor");
    expect(contains(ci, "t[\"name\"] = \"test_audit_fold_in\""),
           "schema: test_audit_fold_in descriptor");
    EXPECT_EQ(0, expect_failures());
}
