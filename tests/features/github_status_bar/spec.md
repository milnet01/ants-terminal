# Feature: GitHub-aware status bar

User feedback 2026-04-27 (two requests in one session):

> "Can we also have the status bar inform whether the repo is a public
> or a private repo?"

> "Can we add an auto-update feature?"

Bundled into 0.7.45 because both are GitHub-context badges in the
status bar — same widget shape (`QLabel`), same lifecycle (per-tab
where applicable, hide-on-absence), same source-of-truth (the
GitHub API or `gh` CLI).

## Surface

Two `QLabel` widgets in the status bar with different placements:

- **Repo visibility** sits on the **left** (added via `addWidget`)
  next to the git-branch chip — the user-feedback placement
  (2026-04-27, shipped 0.7.49). Reads as a "branch · visibility"
  pair on the left side of the status bar; the right side stays
  reserved for Claude Code chrome.
- **Update available** sits on the right (added via
  `addPermanentWidget`) alongside the Claude Code chrome
  (Review Changes → Background Tasks → Roadmap → **Update
  available** → Error label).

### Repo visibility — `m_repoVisibilityLabel` (objectName `repoVisibilityLabel`)

A chip-styled badge that reads "Public" or "Private" with the
same `bgSecondary` fill + `border-1px` frame as the git-branch
chip and a theme-derived foreground colour (`th.ansi[2]` green
for public, `th.ansi[3]` amber for private). Tooltip:
`<owner>/<repo> on GitHub`. Hidden by default. Placement: left
side of the status bar via `addWidget`, immediately after
`m_statusGitBranch` and before `m_statusGitSep`, so the two
chips read as a pair.

The badge's content reflects **the active tab's project** — the
same per-tab lifecycle as the Roadmap button. Tab switch fires
`refreshStatusBarForActiveTab` → `refreshRepoVisibility`, which:

1. Probes `gh` availability **once per session** (`QStandardPaths::
   findExecutable("gh")`); the result is cached in `m_ghAvailable`.
   If absent, the label is hidden and never queried.
2. Reads the active tab's `shellCwd()`.
3. Walks the cwd up via `findGitRepoRoot()` looking for a `.git`
   entry (file or directory). If absent, hides.
4. Parses `<repoRoot>/.git/config` for the
   `[remote "origin"] url = ...` line via `parseGithubOriginSlug()`.
   Handles both `https://github.com/owner/repo[.git]` and
   `git@github.com:owner/repo[.git]` forms. Non-GitHub origins return
   empty → hide.
5. Hits the cache `m_repoVisibilityCache` (keyed by repo root).
   - **Hit, fresh** (≤ 10 minute TTL) → render synchronously, no
     shell-out.
   - **Hit, stale** OR **miss** → hide the label, kick a `QProcess`
     running `gh repo view <owner/repo> --json visibility -q .visibility`,
     populate the cache + render in the `finished` signal.
6. Negative results (gh authentication failure, network error, etc.)
   are cached as empty strings with the same `fetchedAt` so the
   per-tab refresh doesn't hammer `gh` for unauthenticated repos.

### Update available — `m_updateAvailableLabel` (objectName `updateAvailableLabel`)

A clickable label showing `↗ Update v<X.Y.Z> available` (cyan
`#5DCFCF`, matches the Thinking state-dot colour). Click is
intercepted by `MainWindow::handleUpdateClicked`. Since ANTS-5560 it
routes on `SelfUpdate::installKind()`: an AppImage opens
`UpdateDialog` (download, verify, swap, then Restart now / Restart
later); any other install opens the release page
(`QDesktopServices::openUrl`). The restart warnings live in
`UpdateDialog`; `docs/specs/ANTS-5560-appimage-self-update.md` owns
that flow.

Tooltip: `Click to update to v<X.Y.Z>. Currently running v<current>.`

