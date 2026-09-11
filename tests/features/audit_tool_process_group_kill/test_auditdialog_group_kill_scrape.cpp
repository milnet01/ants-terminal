// ANTS-5038 — source-scrape regression test. See spec.md INV-3.
//
// AuditDialog is a QDialog; this project's house pattern for its
// invariants is source-scrape (see audit_dialog_render_hardening,
// audit_dialog_v2), not construction. Two claims:
//
//  (a) runNextCheck() puts m_process in its own process group before
//      starting it.
//  (b) every m_process->kill(); call site signals the group first, in the
//      same statement block — so a helper the tool forked (or a grandchild
//      a shell pipeline forked) is stopped along with the tool, not left
//      running.
//
// Both are expected to fail against the current tree: src/processgroup.h
// ships as a stub and nothing in auditdialog.cpp calls it yet.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>
#include <vector>

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Every start offset of `needle` in `hay`, non-overlapping.
std::vector<size_t> findAll(const std::string &hay, const std::string &needle) {
    std::vector<size_t> out;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size()))
        out.push_back(pos);
    return out;
}

// True when "ProcessGroup::signalGroup(" appears between the nearest
// enclosing block's start and `killPos` — i.e. in the SAME statement block
// as the kill call, before it. Rather than a full brace parse: the last '}'
// before killPos within a bounded lookback window is a SIBLING block
// closing, so anything before that boundary belongs to an earlier,
// unrelated block, not this one; the region after it (up to killPos) is
// this statement block's own preceding statements.
bool signalsGroupBeforeKill(const std::string &stripped, size_t killPos) {
    constexpr size_t kWindow = 600;
    const size_t begin = (killPos > kWindow) ? killPos - kWindow : 0;
    std::string region = stripped.substr(begin, killPos - begin);
    const size_t lastClose = region.rfind('}');
    if (lastClose != std::string::npos) region = region.substr(lastClose + 1);
    return contains(region, "ProcessGroup::signalGroup(");
}

}  // namespace

TEST(AuditDialogGroupKillScrape, RunNextCheckStartsOwnGroupBeforeStart) {
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());

    const std::string body =
        ants_test::slurpFunctionBody(src, "void AuditDialog::runNextCheck(");
    ASSERT_FALSE(body.empty()) << "runNextCheck() body not found";

    const size_t startPos = body.find("m_process->start(");
    ASSERT_NE(startPos, std::string::npos)
        << "m_process->start( moved out of runNextCheck()";

    const size_t groupPos = body.find("ProcessGroup::startsOwnGroup(");
    EXPECT_NE(groupPos, std::string::npos)
        << "ANTS-5038: runNextCheck() never calls "
           "ProcessGroup::startsOwnGroup(*m_process) — expected: a call "
           "before m_process->start(); actual: none found in the function "
           "body";
    if (groupPos != std::string::npos) {
        EXPECT_LT(groupPos, startPos)
            << "ANTS-5038: ProcessGroup::startsOwnGroup must run BEFORE "
               "m_process->start(), not after — expected: startsOwnGroup "
               "offset < start offset; actual: " << groupPos << " >= "
            << startPos;
    }
}

TEST(AuditDialogGroupKillScrape, EveryKillSiteSignalsTheGroupFirst) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    const std::vector<size_t> kills =
        findAll(stripped, "m_process->kill();");
    ASSERT_FALSE(kills.empty())
        << "test setup: no m_process->kill(); call sites found at all — "
           "the source shape changed under this test";

    int missing = 0;
    for (size_t pos : kills) {
        if (signalsGroupBeforeKill(stripped, pos)) continue;
        ++missing;
        const size_t ctxStart = pos > 80 ? pos - 80 : 0;
        ADD_FAILURE()
            << "ANTS-5038: m_process->kill(); at stripped-source offset "
            << pos << " has no preceding ProcessGroup::signalGroup( call "
               "in its own statement block — expected: a signalGroup call "
               "right before it; actual context: \""
            << stripped.substr(ctxStart, pos - ctxStart + 18) << "\"";
    }
    EXPECT_EQ(missing, 0)
        << missing << " of " << kills.size()
        << " m_process->kill(); site(s) kill the leader without first "
           "signalling the process group, so a helper the tool forked "
           "survives.";
}
