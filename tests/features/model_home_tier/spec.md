# model_home_tier — ANTS-1974 home/baseline model tier

Contract for `ModelRecommender::tierForScore(sc, mechanical, home)` and the
home-relative `score()` band. See `docs/specs/ANTS-1974.md`.

- **INV-1** — `tierForScore(sc, mechanical, Sonnet)` reproduces the pre-1974
  mapping: `sc>=2`→Opus, `sc<=-2`→Haiku (both mechanical values), else Sonnet.
- **INV-2** — neutral band (`-2 < sc < 2`) returns exactly `home` for every home.
- **INV-3** — up arm (`sc>=2`) never exceeds Opus.
- **INV-4** — down arms (`sc<=-2`) never go below Haiku.
- **INV-5** — home=Opus: mild-down (`sc<=-2`, not mechanical)→Sonnet;
  strong-down (`sc<=-2`, mechanical)→Haiku.
- **INV-8** — `score()` on an absent transcript returns the home tier; the
  default `homeTier` argument is Sonnet (back-compat).
