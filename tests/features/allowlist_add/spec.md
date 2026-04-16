# Feature: Add-to-Allowlist button + rule normalisation

## Contract

### A. Rule normalisation (`normalizeRule`)

Given a raw rule string captured from the terminal's permission-prompt
scraping, produce a canonical form that matches Claude Code's
`.claude/settings.local.json` allowlist schema. Canonical form:

- `Bash(<cmd-pattern>)` where `<cmd-pattern>` uses spaces (not colons)
  to separate positional args/globs — `Bash(git:*)` → `Bash(git *)`.
- Bare single-word commands expand to `Bash(<cmd> *)` (except `make` and
  `env` which often run with no args).
- Bare `sudo <cmd>` invocations convert to the SUDO_ASKPASS form used by
  this project's workflow: `Bash(SUDO_ASKPASS=… sudo -A <cmd>)`.
- `Read(/abs/path)` / `Write` / `Edit` get the `//` absolute-path prefix
  Claude Code expects: `Read(//abs/path)`.
- `WebFetch(example.com)` expands to `WebFetch(domain:example.com)`.

### B. Rule generalisation (`generalizeRule`)

Given a normalized concrete-command rule, produce a useful wildcard
pattern. Return an empty string when no sensible generalisation applies
(rather than over-generalising).

- `Bash(cd /path && cmd arg)` → `Bash(cd * && cmd *)`
- `Bash(cmd1 && cmd2 arg)` → `Bash(cmd1 * && cmd2 *)` — arbitrary
  `&&` chains, NOT just `cd`-prefixed.
