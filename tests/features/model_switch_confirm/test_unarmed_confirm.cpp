// Feature-conformance test for ANTS-1951 — auto-confirm the "Switch model?"
// dialog for user-typed /model commands. See spec.md.
//
// Pure gate: ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch(dialogVisible,
// enabled, handshakeInFlight, alreadyConfirmed, claudeRunning,
// permissionPending, userTypedModel) returns true only when a dialog is
// visible, the feature is on, no Ants-initiated handshake is already polling
// it, we have not already pressed ENTER for this dialog instance, Claude Code
// runs in the tab, no permission prompt is pending, and the user just
// submitted /model there (ANTS-6437, with the tracker that sees it).

#include <gtest/gtest.h>
#include <QString>
#include "modelautoswitch.h"

#include "../../_support/srcgrep.h"

using ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch;
using ModelAutoSwitch::shouldContinueAfterUnarmedConfirm;
using ModelAutoSwitch::directModelSwitchVisible;
using ModelAutoSwitch::shouldContinueAfterDirectSwitch;

// The happy path: dialog up, feature on, nobody else handling it, not yet
// confirmed → press ENTER.
TEST(UnarmedConfirm, FiresOnlyWhenAllConditionsHold) {
    EXPECT_TRUE(shouldAutoConfirmUnarmedSwitch(
        /*dialogVisible=*/true, /*enabled=*/true,
        /*handshakeInFlight=*/false, /*alreadyConfirmed=*/false,
        /*claudeRunning=*/true, /*permissionPending=*/false,
        /*userTypedModel=*/true));
}

// No dialog on screen → nothing to confirm.
TEST(UnarmedConfirm, NoDialogNoFire) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(false, true, false, false, true, false, true));
}

// Feature toggled off → never acts, even with a dialog present.
TEST(UnarmedConfirm, DisabledNeverFires) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, false, false, false, true, false, true));
}

// An Ants-initiated handshake owns this dialog → stand down (no double ENTER).
TEST(UnarmedConfirm, HandshakeInFlightStandsDown) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, true, false, true, false, true));
}

// Already pressed ENTER for this dialog instance → don't press again.
TEST(UnarmedConfirm, LatchPreventsDoublePress) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, false, true, true, false, true));
}

// ANTS-6433 — the dialog's words can come from any output, such as `cat` of a
// file in a bare shell. Without Claude Code in the tab, never press ENTER.
TEST(UnarmedConfirm, NoClaudeInTabNeverFires) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, false, false,
                                                /*claudeRunning=*/false, false, true));
}

// ANTS-6433 — a pending permission prompt takes ENTER as "1. Yes", so the words
// shown inside it (a command quoting them) must not make Ants approve it.
TEST(UnarmedConfirm, PermissionPromptNeverFires) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, false, false, true,
                                                /*permissionPending=*/true, true));
}

// ANTS-6437 — at idle, with no prompt pending, output can still show the
// dialog's words. Without a /model the user just typed, never press ENTER.
TEST(UnarmedConfirm, NoRecentUserModelCommandNeverFires) {
    EXPECT_FALSE(shouldAutoConfirmUnarmedSwitch(true, true, false, false, true,
                                                false, /*userTypedModel=*/false));
}

// ANTS-6433 / ANTS-6437 — both callers feed the gate the focused shell's
// tracked state and the tab's /model stamp, not constants.
TEST(UnarmedConfirm, CallersPassTheTrackedState) {
    const std::string src = ants_test::slurpFile(ANTS_SRC_DIR "/claudestatuswidgets.cpp");
    const std::string call = "ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch(";
    int calls = 0;
    for (size_t at = src.find(call); at != std::string::npos; at = src.find(call, at + 1)) {
        const size_t end = src.find(';', at);
        const std::string args = src.substr(at, end - at);
        EXPECT_NE(args.find("ss.state != ClaudeState::NotRunning, ss.awaitingInput,"),
                  std::string::npos) << args;
        EXPECT_NE(args.find("userTypedModelRecently(focused)"), std::string::npos) << args;
        EXPECT_LT(args.size(), 400u) << args;
        ++calls;
    }
    EXPECT_EQ(calls, 2);
}

// ANTS-6437 — the per-tab tracker of a typed `/model` line.
namespace {
using ModelAutoSwitch::ModelCommandTracker;
using ModelAutoSwitch::feedModelCommandKey;
using K = ModelAutoSwitch::TypedKey;

// Types `text` then Enter; returns whether the tracker saw a /model line.
bool typeAndSubmit(ModelCommandTracker &t, const QString &text) {
    for (const QChar c : text) feedModelCommandKey(t, K::Text, QString(c));
    return feedModelCommandKey(t, K::Enter, QString());
}
}  // namespace

