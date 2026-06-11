// Source-grep harness for ANTS-1569 — locks the wiring contract for
// the `current_state` MCP aggregator. See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "../../_support/expect.h"

#include <cstdio>
#include <fstream>
#include <regex>
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

// Extract a function body by name. Returns the substring from the
// declaration up to (but not including) the next top-level function
// declaration or end-of-file. Crude but adequate for source-grep.
std::string extractFunctionBody(const std::string &src,
                                const std::string &declarationStart) {
    const auto pos = src.find(declarationStart);
    if (pos == std::string::npos) return {};
    // Find the next "^QJsonDocument RemoteControl::cmd" header AFTER
    // this one. Use it as the right-bound.
    auto end = src.find("\nQJsonDocument RemoteControl::cmd", pos + declarationStart.size());
    if (end == std::string::npos) end = src.size();
    return src.substr(pos, end - pos);
}

}  // namespace

TEST(McpCurrentState, WiringContract) {
    expect_reset();

    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdCurrentState(const QJsonObject &req)"),
           "INV-1",
           "cmdCurrentState decl missing from src/remotecontrol.h "
           "(must be declared public alongside cmdLastAuditSummary)");

    // INV-2 — definition + ANTS-1569 anchor in remotecontrol.cpp.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdCurrentState(");
    expect(!body.empty(),
           "INV-2a",
           "cmdCurrentState body missing from src/remotecontrol.cpp");
    expect(contains(body, "ANTS-1569"),
           "INV-2b",
           "cmdCurrentState body must carry an ANTS-1569 anchor "
           "(comment-pin to the spec)");

    // INV-3 — delegation to the three upstream verbs.
    expect(contains(body, "cmdRoadmapQuery"),
           "INV-3a",
           "cmdCurrentState must delegate to cmdRoadmapQuery");
    expect(contains(body, "cmdGitState"),
           "INV-3b",
           "cmdCurrentState must delegate to cmdGitState");
    expect(contains(body, "cmdLastAuditSummary"),
           "INV-3c",
           "cmdCurrentState must delegate to cmdLastAuditSummary");

    // INV-4 — MCP-only: no `cmd == "current-state"` branch in
    // RemoteControl::dispatch.
    expect(!contains(rcCpp, "\"current-state\""),
           "INV-4",
           "current_state must NOT have an IPC dispatch branch — "
           "mirrors last_audit_summary (MCP-only)");

    // INV-5 — provider registration in mainwindow.cpp.
    {
        // Look for the registerToolProvider("current_state", … lambda
        // that delegates to cmdCurrentState.
        std::regex providerRe(R"(registerToolProvider\(\s*"current_state")");
        expect(std::regex_search(mwCpp, providerRe),
               "INV-5a",
               "MainWindow::setupClaudeMcpProviders must register "
               "\"current_state\" via registerToolProvider");
        expect(contains(mwCpp, "cmdCurrentState"),
               "INV-5b",
               "MainWindow provider lambda must delegate to "
               "m_remoteControl->cmdCurrentState");
        expect(contains(mwCpp, "kRcUnavailable"),
               "INV-5c",
               "MainWindow provider lambda must fall back to "
               "kRcUnavailable when m_remoteControl is null");
    }

    // INV-6 — tools/list registration in claudeintegration.cpp.
    expect(contains(ciCpp, "\"current_state\""),
           "INV-6a",
           "claudeintegration.cpp must register a \"current_state\" "
           "tools/list entry");
    expect(contains(ciCpp, "makeCallerCwdReadProp") &&
           contains(ciCpp, "makeEtagMatchProp"),
           "INV-6b",
           "current_state schema must use the shared "
           "makeCallerCwdReadProp + makeEtagMatchProp helpers");

    // INV-7 — Required caller_cwd contract.
    {
        std::regex contractRe(
            R"(toolName == QStringLiteral\("current_state"\)\)\s*return C::Required)");
        expect(std::regex_search(ciCpp, contractRe),
               "INV-7",
               "callerCwdContractFor must classify \"current_state\" "
               "as C::Required (declarative parity with siblings)");
    }

    // INV-8 — isEtagSupportedTool opt-in.
    expect(contains(ciCpp,
                    "QStringLiteral(\"current_state\")"),
           "INV-8a",
           "current_state must appear as a QStringLiteral in "
           "claudeintegration.cpp");
    {
        // Find the isEtagSupportedTool body and assert current_state
        // shows up inside it.
        const auto pos = ciCpp.find("isEtagSupportedTool(const QString &toolName)");
        expect(pos != std::string::npos,
               "INV-8b-locate",
               "isEtagSupportedTool body not found");
        if (pos != std::string::npos) {
            // Body runs until the closing }.
            const auto bodyEnd = ciCpp.find("\n}\n", pos);
            const std::string etagBody = ciCpp.substr(pos,
                bodyEnd == std::string::npos ? ciCpp.size() - pos : bodyEnd - pos);
            expect(contains(etagBody, "current_state"),
                   "INV-8c",
                   "isEtagSupportedTool must return true for "
                   "\"current_state\" (ANTS-1499 304 opt-in)");
        }
    }

    // INV-9 — tools/list description names the four upstream sources.
    {
        // Locate the description block. Use a sliding window: find
        // the "current_state" entry and inspect the surrounding
        // description literal.
        const auto pos = ciCpp.find("csTool[\"name\"] = \"current_state\"");
        expect(pos != std::string::npos,
               "INV-9-locate",
               "current_state tools/list registration block not found");
        if (pos != std::string::npos) {
            // Description should appear in the next 2 KiB after the
            // name assignment.
            const std::string desc = ciCpp.substr(pos,
                std::min<std::size_t>(2048, ciCpp.size() - pos));
            expect(contains(desc, "roadmap_query"),
                   "INV-9a",
                   "current_state description must name roadmap_query");
            expect(contains(desc, "git_state"),
                   "INV-9b",
                   "current_state description must name git_state");
            expect(contains(desc, "last_audit_summary"),
                   "INV-9c",
                   "current_state description must name last_audit_summary");
            expect(contains(desc, "workflow.md"),
                   "INV-9d",
                   "current_state description must name "
                   ".claude/workflow.md");
        }
    }

    // INV-10 — `ok:true` for every successful call; only `no_project`
    // refusal code in the body.
    expect(contains(body, "csErr(QStringLiteral(\"no_project\")"),
           "INV-10a",
           "cmdCurrentState must emit csErr with \"no_project\" code "
           "on resolveRootCanonical failure");
    {
        // Count csErr call sites in the body. Only the no_window
        // + no_project pair is allowed (no other refusal codes).
        std::regex csErrCall(R"(csErr\(QStringLiteral\(\"([^\"]+)\"\))");
        auto begin = std::sregex_iterator(body.begin(), body.end(), csErrCall);
        auto end   = std::sregex_iterator();
        std::vector<std::string> codes;
        for (auto it = begin; it != end; ++it) codes.push_back((*it)[1].str());
        // Expected: at most {no_window, no_project}. Any other code
        // means an upstream-failure escalation snuck in.
        for (const auto &c : codes) {
            const bool ok = (c == "no_window" || c == "no_project");
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "cmdCurrentState emits unexpected refusal "
                          "code \"%s\" — only no_window / no_project "
                          "are allowed (INV-14 ok:true preservation)",
                          c.c_str());
            expect(ok, "INV-10b", detail);
        }
    }
}
