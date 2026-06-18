// Feature-conformance test for spec.md (ANTS-1630 — caller-cwd anchoring of
// cold_eyes_fold_in + indie_review_fold_in).
//
// Both handlers are GUI-bound (live MainWindow + RemoteControl), so the
// migration off the focused-tab gate is pinned by source-scrape of each
// handler's definition body window. Mirrors the cold_eyes_fold_in_narrative
// extraction idiom, but anchors on the definition signature so the window
// cannot span into a neighbouring gated handler.
//
// Pre-fix these tests FAIL: both bodies contained `gate.focused` +
// `RcGate::checkCallerCwd`, and neither contained `resolveCallerCwdRoot`.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Body window for a RemoteControl handler: from its definition signature
// `QJsonDocument RemoteControl::<fn>(const QJsonObject &req)` through the
// next `\nQJsonDocument RemoteControl::` definition. Anchoring on the full
// signature skips dispatch-table mentions (`&RemoteControl::<fn>`), so the
// window is exactly one handler body.
std::string handlerBody(const std::string &rc, const char *fn) {
    const std::string sig =
        std::string("QJsonDocument RemoteControl::") + fn +
        "(const QJsonObject &req)";
    const auto start = rc.find(sig);
    if (start == std::string::npos) return {};
    const auto end = rc.find("\nQJsonDocument RemoteControl::", start + 1);
    return end == std::string::npos ? rc.substr(start)
                                    : rc.substr(start, end - start);
}

}  // namespace

// INV-1 / INV-2 — cold_eyes_fold_in anchors root to caller_cwd, not focused.
TEST(FoldInCallerAnchor, ColdEyesAnchorsToCallerCwd) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const std::string body = handlerBody(rc, "cmdColdEyesFoldIn");
    ASSERT_FALSE(body.empty()) << "could not locate cmdColdEyesFoldIn body";

    EXPECT_TRUE(has(body, "resolveCallerCwdRoot("))
        << "handler must resolve root via the caller_cwd decoder";
    EXPECT_FALSE(has(body, "gate.focused"))
        << "handler must NOT anchor the write to the focused tab";
    EXPECT_FALSE(has(body, "RcGate::checkCallerCwd"))
        << "handler must NOT call the focused-tab gate (ANTS-1630)";
    // INV-3 — unresolvable / non-dir caller_cwd refuses cwd_bad.
    EXPECT_TRUE(has(body, "cwd_bad"))
        << "handler must refuse cwd_bad on an unresolvable caller_cwd";
    EXPECT_TRUE(has(body, "isDir()"))
        << "refusal must gate on !isDir() (anchor must be a directory)";
}

// INV-1 / INV-2 / INV-3 — same contract for indie_review_fold_in.
TEST(FoldInCallerAnchor, IndieReviewAnchorsToCallerCwd) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const std::string body = handlerBody(rc, "cmdIndieReviewFoldIn");
    ASSERT_FALSE(body.empty()) << "could not locate cmdIndieReviewFoldIn body";

    EXPECT_TRUE(has(body, "resolveCallerCwdRoot("))
        << "handler must resolve root via the caller_cwd decoder";
    EXPECT_FALSE(has(body, "gate.focused"))
        << "handler must NOT anchor the write to the focused tab";
    EXPECT_FALSE(has(body, "RcGate::checkCallerCwd"))
        << "handler must NOT call the focused-tab gate (ANTS-1630)";
    EXPECT_TRUE(has(body, "cwd_bad"))
        << "handler must refuse cwd_bad on an unresolvable caller_cwd";
    EXPECT_TRUE(has(body, "isDir()"))
        << "refusal must gate on !isDir() (anchor must be a directory)";
}
