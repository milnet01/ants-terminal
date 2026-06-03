# ANTS-1974 — User-selectable home/baseline model tier

**Status:** accepted (2026-06-04), cold-eyes loop 1 folded (polish-only, no design change)
**Kind:** feature
**Source:** user-request-2026-06-03 / 2026-06-04 (confirmed home-tier = baseline interpretation)
**Lanes:** modelrecommender, modelautoswitch, config, settingsdialog

## Problem

The recommender hardcodes Sonnet as the neutral band: a task-shape score in
`[-2, 2)` recommends Sonnet, `>= 2` Opus, `<= -2` Haiku (`modelrecommender.cpp`
§score). The user wants **Opus as the working baseline**, downgrading only on
clear evidence — the opposite default. Today the only way to stay on Opus is to
disable the auto-switcher entirely, which also forfeits the upgrade signal.

Confirmed with the user (2026-06-03): "default model" means the **home/baseline
tier** the recommender centres on (interpretation B), not merely the new-session
starting model (interpretation A). The auto-switcher returns to the home tier
for ordinary work and only steps below it on clear mechanical/cheap evidence.

The user's repeated frustration (2026-06-04, "Sonnet/Haiku make too many
mistakes") is the motivating case: home = Opus.

## Surface

- **Config key** `claude.auto_model_home_tier` (string `"haiku"|"sonnet"|"opus"`,
  default `"sonnet"`). Surfaced in the `claudeAutoModel()` envelope as
  `"home_tier"` (snake_case, matching the existing `switch_enabled` /
  `min_dwell_sec` / `floor` keys). Read accessor +
  `setClaudeAutoModelHomeTier(const QString&)`, which follows the established
  `storeIfChanged(...) + save()` setter pattern (`config.cpp` §setClaudeAutoModelToastEnabled).
- **`ModelRecommender::tierForScore(int sc, bool mechanical, Tier home)`** — new
  pure helper holding the band logic. `Tier` is `enum class {Haiku, Sonnet,
  Opus}` (= 0/1/2), so the `home ± N` steps are computed via `static_cast<int>`,
  clamped to `[0, 2]`, and re-cast to `Tier`. `score()` gains a
  `Tier homeTier = Tier::Sonnet` parameter (default preserves every existing
  caller + test) and delegates the final band decision to it. The memo cache key
  becomes `(mtime, size, homeTier)` so the same transcript scored under two home
  tiers does not return a stale tier; the stored value remains the **post-band
  `Result`** (final tier), not the pre-band score.
- **Controller** (`claudestatuswidgets.cpp`) reads the configured home tier and
  passes it to `score()`, so the chip *and* the auto-switch Gate see the same
  home-relative recommendation.
- **Settings dialog** — a dropdown "Home model (auto-switch baseline)" with
  Haiku / Sonnet / Opus, persisting `claude.auto_model_home_tier`. Lives beside
  the existing "Let Ants pick the Claude model for me" toggle.
- **Floor interaction** — the floor (`claude.auto_model_floor`) is `{haiku,
  sonnet}`-only in `config.cpp` (an `"opus"` floor is coerced to `"haiku"`); this
  spec does NOT widen it. The clamp is **one-sided**: `effectiveHome =
  max(home, floor)`, so a home below the floor is raised to the floor, but the
  floor can never force a home down (the home is the baseline; the floor is only
  the hard lower bound). The Gate's existing `clampToFloor(rec, floor)` still
  applies to the final recommendation on top of this.
- **Epoch** — `kSwitcherEpoch` 2 → 3 (`modelswitchledger.h`). The *meaning* of a
  downgrade record changes once the neutral band is user-relative, so the trust
  signal recalibrates cleanly rather than blending pre/post-home records. The
  per-value rationale comment block above the constant is append-only — add an
  ANTS-1974 line, never edit prior lines.

## Band design

`tierForScore(sc, mechanical, home)`, with `Tier` ordered Haiku(0) < Sonnet(1)
< Opus(2) and `clamp` to `[Haiku, Opus]`:

| Score band            | Result                         |
|-----------------------|--------------------------------|
| `sc >= 2` (up)        | `min(home + 1, Opus)`          |
| `-2 < sc < 2` (neutral) | `home`                       |
| `sc <= -2`, not mechanical (mild down) | `max(home - 1, Haiku)` |
| `sc <= -2`, mechanical (strong down)   | `max(home - 2, Haiku)` |

`home = Sonnet` reproduces today's mapping byte-for-byte: up→Opus,
neutral→Sonnet, both down arms→Haiku (the `-2` clamp collapses the mechanical
distinction at Sonnet, exactly as today). `home = Opus`: up→Opus, neutral→Opus,
mild-down→Sonnet, strong-down(mechanical)→Haiku. The Gate's existing
`clampToFloor` still applies on top of the result.

## Invariants

- **INV-1** — `tierForScore(sc, mechanical, Tier::Sonnet)` is identical to the
  pre-1974 inline band for every `(sc, mechanical)`: `sc>=2`→Opus, `sc<=-2`→Haiku
  (both mechanical values), else Sonnet. *(back-compat: default home is a no-op)*
- **INV-2** — Neutral band (`-2 < sc < 2`) always returns exactly `home`, for
  every home tier.
- **INV-3** — Upgrade arm never exceeds Opus: `sc>=2` with `home==Opus` returns
  Opus (not an out-of-range tier).
- **INV-4** — Downgrade arms never go below Haiku: `sc<=-2` with `home==Haiku`
  returns Haiku for both mechanical values.
- **INV-5** — With `home==Opus`: mild-down (`sc<=-2`, not mechanical) → Sonnet;
  strong-down (`sc<=-2`, mechanical) → Haiku. A non-mechanical heavy task never
  drops to Haiku from an Opus home.
- **INV-6** — `claudeAutoModel()["home"]` defaults to `"sonnet"` when the key is
  absent or holds an unrecognised string; round-trips `"haiku"/"sonnet"/"opus"`.
- **INV-7** — A configured home below the floor is clamped to the floor when the
  controller builds the recommendation (`effectiveHome = max(home, floor)`).
- **INV-8** — `score()` called without a `homeTier` argument behaves exactly as
  before 1974 (Tier::Sonnet default); the memo cache returns a home-correct
  result when the same transcript is scored under two different home tiers.

## Tests

`tests/features/model_home_tier/` (new bundle source compiled into `test_core`,
alongside `model_recommender` / `model_auto_switch` — that target links
`modelrecommender.cpp` directly):
- `tierForScore` truth table covering INV-1..INV-5 (every band × home × mechanical).
- Config round-trip for INV-6 (default, each tier, garbage → sonnet).
- Floor-clamp for INV-7.
- Memo correctness for INV-8 (score twice under sonnet then opus home → distinct tiers).

## Cold-eyes loop log

- **Loop 1 (2026-06-04):** 0 CRITICAL, 2 HIGH, 3 MEDIUM, 2 LOW — all verified
  and folded. H1 floor value-domain + one-sided clamp pinned; H2 envelope key
  `"home"`→`"home_tier"` (snake_case precedent); M1 epoch comment append-only
  note; M2 memo key spelled `(mtime, size, homeTier)` storing the post-band
  Result; M3 test bundle corrected `test_claude`→`test_core` (verified in
  CMakeLists.txt); L1 enum-cast arithmetic note; L2 this log. Open questions
  resolved: floor stays `{haiku,sonnet}` (no widen); setter follows
  `storeIfChanged+save`. Findings were accuracy/clarity polish (no design
  change), so per user direction implementation proceeds without a 2nd loop.
