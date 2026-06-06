// Claude permission-prompt structural gate (ANTS-1993). See spec.md.
//
// Drives ClaudePromptDetect::isPermissionPromptStructure through real and
// hostile line sets. The pre-fix detector anchored on any single footer
// phrase, so INV-4 / INV-5 (lone weak-anchor lines) would have fired a
// phantom allowlist button.

#include "../../_support/expect.h"
#include "claudepromptdetect.h"

#include <gtest/gtest.h>
#include <QString>
#include <QStringList>

ANTS_TEST_SCOPE();

using ClaudePromptDetect::isPermissionPromptStructure;

TEST(ClaudePermissionPromptStructure, Main) {
    expect_reset();

    // INV-1 — real older prompt: "Tab to accept" footer corroborates alone.
    expect(isPermissionPromptStructure({
               QStringLiteral("Bash command"),
               QStringLiteral("npm install"),
               QStringLiteral("y \xC2\xB7 yes   n \xC2\xB7 no"),
               QStringLiteral("Esc to cancel \xC2\xB7 Tab to accept \xC2\xB7 Ctrl+e to explain"),
           }),
           "INV-1/older-prompt-true");

    // INV-2 — real newer prompt: question + numbered options.
    expect(isPermissionPromptStructure({
               QStringLiteral("Read"),
               QStringLiteral("/home/u/project/file.txt"),
               QStringLiteral("Do you want to proceed?"),
               QStringLiteral("\xE2\x9D\xAF 1. Yes"),
               QStringLiteral("  2. No, and tell Claude what to do differently"),
           }),
           "INV-2/newer-prompt-true");

    // INV-3 — file-access prompt: weak anchor + Esc-to-cancel selection.
    expect(isPermissionPromptStructure({
               QStringLiteral("Claude wants to allow access to /home/u/x"),
               QStringLiteral("Esc to cancel"),
           }),
           "INV-3/allow-access-corroborated-true");

    // INV-4 — headline injection vector: a lone "always allow" line.
    expect(!isPermissionPromptStructure({
               QStringLiteral("config: always allow reconnect on timeout"),
               QStringLiteral("Read"),
           }),
           "INV-4/lone-always-allow-false");

    // INV-5 — lone "allow access to <path>" with no prompt structure.
    expect(!isPermissionPromptStructure({
               QStringLiteral("note: allow access to /etc/shadow for backup"),
           }),
           "INV-5/lone-allow-access-false");

    // INV-6 — unrelated benign output, no anchor at all.
    expect(!isPermissionPromptStructure({
               QStringLiteral("Compiling target ants-terminal"),
               QStringLiteral("[ 42% ] Building CXX object"),
               QStringLiteral("y/n prompt elsewhere? no"),
           }),
           "INV-6/no-anchor-false");

    // INV-7 — anchor and selection on different lines still corroborate.
    expect(isPermissionPromptStructure({
               QStringLiteral("always allow this tool"),
               QStringLiteral("blank-ish separator"),
               QStringLiteral("(y/n)"),
           }),
           "INV-7/split-markers-true");

    ASSERT_EQ(0, expect_finish());
}
