// A parse thread that outlives the 2 s wait on tab close is detached, never
// terminated — see spec.md. ANTS-5077.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#ifndef SRC_TERMINALWIDGET_IMPL_PATH
#  error "SRC_TERMINALWIDGET_IMPL_PATH compile definition required"
#endif

namespace {

std::string destructorBody() {
    return ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALWIDGET_IMPL_PATH, "TerminalWidget::~TerminalWidget("));
}

// The block run when `m_parseThread->wait(2000)` fails: from the `{` after the
// wait to its matching `}`. Empty when either is missing.
std::string timeoutBranch(const std::string &body) {
    const std::size_t wait = body.find("m_parseThread->wait(2000)");
    if (wait == std::string::npos) return {};
    const std::size_t open = body.find('{', wait);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < body.size(); ++i) {
        if (body[i] == '{') ++depth;
        else if (body[i] == '}' && --depth == 0) return body.substr(open, i - open + 1);
    }
    return {};
}

}  // namespace

// INV-1
TEST(ParseThreadDetachOnClose, Inv1DestructorNeverTerminates) {
    const std::string body = destructorBody();
    ASSERT_FALSE(body.empty()) << "setup: ~TerminalWidget body not found";
    EXPECT_EQ(body.find("terminate("), std::string::npos)
        << "INV-1: ~TerminalWidget still calls QThread::terminate()";
}

// INV-2
TEST(ParseThreadDetachOnClose, Inv2StuckThreadIsDetached) {
    const std::string branch = timeoutBranch(destructorBody());
    ASSERT_FALSE(branch.empty()) << "setup: the wait(2000) timeout branch not found";
    EXPECT_NE(branch.find("m_parseThread->setParent(nullptr)"), std::string::npos)
        << "INV-2: the stuck thread stays parented to the widget being destroyed";
    EXPECT_NE(branch.find("&QThread::finished"), std::string::npos)
        << "INV-2: the stuck thread is not connected to its own finished signal";
    EXPECT_NE(branch.find("&QObject::deleteLater"), std::string::npos)
        << "INV-2: the stuck thread never deletes itself";
}

// INV-3
TEST(ParseThreadDetachOnClose, Inv3NoUnboundedWaitAfterTimeout) {
    const std::string branch = timeoutBranch(destructorBody());
    ASSERT_FALSE(branch.empty()) << "setup: the wait(2000) timeout branch not found";
    EXPECT_EQ(branch.find("->wait()"), std::string::npos)
        << "INV-3: the GUI still waits without a bound for a stuck worker";
}

// INV-4
TEST(ParseThreadDetachOnClose, Inv4AlreadyFinishedThreadIsDeleted) {
    const std::string branch = timeoutBranch(destructorBody());
    ASSERT_FALSE(branch.empty()) << "setup: the wait(2000) timeout branch not found";
    EXPECT_NE(branch.find("isFinished()"), std::string::npos)
        << "INV-4: a worker that ended before the connection would be leaked";
}
