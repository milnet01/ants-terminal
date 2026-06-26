// Feature-conformance test for ANTS-1951 — auto-confirm the "Switch model?"
// dialog for user-typed /model commands. See spec.md.
//
// Pure gate: ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch(dialogVisible,
// enabled, handshakeInFlight, alreadyConfirmed) returns true only when a dialog
// is visible, the feature is on, no Ants-initiated handshake is already polling
// it, and we have not already pressed ENTER for this dialog instance.

#include <gtest/gtest.h>
#include <QString>
#include "modelautoswitch.h"

using ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch;
using ModelAutoSwitch::shouldContinueAfterUnarmedConfirm;
using ModelAutoSwitch::directModelSwitchVisible;
using ModelAutoSwitch::shouldContinueAfterDirectSwitch;

// The happy path: dialog up, feature on, nobody else handling it, not yet
// confirmed → press ENTER.
TEST(UnarmedConfirm, FiresOnlyWhenAllConditionsHold) {
    EXPECT_TRUE(shouldAutoConfirmUnarmedSwitch(
        /*dialogVisible=*/true, /*enabled=*/true,
        /*handshakeInFlight=*/false, /*alreadyConfirmed=*/false));
}

// No dialog on screen → nothing to confirm.
TEST(UnarmedConfirm, NoDialogNoFire) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(false, true, false, false));
}

// Feature toggled off → never acts, even with a dialog present.
TEST(UnarmedConfirm, DisabledNeverFires) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, false, false, false));
}

// An Ants-initiated handshake owns this dialog → stand down (no double ENTER).
TEST(UnarmedConfirm, HandshakeInFlightStandsDown) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, true, false));
}

// Already pressed ENTER for this dialog instance → don't press again.
TEST(UnarmedConfirm, LatchPreventsDoublePress) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, false, true));
}

// ANTS-1969 — continuation after a user-typed confirm.
// Auto mode ON + active turn → resume the task automatically.
TEST(UnarmedConfirm, ContinuesWhenAutoModeOnAndActiveTurn) {
    EXPECT_TRUE(shouldContinueAfterUnarmedConfirm(
        /*autoModeOn=*/true, /*activeTurn=*/true));
}

// Auto mode OFF → ANTS-1958 holds: the user has their own next message ready.
TEST(UnarmedConfirm, NoContinuationWhenAutoModeOff) {
    EXPECT_FALSE(shouldContinueAfterUnarmedConfirm(false, true));
}

// Idle (no active turn) → ANTS-1959 billing safety: never start unrequested work.
TEST(UnarmedConfirm, NoContinuationAtIdle) {
    EXPECT_FALSE(shouldContinueAfterUnarmedConfirm(true, false));
    EXPECT_FALSE(shouldContinueAfterUnarmedConfirm(false, false));
}

// ANTS-1975 — direct /model <tier> (no dialog) detection and continuation.

// CC's banner "Set model to Sonnet 4.6…" triggers the detector.
TEST(UnarmedConfirm, DirectSwitchBannerDetected) {
    EXPECT_TRUE(directModelSwitchVisible(
        QStringLiteral("Set model to Sonnet 4.6 and saved as your default for new sessions")));
    EXPECT_TRUE(directModelSwitchVisible(
        QStringLiteral("Set model to Opus 4.8 and saved as your default for new sessions")));
    // Case-insensitive (full banner, lowercased).
    EXPECT_TRUE(directModelSwitchVisible(
        QStringLiteral("set model to haiku and saved as your default for new sessions")));
}

// ANTS-2197 — the tier-anchored title ALONE (no "saved as your default" tail) must
// NOT match: a bare quote of "Set model to Opus" in scrollback prose / a
// transcript would otherwise fire an unwanted continuation under auto mode. The
// detector now requires the banner's distinctive persistent-default corroborator.
TEST(UnarmedConfirm, DirectSwitchTitleWithoutCorroboratorNotFalsePositive) {
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral("Set model to Opus")));
    EXPECT_FALSE(directModelSwitchVisible(
        QStringLiteral("...as the review noted, Set model to Opus 4.8 would...")));
    // The corroborator alone (no tier title) also must not match.
    EXPECT_FALSE(directModelSwitchVisible(
        QStringLiteral("your edits were saved as your default formatting")));
}

// An unrelated output line must not match.
TEST(UnarmedConfirm, DirectSwitchBannerNotFalsePositive) {
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral("")));
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral("Switch model?")));
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral("Yes, switch to Sonnet")));
}

// ANTS-2020 — the bare phrase "Set model to" without a tier token must NOT
// trip the detector: build output, a log line, or a transcript quoting the
// phrase would otherwise fire an unwanted continuation under auto mode.
TEST(UnarmedConfirm, DirectSwitchBarePhraseNotFalsePositive) {
    // Prose / instruction quoting the phrase — no tier follows.
    EXPECT_FALSE(directModelSwitchVisible(
        QStringLiteral("Please set model to whatever you think is best.")));
    // A log/build line that happens to contain the words.
    EXPECT_FALSE(directModelSwitchVisible(
        QStringLiteral("[debug] handler: set model to value from config")));
    // The literal phrase alone (the pre-2020 false-positive surface).
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral("Set model to")));
}

// ANTS-2186 — continuation fires only with auto mode ON *and* an active turn.
TEST(UnarmedConfirm, DirectSwitchContinuesWhenAutoModeOnAndActiveTurn) {
    EXPECT_TRUE(shouldContinueAfterDirectSwitch(/*autoModeOn=*/true,
                                                /*activeTurn=*/true));
}

// Auto mode OFF → no continuation, regardless of turn state.
TEST(UnarmedConfirm, DirectSwitchNoContinuationWhenAutoModeOff) {
    EXPECT_FALSE(shouldContinueAfterDirectSwitch(false, true));
    EXPECT_FALSE(shouldContinueAfterDirectSwitch(false, false));
}

// ANTS-2186 — idle (no active turn) → never continue, even with auto mode on:
// pre-picking a model with `/model` is not consent to start a billable turn
// (the ANTS-1959 invariant), mirroring shouldContinueAfterUnarmedConfirm.
TEST(UnarmedConfirm, DirectSwitchNoContinuationAtIdle) {
    EXPECT_FALSE(shouldContinueAfterDirectSwitch(/*autoModeOn=*/true,
                                                 /*activeTurn=*/false));
}
