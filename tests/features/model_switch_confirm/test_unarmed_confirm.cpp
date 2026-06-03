// Feature-conformance test for ANTS-1951 — auto-confirm the "Switch model?"
// dialog for user-typed /model commands. See spec.md.
//
// Pure gate: ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch(dialogVisible,
// enabled, handshakeInFlight, alreadyConfirmed) returns true only when a dialog
// is visible, the feature is on, no Ants-initiated handshake is already polling
// it, and we have not already pressed ENTER for this dialog instance.

#include <gtest/gtest.h>
#include "modelautoswitch.h"

using ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch;
using ModelAutoSwitch::shouldContinueAfterUnarmedConfirm;

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
