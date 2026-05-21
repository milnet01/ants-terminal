# model_recommender — Feature Spec (ANTS-1226 Shape A)

## Purpose
Passive status-bar chip that scores session complexity from the Claude
Code transcript and recommends a model tier, saving spend on simple
turns under a Max(5) subscription.

## Active model detection
The running model is read from `message.model` of the most-recent
`assistant` turn in the transcript JSONL (verified: always present as
e.g. "claude-opus-4-7"). Returns empty string when the transcript has
no assistant turns → treated as Sonnet tier.

## Scoring algorithm (last 20 assistant turns, tail-read ≤ 512 KB)

Feature                                        | Weight
---------------------------------------------- | ------
file_write_count ≥ 4 (Edit/Write tool calls)   | +2 (Opus signal)
tool_diversity ≥ 6 unique tool names           | +1 (Opus signal)
plan_keyword in assistant text                 | +2 (Opus: "spec","design","architecture","review","plan","refactor")
avg_message_len ≥ 500 chars                    | +1 (Opus: long context)
file_write_count == 0 AND tool_diversity ≤ 2   | -2 (Haiku: mechanical)

Score ≥ 3  → OPUS_TIER
Score ≤ -1 → HAIKU_TIER
Otherwise  → SONNET_TIER

## Invariants

- **INV-1** `score(transcriptPath)` returns `SONNET_TIER` when the
  transcript file does not exist or has 0 assistant turns.
- **INV-2** Returns `OPUS_TIER` when a plan_keyword is present AND
  file_write_count ≥ 4 across the last 20 turns.
- **INV-3** Returns `HAIKU_TIER` when file_write_count == 0 AND
  tool_diversity ≤ 2 across the last 20 turns.
- **INV-4** The chip is hidden when the recommended tier matches the
  tier inferred from `message.model` in the most-recent assistant turn.
- **INV-5** The chip shows "→ Haiku" / "→ Sonnet" / "→ Opus" text when
  the recommendation differs from the current model tier.
- **INV-6** Clicking the chip calls `sendToPty("/model haiku\n")` (or
  sonnet/opus) on the focused TerminalWidget.
- **INV-7** `score()` reads at most 512 KB of trailing transcript data;
  does not load a 100 MB transcript into a QStringList.
- **INV-8** Only the last 20 assistant turns are scored (older turns
  do not influence the recommendation).
- **INV-9** `score()` is a stateless free function — no per-call
  mutable state; calling it twice with the same path and unchanged
  file returns the same result.
