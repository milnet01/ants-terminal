# Feature: Review Changes button activates for untracked-only changes

## Problem

`MainWindow::refreshReviewButton()` decides the bottom-bar "Review
Changes" button's enabled state from `git status --porcelain=v1 -b`.
A carve-out added 2026-05-08 explicitly skipped untracked (`?? `)
entries: at the time the diff viewer rendered nothing for new files,
so a button that opened an empty viewer was worse than no button.

ANTS-1886 (shipped 2026-05-29) changed that — the Review Changes
dialog now renders new files as synthetic addition diffs. So a repo
whose only change is a brand-new file Claude just wrote IS reviewable,
but the button stayed dark because the predicate still discarded
untracked entries. Reported by user 2026-05-26 (ANTS-1874).

## Fix

The porcelain → {dirty, ahead} decision is extracted from the inline
lambda in `refreshReviewButton` into a pure header function,
`ants::parseReviewPorcelain` in `src/reviewbuttonstate.h`, and the
untracked carve-out is removed: any non-header porcelain line (tracked
*or* untracked) sets `dirty`.

## Contract

### Invariant 1 — untracked-only output is dirty

`parseReviewPorcelain` on a porcelain payload whose only non-header
line is `?? newfile.cpp` returns `dirty == true`.

### Invariant 2 — clean repo is neither dirty nor ahead

A payload with only the `## branch...origin/branch` header returns
`dirty == false` and `ahead == false` (button visible-but-disabled).

### Invariant 3 — tracked changes remain dirty

A payload with ` M tracked.cpp` (modified, unstaged) returns
`dirty == true` — no regression from the pre-fix behaviour.

### Invariant 4 — ahead detection survives extraction

Both `## main...origin/main [ahead 2]` and
`## main...origin/main [ahead 1, behind 3]` return `ahead == true`;
a `[behind 3]`-only header returns `ahead == false`.

### Invariant 5 — call site uses the helper

`src/mainwindow.cpp`'s `refreshReviewButton` calls
`ants::parseReviewPorcelain` and no longer carries a `?? ` skip
branch.

## Regression history

- **Carve-out added:** 0.7.x (user report 2026-05-08).
- **Made obsolete by:** ANTS-1886 (2026-05-29).
- **Reported:** 2026-05-26 — "Review Changes button doesn't appear
  when Claude only writes new files." (ANTS-1874)