TEST(UnarmedConfirm, TrackerSeesTypedModelCommands) {
    ModelCommandTracker t;
    EXPECT_TRUE(typeAndSubmit(t, QStringLiteral("/model")));
    EXPECT_TRUE(typeAndSubmit(t, QStringLiteral("/model sonnet")));
    // `/mo` picked from Claude Code's slash menu with Enter.
    EXPECT_TRUE(typeAndSubmit(t, QStringLiteral("/mo")));
}

TEST(UnarmedConfirm, TrackerIgnoresOtherLines) {
    ModelCommandTracker t;
    EXPECT_FALSE(typeAndSubmit(t, QStringLiteral("")));
    EXPECT_FALSE(typeAndSubmit(t, QStringLiteral("/m")));
    EXPECT_FALSE(typeAndSubmit(t, QStringLiteral("/mcp")));
    EXPECT_FALSE(typeAndSubmit(t, QStringLiteral("please /model")));
    EXPECT_FALSE(typeAndSubmit(t, QStringLiteral("ls /mo")));
    // Enter resets the line: the next empty Enter is not a /model.
    EXPECT_TRUE(typeAndSubmit(t, QStringLiteral("/model")));
    EXPECT_FALSE(feedModelCommandKey(t, K::Enter, QString()));
}

TEST(UnarmedConfirm, TrackerFollowsBackspace) {
    ModelCommandTracker t;
    // "/x", erase the x, then "mo" → the line reads "/mo".
    feedModelCommandKey(t, K::Text, QStringLiteral("/"));
    feedModelCommandKey(t, K::Text, QStringLiteral("x"));
    feedModelCommandKey(t, K::Backspace, QString());
    EXPECT_TRUE(typeAndSubmit(t, QStringLiteral("mo")));
    // "/mo", erase back to "/m", then "cp" → "/mcp".
    for (const QChar c : QStringLiteral("/mo")) feedModelCommandKey(t, K::Text, QString(c));
    feedModelCommandKey(t, K::Backspace, QString());
    EXPECT_FALSE(typeAndSubmit(t, QStringLiteral("cp")));
}

TEST(UnarmedConfirm, TrackerTabKeepsTheLine) {
    ModelCommandTracker t;
    for (const QChar c : QStringLiteral("/mo")) feedModelCommandKey(t, K::Text, QString(c));
    feedModelCommandKey(t, K::Tab, QString());
    EXPECT_TRUE(feedModelCommandKey(t, K::Enter, QString()));
}

// A cursor move or control key makes the line unknown, so it never counts.
TEST(UnarmedConfirm, TrackerOtherKeyMakesTheLineUnknown) {
    ModelCommandTracker t;
    for (const QChar c : QStringLiteral("/mo")) feedModelCommandKey(t, K::Text, QString(c));
    feedModelCommandKey(t, K::Other, QString());
    EXPECT_FALSE(feedModelCommandKey(t, K::Enter, QString()));
}

// The window opens at a stamp and closes kUserModelCommandWindowMs later.
TEST(UnarmedConfirm, ModelCommandWindow) {
    using ModelAutoSwitch::modelCommandRecent;
    using ModelAutoSwitch::kUserModelCommandWindowMs;
    EXPECT_FALSE(modelCommandRecent(/*stampMs=*/-1, /*nowMs=*/1000));
    EXPECT_TRUE(modelCommandRecent(1000, 1000));
    EXPECT_TRUE(modelCommandRecent(1000, 1000 + kUserModelCommandWindowMs));
    EXPECT_FALSE(modelCommandRecent(1000, 1001 + kUserModelCommandWindowMs));
    EXPECT_FALSE(modelCommandRecent(5000, 1000));  // a stamp in the future
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

// ANTS-6754 — the banner Claude Code prints today, read from its transcripts
// (2026-11-13): the model name sits in backticks, and the family may be one the
// old list never named.
TEST(UnarmedConfirm, DirectSwitchBannerNamesAnyModel) {
    EXPECT_TRUE(directModelSwitchVisible(QStringLiteral(
        "Set model to `Fable 5.2` and saved as your default for new sessions")));
    EXPECT_TRUE(directModelSwitchVisible(QStringLiteral(
        "Set model to `Opus 5.5` and saved as your default for new sessions")));
    EXPECT_TRUE(directModelSwitchVisible(QStringLiteral(
        "Set model to Fable 5.2 and saved as your default for new sessions")));
    // Wrapped by a narrow terminal.
    EXPECT_TRUE(directModelSwitchVisible(QStringLiteral(
        "Set model to `Fable 5.2` and saved as\nyour default for new sessions")));
}

// ANTS-6754 — the title and the tail are one sentence. Both phrases present but
// apart is prose, not the banner.
TEST(UnarmedConfirm, DirectSwitchTitleAndTailMustBeOneSentence) {
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral(
        "Set model to Opus is what I typed earlier in the session, and much later "
        "the docs said it is saved as your default for new sessions")));
    EXPECT_FALSE(directModelSwitchVisible(QStringLiteral(
        "set model to value from config; saved as your default")));
}
