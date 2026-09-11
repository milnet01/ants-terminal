// Feature-conformance test for ANTS-1111 AuditEngine::applyCorroborationShift
// and ANTS-5041 AuditEngine::applyCorroborationShiftAcross.

#include <gtest/gtest.h>

#include <string>

#include <QList>
#include <QSet>
#include <QString>

#include "auditengine.h"
#include "../../_support/srcgrep.h"

#ifndef SRC_AUDIT_CPP_PATH
#  error "SRC_AUDIT_CPP_PATH compile definition required"
#endif

namespace {

Finding mk(const QString &checkId, const QString &file, int line,
           Severity sev) {
    Finding f;
    f.checkId = checkId;
    f.file = file;
    f.line = line;
    f.severity = sev;
    f.message = "test";
    return f;
}

CheckResult mkResult(const QString &checkId, QList<Finding> findings) {
    CheckResult r;
    r.checkId = checkId;
    r.findings = std::move(findings);
    return r;
}

}  // namespace

TEST(AuditCorroborationShift, Inv1PromotesOnTwoDistinctTools) {
    QList<Finding> findings = {
        mk("clazy-X", "src/foo.cpp", 10, Severity::Minor),
        mk("cppcheck-Y", "src/foo.cpp", 10, Severity::Major),
        mk("clazy-X", "src/bar.cpp", 20, Severity::Major),  // single tool, control
    };
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);

    EXPECT_EQ(int(findings[0].severity), int(Severity::Major));    // Minor +1
    EXPECT_EQ(int(findings[1].severity), int(Severity::Critical)); // Major +1
    EXPECT_EQ(int(findings[2].severity), int(Severity::Major));    // unchanged
}

TEST(AuditCorroborationShift, Inv1ClampsToBlocker) {
    QList<Finding> findings = {
        mk("clazy-X", "src/foo.cpp", 10, Severity::Blocker),
        mk("cppcheck-Y", "src/foo.cpp", 10, Severity::Blocker),
    };
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Blocker));
    EXPECT_EQ(int(findings[1].severity), int(Severity::Blocker));
}

TEST(AuditCorroborationShift, Inv2DemotesNoisySingleTool) {
    QList<Finding> findings = {
        mk("noisy-rule", "src/foo.cpp", 10, Severity::Major),
    };
    QSet<QString> noisy = {"noisy-rule"};
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Minor));  // Major -1
}

TEST(AuditCorroborationShift, Inv2ClampsToInfo) {
    QList<Finding> findings = {
        mk("noisy-rule", "src/foo.cpp", 10, Severity::Info),
    };
    QSet<QString> noisy = {"noisy-rule"};
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Info));
}

TEST(AuditCorroborationShift, Inv3NoShiftForCleanSingleTool) {
    QList<Finding> findings = {
        mk("clean-rule", "src/foo.cpp", 10, Severity::Minor),
    };
    QSet<QString> noisy;  // empty
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Minor));
}

TEST(AuditCorroborationShift, SameCheckIdTwiceDoesNotPromote) {
    // Two findings from the SAME checkId at the same line — that's not
    // cross-tool corroboration; severity must stay.
    QList<Finding> findings = {
        mk("clazy-X", "src/foo.cpp", 10, Severity::Minor),
        mk("clazy-X", "src/foo.cpp", 10, Severity::Minor),
    };
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Minor));
    EXPECT_EQ(int(findings[1].severity), int(Severity::Minor));
}

TEST(AuditCorroborationShift, FindingsWithoutFileLineSkipped) {
    Finding f;
    f.checkId = "rule";
    f.severity = Severity::Major;
    // file empty + line=-1
    QList<Finding> findings = {f};
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Major));
}

TEST(AuditCorroborationShift, EmptyInputNoOp) {
    QList<Finding> findings;
    QSet<QString> noisy = {"any"};
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_TRUE(findings.isEmpty());
}

