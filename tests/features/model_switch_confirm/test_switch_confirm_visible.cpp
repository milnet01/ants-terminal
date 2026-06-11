// Feature-conformance test for ANTS-1920 — output-driven model-switch confirm.
// See tests/features/model_switch_confirm/spec.md.
//
// Pure detector (INV-1..INV-4). No live terminal: every case feeds a string to
// ModelAutoSwitch::switchConfirmVisible and asserts the boolean.

#include <gtest/gtest.h>
#include "modelautoswitch.h"

namespace {

using ModelAutoSwitch::switchConfirmVisible;

// ANTS-2068 — the version suffixes ("4.6", "4.8", "4.5") in the fixtures
// below are illustrative only and do NOT need bumping when Claude Code
// ships new model versions: switchConfirmVisible / switchBannerVisible key
// on the TIER name ("Sonnet"/"Opus"/"Haiku"/"Default") and the surrounding
// banner phrasing, never the version number (see modelautoswitch.cpp).
//
// The real CC dialog (verified 2026-06-01), Sonnet variant.
const char *kRealDialogSonnet =
    "Switch model?\n"
    "Your next response will be slower and use more tokens\n"
    "\n"
    "This conversation is cached for the current model. Switching to Sonnet 4.6\n"
    "means the full history gets re-read on your next message.\n"
    "\n"
    "\xe2\x9d\xaf 1. Yes, switch to Sonnet 4.6\n"
    "  2. No, go back\n";

// Same dialog shape, a different target model.
const char *kRealDialogOpus =
    "Switch model?\n"
    "Your next response will be slower and use more tokens\n"
    "\n"
    "\xe2\x9d\xaf 1. Yes, switch to Opus 4.8\n"
    "  2. No, go back\n";

}  // namespace

// INV-1 — the real dialog is detected (both variants).
TEST(SwitchConfirmVisible, Inv1DetectsRealDialog) {
    EXPECT_TRUE(switchConfirmVisible(QString::fromUtf8(kRealDialogSonnet)));
    EXPECT_TRUE(switchConfirmVisible(QString::fromUtf8(kRealDialogOpus)));
}

// INV-1 — title plus either option line suffices (e.g. only "No, go back").
TEST(SwitchConfirmVisible, Inv1TitlePlusEitherOption) {
    EXPECT_TRUE(switchConfirmVisible(
        QStringLiteral("Switch model?\n  2. No, go back\n")));
    EXPECT_TRUE(switchConfirmVisible(
        QStringLiteral("Switch model?\n  1. Yes, switch to Haiku 4.5\n")));
}

// INV-2 — ordinary terminal output and empty input never match.
TEST(SwitchConfirmVisible, Inv2NoFalsePositive) {
    EXPECT_FALSE(switchConfirmVisible(QString()));
    EXPECT_FALSE(switchConfirmVisible(QStringLiteral("")));
    EXPECT_FALSE(switchConfirmVisible(QStringLiteral(
        "$ git status\nOn branch main\nnothing to commit, working tree clean\n")));
    EXPECT_FALSE(switchConfirmVisible(QStringLiteral(
        "Running tests... 1483/1483 passed.\nDone.\n")));
}

// INV-3 — the title alone, with no option line, is not a confirmable dialog.
TEST(SwitchConfirmVisible, Inv3TitleAloneInsufficient) {
    EXPECT_FALSE(switchConfirmVisible(QStringLiteral(
        "I added a 'Switch model?' prompt detector to the actuator today.\n")));
    EXPECT_FALSE(switchConfirmVisible(QStringLiteral("Switch model?\n")));
    // An option line without the title is also insufficient.
    EXPECT_FALSE(switchConfirmVisible(QStringLiteral(
        "  1. Yes, switch to Opus 4.8\n  2. No, go back\n")));
}

// INV-4 — detection is case-insensitive.
TEST(SwitchConfirmVisible, Inv4CaseInsensitive) {
    EXPECT_TRUE(switchConfirmVisible(QStringLiteral(
        "SWITCH MODEL?\n  1. YES, SWITCH TO SONNET 4.6\n")));
}
