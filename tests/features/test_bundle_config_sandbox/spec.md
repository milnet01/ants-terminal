# ANTS-4898 — a test process must not read the developer's real config

## Background

ANTS-3856 sandboxed `XDG_DATA_HOME` in both bundle mains: `RoadmapStore`
defaults its path under `GenericDataLocation`, so `RoadmapStore store;`
in a test opened the developer's live roadmap database and passed. A
fixture project rooted at `/tmp/test_core-ZnzBrv` was found registered in
it.

`XDG_CONFIG_HOME` was never given the same treatment. `Config::load()`
reads `~/.config/ants-terminal/config.json` on construction, and every
verb that consults a setting therefore consulted the developer's own.

**Measured 2026-09-06.** `claude.mcp_feedback_root` (ANTS-4471) was set
to the shared feedback corpus, which is what that key is for. Twelve
tests in `McpFeedbackQuery` and `McpFeedbackLog` went red: each asserts a
path derived under its own `QTemporaryDir`, and the live key redirected
the derivation. The failure was the harmless half. The writes went
through: two files were created in the real corpus and three real corpus
files were appended to with fixture text (`### Title` / `- **What:**
what`). Repaired by hand.

A test asserting behaviour that depends on a setting outside the repo is
not a test — its result changes with the machine it runs on.

## Invariants

### INV-1 — the config location is a sandbox

Inside a test process, the writable config location does not resolve
under the invoking user's real config directory. Both bundle mains
(`bundle_main_gui.cpp`, `bundle_main_core.cpp`) carry the guard; the
assertion here rides the GUI bundle, where the damage was measured.

### INV-2 — a freshly constructed Config carries no user settings

`Config().claudeMcpFeedbackRoot()` is empty in a test process however the
developer has configured the app. That is the specific property whose
absence redirected twelve tests into the real corpus, and an empty key is
what the derivation rules already document as "the parent of
`caller_cwd`" — the behaviour every path-derivation test assumes.

### INV-3 — a test that wants its own config still overrides it

`ants_test::XdgGuard::setEnv("XDG_CONFIG_HOME", ...)` continues to win,
so the per-test sandboxes written before this change are unaffected.