// ---------------------------------------------------------------------
// ANTS-5041 — applyCorroborationShiftAcross: the shift must see every
// check's findings together, not one CheckResult at a time.
// ---------------------------------------------------------------------

TEST(AuditCorroborationShift, Inv4PromotesAcrossDistinctCheckResults) {
    // Two CheckResults, two different tools, both citing src/foo.cpp:10.
    // The stub (ANTS-5041, pre-fix) calls applyCorroborationShift once per
    // CheckResult, so neither call ever sees the other tool's finding at
    // that line — the promotion this asserts cannot fire under the stub.
    CheckResult a = mkResult("cppcheck-A", {
        mk("cppcheck-A", "src/foo.cpp", 10, Severity::Minor),
        mk("cppcheck-A", "src/bar.cpp", 20, Severity::Minor),  // control
    });
    CheckResult b = mkResult("clazy-B", {
        mk("clazy-B", "src/foo.cpp", 10, Severity::Major),
    });
    QList<CheckResult> results = {a, b};
    QSet<QString> noisy;  // empty — this is a promotion-only scenario
    AuditEngine::applyCorroborationShiftAcross(results, noisy);

    EXPECT_EQ(int(results[0].findings[0].severity), int(Severity::Major))
        << "INV-4: cppcheck-A's foo.cpp:10 finding (was Minor) should "
           "promote to Major because clazy-B also cites foo.cpp:10 — got "
           << int(results[0].findings[0].severity)
           << " (Minor=" << int(Severity::Minor)
           << ", Major=" << int(Severity::Major) << "). A per-check-scoped "
           "shift never sees clazy-B's finding from cppcheck-A's call.";
    EXPECT_EQ(int(results[1].findings[0].severity), int(Severity::Critical))
        << "INV-4: clazy-B's foo.cpp:10 finding (was Major) should promote "
           "to Critical for the same reason — got "
           << int(results[1].findings[0].severity)
           << " (Major=" << int(Severity::Major)
           << ", Critical=" << int(Severity::Critical) << ").";
    EXPECT_EQ(int(results[0].findings[1].severity), int(Severity::Minor))
        << "INV-4: the control finding at bar.cpp:20 (single tool, no "
           "corroboration) must stay Minor — got "
           << int(results[0].findings[1].severity) << ".";
}

TEST(AuditCorroborationShift, Inv5DemotesAcrossDistinctCheckResults) {
    // A noisy-rule finding in one CheckResult, a clean control finding in
    // another. Demotion doesn't need cross-check visibility to work, so
    // this locks that combining findings from every check does not break
    // it (e.g. a write-back bug that shuffles severities by index).
    CheckResult clean = mkResult("cppcheck-A", {
        mk("cppcheck-A", "src/clean.cpp", 5, Severity::Minor),  // control
    });
    CheckResult noisyResult = mkResult("noisy-rule", {
        mk("noisy-rule", "src/foo.cpp", 10, Severity::Major),
    });
    QList<CheckResult> results = {clean, noisyResult};
    QSet<QString> noisy = {"noisy-rule"};
    AuditEngine::applyCorroborationShiftAcross(results, noisy);

    EXPECT_EQ(int(results[1].findings[0].severity), int(Severity::Minor))
        << "INV-5: the single-tool noisy-rule finding (was Major) should "
           "demote to Minor — got " << int(results[1].findings[0].severity)
           << " (Major=" << int(Severity::Major)
           << ", Minor=" << int(Severity::Minor) << ").";
    EXPECT_EQ(int(results[0].findings[0].severity), int(Severity::Minor))
        << "INV-5: the control finding from a non-noisy rule in a "
           "different CheckResult must stay Minor — got "
           << int(results[0].findings[0].severity) << ".";
}

// ---------------------------------------------------------------------
// ANTS-5041 — source-grep: the shift must run once per completed audit
// run, never once per render. A behavioural test can't observe how many
// times a private method fired internally, so this pins the call sites.
// ---------------------------------------------------------------------

