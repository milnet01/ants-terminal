// Source-grep harness for ANTS-1300 — locks the wiring contract for
// the `test_results` MCP tool. See spec.md.
//
// Exit 0 = all 12 invariants hold.

#include "../../_support/expect.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_TESTRESCACHE_H_PATH
#error "SRC_TESTRESCACHE_H_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

std::string extractFunctionBody(const std::string &src,
                                const std::string &declarationStart) {
    const auto pos = src.find(declarationStart);
    if (pos == std::string::npos) return {};
    auto end = src.find("\nQJsonDocument RemoteControl::cmd",
                        pos + declarationStart.size());
    if (end == std::string::npos) end = src.size();
    return src.substr(pos, end - pos);
}

}  // namespace

TEST(McpTestResults, WiringContract) {
    expect_reset();

    const std::string trcHdr = ants_test::slurpFile(SRC_TESTRESCACHE_H_PATH);
    const std::string rcHdr  = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp  = ants_test::slurpRemoteControl();
    const std::string ciCpp  = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp  = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — TestResCache namespace API.
    expect(contains(trcHdr, "QString cachePath("),
           "INV-1a",
           "TestResCache::cachePath decl missing from testrescache.h");
    expect(contains(trcHdr, "ParsedTests parseCtestOutput("),
           "INV-1b",
           "TestResCache::parseCtestOutput decl missing");
    expect(contains(trcHdr, "bool recordTests("),
           "INV-1c",
           "TestResCache::recordTests decl missing");
    expect(contains(trcHdr, "loadTests("),
           "INV-1d",
           "TestResCache::loadTests decl missing");
    expect(contains(trcHdr, "toJsonWire"),
           "INV-1e",
           "TestResCache::toJsonWire decl missing (wire envelope "
           "strips full_excerpt — INV-18 of ANTS-1300 spec)");

    // INV-2 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdTestResults(const QJsonObject &req)"),
           "INV-2",
           "cmdTestResults decl missing from src/remotecontrol.h");

    // INV-3 — definition + ANTS-1300 anchor.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdTestResults(");
    expect(!body.empty(),
           "INV-3a",
           "cmdTestResults body missing from the remotecontrol TUs");
    expect(contains(rcCpp, "ANTS-1300"),
           "INV-3b",
           "cmdTestResults section must carry an ANTS-1300 anchor");

    // INV-4 — op dispatch + bad_args + detail-on-record refusal.
    expect(contains(body, "\"record\""),
           "INV-4a",
           "cmdTestResults must dispatch on op:\"record\"");
    expect(contains(body, "\"read\""),
           "INV-4b",
           "cmdTestResults must dispatch on op:\"read\"");
    expect(contains(body, "bad_args"),
           "INV-4c",
           "cmdTestResults must surface code:bad_args");
    expect(contains(body, "\"detail\""),
           "INV-4d",
           "cmdTestResults must inspect the detail arg");

    // INV-5 — refusal codes.
    expect(contains(body, "not_cached"),
           "INV-5a",
           "cmdTestResults must refuse missing cache with "
           "code:not_cached");
    expect(contains(body, "detail_not_found"),
           "INV-5b",
           "cmdTestResults must refuse missing detail with "
           "code:detail_not_found");
    expect(contains(body, "write_failed"),
           "INV-5c",
           "cmdTestResults must refuse persistence failure with "
           "code:write_failed");

    // INV-6 — wire-envelope strip + detail full body.
    expect(contains(body, "toJsonWire"),
           "INV-6a",
           "cmdTestResults default-read path must serialise via "
           "toJsonWire (strips full_excerpt)");
    expect(contains(body, "fullExcerpt"),
           "INV-6b",
           "cmdTestResults detail path must return the failing "
           "test's fullExcerpt as excerpt");

    // INV-7 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"test_results\""),
           "INV-7a",
           "MainWindow must register \"test_results\" via "
           "registerToolProvider");
    expect(contains(mwCpp, "cmdTestResults"),
           "INV-7b",
           "MainWindow registration must delegate to "
           "m_remoteControl->cmdTestResults");

    // INV-8 — tools/list schema entry.
    expect(contains(ciCpp, "t[\"name\"] = \"test_results\""),
           "INV-8a",
           "tools/list block must register a \"test_results\" entry");
    {
        const auto anchorPos = ciCpp.find("ANTS-1300 — test_results");
        ASSERT_NE(anchorPos, std::string::npos);
        // Bound the region to the test_results tool block (anchor → next
        // tools.append) rather than a fixed byte budget: the block grows as
        // schema knobs are added (ANTS-2029), and generic props like
        // "etag_match"/"detail" recur in every tool's schema, so a too-small
        // window under-reaches while a too-large one matches the NEXT tool.
        const auto blockEnd = ciCpp.find("tools.append(", anchorPos);
        ASSERT_NE(blockEnd, std::string::npos);
        const std::string region = ciCpp.substr(anchorPos, blockEnd - anchorPos);
        expect(contains(region, "req.append(\"caller_cwd\")"),
               "INV-8b",
               "test_results schema must mark \"caller_cwd\" as "
               "required");
        expect(contains(region, "\"detail\""),
               "INV-8c",
               "test_results schema must declare detail property");
        expect(contains(region, "etag_match"),
               "INV-8d",
               "test_results schema must declare etag_match "
               "property");
        expect(contains(region, "\"duration_ms\""),
               "INV-8e",
               "test_results schema must declare duration_ms");
    }

    // INV-9 — Required contract.
    {
        const auto pos = ciCpp.find(
            "callerCwdContractFor(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        const auto branch = fn.find("\"test_results\"");
        ASSERT_NE(branch, std::string::npos)
            << "test_results must have an explicit branch in "
               "callerCwdContractFor";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = fn.substr(branch, eol - branch);
        expect(line.find("C::Required;") != std::string::npos,
               "INV-9",
               "test_results must be classified C::Required");
    }

    // INV-10 — kindForName "test" bucket.
    {
        const auto pos = ciCpp.find("auto kindForName");
        ASSERT_NE(pos, std::string::npos);
        // ANTS-3368 — brace-matched body, not a fixed byte window. The old
        // `substr(pos, 8000)` put "test_results" at offset 7987, so adding
        // ONE verb to an earlier branch sliced the token in half and the
        // test reported a missing branch that was still there. Widening the
        // budget only moves the next such failure; slurpFunctionBody bounds
        // the window to the lambda itself and cannot drift (ANTS-1348).
        const std::string fn =
            ants_test::slurpFunctionBody(ciCpp, "auto kindForName");
        ASSERT_FALSE(fn.empty())
            << "kindForName's body did not brace-match";
        const auto branch = fn.find("\"test_results\"");
        ASSERT_NE(branch, std::string::npos)
            << "test_results must have an explicit branch in "
               "kindForName";
        const std::string region = fn.substr(branch, 200);
        expect(region.find("\"test\"") != std::string::npos,
               "INV-10",
               "kindForName(\"test_results\") must return \"test\"");
    }

    // INV-11 — isEtagSupportedTool.
    {
        const auto pos = ciCpp.find(
            "isEtagSupportedTool(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        expect(fn.find("\"test_results\"") != std::string::npos,
               "INV-11",
               "isEtagSupportedTool must return true for "
               "\"test_results\"");
    }

    // INV-12 — Token-cost ledger entry.
    expect(contains(ciCpp,
                    "QStringLiteral(\"test_results\"),       {800,  3000}"),
           "INV-12",
           "Token-cost ledger must carry "
           "{QStringLiteral(\"test_results\"), {800, 3000}}");
}
