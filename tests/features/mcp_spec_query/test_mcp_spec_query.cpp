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

TEST(McpSpecQuery, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

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
    // Scope to the actual spec_query registration block — from its
    // `t["name"]` line to the closing `tools.append(t);` — rather than a
    // fixed byte window (the window broke when ANTS-1906 / ANTS-3360 grew
    // the description). caller_cwd is the ONLY unconditionally-required
    // arg: ANTS-1906 made `id` optional (pass `path` instead), and
    // ANTS-3360 made *both* id and path optional (list mode), so the
    // schema must NOT mark `id` required.
    {
        const auto sqPos = ciCpp.find("t[\"name\"] = \"spec_query\"");
        ASSERT_NE(sqPos, std::string::npos);
        const auto sqEnd = ciCpp.find("tools.append(t);", sqPos);
        ASSERT_NE(sqEnd, std::string::npos);
        const std::string block = ciCpp.substr(sqPos, sqEnd - sqPos);
        expect(contains(block, "req.append(\"caller_cwd\")"),
               "INV-7b",
               "spec_query schema must mark \"caller_cwd\" as required");
        expect(!contains(block, "req.append(\"id\")"),
               "INV-7c",
               "spec_query schema must NOT mark \"id\" as required "
               "(ANTS-1906 optional id, ANTS-3360 list mode)");
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

    // INV-9 — ANTS-3360 list mode: cmdSpecQuery delegates the no-id/no-path
    // case to the specListEnvelope helper (spec discovery).
    expect(contains(body, "specListEnvelope"),
           "INV-9a",
           "cmdSpecQuery must delegate list mode to specListEnvelope");
    expect(contains(rcCpp, "ANTS-3360"),
           "INV-9b",
           "list-mode code must carry an ANTS-3360 anchor");

    // INV-10 — ANTS-3356 generalised id routing: any <PREFIX>-NNNN id
    // resolves via resolveSpecRelForId (exact `<id>.md`, then `<id>-*.md`).
    expect(contains(body, "resolveSpecRelForId"),
           "INV-10a",
           "cmdSpecQuery must resolve the spec file via resolveSpecRelForId");
    expect(contains(rcCpp, "ANTS-3356"),
           "INV-10b",
           "generalised id routing must carry an ANTS-3356 anchor");

    // INV-11 — ANTS-3436: isValidSpecId accepts the numeric `NN` / `NN-topic`
    // ids that list mode (specListEnvelope) emits as the file stem, so the
    // read surface accepts the identifiers it hands out (a project named
    // `17-emission-model.md` no longer gets bad_id on `id=17-emission-model`).
    // The numeric-led arm keeps the `[A-Za-z0-9_-]` char class (no `/`/`.`),
    // so routing to `docs/specs/<id>.md` cannot traverse out.
    expect(contains(rcCpp, "[0-9]+(?:-[A-Za-z0-9_-]+)*"),
           "INV-11a",
           "isValidSpecId must include the numeric NN-topic arm (ANTS-3436)");
    expect(contains(rcCpp, "ANTS-3436"),
           "INV-11b",
           "numeric-id arm must carry an ANTS-3436 anchor");

    // The INVs above are counted by expect(); enforce them here so a
    // regression actually fails the test (previously omitted — the
    // source-grep INVs were toothless).
    EXPECT_EQ(0, expect_failures());
}
