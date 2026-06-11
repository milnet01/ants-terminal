// Source-grep harness for ANTS-1308 — locks the wiring contract for
// the `invariant_check` MCP tool. See spec.md.
//
// Exit 0 = all 8 invariants hold.

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

TEST(McpInvariantCheck, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdInvariantCheck(const QJsonObject &req)"),
           "INV-1",
           "cmdInvariantCheck decl missing from src/remotecontrol.h");

    // INV-2 — definition + ANTS-1308 anchor.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdInvariantCheck(");
    expect(!body.empty(),
           "INV-2a",
           "cmdInvariantCheck body missing from src/remotecontrol.cpp");
    expect(contains(rcCpp, "ANTS-1308"),
           "INV-2b",
           "cmdInvariantCheck section must carry an ANTS-1308 anchor");

    // INV-3 — bad_files refusal.
    expect(contains(body, "bad_files"),
           "INV-3",
           "cmdInvariantCheck must refuse missing/empty files with "
           "code:bad_files");

    // INV-4 — directory walk via QDir + ANTS-*.md filter.
    expect(contains(body, "QDir"),
           "INV-4a",
           "cmdInvariantCheck must iterate via QDir");
    expect(contains(body, "ANTS-*.md"),
           "INV-4b",
           "cmdInvariantCheck must filter directory entries by "
           "the ANTS-*.md glob");

    // INV-5 — shared parser delegation.
    expect(contains(body, "parseSpecBody"),
           "INV-5",
           "cmdInvariantCheck must delegate parsing to the shared "
           "parseSpecBody helper (single-source the parser)");

    // INV-6 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"invariant_check\""),
           "INV-6a",
           "MainWindow must register \"invariant_check\" via "
           "registerToolProvider");
    expect(contains(mwCpp, "cmdInvariantCheck"),
           "INV-6b",
           "MainWindow registration must delegate to "
           "m_remoteControl->cmdInvariantCheck");

    // INV-7 — tools/list schema entry.
    expect(contains(ciCpp, "t[\"name\"] = \"invariant_check\""),
           "INV-7a",
           "tools/list block must register an \"invariant_check\" "
           "entry");
    {
        const auto anchorPos = ciCpp.find("ANTS-1308");
        ASSERT_NE(anchorPos, std::string::npos);
        const std::string region = ciCpp.substr(anchorPos, 3000);
        expect(contains(region, "req.append(\"files\")"),
               "INV-7b",
               "invariant_check schema must mark \"files\" as required");
        expect(contains(region, "req.append(\"caller_cwd\")"),
               "INV-7c",
               "invariant_check schema must mark \"caller_cwd\" as "
               "required");
        expect(contains(region, "minItems"),
               "INV-7d",
               "invariant_check schema's files array must declare a "
               "minItems constraint (non-empty)");
    }

    // INV-8 — Required contract.
    {
        const auto pos = ciCpp.find(
            "callerCwdContractFor(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        const auto branch = fn.find("\"invariant_check\"");
        ASSERT_NE(branch, std::string::npos)
            << "invariant_check must have an explicit branch in "
               "callerCwdContractFor";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = fn.substr(branch, eol - branch);
        expect(line.find("C::Required;") != std::string::npos,
               "INV-8",
               "invariant_check must be classified C::Required");
    }
}
