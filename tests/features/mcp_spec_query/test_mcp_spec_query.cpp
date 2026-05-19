// Source-grep harness for ANTS-1309 — locks the wiring contract for
// the `spec_query` MCP tool. See spec.md.
//
// Exit 0 = all 8 invariants hold.

#include "../../_support/expect.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

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

TEST(McpSpecQuery, WiringContract) {
    expect_reset();

    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdSpecQuery(const QJsonObject &req)"),
           "INV-1",
           "cmdSpecQuery decl missing from src/remotecontrol.h");

    // INV-2 — definition + ANTS-1309 anchor in remotecontrol.cpp.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdSpecQuery(");
    expect(!body.empty(),
           "INV-2a",
           "cmdSpecQuery body missing from src/remotecontrol.cpp");
    // The anchor lives in the section banner above the function;
    // grep over the whole file rather than just the body.
    expect(contains(rcCpp, "ANTS-1309"),
           "INV-2b",
           "cmdSpecQuery section must carry an ANTS-1309 anchor "
           "(comment-pin to the roadmap entry)");

    // INV-3 — id validation. `bad_id` refusal code present.
    expect(contains(body, "bad_id"),
           "INV-3",
           "cmdSpecQuery must refuse malformed ids with code:bad_id");

    // INV-4 — not_found refusal for missing spec file.
    expect(contains(body, "not_found"),
           "INV-4",
           "cmdSpecQuery must refuse missing files with code:not_found");

    // INV-5 — returns parsed invariants via the shared helper.
    expect(contains(body, "parseSpecBody"),
           "INV-5",
           "cmdSpecQuery must delegate parsing to parseSpecBody helper");

    // INV-6 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"spec_query\""),
           "INV-6a",
           "MainWindow must register \"spec_query\" via "
           "registerToolProvider");
    expect(contains(mwCpp, "cmdSpecQuery"),
           "INV-6b",
           "MainWindow registration must delegate to "
           "m_remoteControl->cmdSpecQuery");

    // INV-7 — tools/list schema entry.
    expect(contains(ciCpp, "t[\"name\"] = \"spec_query\""),
           "INV-7a",
           "tools/list block must register a \"spec_query\" entry");
    // The spec_query registration block must carry both `id` and
    // `caller_cwd` in `required` — look for the block by its
    // unique ANTS-1309 anchor comment, then scan a window.
    {
        const auto anchorPos = ciCpp.find("ANTS-1309");
        ASSERT_NE(anchorPos, std::string::npos);
        const std::string region = ciCpp.substr(anchorPos, 2500);
        expect(contains(region, "req.append(\"id\")"),
               "INV-7b",
               "spec_query schema must mark \"id\" as required");
        expect(contains(region, "req.append(\"caller_cwd\")"),
               "INV-7c",
               "spec_query schema must mark \"caller_cwd\" as required");
    }

    // INV-8 — Required contract.
    {
        const auto pos = ciCpp.find(
            "callerCwdContractFor(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        const auto branch = fn.find("\"spec_query\"");
        ASSERT_NE(branch, std::string::npos)
            << "spec_query must have an explicit branch in "
               "callerCwdContractFor";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = fn.substr(branch, eol - branch);
        expect(line.find("C::Required;") != std::string::npos,
               "INV-8",
               "spec_query must be classified C::Required");
    }
}