- `Bash(cmd1 ; cmd2 arg)` → `Bash(cmd1 * ; cmd2 *)`
- `Bash(cmd1 | cmd2 arg)` → `Bash(cmd1 * | cmd2 *)`
- `Bash(cmd1 || cmd2 arg)` → `Bash(cmd1 * || cmd2 *)`
- Deep chains: `Bash(a && b && c x)` → `Bash(a && b && c *)`
  (preserve structure; only wildcard the final command's args).
- `Bash(SUDO_ASKPASS=… sudo -A <cmd> …)` → `Bash(SUDO_ASKPASS=… sudo
  -A <cmd> *)`.
- `Bash(<cmd> …)` → `Bash(<cmd> *)` for a plain single-command rule.
- **Safety denylist:** NEVER auto-generalise `rm`, `sudo` (bare, not
  the SUDO_ASKPASS form), or any command starting with `SUDO`
  (keep specific so users review destructive patterns explicitly).
  Return empty for these.
- Already-wildcard patterns (`Bash(cmd *)` or `Bash(cmd **)`) return
  empty — nothing useful to do.

### C. Rule subsumption (`ruleSubsumes`)

Before the dialog auto-adds a rule (or writes the user's manual addition),
it checks whether an existing rule already covers the new one. The check
MUST match Claude Code's allowlist-matcher semantics — any mismatch
either blocks a rule the user needs (false positive) or creates a
redundant rule (false negative, benign).

- Equal rules subsume each other.
- A bare tool name (`Read`) subsumes any qualified form (`Read(path)`).
- A simple `Bash(cmd *)` rule subsumes a simple narrow rule whose
  command starts with `cmd ` or is exactly `cmd`.
- A simple `Bash(cmd *)` rule subsumes a **compound** narrow rule
  (segments separated by `&&`, `||`, `;`, `|`) **only if every segment**
  independently starts with `cmd ` (or equals `cmd`). Matches Claude
  Code's per-segment evaluation. Flat-prefix-based subsumption is
  prohibited — `Bash(make *)` does NOT subsume
  `Bash(make x | tail y && ctest z | tail w)` because the `tail` and
  `ctest` segments need their own rules.
- Path-glob rules (`Read(//etc/**)`) subsume narrower paths under the
  same tool (`Read(//etc/passwd)`).
- **Compound broad rules** (containing splitters in the pattern
  itself) deliberately return false. Segment-wise compound-vs-compound
  matching isn't modeled; prefer a redundant rule over a false claim
  of coverage.

### D. Button lifecycle

The "Add to allowlist" button appears on the status bar when either
(a) the terminal scroll-scanner emits `claudePermissionDetected` or
(b) the ClaudeIntegration HTTP hook emits `permissionRequested`. The
button:

1. **Is unique.** Any pre-existing button is removed before a new one
   is shown. Multiple back-to-back prompts don't accumulate buttons.
2. **Is prompt-scoped.** It must disappear when the prompt is no longer
   relevant. Retraction triggers:
   - Scroll-scan path: `TerminalWidget::claudePermissionCleared` —
     fires once the footer pattern ("Tab to accept" / "Do you want
     to proceed") leaves the last 12 on-screen lines.
   - Hook path: `claudePermissionCleared` (when the scroll-scanner
     also saw the prompt) PLUS `ClaudeIntegration::toolFinished`
     (permission granted and tool completed) PLUS
     `ClaudeIntegration::sessionStopped` (session ended). The extra
     triggers are load-bearing: a `PermissionRequest` hook can fire
     for prompts the scroll-scanner never observed (unmatched prompt
     format, prompt scrolled past the lookback window, headless
     Claude Code session). Relying on `claudePermissionCleared`
     alone orphans the button — user-reported 0.6.30 symptom:
     "Claude Code permission: Bash(...) —" visible on status bar
     with no live prompt in any terminal.
3. **Is self-cleaning on click.** If the user clicks the button and
   the allowlist dialog opens, the button removes itself
   immediately (no wait for the retraction signals).
4. **Is tab-scoped.** A prompt in tab A must not produce a button
   visible to tab B — the `claudePermissionDetected` handler filters
   by focused/current terminal before creating the button. Tab-
   switch cleanup in `onTabChanged` removes any lingering button
   via `findChildren<QWidget*>("claudeAllowBtn")` — matches both
   the scroll-scan path's `QPushButton` and the hook path's
   `QWidget` container.

### E. Dialog becomes visible on click

Clicking "Add to allowlist" calls `openClaudeAllowlistDialog(rule)`.
The dialog MUST become the active window, not just appear somewhere
in the stacking order. On a frameless `QMainWindow` parent (the
project uses `Qt::FramelessWindowHint` at mainwindow.cpp:74), KWin's
stacking heuristics can place a child QDialog BEHIND the parent
unless the caller invokes `raise()` + `activateWindow()` after
`show()`. The sister Review Changes button has the same constraint
(see `tests/features/review_changes_click/spec.md` invariant A) and
went through the same regression (0.6.29). The same fix applies here:
`openClaudeAllowlistDialog` must end with `show()` + `raise()` +
`activateWindow()`. Without `activateWindow()`, the user-reported
symptom is identical to the Review Changes bug: "Add to allowlist
click does nothing — no dialog opens, no visible effect" — the
dialog IS up, just obscured by the main window.

## Rationale

- The allowlist is security-sensitive user policy. A rule that looks
  generalised correctly but actually has a typo ("`&&`" mis-split,
  e.g.) causes Claude Code to re-prompt on every retry, which leads
  users to either approve broader-than-intended rules or give up and
  disable prompts entirely. Both are worse than the friction of
  correct prompting.
- Missing generalisation for common shell operators (`&&`, `;`, `|`,
  `||`) meant compound commands were offered as narrow, concrete
  rules that only ever match the exact original command string —
  useless for future invocations. The user-reported symptom: "Add to
  allowlist" click produces a rule that doesn't cover the next
  similar command, so the prompt returns next turn.
- The button's prompt-scope is critical. A button that lingers past
  the prompt invites the user to click the wrong "approve" button
  for a command that was actually declined.

## Scope

### In scope
- `normalizeRule` contract for every documented input form.
- `generalizeRule` contract for single-command, `&&`, `;`, `|`, `||`
  patterns, and the safety denylist (`rm`, bare `sudo`, `SUDO_*`).
- `ruleSubsumes` contract for simple-vs-simple, simple-vs-compound
  (including the 0.6.22-era reproduction where `Bash(make *)` was
  wrongly reported as subsuming a pipe+&& chain), and compound-broad
  safe fallback.
- Idempotency: `normalizeRule(normalizeRule(x)) == normalizeRule(x)`
  and `generalizeRule(generalizeRule(x)) == "" (or input)`.

### In scope (widget lifecycle)
- §D hook-path retraction wiring: the production code at
  `mainwindow.cpp` inside the `permissionRequested` handler MUST
  connect `toolFinished` and `sessionStopped` to the button's
  deleteLater path. Source-grep assertion — catches reverts that
  reintroduce the 0.6.30 orphaned-button bug.
- §E production binding: `openClaudeAllowlistDialog` MUST call
  `show()` + `raise()` + `activateWindow()` on `m_claudeDialog`.
  Same pattern as `review_changes_click`'s Invariant A production
  binding.
- §E API-level: a `QDialog` shown on a frameless `QMainWindow`
  parent becomes the active window ONLY after show+raise+activate.

### Out of scope (covered by integration or future tests)
- Parsing the raw permission prompt text from the terminal scrollback
  (`terminalwidget.cpp::claudePermissionDetected` signal emission).
- End-to-end click simulation with real MainWindow instantiation —
  pulls in half the project's sources (TerminalWidget, PTY,
  VtParser, ClaudeIntegration, etc.). The API-level + source-grep
  pair captures both regression vectors with minimal friction.

## Regression history

- **0.6.22:** user feedback on command-splitter coverage. `&&`/`;`/`|`
  compound commands produced rules that never matched future
  invocations, so the button's usefulness depended on which
  splitter was in play. This spec + test commit that across all four
  splitters, with the safety denylist carved out.
- **0.6.23:** user-reported: clicking "Add to allowlist" for a
  compound command opened the dialog with a correct compound rule
  (`Bash(make * | tail * && ctest * | tail *)`) — but the dialog's
  subsumption check claimed `Bash(make *)` already covered it, refusing
  to add. Root cause: `ruleSubsumes` did a flat `startsWith(broad
  prefix)` check; any compound narrow whose first segment matched
  looked subsumed even when later segments (tail/ctest) weren't. Fix:
  split the narrow rule on shell splitters and require every segment
  to match the broad prefix before declaring subsumption. Matches the
  per-segment semantics `generalizeRule` has used since 0.6.22.
- **0.6.31 (bug #1):** user-reported: clicking "Add to allowlist"
  did nothing — no dialog, no visible effect. Root cause:
  `openClaudeAllowlistDialog` called `show()` + `raise()` but not
  `activateWindow()`. Identical to the 0.6.29 Review Changes bug
  (see `tests/features/review_changes_click/spec.md`) — the
  frameless QMainWindow + focusChanged refocus-redirect combo
  means show+raise alone leaves the dialog obscured by the main
  window. Fix: add `m_claudeDialog->activateWindow()` after raise.
- **0.6.31 (bug #2):** user-reported: "Claude Code permission:
  Bash(cd * && cmake * | tail *) —" visible on status bar long
  after the underlying prompt had been resolved. Root cause: the
  hook-path button (created by `ClaudeIntegration::permissionRequested`)
  listened ONLY to `TerminalWidget::claudePermissionCleared` for
  retraction. That signal is gated on
  `m_lastDetectedRule` being non-empty in
  `TerminalWidget::checkForClaudePermissionPrompt` — i.e. it only
  fires if the terminal scroll-scanner had independently observed
  the prompt. When the hook fires for a prompt the scanner never
  saw (unmatched footer format, off-screen, or headless session),
  the button lived forever. Fix: add `toolFinished` +
  `sessionStopped` as retraction triggers on the hook path. Proxy
  signals — Claude Code has no canonical `PermissionResolved` hook
  — so we err toward closing too early rather than too late
  (stranding the button is worse than the user briefly losing
  access to a button they were about to click).
