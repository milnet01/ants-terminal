// Source-grep harness for ANTS-1306 — locks the wiring contract and
// load-bearing logic markers for the `task_priors` MCP tool. See
// spec.md and docs/specs/ANTS-1306.md.
//
// task_priors resolves the project root via resolveRootCanonical(
// m_main, req), which dereferences a live MainWindow, so the verb is
// not GUI-free-exercisable — same constraint as current_state /
// invariant_check, hence the source-grep approach.

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

ANTS_TEST_SCOPE();

namespace {


bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Slice cmdTaskPriors's body so logic-marker assertions don't match
// stray text elsewhere in the file.
std::string fnBody(const std::string &src, const char *decl) {
    const auto pos = src.find(decl);
    if (pos == std::string::npos) return {};
    auto end = src.find("\nQJsonDocument RemoteControl::cmd",
                        pos + std::string(decl).size());
    if (end == std::string::npos) end = src.size();
    return src.substr(pos, end - pos);
}

}  // namespace

TEST(McpTaskPriors, WiringAndLogicContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    const std::string body =
        fnBody(rcCpp, "QJsonDocument RemoteControl::cmdTaskPriors(");

    // INV-1 — declaration + definition + anchor.
    expect(has(rcHdr, "cmdTaskPriors(const QJsonObject &req)"),
           "INV-1", "cmdTaskPriors decl missing from remotecontrol.h");
    expect(!body.empty(),
           "INV-1", "cmdTaskPriors definition missing from remotecontrol.cpp");
    expect(has(rcCpp, "ANTS-1306"),
           "INV-1", "ANTS-1306 anchor comment missing");

    // INV-2 — the three extraction regexes + a stopword set.
    expect(has(rcCpp, "ANTS-[0-9]+"),
           "INV-2", "id regex ANTS-[0-9]+ missing");
    expect(has(rcCpp, "cpp|cc|cxx|c|h|hpp|hh|hxx|py|lua|sh|md|json|cmake|txt"),
           "INV-2", "path-extension regex set missing");
    expect(has(rcCpp, "[A-Za-z_][A-Za-z0-9_]{3,}"),
           "INV-2", "term regex missing");
    expect(has(rcCpp, "taskPriorStopwords"),
           "INV-2", "stopword set helper missing");

    // INV-3 — refusal codes.
    expect(has(body, "bad_args"),
           "INV-3", "bad_args refusal missing");
    expect(has(body, "no_project"),
           "INV-3", "no_project refusal missing");

    // INV-4 — composes over cmdRoadmapQuery + cmdGitState.
    expect(has(body, "cmdRoadmapQuery"),
           "INV-4", "does not compose over cmdRoadmapQuery");
    expect(has(body, "include_body"),
           "INV-4", "roadmap query does not request include_body");
    expect(has(body, "500"),
           "INV-4", "roadmap query limit:500 missing");
    expect(has(body, "cmdGitState"),
           "INV-4", "does not compose over cmdGitState");

    // INV-5 — id boost + descending score sort.
    expect(has(body, "score += 5"),
           "INV-5", "+5 id-in-filename boost missing");
    expect(has(body, "a.score > b.score"),
           "INV-5", "descending score sort comparator missing");

    // INV-6 — caps with defaults + clamp.
    expect(has(body, "max_specs") && has(body, "max_cards") &&
           has(body, "max_commits") && has(body, "max_adrs"),
           "INV-6", "one or more cap args missing");
    expect(has(body, "n > 20"),
           "INV-6", "cap clamp upper bound (20) missing");

    // INV-7 — envelope keys.
    for (const char *k : {"\"terms\"", "\"ids\"", "\"paths\"",
                          "\"specs\"", "\"specs_count\"",
                          "\"roadmap_cards\"", "\"cards_count\"",
                          "\"commits\"", "\"commits_count\"",
                          "\"adrs\"", "\"adrs_count\""}) {
        std::string msg = std::string("INV-7 envelope key ") + k;
        expect(has(body, k), "INV-7", msg.c_str());
    }

    // INV-8 — MainWindow registration, Required contract.
    expect(has(mwCpp, "registerToolProvider(\"task_priors\""),
           "INV-8", "task_priors not registered in mainwindow.cpp");
    {
        const auto pos = mwCpp.find("registerToolProvider(\"task_priors\"");
        const std::string window =
            pos == std::string::npos ? std::string()
                                     : mwCpp.substr(pos, 160);
        expect(has(window, "CallerCwdContract::Required"),
               "INV-8", "task_priors registration not Required");
    }

    // INV-9 — callerCwdContractFor Required branch.
    expect(has(ciCpp, "task_priors\"))        return C::Required") ||
           has(ciCpp, "task_priors\")) return C::Required") ||
           (has(ciCpp, "task_priors") && has(ciCpp, "C::Required")),
           "INV-9", "callerCwdContractFor missing task_priors Required");

    // INV-10 — kindForName "context" bucket.
    expect(has(ciCpp, "task_priors") && has(ciCpp, "\"context\""),
           "INV-10", "kindForName missing task_priors -> context");

    // INV-11 — token-cost ledger.
    expect(has(ciCpp, "task_priors\"),        {1200, 6000}") ||
           has(ciCpp, "task_priors\"), {1200, 6000}") ||
           (has(ciCpp, "task_priors") && has(ciCpp, "1200") &&
            has(ciCpp, "6000")),
           "INV-11", "token-cost {1200,6000} for task_priors missing");

    // INV-12 — tools/list schema.
    expect(has(ciCpp, "t[\"name\"] = \"task_priors\""),
           "INV-12", "tools/list schema entry for task_priors missing");
    {
        const auto pos = ciCpp.find("t[\"name\"] = \"task_priors\"");
        const auto endp = ciCpp.find("tools.append(t);", pos);
        const std::string block =
            (pos == std::string::npos || endp == std::string::npos)
                ? std::string()
                : ciCpp.substr(pos, endp - pos);
        expect(has(block, "selection_hint"),
               "INV-12", "task_priors schema missing selection_hint");
        expect(has(block, "\"description\""),
               "INV-12", "task_priors schema missing description prop");
        expect(has(block, "req.append(\"description\")") &&
               has(block, "req.append(\"caller_cwd\")"),
               "INV-12", "task_priors required args missing");
    }

    EXPECT_EQ(0, expect_failures());
}
