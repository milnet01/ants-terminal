# Feature: Unified Claude state-dot palette (tabs + status bar)

User request 2026-04-27: "Let's change how we indicate Claude sessions
in various tabs. Let's have a round dot on each tab that has a Claude
Code session running (no icons or anything else other than the tab
label). The dot will change colour with the various states that Claude
Code is in. Each state has its own colour (grey for idle). Then extend
those colours to the status bar Claude Code status too."

## Contract

For every running Claude Code session, the visual indicator is a
**single uniform circle** on the tab chrome. **Hue is the only state
signal** — no badges, icons, or size variations, and no *per-state*
outline. Every dot carries one identical, state-independent ring (added
in ANTS-1847 for edge definition on any theme), so the ring conveys no
state and does not break the "colour is the signal" rule. The dot's fill
is the canonical base hue, contrast-adapted (lightness-only) to the tab
background so the hue stays legible on light themes. The bottom status
bar's "Claude: …" text label adopts the same contrast-adapted per-state
colour (no ring) so the state colour is consistent across the two
surfaces.

## State → colour palette

The contract is a **fixed hue identity**, not a fixed RGB triple. State
identity (the *hue*) stays constant across themes so muscle memory
("orange = needs me") works. The hex values below are the canonical
**base palette**, tuned for dark backgrounds; on a light theme each dot's
*lightness* is lowered just enough to clear the WCAG 3:1 non-text-contrast
floor against the tab background, while its hue and saturation are kept
(see ANTS-1847 and the contrast-adaptation invariant below).

This supersedes the original "fixed RGB, contrast from hue alone" rule.
A single RGB value mathematically cannot clear 3:1 against both a
near-black and a near-white background, so the dark-tuned base palette
washed out on the Light / Catppuccin-Latte themes — 7 of 8 dots fell
below 3:1 against `bgSecondary` (`#F5F5F5`), including the
operationally-critical AwaitingInput orange at 2.28:1 (ANTS-1847).

| State          | Source signal                                    | Hex      | Name      |
|----------------|--------------------------------------------------|----------|-----------|
| Idle           | Last assistant event has terminal `stop_reason`  | `#888888`| grey      |
| Thinking       | Active reasoning, no tool yet                    | `#5BA0E5`| blue      |
| ToolUse        | Generic tool (Read/Write/Edit/Grep/Glob/Task/…)  | `#E5C24A`| yellow    |
| Bash           | `tool_use` block with `name == "Bash"`           | `#6FCF50`| green     |
| Planning       | Most recent permission-mode event = `plan`       | `#5DCFCF`| cyan      |
| Auditing       | `/audit` skill in flight (latched user message)  | `#C76DC7`| magenta   |
| Compacting     | `/compact` in flight                             | `#A87FE0`| violet    |
| AwaitingInput  | `PermissionRequest` hook pending                 | `#F08A4B`| orange    |
| *(NotRunning)* | No Claude process under tab's shell              | *(hidden)*| —        |

Red is intentionally absent — Claude awaiting input is a normal
interaction state, not an error condition.

## Architectural invariants

1. **Single source of truth.** A new pure helper
   `Claude::stateColor(ClaudeTabIndicator::Glyph)` lives in a header
   reachable from both `coloredtabbar.cpp` and `mainwindow.cpp`. Both
   call it; no copy-pasted hex literals.

2. **Uniform dot geometry + uniform ring.** Every dot — Idle, Thinking,
   ToolUse, Bash, Planning, Auditing, Compacting, AwaitingInput — is a
   circle of radius 4 px. The previous AwaitingInput "outline + radius 5"
   *per-state* treatment stays removed (it singled out one state, which
   the user found over-decorated). What every dot now shares is a single
   **uniform thin ring** (1 px), identical for every state — a
   semi-transparent stroke that is dark on light themes and light on dark
   themes (derived from the tab background luminance) so the dot keeps a
   crisp edge on any background (user request 2026-05-25, ANTS-1847). The
   ring is state-independent: it carries no state meaning, so it does not
   reintroduce per-state geometry. Hue (the state signal) still comes from
   the fill, not the ring.

   A new pure helper `ClaudeTabIndicator::ringColor(const QColor
   &background)` (a sibling of `ClaudeTabIndicator::color`) is the single
   source of the ring colour; both the dark/light decision and the alpha
   live there, not inline in `paintEvent`. (Note: architectural invariant
   1 and several INVs below name the colour helper `Claude::stateColor`
   aspirationally; the shipped name is `ClaudeTabIndicator::color`.)

