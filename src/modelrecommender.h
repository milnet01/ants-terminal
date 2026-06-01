// ANTS-1226 — Passive model-tier recommender for the status-bar chip.
// Stateless free function: score(transcriptPath) → Tier + reason.
#pragma once
#include <QString>

namespace ModelRecommender {

enum class Tier { Haiku, Sonnet, Opus };

struct Result {
    Tier    tier         = Tier::Sonnet;
    QString reason;       // short rationale for the tooltip
    QString currentModel; // message.model from most-recent assistant turn
    // ANTS-1940 — task-shape signal: true when the last scoring window shows
    // purely mechanical work (fileWriteCount==0 && toolDiversity<=2). Used by
    // the auto-switch gate to relax the dwell floor on downgrade-to-Haiku.
    bool    isMechanical = false;
};

// score() reads at most 512 KB from the tail of the JSONL transcript
// at transcriptPath, scores the last 20 assistant turns.
// Returns Sonnet if the file is absent or has no assistant turns.
// Stateless: no per-call mutable state.
Result score(const QString &transcriptPath);

// tierName() converts a Tier to the /model command argument string.
// Haiku → "haiku", Sonnet → "sonnet", Opus → "opus".
QString tierName(Tier tier);

// tierFromModelId() maps a Claude model ID string to a Tier.
// Contains "haiku" → Haiku, contains "opus" → Opus, else → Sonnet.
Tier tierFromModelId(const QString &modelId);

// ANTS-1888 — Passive thinking-level readout for the per-tab status chip.
//
// Claude Code accepts inline thinking-budget directives in the user's prompt:
// `ultrathink`, `think harder`, `think hard`, `think`, or the explicit
// `/nothink` (treated as Standard). This enum makes the directive-set
// stable across the chip + tests without leaking it into the broader scorer.
enum class ThinkingLevel {
    Unknown,     // transcript absent OR no user turn found in the tail window
    Standard,    // user turn present, no directive matched (or /nothink)
    Think,
    ThinkHard,
    Ultrathink,
};

// thinkingLevelFromLatestUserTurn() tail-reads up to 512 KB of the JSONL
// transcript at `transcriptPath`, locates the most recent `{type:"user"}`
// line, joins its text-content blocks, and returns the matching directive
// (longest match wins). Returns Unknown when the transcript is missing or
// no user turn is present in the window; Standard when a user turn is
// present but carries no directive. Pure; no Qt threading.
ThinkingLevel thinkingLevelFromLatestUserTurn(const QString &transcriptPath);

// ANTS-1890 — Pure stem-regex over the latest user prompt's text. Returns
// true when any of `commit/push/stage/bump/rebase` (plus their `-ed`/`-ing`/
// `-s` inflections, optional `/`-prefix for `/commit` style slash commands)
// matches as a word-boundary stem. Substrings inside other words (e.g.
// "committee", "pushcart", "thinkpad") do NOT match. Caller pre-lowercases
// `latestUserText` via QString::toLower() — the helper matches plain ASCII.
// Pure; O(N) in input length; no I/O. Drives the score() hard override.
bool hasCommitIntent(const QString &latestUserText);

// ANTS-1890 — Recency weight for a turn at zero-based index `idx` within a
// window of `total` turns (idx=total-1 is the LATEST turn). Returns a
// double in [1.0, 3.0]: linear, w = 1.0 + 2.0 * (idx / max(1, total-1)).
// w(0,n)==1.0 for all n>=1; w(total-1,total)==3.0 for total>=2;
// w(0,1)==1.0. Undefined for total<1 — caller must short-circuit on empty
// windows. Pure; no I/O.
double weightForTurnIndex(int idx, int total);

// thinkingLevelLabel() returns a short human label for the chip.
// Unknown → empty string (chip hides the thinking half — never "Unknown");
// Standard → "standard"; rest → "think" / "think hard" / "ultrathink".
QString thinkingLevelLabel(ThinkingLevel level);

}  // namespace ModelRecommender
