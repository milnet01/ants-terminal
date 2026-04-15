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

### C. Button lifecycle

The "Add to allowlist" button appears on the status bar when the
terminal emits `claudePermissionDetected`. The button:

1. **Is unique.** Any pre-existing button is removed before a new one
   is shown. Multiple back-to-back prompts don't accumulate buttons.
2. **Is prompt-scoped.** It must disappear when the prompt is no longer
   relevant. Current impl: after a 1000 ms grace (to ride out the
   brief quiet window right after the prompt), the first
   `outputReceived` signal from the terminal removes it. This covers
   both "user clicked yes" and "user clicked no / new output arrived"
   cases from a single timer+signal pattern.
3. **Is self-cleaning on click.** If the user clicks the button and
   the allowlist dialog opens, the button removes itself
   immediately (no wait for `outputReceived`).
4. **Is tab-scoped.** A prompt in tab A must not produce a button
   visible to tab B — the `claudePermissionDetected` handler filters
   by focused/current terminal before creating the button.

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
- Idempotency: `normalizeRule(normalizeRule(x)) == normalizeRule(x)`
  and `generalizeRule(generalizeRule(x)) == "" (or input)`.

### Out of scope (covered by integration or future tests)
- The Qt widget lifecycle (needs QApplication + a MainWindow
  harness). Contract documented in §C above; a future GUI-enabled
  test can drive it.
- Parsing the raw permission prompt text from the terminal scrollback
  (`terminalwidget.cpp::claudePermissionDetected` signal emission).

## Regression history

- **0.6.22:** user feedback on command-splitter coverage. `&&`/`;`/`|`
  compound commands produced rules that never matched future
  invocations, so the button's usefulness depended on which
  splitter was in play. This spec + test commit that across all four
  splitters, with the safety denylist carved out.