3. **NotRunning paints nothing.** Tabs without a Claude session leave
   the leading gutter clean.

4. **Status-bar colour parity.** `ClaudeStatusBarController::apply`
   (post-ANTS-1146; pre-1146 it was `MainWindow::applyClaudeStatusLabel`)
   resolves the colour by mapping the current label state through the
   *same* contrast-adaptation path as the dot
   (`ClaudeTabIndicator::contrastColor(glyph, bg)`), against the status
   bar's own background (`theme.bgSecondary` — the same surface the tab
   bar uses, so the two adjusted colours are identical). The status-bar
   text reads in the same colour as the active tab's dot on every theme.
   The ring is dot-only (text has no ring); parity is over the *fill*
   colour. Rationale for the shared 3:1 target on the text (rather than
   the 4.5:1 normal-text floor): exact parity with the dot is the
   load-bearing contract, the dot's 3:1 is the binding accessibility
   requirement, and the status label's literal words ("Claude: thinking")
   plus its `accessibleDescription` already carry the semantic load, so
   the colour is reinforcement rather than the sole channel.

5. **Auditing extends to the tab.** Today auditing is surfaced only on
   the status bar (active-tab `m_auditing` flag on the controller; pre-
   ANTS-1146 it was `m_claudeAuditing` on MainWindow). The tracker
   (`ClaudeTabTracker::ShellState`) gains an `auditing` bool plumbed
   from the existing transcript-tail parser. `ClaudeTabIndicator::Glyph`
   gains an `Auditing` member; the provider lambda installed by
   `ClaudeStatusBarController::attach` (pre-1146 the provider was
   inline in `MainWindow::setupClaudeIntegration`) returns it
   when `state.auditing` is true and no higher-precedence state
   (AwaitingInput / Planning) wins.

6. **Precedence.** AwaitingInput → Planning (only when underlying
   state ≠ NotRunning) → Auditing → state-derived (Compacting,
   Bash, ToolUse, Thinking, Idle). Same chain in tab provider and
   status-bar applier so they never diverge.

7. **Toggle gate retained.** `claude_tab_status_indicator = false`
   continues to suppress all dots. The status-bar colour is unaffected
   by the toggle (the toggle only governs the per-tab visual surface,
   per the existing 0.7.32 spec).

8. **Contrast adaptation (ANTS-1847).** The dot fill and the status-bar
   text colour both come from `ClaudeTabIndicator::contrastColor(glyph,
   background)`, which returns the base palette entry unchanged when it
   already clears WCAG 3:1 against `background` (every dark theme), and
   otherwise lowers only the HSL *lightness* — preserving hue and
   saturation — until the 3:1 floor is met. Contrast is monotonic in
   lightness against a light background and black always clears a light
   background, so a solution always exists. On the Light and
   Catppuccin-Latte themes all eight dots clear 3:1 against `bgSecondary`
   after adaptation; on dark themes the shipped appearance is unchanged
   (the base hex literals still render verbatim).

## Out of scope

- Tab colour-group gradient strip — orthogonal feature, untouched. The
  dot's contrast is computed against the base tab background
  (`bgSecondary`), not the per-tab colour-group gradient washed over it.
- Tab close-button decoration — untouched.
- *Per-theme* hand-tuned palettes — still rejected. Adaptation is
  algorithmic and hue-preserving (lightness-only), not a second curated
  RGB set per theme; the base hue is the durable contract. (This narrows
  the original blanket "no theme-adaptive variants" rejection: contrast
  adaptation against the live background is now in scope; bespoke
  per-theme colour curation is not.)
- Any animation / pulse — explicitly rejected per "no icons or anything
  else other than the tab label."
- Status-bar context-percent progress bar styling — the colour change
  applies only to the text label, not the progress chunk colour.
- A ring on the status-bar text — the uniform ring is a dot-only
  affordance; text carries no ring.

## Test invariants (`test_claude_state_dot_palette.cpp`)