TEST(AuditCorroborationShift, Inv6aRenderResultsDoesNotCallTheShift) {
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "precondition: could not read "
                                  "auditdialog.cpp at SRC_AUDIT_CPP_PATH";
    const std::string stripped = ants_test::stripComments(src);

    const std::string renderBody = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::renderResults(");
    ASSERT_FALSE(renderBody.empty())
        << "precondition: renderResults() body not located in "
           "auditdialog.cpp — update this scrape's anchor if the "
           "function was renamed or restructured.";

    // Literal "applyCorroborationShift(" (immediate open-paren) does NOT
    // match "applyCorroborationShiftAcross(" — the next character there
    // is 'A', not '('. So this only catches the per-CheckResult call.
    const auto pos = renderBody.find("applyCorroborationShift(");
    EXPECT_EQ(pos, std::string::npos)
        << "INV-6a: renderResults() still calls applyCorroborationShift "
           "at offset " << pos << " in its (comment-stripped) body. "
           "renderResults() runs on every filter keystroke, pill toggle, "
           "sort toggle and AI verdict, so a shift with no repeat guard "
           "re-applies there and drifts severity further each render.";
}

TEST(AuditCorroborationShift, Inv6bRunCompletionShiftsBeforeFirstRender) {
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "precondition: could not read "
                                  "auditdialog.cpp at SRC_AUDIT_CPP_PATH";
    const std::string stripped = ants_test::stripComments(src);

    // Asserts, within `body`, that applyCorroborationShiftAcross( is
    // present and its first occurrence precedes the first renderResults().
    auto checkShiftBeforeRender = [](const std::string &body,
                                      const char *label) {
        ASSERT_FALSE(body.empty())
            << "precondition: " << label << " body/region not found — "
               "update this scrape's anchors if the code moved.";
        const auto acrossPos = body.find("applyCorroborationShiftAcross(");
        const auto renderPos = body.find("renderResults()");
        EXPECT_NE(acrossPos, std::string::npos)
            << "INV-6b: " << label << " never calls "
               "applyCorroborationShiftAcross. The shift must run once per "
               "completed run, over every check's combined findings, in "
               "the run-completion path.";
        EXPECT_NE(renderPos, std::string::npos)
            << "precondition: " << label << " has no renderResults() call "
               "to order against.";
        if (acrossPos != std::string::npos && renderPos != std::string::npos) {
            EXPECT_LT(acrossPos, renderPos)
                << "INV-6b: " << label << " calls renderResults() (at "
                   "offset " << renderPos
                   << ") before applyCorroborationShiftAcross (at offset "
                   << acrossPos << ") — findings must be shifted before "
                   "they are rendered, not after.";
        }
    };

    const std::string cancelBody = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::cancelAudit(");
    checkShiftBeforeRender(cancelBody, "AuditDialog::cancelAudit");

    const std::string runNextBody = ants_test::slurpFunctionBody(
        stripped, "void AuditDialog::runNextCheck(");
    ASSERT_FALSE(runNextBody.empty())
        << "precondition: runNextCheck() body not located in "
           "auditdialog.cpp — update this scrape's anchor if the "
           "function was renamed or restructured.";
    // Narrow to the run-completion branch specifically (m_currentCheck
    // has walked off the end of m_checks) — the rest of runNextCheck
    // kicks off the next check and has its own unrelated control flow.
    const std::string finishBranch = ants_test::regionBetween(
        runNextBody,
        "if (m_currentCheck >= m_checks.size()) {",
        "const auto &check = m_checks[m_currentCheck];");
    ASSERT_FALSE(finishBranch.empty())
        << "precondition: runNextCheck's run-completion branch anchors "
           "not found — update this scrape's anchors if the code moved.";
    checkShiftBeforeRender(finishBranch, "runNextCheck's run-completion branch");
}
