// Feature-conformance test for ANTS-1111 — `// audit: drop[=rule]` alias for
// the inline-suppress regex.
//
// ANTS-3355 — upgraded from source-grep to RUNTIME coverage: the previous
// version only slurped auditdialog.cpp and regex-matched the source string,
// so it could not catch a behavioural regression in the parser (a moved
// alternative, a broken terminator class, a rule-list mismatch). It now calls
// the real compiled AuditEngine::commentSuppresses() — a free function needing
// only Qt Core, no QApplication or dialog instance — and asserts the ANTS-1111
// §4 INV-9 truth table directly. Behaviour coverage strictly subsumes the old
// source-shape grep.
//
// ANTS-3506 — the predicate moved from AuditDialog (GUI TU) to the
// AuditEngine namespace in ants_audit_lib, so this test now lives in the
// engine-only test_audit bundle (no GUI lib linked) and calls it directly.

#include <gtest/gtest.h>

#include "auditengine.h"

#include <QString>

namespace {

// Shorthand: `comment` is the token text as commentSuppresses sees it (the
// leading `//` is already stripped by the caller before this point).
bool suppresses(const char *comment, const char *rule) {
    return AuditEngine::commentSuppresses(QString::fromUtf8(comment),
                                          QString::fromUtf8(rule));
}

}  // namespace

// INV-9 — the new `audit: drop` alias behaves identically to the long form.
TEST(AuditDropAlias, Inv9BareFormSuppressesEverything) {
    // Bare token, no rule list → suppresses any rule on the line.
    EXPECT_TRUE(suppresses("audit: drop", "anything"));
    EXPECT_TRUE(suppresses("audit:drop", "some-rule"));         // no-space variant
    EXPECT_TRUE(suppresses("audit: drop-next-line", "x"));      // -next-line variant
}

TEST(AuditDropAlias, Inv9RuleListTargetsOnlyNamedRule) {
    // `=rule` suffix → suppresses that rule only, not others.
    EXPECT_TRUE(suppresses("audit: drop=foo-bar", "foo-bar"));
    EXPECT_FALSE(suppresses("audit: drop=foo-bar", "baz"));
}

TEST(AuditDropAlias, Inv9GlobRuleMatches) {
    // A `*` glob in the rule list matches by prefix.
    EXPECT_TRUE(suppresses("audit: drop=goog*", "google-creds"));
    EXPECT_FALSE(suppresses("audit: drop=goog*", "aws-creds"));
}

TEST(AuditDropAlias, NonSuppressCommentDoesNotSuppress) {
    // A comment that merely mentions "audit"/"drop" but is not the token must
    // NOT suppress (over-match guard, ANTS-1111 §2.6).
    EXPECT_FALSE(suppresses("audit: see ANTS-1111", "x"));
    EXPECT_FALSE(suppresses("dropping the audit table", "x"));
    EXPECT_FALSE(suppresses("", "x"));  // empty comment
}

// The long form the alias was added alongside must still work — both coexist.
TEST(AuditDropAlias, LongFormStillSuppresses) {
    EXPECT_TRUE(suppresses("ants-audit: disable", "anything"));
    EXPECT_TRUE(suppresses("ants-audit: disable=foo-bar", "foo-bar"));
    EXPECT_FALSE(suppresses("ants-audit: disable=foo-bar", "baz"));
}
