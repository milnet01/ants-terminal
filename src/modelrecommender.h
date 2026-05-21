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

}  // namespace ModelRecommender
