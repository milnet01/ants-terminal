// Source-grep harness for ANTS-1299 — locks the wiring contract for
// the `build_status` MCP tool. See spec.md.
//
// Exit 0 = all 11 invariants hold.

#include "../../_support/expect.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_BUILDCACHE_H_PATH
#error "SRC_BUILDCACHE_H_PATH compile definition required"
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

TEST(McpBuildStatus, WiringContract) {
    expect_reset();

    const std::string bcHdr = ants_test::slurpFile(SRC_BUILDCACHE_H_PATH);
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — BuildCache namespace API.
    expect(contains(bcHdr, "QString cachePath("),
           "INV-1a",
           "BuildCache::cachePath decl missing from buildcache.h");
    expect(contains(bcHdr, "ParsedBuild parseBuildOutput("),
           "INV-1b",
           "BuildCache::parseBuildOutput decl missing");
    expect(contains(bcHdr, "bool recordBuild("),
           "INV-1c",
           "BuildCache::recordBuild decl missing");
    expect(contains(bcHdr, "loadBuild("),
           "INV-1d",
           "BuildCache::loadBuild decl missing");
    expect(contains(bcHdr, "checkStale("),
           "INV-1e",
           "BuildCache::checkStale decl missing");

    // INV-2 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdBuildStatus(const QJsonObject &req)"),
           "INV-2",
           "cmdBuildStatus decl missing from src/remotecontrol.h");

    // INV-3 — definition + ANTS-1299 anchor.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdBuildStatus(");
    expect(!body.empty(),
           "INV-3a",
           "cmdBuildStatus body missing from src/remotecontrol.cpp");
    expect(contains(rcCpp, "ANTS-1299"),
           "INV-3b",
           "cmdBuildStatus section must carry an ANTS-1299 anchor");

    // INV-4 — op dispatch + bad_args.
    expect(contains(body, "\"record\""),
           "INV-4a",
           "cmdBuildStatus must dispatch on op:\"record\"");
    expect(contains(body, "\"read\""),
           "INV-4b",
           "cmdBuildStatus must dispatch on op:\"read\"");
    expect(contains(body, "bad_args"),
           "INV-4c",
           "cmdBuildStatus must surface code:bad_args for arg "
           "validation failures");

    // INV-5 — not_cached + write_failed refusals.
    expect(contains(body, "not_cached"),
           "INV-5a",
           "cmdBuildStatus must refuse missing cache with "
           "code:not_cached on op=read");
    expect(contains(body, "write_failed"),
           "INV-5b",
           "cmdBuildStatus must refuse persistence failure with "
           "code:write_failed on op=record");

    // INV-6 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"build_status\""),
           "INV-6a",
           "MainWindow must register \"build_status\" via "
           "registerToolProvider");
    expect(contains(mwCpp, "cmdBuildStatus"),
           "INV-6b",
           "MainWindow registration must delegate to "
           "m_remoteControl->cmdBuildStatus");

    // INV-7 — tools/list schema entry.
    expect(contains(ciCpp, "t[\"name\"] = \"build_status\""),
           "INV-7a",
           "tools/list block must register a \"build_status\" entry");
    {
        const auto anchorPos = ciCpp.find("ANTS-1299 — build_status");
        ASSERT_NE(anchorPos, std::string::npos);
        const std::string region = ciCpp.substr(anchorPos, 3500);
        expect(contains(region, "req.append(\"caller_cwd\")"),
               "INV-7b",
               "build_status schema must mark \"caller_cwd\" as "
               "required");
        expect(contains(region, "\"exit_code\""),
               "INV-7c",
               "build_status schema must declare exit_code property");
        expect(contains(region, "\"output\""),
               "INV-7d",
               "build_status schema must declare output property");
        expect(contains(region, "etag_match"),
               "INV-7e",
               "build_status schema must declare etag_match property");
    }

    // INV-8 — Required contract.
    {
        const auto pos = ciCpp.find(
            "callerCwdContractFor(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        const auto branch = fn.find("\"build_status\"");
        ASSERT_NE(branch, std::string::npos)
            << "build_status must have an explicit branch in "
               "callerCwdContractFor";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = fn.substr(branch, eol - branch);
        expect(line.find("C::Required;") != std::string::npos,
               "INV-8",
               "build_status must be classified C::Required");
    }

    // INV-9 — kindForName "build" bucket.
    {
        const auto pos = ciCpp.find("auto kindForName");
        ASSERT_NE(pos, std::string::npos);
        const std::string fn = ciCpp.substr(pos, 5000);
        const auto branch = fn.find("\"build_status\"");
        ASSERT_NE(branch, std::string::npos)
            << "build_status must have an explicit branch in "
               "kindForName";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        // The branch returns "build" in the next 1-2 lines.
        const std::string region = fn.substr(branch, 200);
        expect(region.find("\"build\"") != std::string::npos,
               "INV-9",
               "kindForName(\"build_status\") must return \"build\"");
    }

    // INV-10 — isEtagSupportedTool.
    {
        const auto pos = ciCpp.find(
            "isEtagSupportedTool(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        expect(fn.find("\"build_status\"") != std::string::npos,
               "INV-10",
               "isEtagSupportedTool must return true for "
               "\"build_status\"");
    }

    // INV-11 — Token-cost ledger entry.
    expect(contains(ciCpp,
                    "QStringLiteral(\"build_status\"),       {500,  2000}"),
           "INV-11",
           "Token-cost ledger must carry "
           "{QStringLiteral(\"build_status\"), {500, 2000}}");
}
