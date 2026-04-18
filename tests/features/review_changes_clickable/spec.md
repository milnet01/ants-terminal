# Feature: Review Changes button tri-state policy

## Contract

The status-bar **Review Changes** button (`m_claudeReviewBtn`) has exactly
three valid display states, selected per-tab from the active tab's git
working directory:

| Git state                                                | `isVisible()` | `isEnabled()` |
|----------------------------------------------------------|---------------|---------------|
| Not a git repo (or git unavailable)                      | `false`       | (don't care)  |
| Git repo, clean worktree AND in-sync with upstream       | `true`        | `false`       |
| Git repo with dirty worktree OR unpushed commits         | `true`        | `true`        |

The state `isVisible() == true && isEnabled() == false` IS a legitimate
state now (revised from the 0.6.29 contract): it signals "this tab
tracks a git repo, but there's nothing to review right now." The user
can see at a glance that the repo is clean without the button
disappearing entirely.

## Rationale

User feedback 2026-04-18 (the third round of Review Changes complaints):

> The Review Changes button should be visible if the tab has a git
> branch. If there is no git branch for that tab, it should not be
> visible. The button should be disabled if there is no difference
> between local and the git project. The button should be enabled when
> there is a difference between local and the git project, have the
> mouseover highlight and be clickable.

This overrides the 0.6.29 policy (hide-on-clean). Two load-bearing
changes made this contract workable where the old one wasn't:

1. **The global `QPushButton:hover` stylesheet rule is now gated with
   `:enabled`** (`src/mainwindow.cpp:~1850`). A disabled button no
   longer lights up on hover, so it reads as passive without needing
   custom per-widget dashed-border styling. The old contract's
   "disabled button lies via hover" problem is eliminated at the
   stylesheet level.

2. **The probe is now `git status --porcelain=v1 -b`** (not
   `git diff --quiet HEAD`). That single command reports BOTH dirty
   worktree AND branch-ahead-of-upstream, so "enabled" means
   "reviewable local delta" — either unstaged/staged changes, or
   unpushed commits. The old probe missed unpushed commits entirely.

The companion dialog (`showDiffViewer`) now renders three sections —
Status, Unpushed commits, Diff — instead of just the diff, so that
"reviewable" across all enabled paths shows the user something
actionable.

## Scope

### In scope
- Source-level check that `refreshReviewButton`'s exit-code-0 branch
  uses `setEnabled(...)` (possibly false) + `show()` rather than
  `hide()` — the tri-state policy.
- Source-level check that the global `QPushButton:hover` rule IS
  gated with `:enabled` — this is the invariant that makes
  visible-but-disabled tolerable UX. If a future refactor removes the
  gate, this spec's tri-state policy degrades to the 0.6.29
  click-swallowing bug.
- Source-level check that `refreshReviewButton` uses
  `git status --porcelain=v1 -b` (or an equivalent that covers
  ahead-of-upstream) — future ways the probe could regress to a
  worktree-only check.

### Out of scope
- The dialog-show contract on click (covered by
  `tests/features/review_changes_click/`).
- Runtime widget state (depends on a real git repo + a real
  MainWindow and is covered by the separate click test).

## Regression history

- **0.6.22** — button shown on every Claude `fileChanged`, stayed
  visible on clean repos. Probe: `git diff --quiet HEAD`.
- **0.6.29** — hide-on-clean policy introduced because the visible-
  but-disabled state was misleading via unconditional hover
  highlight. Spec locked to `isVisible()==true → isEnabled()==true`.
- **0.6.32** — probe still missed unpushed commits; user feedback
  flagged this as "button disabled when I have unpushed work."
- **0.6.33** (this revision) — probe switched to `git status
  --porcelain=v1 -b`, hover rule gated with `:enabled`, tri-state
  policy restored. Button hides only when tab's cwd isn't a git repo.
