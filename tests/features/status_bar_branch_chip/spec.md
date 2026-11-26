# Git-branch chip styling matches visibility pill (ANTS-1109)

User screenshot 2026-04-30: the `main` branch chip and the framed
`Public` visibility pill sat side-by-side on the status bar but
read as inconsistent — different shape cues. Pair them visually:
same border-radius, padding, font; colour cue derived from the
branch name (green for the project's primary branches, amber for
feature branches as a "you're not on main" hint).

## Fix

Inline helper in `src/branchchip.h`:

```cpp
namespace branchchip {
inline bool isPrimaryBranch(const QString &branch);
inline const QColor &chipColour(const Theme &theme, const QString &branch);
}
```

Returns `true` for `main`, `master`, `trunk` (the three names that
GitHub / GitLab / git-init plus older convention pick as the
project's principal branch). All other names — feature branches,
release branches, hotfix branches, detached-HEAD short SHAs — fall
through to `false`.

`chipColour` turns that into the chip's colour: green outline +
foreground (`theme.ansi[2]`, the same role the visibility pill uses
for `Public`) when primary, amber otherwise (`theme.ansi[3]`,
matching the visibility pill's `Private` cue). `MainWindow` styles
`m_statusGitBranch` from it.

## Out of scope

- Recolouring detached-HEAD specifically — falls into the
  feature-branch (amber) bucket alongside any other non-primary
  ref. A future bullet can split it out if user feedback asks.
- Changing the chip's shape, padding, font size — these were
  already aligned with the visibility pill (border-radius 3px,
  padding 1px 8px, font-size 11px, font-weight 600).
- Margin alignment — the chip uses `2px 6px 2px 4px` (4px left
  margin to space from the status-bar edge); the pill uses
  `2px 6px 2px 0` (sits flush to the chip's 6px right margin).
  This asymmetry is intentional — the chip is the leftmost
  status-bar widget; the pill sits to its right.
- Recolouring on every branch change in real time — the existing
  `updateStatusBar` poll runs every 2s and re-applies the
  stylesheet, which is fast enough for the user-visible signal.

## Invariants

Source-grep + behavioural drive of the pure helper.

- **INV-1** `branchchip::isPrimaryBranch("main")` returns `true`.
  Behavioural drive.
- **INV-2** `branchchip::isPrimaryBranch("master")` returns
  `true`. Behavioural drive.
- **INV-3** `branchchip::isPrimaryBranch("trunk")` returns
  `true`. Behavioural drive.
- **INV-4** `branchchip::isPrimaryBranch("feature/x")` returns
  `false`. Behavioural drive.
- **INV-5** `branchchip::isPrimaryBranch("")` returns `false`
  (empty branch is the "not in a repo" sentinel, must be amber).
  Behavioural drive.
- **INV-6** `branchchip::isPrimaryBranch("MAIN")` returns
  `false` — case-sensitive match. Git branch names are
  case-sensitive; `MAIN` is a different branch from `main`.
  Behavioural drive.
- **INV-7** `branchchip::chipColour(theme, branch)` returns
  `theme.ansi[2]` (green) for `main` and `theme.ansi[3]` (amber)
  for `feature/foo`. Behavioural drive.
- **INV-8** Every site in `mainwindow.cpp` that styles the chip
  takes its colour from `branchchip::chipColour(`, so the theme
  switch and the poll cannot pick different colours. There are at
  least two such sites. Asserted by source-grep on the region
  around each `m_statusGitBranch->setStyleSheet(`.
- **INV-9** (ANTS-6187) `GitRepo::branchOf` reads a git worktree's
  branch: its `.git` is a file whose `gitdir:` line points at the
  folder holding `HEAD`. A main checkout, a detached worktree (the
  short commit) and a folder outside any repository (empty) are
  checked too. Behavioural drive on a hand-made layout.
- **INV-10** (ANTS-6187) The chip and the repo-visibility badge use
  the one finder: the main window sources call `GitRepo::branchOf(`
  and `GitRepo::rootOf(`, and read no `.git/HEAD` path themselves.
  Source-grep.

## CMake wiring

The test target is wired into `CMakeLists.txt` via `add_executable`
+ `add_test` with `LABELS "features;fast"`,
`target_compile_definitions` for `MAINWINDOW_CPP`, and
`target_link_libraries Qt6::Core` (the helper uses `QString` /
`QLatin1String` only — no widgets needed; the test runs without
a `QApplication`).

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that lands ANTS-1109.
git checkout <impl-sha>~1 -- src/mainwindow.cpp
# (src/branchchip.h didn't exist pre-fix — INVs 1-7 also fail.)
cmake --build build --target test_status_bar_branch_chip
ctest --test-dir build -R status_bar_branch_chip
# Expect every INV to fail: helper missing, consultation site
# missing, palette indices wrong.
```
