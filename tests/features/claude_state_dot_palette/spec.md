# Feature: Unified Claude state-dot palette (tabs + status bar)

User request 2026-04-27: "Let's change how we indicate Claude sessions
in various tabs. Let's have a round dot on each tab that has a Claude
Code session running (no icons or anything else other than the tab
label). The dot will change colour with the various states that Claude
Code is in. Each state has its own colour (grey for idle). Then extend
those colours to the status bar Claude Code status too."

## Contract

For every running Claude Code session, the visual indicator is a
**single uniform circle** on the tab chrome. Colour is the only signal —
no outlines, badges, icons, or size variations. The bottom status bar's
"Claude: …" text label adopts the same per-state palette so colour is
consistent across the two surfaces.

## State → colour palette

The palette is **fixed**, not theme-derived. State identity stays
constant across themes so muscle memory ("orange = needs me") works.
Hex values picked for sufficient contrast against both the dark Solarized
defaults and the light Gruvbox variant.

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

2. **Uniform dot geometry.** Every dot — Idle, Thinking, ToolUse, Bash,
   Planning, Auditing, Compacting, AwaitingInput — is a circle of
   radius 4 px with no outline. The previous AwaitingInput
   "outline + radius 5" treatment is removed (per "no icons or
   anything else other than the tab label"). Contrast for AwaitingInput
   comes from hue alone, not geometry.

3. **NotRunning paints nothing.** Tabs without a Claude session leave
   the leading gutter clean.

4. **Status-bar colour parity.** `MainWindow::applyClaudeStatusLabel`
   resolves the colour by mapping the current label state to the same
   `Claude::stateColor` helper. The status-bar text reads in the same
   colour as the active tab's dot.

5. **Auditing extends to the tab.** Today auditing is surfaced only on
   the status bar (active-tab `m_claudeAuditing` flag). The tracker
   (`ClaudeTabTracker::ShellState`) gains an `auditing` bool plumbed
   from the existing transcript-tail parser. `ClaudeTabIndicator::Glyph`
   gains an `Auditing` member; the provider in `MainWindow` returns it
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

## Out of scope

- Tab colour-group gradient strip — orthogonal feature, untouched.
- Tab close-button decoration — untouched.
- Theme-adaptive variants of the dot palette — explicitly rejected;
  fixed palette is the contract.
- Any animation / pulse — explicitly rejected per "no icons or anything
  else other than the tab label."
- Status-bar context-percent progress bar styling — the colour change
  applies only to the text label, not the progress chunk colour.

## Test invariants (`test_claude_state_dot_palette.cpp`)

The harness is source-grep + structural; no Qt link. Keeps with the
existing `claude_tab_status_indicator` test pattern.

- INV-1 Helper exists at the documented header path with the documented
  signature (`QColor Claude::stateColor(ClaudeTabIndicator::Glyph)`).
- INV-2 Helper covers all eight non-None glyph values: source-grep finds
  each `Glyph::Idle`, `Glyph::Thinking`, `Glyph::ToolUse`, `Glyph::Bash`,
  `Glyph::Planning`, `Glyph::Auditing`, `Glyph::Compacting`,
  `Glyph::AwaitingInput` literal in a switch/case under the helper.
- INV-3 Hex literals match the spec table (`"#888888"`, `"#5BA0E5"`,
  `"#E5C24A"`, `"#6FCF50"`, `"#5DCFCF"`, `"#C76DC7"`, `"#A87FE0"`,
  `"#F08A4B"`).
- INV-4 `coloredtabbar.cpp::paintEvent` calls `Claude::stateColor` (no
  inline hex literals remain in the dot-rendering switch).
- INV-5 Uniform radius — source-grep confirms `paintEvent` no longer
  contains `radius = 5` and the `drawEllipse` call uses a single
  `radius` value of 4. AwaitingInput specifically does NOT set an
  outline pen (no `painter.setPen(QPen(outline, …))` branch followed
  by an outline assignment).
- INV-6 `mainwindow.cpp::applyClaudeStatusLabel` calls
  `Claude::stateColor` to derive the label colour rather than
  `th.ansi[…]`. Theme-ansi mappings for the Claude label are removed.
- INV-7 `ClaudeTabTracker::ShellState` exposes `auditing` (header source-
  grep). `ClaudeTabIndicator::Glyph` exposes `Auditing` (header source-
  grep).
- INV-8 Status-bar applier handles the auditing branch by routing to
  `Claude::stateColor(Glyph::Auditing)` — source-grep confirms the call
  site sits in the `m_claudeAuditing` branch.

## Rationale

The 0.7.27 indicator landed with state-distinguishing hues but used
ad-hoc colours (some via `Theme::ansi[]`, some via inline RGB triples)
that drifted between the dot and the status-bar text. Users had to
re-learn "what does this colour mean" when their eyes moved between
tab chrome and status bar. Unifying around a single fixed palette —
with grey as the explicit "nothing happening" anchor — makes the state
language consistent and removes the AwaitingInput special-casing
(which the user found over-decorated).
