// ANTS-4448 — credentials must not leave the machine in audit artifacts.
// See spec.md. INV-1/INV-2 are behavioural against SecretRedact; INV-3 to
// INV-5 are source-scrapes over the three egress sites, because each is
// reachable only through a GUI dialog or a live SARIF capture.

#include "../../_support/expect.h"
#include "secretredact.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QString>

#include <string>

#ifndef SRC_AUDITENGINE_CPP_PATH
#error "SRC_AUDITENGINE_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_AUDITDIALOG_CPP_PATH
#error "SRC_AUDITDIALOG_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Bounded substring between two signatures, so an assertion about one
// function's body cannot be satisfied by a match somewhere else in the
// file. Empty when either signature is missing — that miss IS the guard.
std::string boundedBetween(const std::string &cpp,
                           const std::string &startSig,
                           const std::string &endSig) {
    const auto startPos = cpp.find(startSig);
    if (startPos == std::string::npos) return {};
    const auto endPos = cpp.find(endSig, startPos + startSig.size());
    if (endPos == std::string::npos) return {};
    return cpp.substr(startPos, endPos - startPos);
}

}  // namespace

// INV-1 — userinfo is removed from a scheme-bearing URL, host and path kept.
TEST(AuditProvenanceRedaction, Inv1StripsUserinfo) {
    expect_reset();

    const QString userPass = SecretRedact::stripUrlCredentials(
        QStringLiteral("https://user:s3cr3t@github.com/owner/repo.git"));
    expect(!userPass.contains(QStringLiteral("s3cr3t")),
           "INV-1: the password is gone");
    expect(!userPass.contains(QLatin1Char('@')),
           "INV-1: no userinfo separator remains");
    expect(userPass.contains(QStringLiteral("github.com/owner/repo.git")),
           "INV-1: host and path are preserved");

    // A bare token in the USERNAME slot is the common GitHub form, and has
    // no password component at all — stripping only the password would miss
    // it entirely.
    const QString tokenOnly = SecretRedact::stripUrlCredentials(
        QStringLiteral("https://ghp_exampletoken@github.com/owner/repo.git"));
    expect(!tokenOnly.contains(QStringLiteral("ghp_exampletoken")),
           "INV-1: a token in the username slot is removed too");
    expect(tokenOnly.contains(QStringLiteral("github.com/owner/repo.git")),
           "INV-1: host and path preserved for the token-only form");

    EXPECT_EQ(0, expect_failures());
}

// INV-2 — an scp-style remote, and any URL with no userinfo, are unchanged.
TEST(AuditProvenanceRedaction, Inv2LeavesNonCredentialledRemotes) {
    expect_reset();

    // scp syntax parses with no scheme; its `git@` is a username, not a
    // secret, and rewriting it would corrupt the remote.
    const QString scp = QStringLiteral("git@github.com:owner/repo.git");
    expect(SecretRedact::stripUrlCredentials(scp) == scp,
           "INV-2: an scp-style remote is returned unchanged");

    const QString plain = QStringLiteral("https://github.com/owner/repo.git");
    expect(SecretRedact::stripUrlCredentials(plain) == plain,
           "INV-2: a URL with no userinfo is returned unchanged");

    const QString local = QStringLiteral("file:///home/user/project");
    expect(SecretRedact::stripUrlCredentials(local) == local,
           "INV-2: the file:// fallback is returned unchanged");

    EXPECT_EQ(0, expect_failures());
}

// INV-3 — the SARIF provenance writer strips before it writes.
TEST(AuditProvenanceRedaction, Inv3SarifWriterStrips) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITENGINE_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-3: auditengine.cpp not readable";

    const std::string body = boundedBetween(
        cpp, "QJsonArray buildVcsProvenanceBlock(", "\n}\n");
    ASSERT_FALSE(body.empty())
        << "INV-3: failed to bound buildVcsProvenanceBlock";

    expect(contains(body, "stripUrlCredentials"),
           "INV-3: the remote is stripped inside the provenance builder");
    expect(contains(body, "repositoryUri"),
           "INV-3: precondition — the builder still writes repositoryUri");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — the read-back path strips too, because SARIFs captured before the
// fix are still on disk and last_audit_summary reads them.
TEST(AuditProvenanceRedaction, Inv4ReadBackStrips) {
    expect_reset();
    // Scrapes every RemoteControl TU, not one file by name: RcTuSplit INV-11
    // reserves naming a remotecontrol TU to the ANTS_RC_SOURCES_REL block, and
    // a verb that moves to a sibling TU must not read as a verb deleted.
    const std::string cpp = ants_test::slurpRemoteControl();
    ASSERT_FALSE(cpp.empty())
        << "INV-4: RemoteControl sources not readable";

    const std::string assign = boundedBetween(
        cpp, "ok[\"repository_uri\"]", ";");
    ASSERT_FALSE(assign.empty())
        << "INV-4: failed to locate the repository_uri assignment";

    expect(contains(assign, "stripUrlCredentials"),
           "INV-4: repository_uri is stripped on read-back, not just on "
           "write");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — AI triage scrubs the prompt it POSTs.
TEST(AuditProvenanceRedaction, Inv5AiTriageScrubsPrompt) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-5: auditdialog.cpp not readable";

    expect(contains(cpp, "SecretRedact::scrub"),
           "INV-5: the raw-QNAM triage path scrubs, as LlmClient does");

    // The scrubbed text, not the raw userMsg, is what reaches the body.
    expect(contains(cpp, "{\"content\", scrubbedUser.text}"),
           "INV-5: the request body carries the scrubbed user message");
    EXPECT_EQ(0, expect_failures());
}
