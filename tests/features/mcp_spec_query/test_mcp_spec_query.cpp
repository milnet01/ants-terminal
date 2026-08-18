// Source-grep harness for ANTS-1309 — locks the wiring contract for
// the `spec_query` MCP tool. See spec.md.
//
// Exit 0 = all 8 invariants hold.

#include "../../_support/expect.h"

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

// ANTS-4468 — this file is otherwise a source-scrape suite, but the bundle
// (test_claude) links RemoteControl, so the mode contract is tested by CALLING
// the verb. A scrape would only prove the refusal string is present in the
// source; the defect being closed is about which branch actually runs.
#include "remotecontrol.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

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
#ifndef SRC_SPECPARSE_CPP_PATH
#error "SRC_SPECPARSE_CPP_PATH compile definition required"
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
    const std::string rcCpp = ants_test::slurpRemoteControl();
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
           "cmdSpecQuery body missing from the remotecontrol TUs");
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

    // INV-5b — ANTS-3569: parseSpecBody surfaces a possible_untabled_invariants
    // hint (count of INV-N tokens present in prose but absent from the
    // structured table/bullet list) so a caller knows invariants_count may
    // under-report inline-declared invariants (e.g. `**Invariant (INV-N):**`).
    //
    // ANTS-3665 hoisted parseSpecBody out of remotecontrol.cpp's anonymous
    // namespace into src/specparse.cpp (ants_core_lib), so spec_lint can link
    // it. The scrape follows the code rather than being relaxed — asserting
    // against the file the function actually lives in is what keeps this a
    // contract instead of a formality.
    const std::string specParseCpp =
        ants_test::slurpFile(SRC_SPECPARSE_CPP_PATH);
    expect(contains(specParseCpp, "possible_untabled_invariants"),
           "INV-5b",
           "parseSpecBody must emit possible_untabled_invariants (ANTS-3569)");

    // INV-5c — ANTS-3665: the hoist itself. parseSpecBody must NOT be back in
    // remotecontrol.cpp's anonymous namespace, or ANTS-3662's spec_lint engine
    // (in ants_core_lib) silently loses its parser again and the corpus grows a
    // second one. The call site keeps the name, so this checks for the
    // definition, not the mention.
    expect(!contains(rcCpp, "QJsonObject parseSpecBody(const QString"),
           "INV-5c",
           "parseSpecBody must stay hoisted in src/specparse.cpp (ANTS-3665)");

    // INV-5d — ANTS-3665: the bullet branch emits test_surface. specs.md § 6
    // promises it from both invariant forms; for years only the GFM table
    // branch delivered, so nearly every spec in this corpus parsed without one.
    expect(contains(specParseCpp, "\\*Test:\\*"),
           "INV-5d",
           "parseSpecBody must extract the bullet-form *Test:* clause "
           "(ANTS-3665)");

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

// ANTS-4352 — mode:"gate_drift": which gated specs have been EDITED since
// their last review loop.
//
// A spec was stamped Reviewed on one date; two days later a different roadmap
// item rewrote one of its sections while closing a defect elsewhere — a
// legitimate commit nobody read cold. The stamp stayed for six days. The
// eventual re-gate found 20 verified defects across three loops, two of which
// would have shipped a contrast regression into the only appearance mode
// low-vision users have.
//
// The failure is silent AND self-concealing: the document asserts it was
// reviewed, that assertion is what the next session trusts, and the
// invalidating edit sits in another item's commit where nobody looks.
TEST(McpSpecQuery, Ants4352GateDriftMode) {
    const std::string rcCpp = ants_test::slurpRemoteControl();

    EXPECT_NE(rcCpp.find("gate_drift"), std::string::npos)
        << "the mode must be reachable from cmdSpecQuery";
    EXPECT_NE(rcCpp.find("commits_since"), std::string::npos)
        << "commits_since is what makes the answer ACTIONABLE rather than "
           "merely alarming — it is what distinguishes the gate's own fix "
           "pass from an authoring edit by another item";
    for (const char *k : {"\"stale\"", "\"current\"", "\"ungated\""})
        EXPECT_NE(rcCpp.find(k), std::string::npos)
            << "three buckets, not two: \"never gated\" is a different answer "
               "from \"gated and current\" and a caller needs to tell them "
               "apart — " << k;

    // The same-day rule, and it is not cosmetic. `git log --after=YYYY-MM-DD`
    // means "after midnight of that day", so it INCLUDES the day's own
    // commits — which is when the gate's OWN fix pass lands. Global rule 14
    // is explicit that a document whose only changes came from that pass is
    // still gated: the run that made those edits WAS the review.
    //
    // Measured against this repo's 243 specs: the naive form reported 52
    // stale, of which 16 (31%) were same-day gate commits. Reporting those as
    // drift would make the common case a false positive.
    EXPECT_NE(rcCpp.find("same_day"), std::string::npos)
        << "each commit must be flagged when it lands on the loop date";
    EXPECT_NE(rcCpp.find("same_day_commits_only"), std::string::npos)
        << "…and a spec whose ONLY commits since the loop are same-day is "
           "CURRENT, not stale";

    // An unanswerable check is not a pass (ANTS-4374): if git cannot answer,
    // the spec must not be silently reported as current.
    EXPECT_NE(rcCpp.find("git_error"), std::string::npos)
        << "a spec whose git history could not be read must say so rather "
           "than land in `current`";
}