Lifecycle (revised in 0.7.47 — user feedback "An hourly check I
think is a bit much. Let's do the check when the terminal is
opened and when the user clicked on Help > Check for Updates."):

1. **Startup check** — a single `QTimer::singleShot(5000, ...)`
   fires `checkForUpdates(/*userInitiated=*/false)` 5 s after
   construction. The delay keeps the launch path fast and avoids
   racing the first paint. **Silent on negative results** (no
   "Up to date" message at startup) — startup probes that surface
   negative results add launch noise the user didn't ask for.
2. **Manual check** — `Help → Check for Updates` (objectName
   `helpCheckForUpdatesAction`) calls
   `checkForUpdates(/*userInitiated=*/true)`. The lambda also
   shows a 2-second `Checking for updates…` status-bar message
   for immediate visual feedback.
3. **The hourly timer is gone** as of 0.7.47. Nothing in the
   header should reference `m_updateCheckTimer`; INV-9 enforces
   the absence.
4. `checkForUpdates(bool userInitiated)` issues a
   `QNetworkAccessManager::get` against
   `https://api.github.com/repos/milnet01/ants-terminal/releases/latest`
   with `Accept: application/vnd.github+json` and a non-empty
   `User-Agent` (GitHub rejects empty UAs).
5. The `finished` slot parses the JSON, strips the leading `v`
   from `tag_name`, and runs `compareSemver()` against
   `ANTS_VERSION`.
   - **Strictly newer** → badge shown (regardless of
     `userInitiated`).
   - **Equal or older** → badge hidden; if `userInitiated`,
     surface `"Up to date — running v<X.Y.Z> (latest)"` for 4 s.
   - **Network error** → badge unchanged; if `userInitiated`,
     surface `"Update check failed: <err>"` for 5 s. Startup
     probe stays silent.
6. No retry storm — a failed check stays failed until the next
   user-triggered check.

## Architectural invariants enforced by tests

`tests/features/github_status_bar/test_github_status_bar.cpp` —
source-grep harness, no Qt link.

- **INV-1** Both labels exist as `QLabel*` members on `MainWindow`
  with the documented objectNames (`repoVisibilityLabel`,
  `updateAvailableLabel`).
- **INV-2** Both labels are constructed in the status-bar setup
  block and start `hide()`-n. The **repo-visibility** label is on
  the left side via `addWidget` (next to the git-branch chip,
  shipped 0.7.49). The **update-available** label is on the right
  side via `addPermanentWidget` (next to the Claude Code chrome).
- **INV-3** The pure helpers `findGitRepoRoot` and
  `parseGithubOriginSlug` exist in `mainwindow.cpp`, and
  `compareSemver` in `selfupdate.cpp` (moved by ANTS-5560).
- **INV-4** `parseGithubOriginSlug` handles both URL forms — source-
  grep for `https://github.com/` AND `git@github.com:` literals in
  the helper.
- **INV-5** `refreshStatusBarForActiveTab` calls `refreshRepoVisibility`
  alongside the existing `refresh*Button` calls.
- **INV-6** `refreshRepoVisibility` early-returns + `hide()`s the label
  on every failure branch: gh-unavailable, no `.git`, non-GitHub
  origin, empty cwd. Source-grep for the `hide()` call counts.
- **INV-7** The repo-visibility cache is declared with a 10-minute
  TTL (source-grep for `kCacheTtlMs` or `10 * 60 * 1000` near the
  cache lookup).
- **INV-8** `gh repo view` is invoked with `--json visibility -q
  .visibility` (the minimal field set — avoids fetching unrelated
  repo metadata).
- **INV-9** *(revised in 0.7.47)* No hourly timer:
  `m_updateCheckTimer` is **gone** from the header. Two trigger
  paths only:
  (a) Startup `QTimer::singleShot(5000, ...)` calling
      `checkForUpdates` (silent / `userInitiated=false`).
  (b) `Help → Check for Updates` action with objectName
      `helpCheckForUpdatesAction`, calling
      `checkForUpdates(/*userInitiated=*/true)` so the result
      lands as a status-bar message.
- **INV-10** `checkForUpdates` requests
  `api.github.com/repos/milnet01/ants-terminal/releases/latest`
  AND sets a `User-Agent` raw header (GitHub 403s without it).
- **INV-11** *(revised in 0.7.46)* The update-available badge sets
  `setOpenExternalLinks(false)` and connects `linkActivated` to
  `MainWindow::handleUpdateClicked` so the click handler can route
  through the AppImage updater before falling back to the browser.
- **INV-12** `compareSemver` returns `> 0` only when `tag_name` is
  strictly greater than `ANTS_VERSION`. Verified with a small table
  of cases (`0.7.44` < `0.7.45`, `0.7.45` == `0.7.45`, `0.7.45` >
  `0.7.44`, `0.8.0` > `0.7.99`).

- **INV-13** *(added 0.7.46)* `.github/workflows/release.yml` sets
  `UPDATE_INFORMATION` env var on the linuxdeploy step using the
  `gh-releases-zsync|milnet01|ants-terminal|latest|Ants_Terminal-*-x86_64.AppImage.zsync`
  schema. Without this, linuxdeploy doesn't embed the update-info
  ELF note and AppImageUpdate refuses to run against the AppImage.

- **INV-14** *(added 0.7.46)* Workflow uploads the
  `${OUTPUT}.zsync` sidecar alongside the `.AppImage`. Without the
  sidecar, AppImageUpdate clients fall back to whole-file fetch
  (slow); with it, only changed blocks transfer.

- **INV-15** *(revised ANTS-5560)* `MainWindow::handleUpdateClicked`
  routes on `SelfUpdate::installKind()`: an AppImage opens
  `UpdateDialog`, any other install opens the release page with
  `QDesktopServices::openUrl`. It never hands off to `AppImageUpdate`
  or `appimageupdatetool`.

- **INV-16** *(revised ANTS-5560)* The relaunch after Restart now is
  started with `QProcess::startDetached`, so it outlives the quitting
  process.

- **INV-17** *(revised ANTS-5560)* `UpdateDialog` is non-modal (no
  `exec()`, no `setModal(true)`), warns that Restart now closes every
  tab and Claude Code session, and offers Restart now and Restart
  later. `MainWindow::restartForUpdate` spawns the relaunch from
  `aboutToQuit`, once the quit is committed.

## Out of scope

- Per-tab update badge. The "Update available" state is a property
  of the running binary, not the active tab — it stays visible
  across tab switches.
- Authenticated GitHub API access (rate limit applies — 60
  requests/hour for unauthenticated, well within the hourly poll).
- Updating a build older than ANTS-5560 in place: it cannot verify a
  signed update, so its user updates once by hand
  (`docs/specs/ANTS-5560-appimage-self-update.md` § 2.3 Rollout).
