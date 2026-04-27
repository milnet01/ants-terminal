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

Two new `QLabel` widgets added as `addPermanentWidget` on the status
bar, in this read order: Review Changes → Background Tasks →
Roadmap → **Repo visibility** → **Update available** → Error label.

### Repo visibility — `m_repoVisibilityLabel` (objectName `repoVisibilityLabel`)

A small badge that reads "Public" or "Private" with a
theme-derived foreground colour (`th.ansi[2]` green for public,
`th.ansi[3]` amber for private). Tooltip: `<owner>/<repo> on GitHub`.
Padding `0 6px`, bold weight. Hidden by default.

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
intercepted by `MainWindow::handleUpdateClicked` (since 0.7.46 —
prior versions used `setOpenExternalLinks(true)` and unconditionally
opened the browser). The handler probes for an AppImage updater
in this order, taking the first hit:

1. `AppImageUpdate` (GUI) — preferred, gives the user a progress
   window they can dismiss.
2. `appimageupdatetool` (CLI) — silent fallback.
3. Browser (`QDesktopServices::openUrl`) — only when neither
   updater is installed or the binary isn't running as an
   AppImage (`$APPIMAGE` env var unset).

When a tool is found AND `$APPIMAGE` is set, it's launched via
`QProcess::startDetached` against the on-disk AppImage path. The
parent process can quit/restart while the download runs. A
status-bar message acknowledges the launch
("AppImageUpdate launched — downloading the new version. Quit and
restart to use it.").

Tooltip: `Click to open release notes for v<X.Y.Z> in your browser.
Currently running v<current>.`

Lifecycle:

1. `m_updateCheckTimer` (`QTimer`) ticks every **1 hour** firing
   `checkForUpdates()`.
2. A `QTimer::singleShot(5000, ...)` fires the first check 5 s
   after construction so the badge surfaces without waiting for the
   first hourly tick. The 5 s delay keeps the launch path fast and
   avoids racing the first paint.
3. `checkForUpdates()` issues a `QNetworkAccessManager::get` against
   `https://api.github.com/repos/milnet01/ants-terminal/releases/latest`
   with `Accept: application/vnd.github+json` and a non-empty
   `User-Agent` (GitHub rejects empty UAs).
4. The `finished` slot parses the JSON, strips the leading `v` from
   `tag_name`, and runs `compareSemver()` against `ANTS_VERSION`.
   If the remote tag is **strictly newer**, the badge is shown; if
   equal or older (the user is on a dev build), the badge is hidden.
5. Network errors and rate-limits are silent — the badge stays in
   its previous state. No retry storm.

## Architectural invariants enforced by tests

`tests/features/github_status_bar/test_github_status_bar.cpp` —
source-grep harness, no Qt link.

- **INV-1** Both labels exist as `QLabel*` members on `MainWindow`
  with the documented objectNames (`repoVisibilityLabel`,
  `updateAvailableLabel`).
- **INV-2** Both labels are constructed in the status-bar setup
  block, added via `addPermanentWidget`, and start `hide()`-n.
- **INV-3** The pure helpers `findGitRepoRoot`, `parseGithubOriginSlug`,
  and `compareSemver` exist in `mainwindow.cpp`.
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
- **INV-9** `m_updateCheckTimer` is a `QTimer*` member, started with a
  60-minute interval, connected to `checkForUpdates`, and a
  `singleShot(5000, ...)` fires the first check soon after start.
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

- **INV-15** *(added 0.7.46)* `MainWindow::handleUpdateClicked`
  probes for `AppImageUpdate` (GUI) first, `appimageupdatetool`
  (CLI) second; reads the `$APPIMAGE` env var (set by the AppImage
  runtime) to find the on-disk path; falls back to
  `QDesktopServices::openUrl` only when neither tool is installed
  or the binary isn't running as an AppImage.

- **INV-16** *(added 0.7.46)* The updater is launched via
  `QProcess::startDetached` so it outlives the parent process —
  the user can quit and restart Ants Terminal while the download
  runs without killing the updater.

## Out of scope

- Per-tab update badge. The "Update available" state is a property
  of the running binary, not the active tab — it stays visible
  across tab switches.
- Authenticated GitHub API access (rate limit applies — 60
  requests/hour for unauthenticated, well within the hourly poll).
- Retroactive in-place update for AppImages built before 0.7.46.
  The `UPDATE_INFORMATION` ELF note is embedded at build time —
  v0.7.45 and earlier binaries were built without it and
  AppImageUpdate refuses to act on them. Those users continue to
  download manually until they're on 0.7.46+; from there forward,
  the in-place flow works.
