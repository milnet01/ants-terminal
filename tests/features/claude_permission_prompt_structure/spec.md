# Feature: Claude permission-prompt structural gate (ANTS-1993)

## Problem

The terminal scroll-scanner (`TerminalWidget::checkForClaudePermissionPrompt`)
detected a Claude Code permission prompt by anchoring on any one of four
footer phrases — `"Tab to accept"`, `"Do you want to proceed"`,
`"allow access to"`, `"always allow"` — then scanned back for a short line
carrying a tool name (`Read`/`Write`/`Bash`/…) to build an allowlist rule.

Two of those anchors are short English fragments. Any program whose stdout
contained `"always allow"` (e.g. a log line "always allow retries") or
`"allow access to <path>"` manufactured a footer; a nearby short `Read`/
`Write` line then produced `claudePermissionDetected(rule)`, which paints a
status-bar **"Add to allowlist"** button pre-filled with an attacker-chosen
rule. A user who trusts that button could add a dangerous rule
(`Read(//etc/shadow)`, `Bash(rm …)`) to `.claude/settings.local.json`.

Terminal output is fully attacker-controlled, so this is **defense-in-depth,
not a trust boundary** — a hostile program can still reproduce the entire
prompt pixel-for-pixel. The goal is that an *incidental* string in normal
output can no longer fire the detector.

## Contract — `ClaudePromptDetect::isPermissionPromptStructure(recentLines)`

A genuine CC permission prompt is a multi-line bordered widget: an anchor
phrase **and** a selection UI. The gate returns true iff both are present in
the recent on-screen lines:

- **anchor** — `"Tab to accept"` | `"Do you want to proceed"` |
  `"allow access to"` | `"always allow"`.
- **strong** — `"Tab to accept"` | `"Ctrl+e to explain"` (CC-specific
  footer fragments; corroborate on their own).
- **selection** — a numbered `N. Yes` / `N. No` option line, a `❯` cursor
  line, an `"Esc to cancel"` navigation hint, or a `y · yes` / `(y/n)` line.

Detection := `hasAnchor && (hasStrong || hasSelection)`.

### Invariants

- **INV-1** — Real older prompt (`Tab to accept` footer) → true.
- **INV-2** — Real newer prompt (`Do you want to proceed?` + numbered
  `❯ 1. Yes` / `2. No` options) → true.
- **INV-3** — Real file-access prompt (`allow access to <path>` +
  `Esc to cancel` hint) → true.
- **INV-4** — A lone `"always allow"` line → false (weak anchor, no
  corroboration). This is the headline injection vector.
- **INV-5** — A lone `"allow access to /etc/shadow"` line → false.
- **INV-6** — Unrelated benign output (no anchor) → false.
- **INV-7** — Anchor + selection split across different lines of the same
  window still corroborates → true (the markers need not share a line).

## Out of scope

- The backwards header scan / rule construction (`Step 2`–`Step 4` of
  `checkForClaudePermissionPrompt`) — gated *behind* this predicate, so a
  false here short-circuits the whole scan. Covered by the existing
  `allowlist_add` spec (rule normalisation) and integration use.
- The hook-path (`ClaudeIntegration::permissionRequested`) authoritative
  detection, which is unaffected.