INV-1 … INV-8 are source-grep + structural. INV-9 / INV-10 (ANTS-1847)
are behavioural — they link `coloredtabbar` (already in the `test_claude`
GUI bundle) and call `ClaudeTabIndicator::contrastColor` /
`ringColor` directly, because contrast is a numeric guarantee that
source-grep cannot verify.

- INV-1 Helper exists at the documented header path with the documented
  signature (`QColor Claude::stateColor(ClaudeTabIndicator::Glyph)`).
- INV-2 Helper covers all eight non-None glyph values: source-grep finds
  each `Glyph::Idle`, `Glyph::Thinking`, `Glyph::ToolUse`, `Glyph::Bash`,
  `Glyph::Planning`, `Glyph::Auditing`, `Glyph::Compacting`,
  `Glyph::AwaitingInput` literal in a switch/case under the helper.
- INV-3 Hex literals match the spec table (`"#888888"`, `"#5BA0E5"`,
  `"#E5C24A"`, `"#6FCF50"`, `"#5DCFCF"`, `"#C76DC7"`, `"#A87FE0"`,
  `"#F08A4B"`).
- INV-4 `coloredtabbar.cpp::paintEvent` derives the dot fill via
  `ClaudeTabIndicator::contrastColor(ind.glyph, m_bg)` (no inline hex
  literals remain in the dot-rendering path; the base palette literals
  live only in the `color()` helper).
- INV-5 Uniform radius + uniform ring — source-grep confirms `paintEvent`
  no longer contains `radius = 5` and `drawEllipse` uses a single
  `kDotRadius` of 4. The ring is uniform: there is no per-state
  outline/variant branch (the old `outline.alpha()` marker stays absent),
  and `paintEvent` sets the pen exactly once from
  `ClaudeTabIndicator::ringColor(m_bg)` (state-independent), not inside
  any `Glyph::`-cased branch.
- INV-6 `claudestatuswidgets.cpp::ClaudeStatusBarController::apply`
  (post-ANTS-1146; pre-1146 was `mainwindow.cpp::applyClaudeStatusLabel`)
  derives the label colour via `ClaudeTabIndicator::contrastColor(glyph,
  …)` rather than `th.ansi[…]` or the unadjusted `color(glyph)`.
  Theme-ansi mappings for the Claude label are removed.
- INV-7 `ClaudeTabTracker::ShellState` exposes `auditing` (header source-
  grep). `ClaudeTabIndicator::Glyph` exposes `Auditing` (header source-
  grep).
- INV-8 Status-bar applier handles the auditing branch by routing to
  `Claude::stateColor(Glyph::Auditing)` — source-grep confirms the call
  site sits in the `m_auditing` branch (controller-internal flag,
  post-ANTS-1146; pre-1146 was `m_claudeAuditing` on MainWindow). Both
  the applier (in `apply()`) and the provider lambda (in `attach()`)
  contain `Glyph::Auditing` references — split across the two source
  files post-extraction.
- INV-9 (behavioural, ANTS-1847) Contrast guarantee + hue preservation +
  dark-theme passthrough. For every non-None glyph and each light
  `bgSecondary` (`#F5F5F5` Light, `#E6E9EF` Catppuccin-Latte),
  `contrastColor(glyph, bg)` clears WCAG 3:1 against `bg`; the result's
  HSL hue equals the base `color(glyph)` hue (skipping achromatic grey);
  and against a dark background (`#1E1E1E`) `contrastColor` returns the
  base palette colour byte-for-byte unchanged.
- INV-10 (behavioural, ANTS-1847) Uniform ring. `ringColor(bg)` returns a
  semi-transparent (alpha < 255) colour that is dark (low luminance) on a
  light `bg` and light (high luminance) on a dark `bg`, and is identical
  for every glyph (it takes no glyph argument — uniformity is structural).

## Rationale

The 0.7.27 indicator landed with state-distinguishing hues but used
ad-hoc colours (some via `Theme::ansi[]`, some via inline RGB triples)
that drifted between the dot and the status-bar text. Users had to
re-learn "what does this colour mean" when their eyes moved between
tab chrome and status bar. Unifying around a single fixed palette —
with grey as the explicit "nothing happening" anchor — makes the state
language consistent and removes the AwaitingInput special-casing
(which the user found over-decorated).