// ANTS-4468 — `mode` is DECLARED, and an unrecognised value is REFUSED.
//
// Two halves of one defect, reported by OneUp. The handler honoured
// mode:"gate_drift" while the inputSchema declared only id/path/caller_cwd,
// so the generic ANTS-2175 arg checker — which diffs a call's keys against
// the declared properties — correctly flagged `mode` as ignored. The envelope
// then asserted both that the argument was applied and that it was discarded,
// with the gate_drift payload proving the first.
//
// The reporter's own severity argument is why the fix is to DECLARE the
// property rather than to suppress the advisory: ignored_args is a
// correctness signal, and one that fires on honoured arguments trains callers
// to disregard it — costing the next session the one signal that would catch
// a genuine typo. Which is the second half: with `mode` now a known key, the
// name-diff checker says nothing about `mode:"gate-drift"`, and that value
// used to fall through to the LIST branch and return ok:true with a spec list.
// A list returned in answer to a drift question is a confident wrong answer,
// so it refuses.
TEST(McpSpecQuery, Ants4468ModeIsDeclaredAndUnknownModeRefuses) {
    // The schema half is declared in ClaudeIntegration's tools/list builder,
    // which this bundle does not link, so it is not asserted here. Worth
    // recording why declaring it mattered beyond the advisory: spec_query's
    // schema sets additionalProperties:false, so an undeclared `mode` was not
    // merely reported as ignored — a strict MCP client was entitled to reject
    // the call outright. What IS asserted here is the branch that runs.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(root + QStringLiteral("/docs/specs"));

    RemoteControl rc(nullptr);
    auto call = [&](const QString &mode) {
        QJsonObject r;
        r[QStringLiteral("caller_cwd")] = root;
        if (!mode.isEmpty()) r[QStringLiteral("mode")] = mode;
        return rc.cmdSpecQuery(r).object();
    };

    // Half 2 — a typo refuses instead of silently listing.
    const QJsonObject typo = call(QStringLiteral("gate-drift"));
    EXPECT_FALSE(typo.value(QStringLiteral("ok")).toBool())
        << "ANTS-4468: a misspelled mode used to fall through to the list "
           "branch and answer a drift question with a spec list";
    EXPECT_EQ(typo.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_args"));
    EXPECT_NE(typo.value(QStringLiteral("error")).toString()
                  .indexOf(QStringLiteral("gate_drift")), -1)
        << "the refusal must name what WAS accepted, so the caller "
           "self-corrects in one round trip rather than guessing";

    // Both declared values still work, so the refusal is not over-broad.
    const QJsonObject drift = call(QStringLiteral("gate_drift"));
    EXPECT_TRUE(drift.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(drift.value(QStringLiteral("mode")).toString(),
              QStringLiteral("gate_drift"));

    const QJsonObject listed = call(QStringLiteral("list"));
    EXPECT_TRUE(listed.value(QStringLiteral("ok")).toBool())
        << "\"list\" is the explicit spelling of what omitting id AND path "
           "already selects — declaring it in the enum must not make it a "
           "value the handler rejects";
    EXPECT_EQ(listed.value(QStringLiteral("mode")).toString(),
              QStringLiteral("list"));

    // Omitting mode entirely is unchanged.
    const QJsonObject bare = call(QString());
    EXPECT_TRUE(bare.value(QStringLiteral("ok")).toBool());
}
