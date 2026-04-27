# Changelog

All notable changes to Ants Terminal are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Sections use the standard categories — **Added** for new features, **Changed**
for changes in existing behavior, **Deprecated** for soon-to-be-removed features,
**Removed** for now-removed features, **Fixed** for bug fixes, and **Security**
for security-relevant changes.

## [0.7.45] — 2026-04-27

**Theme:** GitHub-aware status bar — two new badges that surface
context the user previously had to alt-tab to a browser to see. A
**Public/Private repo visibility** badge for the active tab's
project, and an **"↗ Update vX.Y.Z available"** notifier that
checks the GitHub releases API hourly and lets the user click
through to the release page when a newer version exists.

### Added

- **`m_repoVisibilityLabel` — Public/Private repo badge.** Small
  `QLabel` (`objectName "repoVisibilityLabel"`) added next to the
  Roadmap button on the status bar. Theme-derived foreground:
  `th.ansi[2]` green for public repos, `th.ansi[3]` amber for
  private. Tooltip `<owner>/<repo> on GitHub`. Per-tab via
  `refreshStatusBarForActiveTab` → new `refreshRepoVisibility()`:
  walks the active tab's `shellCwd()` up looking for a `.git`
  ancestor, parses `[remote "origin"] url` from `.git/config`
  (handles both `https://github.com/owner/repo` and
  `git@github.com:owner/repo` URL forms), then resolves visibility
  via `gh repo view <owner>/<repo> --json visibility -q .visibility`.
  Result is cached by repo root with a 10-minute TTL. Failure
  modes (no `.git`, non-GitHub origin, `gh` missing,
  unauthenticated, network error) all hide the label.
- **`m_updateAvailableLabel` — clickable update notifier.**
  Cyan-coloured `QLabel` (`objectName "updateAvailableLabel"`,
  `setOpenExternalLinks(true)`, RichText) shown when a newer
  GitHub release exists. New `checkForUpdates()` slot hits
  `https://api.github.com/repos/milnet01/ants-terminal/releases/latest`
  via `QNetworkAccessManager`, parses `tag_name`, and compares
  against `ANTS_VERSION` via the new pure helper `compareSemver`.
  An hourly `m_updateCheckTimer` plus a 5-second `singleShot` on
  startup keep the badge fresh. Click opens the release page in
  the user's browser. Sets a non-empty `User-Agent` header
  (GitHub 403s requests without one).
- **Pure helpers in `mainwindow.cpp`** (anonymous namespace):
  `findGitRepoRoot(start)` walks up looking for a `.git`;
  `parseGithubOriginSlug(repoRoot)` extracts `owner/repo` from
  the origin URL; `compareSemver(a, b)` compares two `X.Y.Z`
  triples component-wise as integers (so `0.10.0` correctly
  outranks `0.9.0`).

### Tests

- New `tests/features/github_status_bar/` — 12 invariants on the
  bundle:
  - **INV-1/2** — both labels declared, constructed, named,
    `addPermanentWidget`-ed, and start hidden.
  - **INV-3** — three pure helpers (`findGitRepoRoot`,
    `parseGithubOriginSlug`, `compareSemver`) exist.
  - **INV-4** — `parseGithubOriginSlug` recognises both URL forms.
  - **INV-5** — `refreshStatusBarForActiveTab` calls the new
    `refreshRepoVisibility`.
  - **INV-6** — `refreshRepoVisibility` hides the label on every
    failure branch (≥ 4 `hide()` call sites).
  - **INV-7** — 10-minute cache TTL constant present.
  - **INV-8** — `gh` invoked with the minimal `--json visibility
    -q .visibility` slug.
  - **INV-9** — hourly timer + 5 s `singleShot` first run.
  - **INV-10** — `checkForUpdates` hits `releases/latest` with a
    `User-Agent` header.
  - **INV-11** — update label has `setOpenExternalLinks(true)`.
  - **INV-12** — `compareSemver` splits on `.` and uses `toInt` so
    component compares are numeric (not lexicographic).

### Notes

- Out of scope: actual binary auto-update via AppImageUpdate / zsync.
  That's a follow-up release — needs the build workflow to publish a
  `.zsync` sidecar AND embed the `gh-releases-zsync|...`
  update-information string in the linuxdeploy command. Cannot
  retroactively update binaries that didn't ship with the metadata.
  This release only **notifies**; the user clicks through to download
  manually.
- Update-available badge is global (one per running binary), not
  per-tab. Repo-visibility badge is per-tab.
- GitHub API rate limit (60 req/hour for unauthenticated) is well
  within the hourly poll budget.

## [0.7.44] — 2026-04-27

**Theme:** Two themed cleanups in one bundle. Retiring the dormant
GPU-accelerated glyph-atlas renderer (compiled-but-unreachable since
0.7.4) and its no-op user-facing chrome (Settings checkbox + View
menu action) — ~900 LoC removed, retires ROADMAP § "Renderer
subsystem decision". Plus scoping the Background Tasks dialog to the
active tab's project tree so sessions running in another project's
window don't leak into this window's button.

### Changed

- **Background Tasks button now scopes to the active tab's project.**
  Previously `ClaudeIntegration::activeSessionPath()` walked
  `~/.claude/projects/` system-wide and returned the most-recently-
  modified `.jsonl`, which meant a busy session in one project's
  window would surface its bg-tasks count on a sibling window
  pointed at a different project. The method now accepts a
  `projectCwd` argument; the active tab's `shellCwd()` is encoded
  via `encodeProjectPath` and the helper walks up the cwd
  (`cdUp`) probing each ancestor's `~/.claude/projects/<encoded>/`
  subdir, returning the deepest match's newest `.jsonl`. Empty
  `projectCwd` falls back to system-wide newest (kept for callers
  that genuinely want it; nothing in tree currently does).

### Removed

- **`src/glrenderer.{h,cpp}` (~900 LoC).** The optional GPU-
  accelerated glyph-atlas renderer with GLSL 3.3 shaders +
  instanced quads + a per-cell vertex buffer. Disabled in 0.7.4
  when `TerminalWidget` switched from `QOpenGLWidget` to plain
  `QWidget`; sat compiled-but-unreachable for 39 releases. ROADMAP
  framed the choice as "revive via `createWindowContainer` *or*
  delete" — picked delete. The QPainter+QTextLayout path remains
  the only paint path and is fast enough for every shipped corpus.
- **`gpu_rendering` Config key + Settings checkbox + View menu.**
  `Config::gpuRendering()` / `setGpuRendering()`, the Settings →
  Appearance "GPU rendering (glyph atlas + GLSL shaders)"
  checkbox, and the View → "GPU Rendering" menu action all wrote
  to the dormant bool with no observable effect since 0.7.4. All
  three are removed in this release. Existing config files with
  `"gpu_rendering": true` are silently ignored — `Config` doesn't
  validate unknown JSON keys.
- **`TerminalWidget::setGpuRendering(bool)` / `gpuRendering()`** +
  the `m_glRenderer` / `m_gpuRendering` members. The
  `setGpuRendering` setter was a no-op stub; both are gone.
- **Disabled-OpenGL-path landmark comment + commented-out
  `m_glRenderer` cleanup** in the `TerminalWidget` destructor and
  `paintEvent` chain.

### Tests

- `tests/features/claude_bg_tasks_button/test_claude_bg_tasks_button.cpp`
  extended from 10 → 11 invariants:
  - **INV-11** — three-part check on the project-scoping fix:
    (a) header signature `activeSessionPath(const QString
    &projectCwd)`,
    (b) implementation references `encodeProjectPath` AND `cdUp`
    (the walk-up logic — no shortcut to a single string match),
    (c) `MainWindow::refreshBgTasksButton` reads
    `focusedTerminal()->shellCwd()` and passes it through to
    `activeSessionPath(cwd)`.

  Spec updated with the matching contract entry (linking the
  user-feedback quote 2026-04-27).
- `tests/features/settings_restore_defaults/spec.md` updated to
  drop the no-longer-applicable `gpuRendering off` clause from
  Invariant 2's reset-default catalogue.

### Notes

- No user-visible behaviour change from the GL renderer removal —
  the path was never reachable. The user-visible improvements are
  the two pieces of chrome that no longer lie about their effect.
- Out of scope: actually adding a real GPU acceleration path (e.g.
  via `createWindowContainer` + `QOpenGLWindow`). If that comes
  back, it'll be a fresh design with current Qt 6 idioms, not a
  resurrection of the 0.7.4-era code.

## [0.7.43] — 2026-04-27

**Theme:** Roadmap viewer polish — the dialog the previous release
introduced gets a working **Close** button and a **dynamic
section-navigation sidebar** that lists every heading in the active
ROADMAP.md and lets the user click to jump. The TOC is built per
project from whatever headings the file happens to have (no
hardcoded section names) so the same dialog adapts to every repo
that opens with a roadmap.

### Added

- **TOC sidebar in the Roadmap viewer.** `QSplitter(Qt::Horizontal)`
  inside `RoadmapDialog` puts a `QListWidget` (`objectName
  "roadmap-toc"`) on the left and the rendered viewer on the right.
  The list is rebuilt from the markdown on every refresh — including
  live-update events on `ROADMAP.md` change — and entries are
  indented two spaces per level above 1 with level-1 headings shown
  bold. Click or activate an entry → the viewer scrolls to the
  matching heading.
- **Anchor-emitting renderer.** `RoadmapDialog::renderHtml` now
  prepends `<a name="roadmap-toc-N"></a>` before each `<h1>`/`<h2>`/
  `<h3>`/`<h4>` it emits. The N-th anchor matches the N-th entry
  returned by the new pure helper `RoadmapDialog::extractToc`, which
  returns a `QVector<TocEntry>` of `{level, text, anchor}` triples.
  Both helpers are pure and static so they're driven by the
  feature-conformance test without spinning a Qt widget tree.
- **`QTextBrowser` in place of `QTextEdit`.** Required for
  `scrollToAnchor` support. Links are pinned non-navigable
  (`setOpenLinks(false)` / `setOpenExternalLinks(false)`) so a stray
  markdown link can't replace the document.

### Fixed

- **Close button in the Roadmap viewer is now actually clickable.**
  The dialog used the role-based `QDialogButtonBox::rejected` signal
  alone; under some Qt 6 builds the standard `Close` button does not
  trigger that signal, leaving the button looking active but doing
  nothing. The Close button is now wired *both* via `rejected()`
  (kept as belt-and-braces) and via a direct `clicked` connection on
  the underlying `QPushButton` retrieved with
  `button(QDialogButtonBox::Close)`. The direct connect is the
  authoritative fix.

### Tests

- `tests/features/roadmap_viewer/test_roadmap_viewer.cpp` extended
  from 10 → 14 invariants:
  - **INV-11** — `extractToc` returns a 5-entry list with matching
    levels, raw text, and `roadmap-toc-N` anchors against a known
    multi-level input.
  - **INV-12** — `renderHtml` emits an `<a name="roadmap-toc-0">`
    immediately before the first `<h2>` so `scrollToAnchor` can
    position the viewer at any TOC entry.
  - **INV-13** — Source-grep on `roadmapdialog.cpp` confirms a
    `QSplitter`, the `roadmap-toc` objectName, and a
    `scrollToAnchor` invocation in the TOC handler.
  - **INV-14** — Source-grep confirms the Close button is wired
    directly via `QAbstractButton::clicked`, guarding against the
    Qt 6 `rejected()`-only quirk.

### Notes

- The Roadmap viewer's TOC is intentionally flat (`QListWidget`)
  rather than tree-shaped (`QTreeWidget`) — flat-list with leading
  whitespace renders the hierarchy in O(1) widget cost without
  per-node expand/collapse state, which would invite sync bugs on
  live-update refreshes.
- Out of scope for this release: cross-document jumps (e.g. from a
  ROADMAP heading to a CHANGELOG entry), heading-fold/collapse,
  fuzzy-search inside the TOC. The dialog is still a viewer, not an
  editor.

## [0.7.42] — 2026-04-27

**Theme:** Inline ghost-text completion in the Command Palette —
Claude Code's `/slash`-command UX, ported to Ctrl+Shift+P. As you
type, the unmatched suffix of the top fuzzy-match is rendered in a
dimmed colour right after the cursor; `Tab` commits the suggestion
into the input. Retires ROADMAP § "Command Palette ghost-completion
(near-term, small scope)".

### Added

- **`GhostLineEdit` subclass of `QLineEdit`.** Lives in
  `src/commandpalette.h`. Carries a `m_ghost` string set via
  `setGhostSuffix()`; `paintEvent` calls
  `QLineEdit::paintEvent(event)` first, then opens a fresh `QPainter`
  and draws the ghost at `cursorRect().right() + 1` using
  `palette().color(QPalette::Text)` with `setAlphaF(0.45)`. No
  layout changes — the suffix overlays the existing line-edit content
  area, so palette geometry is unchanged.

- **`CommandPalette::updateGhostCompletion(filter)`.** Invoked from
  `populateList` after the result list is rebuilt. Looks at
  `m_list->item(0)`, recovers the underlying `QAction`, strips `&`
  accelerators, and:
  - if the filter is empty, list is empty, or the action name does
    not start with the filter (case-insensitive) →
    `setGhostSuffix("")`;
  - otherwise → `setGhostSuffix(name.mid(filter.length()))`, which
    preserves the action name's original casing in the ghost.

  `contains()`-only matches (where the filter appears mid-string in
  the top match) get an empty ghost — the visual contract is that
  the suffix appears flush after the user's typed input, which only
  makes sense for prefix matches.

- **`Tab`-key commit handler.** `eventFilter` adds
  `Qt::Key_Tab` → `commitGhost()`, which appends the ghost suffix to
  the input via `setText`. The follow-up
  `textChanged → filterActions → populateList → updateGhostCompletion`
  cycle clears the ghost in the same dispatch since the new filter
  equals the action name's tail exactly. The post-commit text equals
  the visible composition (user-typed prefix + ghost suffix), so
  user-typed casing is preserved — same as shell-completion
  semantics. Tab is **always** consumed by the palette (even when
  the ghost is empty) so focus cannot leak out of the input while
  the palette is open. `Tab` does not also execute the action — the
  user presses `Enter` to run, matching Claude Code's
  `/slash`-completion contract.

### Tests

- `tests/features/command_palette_ghost_completion/` — ten
  invariants spanning the contract: I1 the input is a `GhostLineEdit`
  findable by `objectName`; I2 empty filter → empty ghost; I3
  prefix `"ind"` → ghost `"ex Review"` (first item in setActions
  order is `"Index Review"`); I4 uppercase `"INDEX"` →
  ghost `" Review"` (case-insensitive prefix, casing preserved
  from the action name); I5 `contains`-only match → empty ghost;
  I6 no-match → empty ghost; I7 Tab commit → text =
  filter + ghost (`"index Review"`) and ghost cleared; I8 Tab
  with empty ghost is consumed (text unchanged, focus retained on
  input); I9 source-grep on `commandpalette.cpp` (exactly one
  `setAlphaF(0.45` literal, ≥1 `cursorRect(` reference); I10 Esc
  still hides the palette and emits `closed()` exactly once
  (regression guard on existing dismiss behaviour). Drives
  `CommandPalette` directly under `QT_QPA_PLATFORM=offscreen` —
  no MainWindow link, no PTY. Pre-fix verification: with
  `commandpalette.{h,cpp}` stashed, the test fails to even compile
  (missing `GhostLineEdit` symbol); restored, test passes.

### Notes

- **Out of scope.** The in-terminal shell ghost-suggestion
  (fish-style, OSC 133 prompt-detection + history scraping) is a
  separate ROADMAP item marked `💭` with deferral past 1.0. It
  would touch `terminalgrid.cpp` paint path and `vtparser.cpp`
  rather than `commandpalette.cpp`.
- **Frequency-ranked top match.** The palette today picks the top
  match by setActions iteration order. A frequency-ranked source
  (most-used action first) is a noted future refinement; the
  contract above stays stable across that change because the test
  uses a fixed action ordering.

## [0.7.41] — 2026-04-27

**Theme:** Accessibility — explicit `setAccessibleName` /
`setAccessibleDescription` on every glyph-only chrome control so
Orca, Speakup, and other AT-SPI screen readers announce them by
purpose ("Close window") instead of by codepoint name ("eight-spoked
asterisk", "multiplication sign x"). Two ROADMAP items retired in
one bundle: § "Accessibility pass on chrome" (mainwindow / palette /
title-bar control labels) and § "AT-SPI introspection lane" (an
automated check that every user-visible chrome control carries a
name).

### Added

- **Accessible names on TitleBar window controls.**
  `centerBtn` ("Center window" / "Center this window on the active
  screen"), `minimizeBtn` ("Minimize window"), `maximizeBtn`
  ("Maximize window" / "Maximize or restore this window"), and
  `closeBtn` ("Close window") each carry an explicit
  `setAccessibleName` and `setAccessibleDescription` set immediately
  after `setText` in `titlebar.cpp:54-97`. Each button also gains a
  stable `objectName` (the previously-set `closeBtn` is joined by
  the matching three siblings) so the introspection test can find
  them deterministically. Visual rendering is unchanged — the glyphs
  (`✥ – □ ✕`) still drive the on-screen pixels; only the AT-SPI tree
  changed.

- **Accessible names on the Command Palette.** `m_input`
  (objectName `commandPaletteInput`) gains `Command palette search`
  / `Type to filter actions; Tab to commit; Esc to dismiss`.
  `m_list` (objectName `commandPaletteList`) gains
  `Command palette results` / `Available actions matching the
  current filter`. Placeholder text and visible behaviour are
  unchanged. The placeholder is a typing hint for sighted users; the
  accessible name is what screen readers announce on focus —
  separating the two unblocks the keyboard-only workflow.

### Tests

- `tests/features/a11y_chrome_names/` — eight invariants spanning
  the contract: T1–T4 each TitleBar button has the right
  accessibleName + non-empty accessibleDescription; T5/T6 the
  CommandPalette input + list have theirs; T7 the AT-SPI
  introspection lane (every reachable `QAbstractButton` carries
  either a non-empty `accessibleName()` or a non-empty `text()`,
  every reachable `QLineEdit` has an explicit `accessibleName`),
  T8/T9 source-grep counts on `titlebar.cpp` and `commandpalette.cpp`
  so a future refactor that drops the `setAccessibleName` calls
  fails the build, not just AT-SPI usage at runtime. The test
  constructs the `TitleBar` and `CommandPalette` directly under
  `QT_QPA_PLATFORM=offscreen` so it runs in CI without a display.

### Notes

- `tr()` translation hooks for the new strings are deferred to the
  `0.9.0` H10 i18n bundle so the `.qm` files cover both UI text and
  accessibility strings in one pass — pulling them in now would mean
  re-touching every `setAccessibleName(...)` call later.
- A custom `QAccessibleInterface` adapter for `TerminalWidget`
  itself (the `text-changed` event lane behind the H9 a11y bundle in
  `0.9.0`) is a separate, larger design cycle and is out of scope
  for this bundle.

## [0.7.40] — 2026-04-27

**Theme:** Performance — scroll-region rotate + VtBatch zero-copy
across the worker→GUI thread hop. Two Tier 3 perf items from
`ROADMAP.md § 0.7.x ⚡ / 🏗 Tier 3 — structural` retired in one
bundle. Both are observably equivalent at the API level — the
existing `tests/features/scroll_region_rotate/` and
`tests/features/threaded_parse_equivalence/` regressions pass
unchanged — so the win is throughput / cost, not visible behaviour.

### Changed

- **`TerminalGrid::scrollUp` / `scrollDown` use `std::rotate`.**
  Replaced the per-iteration `m_screenLines.erase + insert` loop
  with a single rotate over `[scrollTop, scrollBottom]`, gated by a
  pool-salvage / scrollback-push pre-pass and a fresh-blank
  post-pass. The CSI 2J doubling-guard window check now runs once
  per batch (elapsed time across a batch is microseconds, so the
  per-iteration window-extend was redundant). `m_scrollback` cap
  enforcement collapses from N pop_front passes to one. On `CSI
  100 S` against an 80-row screen, the row-shift cost goes from
  O(count × rows) memmoves (8000) to O(rows) (80).

- **`VtStream::batchReady` carries `VtBatchPtr` instead of
  `const VtBatch &`.** `using VtBatchPtr = std::shared_ptr<const
  VtBatch>;` is the new alias in `vtstream.h`. Qt's queued-
  connection plumbing must own the parameters it dispatches, so
  the prior `const T &` signal forced a deep copy of the entire
  batch (the `actions` vector + `rawBytes` QByteArray) on every
  worker→GUI hop, regardless of any move-from-pending shaping the
  emitter performed. The shared_ptr wrap reduces the cross-thread
  payload to a 16-byte atomic refcount bump; the underlying
  `VtBatch` lives on the heap and is not duplicated. Both emit
  sites (`flushBatch`, `onPtyFinished`) build via
  `std::make_shared<VtBatch>()`. `TerminalWidget::onVtBatch`
  signature changed to `void onVtBatch(VtBatchPtr batch)`; field
  access is now via `batch->…` instead of `batch.…`.

### Tests

- `tests/features/vtbatch_zero_copy/` — 5 source-grep invariants
  locking the cross-thread signal *shape* so a future refactor
  can't silently revert to the deep-copy form. Behavioural
  equivalence on the action stream is already covered by
  `tests/features/threaded_parse_equivalence/`.

- `tests/features/scroll_region_rotate/` — pre-existing 8
  invariants on rotation correctness (I1–I8 in spec.md) still
  pass. The spec was written algorithm-agnostic, anticipating this
  swap.

## [0.7.39] — 2026-04-27

**Theme:** Claude Code UX bundle + status-bar Roadmap viewer. Two
user requests landed in quick succession.

User request 1 (Claude state-dot palette):
> "Let's have a round dot on each tab that has a Claude Code session
> running (no icons or anything else other than the tab label). The
> dot will change colour with the various states that Claude Code is
> in. Each state has its own colour (grey for idle). Then extend
> those colours to the status bar Claude Code status too."

User request 2 (Roadmap viewer):
> "Add a button on the status bar to view the roadmap. So, it brings
> up a dialog showing the roadmap. It should have filters as well to
> show what is outstanding and what is completed. If at all possible,
> it should also highlight what item is being done currently. The
> roadmap button should only show if there is roadmap documentation
> — let's simplify that to requiring a roadmap.md file only."
> Plus a clarification adding a fourth emoji toggle and elevating
> "Currently being tackled" to a peer filter.

### Added

- **Unified Claude state-dot palette across tabs and status bar.**
  New static helper `ClaudeTabIndicator::color(Glyph)` in
  `coloredtabbar.h` is the single source of truth for an
  eight-state palette: Idle grey `#888888`, Thinking blue
  `#5BA0E5`, ToolUse yellow `#E5C24A`, Bash green `#6FCF50`,
  Planning cyan `#5DCFCF`, Auditing magenta `#C76DC7`, Compacting
  violet `#A87FE0`, AwaitingInput orange `#F08A4B`. Red is
  intentionally absent — AwaitingInput is a normal interaction
  state, not an error. `ColoredTabBar::paintEvent` calls the
  helper for fill colour and uses a single `kDotRadius = 4` for
  every state; the prior AwaitingInput "outline + radius 5"
  treatment is removed (per "no icons or anything else"). The
  bottom status-bar Claude Code label was rewired to map current
  state → Glyph → helper colour, replacing the prior
  `Theme::ansi[N]` mappings that drifted from the tab dot's
  colour and varied across themes.

- **Auditing now lights the per-tab dot.** Previously surfaced
  only on the active-tab status bar (`m_claudeAuditing`). Plumbed
  into `ClaudeTabTracker::ShellState::auditing` (mirrored from
  the existing `ClaudeTranscriptSnapshot.auditing` field). Tab
  provider routes it to `Glyph::Auditing` magenta; hover tooltip
  reads "Claude: auditing". Precedence chain unchanged across
  both surfaces:
  AwaitingInput → Planning → Auditing → state-derived.

- **Status-bar Roadmap viewer button (`roadmapdialog.{h,cpp}`,
  new files).** Visible iff the active tab's `shellCwd()`
  contains a `ROADMAP.md` (case-insensitive — accepts
  `Roadmap.md`, `roadmap.md` too). `MainWindow::
  refreshRoadmapButton` is called from the central
  `refreshStatusBarForActiveTab` tick. Click opens
  `RoadmapDialog`: a non-modal QDialog with a top row of five
  peer category checkboxes (✅ Done · 📋 Planned · 🚧 In progress ·
  💭 Considered · Currently being tackled), all default-checked.
  A bullet renders iff any of its enabled category memberships
  matches; plain narration bullets (no status emoji) always
  render. The static `RoadmapDialog::renderHtml` helper is pure
  so tests can drive it without spinning a Qt widget.

- **"Currently being tackled" highlight.** A bullet matches the
  current-work signal if its first-80-character payload (status-
  emoji-stripped, normalised — lowercase, hyphens-and-underscores
  as spaces, punctuation removed) contains a phrase from a signal
  set built from (a) the local `CHANGELOG.md` `[Unreleased]`
  block bullets, (b) the last 5 non-release/non-merge/non-revert
  git commit subjects. Matched bullets get a yellow left-border
  CSS highlight (`border-left: 4px solid #E5C24A` — the same
  ToolUse yellow from the dot palette; the consistency is
  intentional). When the signal set is empty, no highlight
  renders — the feature is silent when there's nothing to point
  at.

- **Live-tail dialog mechanics.** `QFileSystemWatcher` on
  `ROADMAP.md` and `CHANGELOG.md`, 200 ms debounce timer, and
  the same scroll-preservation triple shipped with 0.7.37 /
  0.7.38: `m_lastHtml` shared_ptr cache + skip-identical-HTML
  guard, capture vbar before `setHtml`, restore with
  `qMin(saved, vbar->maximum())` clamp, was-at-bottom pin so
  live appends stay visible.

### Changed

- **`ClaudeTabIndicator::Glyph` enum gains `Auditing`.** Inserted
  between `Planning` and `Compacting` so the palette ordering
  reads Done-side → Action-side → InFlight-side → BlockingUser.
  `ClaudeTabTracker::ShellState` gains a `bool auditing` field
  and a new comparison branch in `maybeEmit` so changes to the
  audit latch trigger `shellStateChanged` like every other
  field.

- **`MainWindow::applyClaudeStatusLabel` no longer reads
  `Theme::ansi[]` for Claude-label colours.** All state →
  colour mappings now route through the unified helper. The
  `Theme &th = Themes::byName(...)` local was removed (unused).

### Removed

- **AwaitingInput-specific dot decoration** (`radius = 5` and
  white outline). Colour alone is the differentiator now, per
  the "no icons or anything else" user spec. The
  `outline.alpha()` branch was removed from `paintEvent`.

### Tests

- `tests/features/claude_state_dot_palette/` — 8 source-grep
  invariants asserting helper signature, full palette,
  paintEvent helper-call, uniform geometry, mainwindow
  status-bar wiring, Auditing plumbing through `ShellState`,
  and double-use of the `Auditing` glyph (provider closure +
  status applier).

- `tests/features/roadmap_viewer/` — 10 hybrid invariants
  (source-grep + behavioral). Links the dialog source so the
  static `renderHtml` helper can be driven against synthetic
  markdown to verify five-bit filter semantics, the highlight
  CSS marker on signal match, the marker's absence on empty
  signal sets, the case-insensitive cwd probe, the wire-up
  shape (`m_roadmapBtn` construction + click connection), and
  the `refreshRoadmapButton` call from
  `refreshStatusBarForActiveTab`.

### Documentation

- ROADMAP user-request entries added for both 0.7.39 features
  with full citation of the user asks. Two stale 📋 entries in
  the cross-cutting themes section flipped to ✅ with
  shipped-release citations (the `/tmp/*.js` TOCTOU and the
  no-auth local-IPC chain — both shipped in the 0.7.12 Tier 1
  batch but the umbrella narrative entries weren't updated at
  the time).

## [0.7.38] — 2026-04-25

**Theme:** Feature. Background-tasks status-bar surface for Claude
Code. User request 2026-04-25:
> "a button on the status bar when there are background tasks being
> run. We then click the button to view what Claude Code shows for
> the background tasks. The button opens a dialog showing the live
> update info on the background tasks."

Claude Code can spawn long-running tasks via `Bash` or `Task` with
`run_in_background: true`. The TUI shows a sidebar listing them with
their tail output; users dropping into a different tab to keep
working had no way to see those tasks from Ants Terminal — they had
to flip back to the TUI and parse the sidebar by hand. The new
status-bar button surfaces the live count and the dialog mirrors the
0.7.37 Review Changes update model (debounced live tail with
scroll-preservation).

### Added

- **Background-tasks tracker (new `claudebgtasks.{h,cpp}`).** A pure
  parser walks the active session's transcript JSONL, recognizing
  `tool_use` blocks whose `input.run_in_background == true` (start)
  and `tool_result` blocks whose `toolUseResult.backgroundTaskId` is
  set (confirmation, with the on-disk output path extracted from
  the result body). Completion / kill state is derived from
  subsequent `BashOutput` results carrying `status: "completed" |
  "killed" | "failed"` and from `KillShell` tool calls. The
  `ClaudeBgTaskTracker` class wraps the parser with a
  `QFileSystemWatcher` on the transcript and emits `tasksChanged()`
  when the running-count or shape changes. Static
  `parseTranscript(path)` is testable without a watcher.

- **Status-bar button (`mainwindow.{h,cpp}`).** Sibling to the
  Review Changes button; same fixed sizePolicy contract. Hidden by
  default; visible only while `runningCount() > 0` for the active
  tab's session. Label re-renders as "Background Tasks (N)";
  tooltip discloses running + total. Re-targeted on tab switch via
  `refreshBgTasksButton()` (called from
  `refreshStatusBarForActiveTab`), so each tab's session drives its
  own count independently.

- **Live-tail dialog (new `claudebgtasksdialog.{h,cpp}`).** Mirrors
  the Review Changes live-update model that 0.7.37 stabilized.
  `QFileSystemWatcher` covers each task's `.output` file plus the
  transcript path, so new starts and completions appear without
  manual refresh. A 200 ms debounce timer collapses bursts of
  fileChanged signals (common when the backgrounded process is
  noisy) into one render. Skip-identical-HTML guard via a per-
  dialog `m_lastHtml` (`std::shared_ptr<QString>`) preserves
  selection and scroll position when the render is unchanged.
  Capture-vbar/hbar before `setHtml`, restore after with `qMin(...,
  maximum())` clamp keeps absolute scroll position when content
  changes — same shape as the 0.7.37 Review Changes fix. Bonus
  "was at bottom" detection: when the user is tailing the bottom,
  pin them to the bottom across appends so live output stays
  visible without manual scrolling.

### Changed

- **`tests/features/claude_bg_tasks_button/spec.md` + test
  (10 invariants).** New regression test pinning the parser shape
  (`run_in_background` and `backgroundTaskId` keys), the tracker's
  `QFileSystemWatcher` + `tasksChanged()` signal, the button's
  hide-when-empty contract, the connect to `showBgTasksDialog`,
  the dialog's reuse of the 0.7.37 scroll-preservation pattern,
  the `outputPath` watch enumeration in `rewatch()`, the debounce
  timer interval (≤ 500 ms, single-shot), and the CMake source
  list. Source-grep harness, no Qt link.

## [0.7.37] — 2026-04-25

**Theme:** UX bug fix. Live-update regression in the Review Changes
dialog (the QFileSystemWatcher + 300ms debounce wiring added in
0.7.32). User report 2026-04-25:
> "When using the Review Changes dialog, the constant resetting of
> the text means that if I scroll, it resets to the beginning every
> refresh. That means I can't scroll basically."

The 0.7.32 finalize lambda called `viewerGuard->setHtml(html)`
unconditionally on every probe completion — every git change, every
debounce tic, every Refresh click. `QTextEdit::setHtml` re-parses
the document and snaps the vertical scroll bar back to the top, also
discarding selection and cursor position. On a long diff with active
live updates the dialog became unscrollable.

### Fixed

- **Review Changes dialog: scroll position preserved across live
  refreshes (`mainwindow.cpp`).** Two-layer guard around the
  `setHtml` call inside `MainWindow::showDiffViewer`'s `finalize`
  lambda. (1) An `auto lastHtml = std::make_shared<QString>()` cache
  threaded through `runProbes` and `finalize` lets `finalize` early-
  return when the new render is byte-identical to the previous one
  — the common case during idle live-update tics, where branch
  metadata refreshes don't change anything visible. Selection,
  cursor, and scroll are byte-perfectly preserved. (2) When content
  does change, vertical and horizontal scroll-bar values are
  captured before `setHtml` and restored after, clamped to
  `bar->maximum()` so a shorter render after a commit doesn't over-
  scroll. First-render carve-out (`isFirstRender =
  lastHtml->isEmpty()`) keeps the initial paint at the top.

### Changed

- **`tests/features/review_changes_scroll_preserve/spec.md` + test
  (6 invariants).** New regression test pinning the `lastHtml`
  cache, the skip-identical guard, the
  capture-scroll-before-setHtml + restore-after pattern, and the
  first-render carve-out. Source-grep harness scoped to the
  `MainWindow::showDiffViewer` body so other QScrollBar / setHtml
  call-sites in `mainwindow.cpp` don't cause false positives.

## [0.7.36] — 2026-04-25

**Theme:** UX bug fix. Same translucent-parent failure mode that bit
the menubar in 0.7.25/0.7.26 caught in two more places (user report,
post-0.7.32):
> "The tabs themselves are fine but the rest of the tab bar across
> the window is transparent as well as the background for the status
> bar."

The 0.7.32 close-button SVG drew the user's eye to the empty-area
fill that had been mis-painted all along; the bug was not introduced
by 0.7.32 but became visible because the new tab look re-balanced
what the user noticed.

### Fixed

- **Tab bar empty area paints opaque under
  `WA_TranslucentBackground` (`coloredtabbar.{h,cpp}`).** Was: the
  strip to the right of the last tab rendered the desktop wallpaper
  through, with the QSS `QTabBar { background-color: ... }` rule
  silently dropped by Qt's stylesheet engine on KWin + Breeze + Qt 6
  once `WA_OpaquePaintEvent` was set on the widget. Same root cause
  documented in `opaquemenubar.h`. Now: `ColoredTabBar` exposes
  `setBackgroundFill(QColor)` and prepends a
  `CompositionMode_Source` `fillRect` to its existing `paintEvent`,
  before the base class draws tabs and before the colour-group
  gradient overlay. `applyTheme()` pushes `theme.bgSecondary` into
  the override.

- **Status bar background paints opaque under
  `WA_TranslucentBackground` (new `opaquestatusbar.h`,
  `mainwindow.{h,cpp}`).** Was: the entire status bar strip rendered
  the desktop wallpaper through — same root cause as the tab bar
  above. Now: a header-only `OpaqueStatusBar` mirrors
  `OpaqueMenuBar` (paintEvent fillRect → delegate). `MainWindow`
  constructs an `OpaqueStatusBar`, installs it via `setStatusBar(...)`
  *before* the first `statusBar()` call (Qt's lazy-creation would
  otherwise install a plain `QStatusBar` that paints transparent),
  and pushes `theme.bgSecondary` via `setBackgroundFill` from
  `applyTheme()`.

### Changed

- **`tests/features/tabbar_statusbar_opaque/spec.md` + test (5
  invariants).** New regression test pinning the `setBackgroundFill`
  + `fillRect` mechanism on both bars, the `setStatusBar` install
  order, the `WA_OpaquePaintEvent` attribute on the tab bar, and the
  paint order inside `ColoredTabBar::paintEvent` (fill before the
  base class draws tabs). Source-grep harness, no display required.

## [0.7.35] — 2026-04-25

**Theme:** UX bug fix. Single user-reported regression in the Help →
About Ants Terminal dialog (the GUI version indicator added in 0.7.22).

### Fixed

- **About Ants Terminal dialog OK button silently no-op'd
  (`mainwindow.cpp`).** Was: `Help → About Ants Terminal…` opened
  the version dialog correctly, but clicking its `OK` button did
  nothing — the dialog could only be dismissed via the
  window-manager close (`X`) button. The 0.7.22 implementation
  used `QMessageBox::Ok` with `Qt::TextBrowserInteraction` (which
  pulls in `Qt::TextSelectableByMouse`); under the combination of
  our frameless `MainWindow`, `Qt::WA_TranslucentBackground`, KDE
  Plasma + KWin, and Qt 6.11 this caused the OK click to be
  silently dropped. Now: the About handler builds a custom
  `QDialog` with a `QDialogButtonBox(QDialogButtonBox::Ok)` whose
  `accepted` signal is explicitly connected to `QDialog::accept`,
  giving us a click path that's standard, testable, and
  independent of `QMessageBox`'s internal standard-button dispatch.

- **About dialog GitHub link was a visual-only no-op
  (`mainwindow.cpp`).** Was: the link in the About body was
  rendered as clickable (cursor changed on hover) because
  `Qt::TextBrowserInteraction` enables `Qt::LinksAccessibleByMouse`,
  but `setOpenExternalLinks(true)` was never called on the label
  — so clicking the link emitted `linkActivated()` into the void.
  Now: the body `QLabel` has `setOpenExternalLinks(true)`, so
  clicking the GitHub URL opens it in the user's browser.

### Changed

- **`tests/features/help_about_menu/spec.md` + test (8
  invariants).** Spec rewritten to lock the QDialog +
  QDialogButtonBox shape, document the 2026-04-25 regression in
  the History section, and assert via source-grep that the
  `QMessageBox::Ok` + `Qt::TextBrowserInteraction` pattern
  cannot reappear without the test failing. Pre-fix source
  fails 6 of the 8 new I4 invariants; post-fix all 8 pass.

## [0.7.34] — 2026-04-25

**Theme:** Terminal correctness. One ROADMAP item retiring the last
known DECOM (origin mode) bug — `tmux`/`screen` save-restore
round-trips were silently dropping the origin-mode flag and
ignoring scroll-region-relative coordinates on absolute cursor
moves.

### Fixed

- **CUP / HVP / VPA translate to scroll-region origin under DECOM
  (`terminalgrid.cpp`).** Was: with origin mode (DECOM, `CSI ?6h`)
  active and a scroll region set via DECSTBM (`CSI top;bottom r`),
  `CSI 1;1H` jumped to absolute (0, 0) and `CSI 99;1H` clamped to
  the bottom of the entire screen instead of the bottom of the
  scroll region. Programs that depend on origin-mode-relative
  positioning (vim split-window cursor restoration, `screen`'s
  caption line, `tmux` status redraws) ended up writing to the
  wrong row. Now: the `'H'`, `'f'`, and `'d'` cases inside
  `processCSI` add `m_scrollTop` to the requested row when
  `m_originMode` is set, then clamp to `[m_scrollTop, m_scrollBottom]`.
  Behaviour matches xterm.

- **DECSC saves DECOM + DECAWM, DECRC restores them
  (`terminalgrid.cpp`, `terminalgrid.h`).** Was: `saveCursor()`
  stored only `(row, col, attrs)`, so a TUI that flipped origin
  mode or auto-wrap, did `CSI s` / `ESC 7`, ran code that
  re-toggled the flag, then `CSI u` / `ESC 8`'d ended up with the
  flag in whatever state the inner code left it in — silent
  coordinate-space corruption. Now: `m_savedOriginMode` and
  `m_savedAutoWrap` members capture the flags on save and restore
  them on restore, matching the VT420 spec for DECSC / DECRC.

- **DECSTBM home position respects DECOM (`terminalgrid.cpp`).**
  Was: `setScrollRegion()` always homed the cursor to absolute
  (0, 0) after setting the region. Now: when `m_originMode` is
  on at the time DECSTBM lands, the cursor moves to
  `(m_scrollTop, 0)` — the top-left of the *origin-mode*
  coordinate space — as xterm does.

Locked by `tests/features/origin_mode_correctness/` — 12
behavioural invariants (CUP/HVP/VPA translation, scroll-region
clamping, DECSTBM home, DECSC/DECRC save-restore in both `CSI s/u`
and `ESC 7/8` forms, DECAWM round-trip via the wrap-or-not at
last-column probe) plus 4 source-grep checks anchoring the fix's
shape (`std::clamp(row, m_scrollTop, m_scrollBottom)` near `case
'H'`, `m_savedOriginMode = m_originMode` in `saveCursor`, the
mirror line in `restoreCursor`, and the `m_originMode ? m_scrollTop
: 0` ternary in `setScrollRegion`).

## [0.7.33] — 2026-04-25

**Theme:** Lifecycle / cleanup. Three ROADMAP items addressing
GUI-thread blocking on shutdown, an xdg-desktop-portal session
leak, and two latent issues (OOM surface + symlink escape) in the
Lua plugin loader.

### Changed

- **PTY destructor escalation runs off the main thread
  (`Pty::~Pty`).** Was: SIGTERM-then-busy-wait-then-SIGKILL ran on
  the GUI thread; N split panes closing together blocked the
  window close N × 500 ms. KWin would throw a "window not
  responding" hint at four splits. Now: the destructor still
  does the cheap pre-escalation reap (SIGHUP + close master fd +
  `waitpid(WNOHANG)`) inline — most shells exit on SIGHUP in
  microseconds. If the cheap reap doesn't take, the
  SIGTERM/SIGKILL escalation moves to a detached `std::thread`
  capturing the pid by value (no `this` reference can outlive
  the destructor). The thread creation is wrapped in a
  `try { ... }.detach() } catch (const std::system_error &)`
  that falls back to the synchronous escalation when thread
  creation fails (rare — `ulimit -u` pressure at exit). Locked
  by `tests/features/pty_dtor_off_main_thread/` — 11 invariants
  on `<thread>` include, lambda capture list (rejects `[this]`
  and `[&]`), `.detach()` call, fallback retention, and the
  pre-escalation cheap reap remaining inline.

- **GlobalShortcutsPortal closes its session on destruction
  (`GlobalShortcutsPortal::~GlobalShortcutsPortal`).** Was: no
  destructor at all — the session handle returned by
  `CreateSession` leaked for the lifetime of the D-Bus client.
  `xdg-desktop-portal` accumulated one orphan session per Ants
  invocation that crashed or was SIGKILLed before the QObject
  parent-tree cleanup could implicitly close the bus connection;
  visible via `busctl --user introspect
  org.freedesktop.portal.Desktop ...`. Now: the destructor
  issues an asynchronous
  `org.freedesktop.portal.Session.Close` call against
  `m_sessionHandle` when non-empty (early-returns on empty
  handle so X11 / GNOME / no-portal paths don't crash Qt's
  D-Bus marshaller). New `kSessionIface` constant in the
  anonymous namespace alongside the existing
  service/path/interface constants. Locked by
  `tests/features/portal_session_close/` — 8 invariants on
  header dtor declaration with `override`, `kSessionIface`
  constant, empty-handle early return, and the
  `asyncCall(createMethodCall(..., kSessionIface, "Close"))`
  dispatch.

- **Plugin manifest cap + canonical plugin path
  (`PluginManager::scanAndLoad`).** Two latent issues addressed
  together. **Manifest cap:** `f.readAll()` was unbounded — a
  multi-GB `manifest.json` (corrupt disk, malicious tarball)
  would allocate that much RAM before `QJsonDocument::fromJson`
  got a chance to reject it. Now: `f.read(kMaxManifestBytes)`
  with `kMaxManifestBytes = 1024 * 1024` (1 MiB ≈ 250 plugins
  worth of permission/description text — real manifests are
  <10 KiB, so the cap never bites legitimate content). Files
  larger than the cap log a warning and skip without parsing.
  **Canonical plugin path:** the scan now anchors on
  `QFileInfo(m_pluginDir).canonicalFilePath()`, passes
  `QDir::NoSymLinks` to `entryList` (cheap first-pass filter),
  and per-entry verifies the resolved path is anchored inside
  the canonical root via
  `startsWith(canonicalRoot + "/")`. Closes the symlink-escape
  shape where a hostile plugin tarball containing
  `evil -> /etc/cron.daily` could trick the loader into
  attempting `init.lua` from outside the user's plugin tree.
  Locked by `tests/features/plugin_manifest_safety/` — 12
  invariants on the cap (named constant + value + bounded read
  + no readAll-feeding-fromJson regression), NoSymLinks flag,
  canonical anchor, per-entry containment check, and the
  reject-warning message text.

## [0.7.32] — 2026-04-25

**Theme:** Dialog UX. Three bundle items from the Settings dialog
ROADMAP list (dependency-UI gating, Cancel rollback for Profiles,
Restore Defaults per-tab) plus one user feedback item from
2026-04-25 — the Review Changes dialog only surfaced current-
branch state and missed work living on other branches.

### Added

- **Restore Defaults button per primary Settings tab
  (`SettingsDialog::setupGeneralTab`, `setupAppearanceTab`,
  `setupTerminalTab`, `setupAiTab`).** Was: only Keybindings had
  a defaults-reset button. A user who tweaked the dialog and
  wanted to start over had to either remember every default or
  delete `~/.config/ants-terminal/config.json` (which lost
  unrelated settings: highlight rules, profiles, plugin grants).
  Now: each substantive tab has its own button with stable
  `objectName` (`restoreDefaultsGeneral`, `restoreDefaultsAppearance`,
  `restoreDefaultsTerminal`, `restoreDefaultsAi`). Reset slots
  mutate widget state only — applySettings still commits, Cancel
  still rolls back. Schema defaults match the second argument of
  each `Config::xxx()` getter so the dialog and config layer can't
  drift. Locked by `tests/features/settings_restore_defaults/`
  — 22 invariants on objectNames, reset-value coverage per tab,
  and the "no `m_config->` writes from within reset slots" rule.

- **Review Changes dialog: live updates via QFileSystemWatcher +
  manual Refresh button (`MainWindow::showDiffViewer`).** User
  feedback 2026-04-25: dialog should show live or near-live
  updates. Was: probes ran once on dialog open and never again —
  the user had to close and re-open to see changes from a commit
  / fetch / branch operation done in the terminal underneath. Now:
  a `QFileSystemWatcher` watches `cwd`, `.git`, `.git/HEAD`,
  `.git/index`, `.git/refs/heads`, `.git/refs/remotes`,
  `.git/logs/HEAD`. `fileChanged` and `directoryChanged` signals
  feed a 300 ms single-shot `QTimer` (debounce — `git pull` /
  `git fetch` fire fileChanged O(refs) times in milliseconds; the
  debounce coalesces them into one re-probe). The probe-spawning
  logic was refactored into a `runProbes` lambda that constructs a
  fresh `ProbeState` per call, so an in-flight probe whose finalize
  outlives the next refresh can't decrement the new pending counter
  and render a half-populated mix. Atomic-rename safe: the
  fs-event handler re-adds paths that exist but are no longer
  watched (Qt loses the watch on `rename(2)`, which git uses for
  HEAD/index/logs/HEAD updates). A live-status label
  (`reviewLiveStatus` objectName) shows "● refreshing…" /
  "● live — auto-refresh on git changes" so the user can confirm
  the watcher is wired. A manual Refresh button
  (`reviewRefreshBtn` objectName) bypasses the debounce for cases
  where state changed outside the watched paths (a build script
  in another shell, a different terminal). Locked by
  `tests/features/review_changes_branches/` (extended) — now 33
  invariants total covering ProbeState fields, runAsync targets,
  finalizer rendering, copy-handler payload, empty-state guard,
  runProbes lambda, watcher armament, debounce timing, atomic-
  rename re-watch, refresh-button bypass, and live-status label
  states.

- **Review Changes dialog: per-branch summary + cross-branch
  unpushed commits (`MainWindow::showDiffViewer`,
  `MainWindow::ProbeState`).** Was: three async git probes
  (`status`, `diff HEAD`, `log @{u}..HEAD`) all scoped to the
  current branch's working tree and HEAD lineage. A user with
  five feature branches each holding unpushed work saw "no
  unpushed" if they happened to be on a clean branch. Branches
  without upstreams or with diverged ahead/behind state were
  invisible. User feedback 2026-04-25: "the Review Changes
  dialog doesn't consider changes in other branches of the
  project." Now: two additional probes drop in alongside the
  existing three —
  `git for-each-ref refs/heads --format='%(refname:short)
   \t%(upstream:short)\t%(upstream:track)\t%(subject)'` for the
  per-branch summary (with ahead/behind/gone/no-upstream colour
  cues), and `git log --branches --not --remotes --oneline
   --decorate` for the cross-branch unpushed log (every commit
  reachable from any local branch but not from any remote-
  tracking branch). Both are O(refs) and finish in milliseconds.
  Copy Diff includes both new sections. Locked by
  `tests/features/review_changes_branches/` — 15 invariants on
  ProbeState fields, runAsync targets, finalizer rendering,
  copy-handler payload, and empty-state guards.

### Changed

- **Tab close button (×) is always visible, not hover-only
  (`MainWindow::applyTheme` stylesheet —
  `QTabBar::close-button`).** User feedback 2026-04-25: "the
  tabs still don't have a visible marker per tab that shows
  where to click to close the tab. The mouseover works but we
  need to also see it when onmouseout." The 0.6.27 fix removed
  `image: none` to let Qt fall back to the platform's standard
  close icon — that worked on Breeze/Adwaita but failed on
  Fusion / qt6ct / certain Plasma colour schemes where the
  platform style still rendered the × hover-only. Now: explicit
  data-URI SVG `image: url("data:image/svg+xml;...")` rules in
  both the default and `:hover` `QTabBar::close-button` variants.
  Glyph re-tints with the active theme via `textSecondary`
  (default) / `textPrimary` (hover); hover keeps the ansi-red
  `background-color` will-click cue. URL-encoded `%23` is
  spliced into the arg list via `QStringLiteral("%23") +
  theme.<color>.name().mid(1)` rather than the format string —
  prevents Qt's CSS parser from truncating the data URI at the
  fragment delimiter and also avoids the QString::arg()
  placeholder-numbering collision. Locked by
  `tests/features/tab_close_button_visible/` — 11 invariants on
  data-URI presence, two-line × shape, arg-side splice, and
  image-rule presence in BOTH state variants.

- **Dependency-UI enable gating (`SettingsDialog::setupAppearanceTab`,
  `setupTerminalTab`, `setupAiTab`).** Was: master checkboxes
  (`m_aiEnabled`, `m_autoColorScheme`, `m_quakeMode`) gated logic
  but not UI — typing an API key into a feature-disabled AI tab,
  or selecting "Solarized" as the light-mode theme while
  auto-switch was off, both produced silent no-ops. Now:
  `QCheckBox::toggled` is wired to `setEnabled` on every
  dependent sibling, with a one-shot sync call at construction
  so the initial state matches the loaded config without
  relying on `setChecked()` always emitting `toggled` (it only
  emits when state actually changes). Disabled controls keep
  their current values, so toggling the master back on restores
  the user's prior selection rather than zeroing it out. Locked
  by `tests/features/settings_dependency_gating/` — 16
  invariants on the three sync lambdas, dependent setEnabled
  calls, toggled-connect wiring, and initial-sync call sites.

- **Profiles tab honors Cancel/OK semantics
  (`SettingsDialog::setupProfilesTab`, `loadSettings`,
  `applySettings`, `m_pendingProfiles`, `m_pendingActiveProfile`).**
  Was: profile Save/Delete/Load buttons mutated `m_config`
  immediately via `setProfiles()` / `setActiveProfile()`. Cancel
  could not roll those edits back — they had already been
  persisted to config.json before the user's intent was known.
  Every other Settings tab follows the standard "stage in
  widgets, commit on applySettings, discard on reject" pattern;
  Profiles broke that contract. Now: the three buttons mutate a
  pending-state pair (`m_pendingProfiles` + `m_pendingActiveProfile`),
  loadSettings re-initializes the pair from `m_config`, and
  applySettings is the single commit point that calls
  `setProfiles` / `setActiveProfile`. Cancel skips applySettings,
  so dialog close leaves m_config unchanged. Locked by
  `tests/features/settings_profile_cancel_rollback/` — 11
  invariants including a global "exactly one m_config->setProfiles
  call site" check that catches a regression where a button
  callback starts writing to m_config directly again.

## [0.7.31] — 2026-04-25

**Theme:** Persistence integrity (cross-file). Four items from the
post-0.7.30 grouping plan, addressing the silent-data-loss /
permission-drift / concurrent-writer surfaces that span Config,
ClaudeAllowlist, SessionManager, and SettingsDialog. Splits the
growing `secureio.h` into `secureio.h` (perms) + `configbackup.h`
(rotation + cooperative write lock) before a third helper landed.

### Added

- **`ConfigWriteLock` cooperative write lock for shared config files
  (`src/configbackup.h`).** Was: two simultaneously-running Ants
  instances saving the same `~/.config/ants-terminal/config.json`
  raced over `<path>.tmp` + `rename(2)` — both writers truncated the
  shared tmp, partial bytes interleaved on the same inode, and
  last-rename-wins silently dropped one process's keystrokes /
  settings / profile / AI key. Same shape for `~/.claude/settings.json`
  where Allowlist + Install-hooks + git-context-installer all
  read-modify-write with no serialization. Now: an RAII guard wraps
  POSIX `flock(2)` on a sibling `<path>.lock` file with a 5-second
  poll deadline (100 × 50 ms), advisory so cooperating callers
  serialize while non-cooperating editors (vim, jq) bypass by
  design. `Config::save`, `ClaudeAllowlistDialog::saveSettings`,
  `SettingsDialog::installClaudeHooks`, and
  `SettingsDialog::installClaudeGitContextHook` now construct
  `ConfigWriteLock writeLock(path)` with an `acquired()` guard
  before the write block; failure to acquire logs and returns
  rather than risking the data race. Locked by
  `tests/features/concurrent_writer_lock/` (5 runtime invariants
  via fork(2) so flock semantics are honestly tested across
  processes, plus 7 source-grep invariants on each save site).
  Pre-fix source fails 7; post-fix all 12 pass.

- **Belt-and-suspenders post-rename `setOwnerOnlyPerms` at every
  persistence site (`Config::save`, `SessionManager::saveSession`,
  `SessionManager::saveTabOrder`, `SettingsDialog::installClaudeHooks`,
  `SettingsDialog::installClaudeGitContextHook`).** Was: each site
  set 0600 on the temp fd before write but relied on `rename(2)`
  preserving perms across the swap. ext4/xfs/btrfs honor that, but
  FAT/exFAT on removable media (no POSIX bits at all), some
  SMB/NFS servers (server-side rename applies server umask), and
  Qt's copy+unlink fallback (cross-device rename, exotic mounts —
  copy creates the destination with the *process* umask) do not.
  Files containing `ai_api_key` (config.json), Claude Code bearer
  tokens (settings.json), or paste-buffer scrollback content
  (session_*.dat) could land 0644 on those filesystems and leak to
  every UID on the host. Now: each site re-chmods the **final
  inode** after `rename`/`commit()` returns success — idempotent on
  POSIX filesystems, essential elsewhere. Locked by
  `tests/features/persistence_post_rename_chmod/` (10 source-grep
  invariants spanning config.cpp / sessionmanager.cpp /
  settingsdialog.cpp). Pre-fix source fails 6; post-fix all 10
  pass.

- **`secureio.h` / `configbackup.h` split.** `secureio.h` is now
  perms-only — `setOwnerOnlyPerms(QFileDevice&)` and
  `setOwnerOnlyPerms(const QString&)`, the original 0600-bitmask
  helpers that ~12 callers reach for. `configbackup.h` is the new
  home for `rotateCorruptFileAside` (silent-data-loss recovery,
  added 0.7.12) and the new `ConfigWriteLock` (concurrent-writer
  guard, added this release). The split preempts a third-helper
  cliff: the file was straddling perms + recovery + lock concerns
  and a fourth would have made the include cost-of-business across
  the codebase. Each downstream caller now picks the include it
  actually needs (`config.cpp`, `claudeallowlist.cpp`,
  `settingsdialog.cpp` pull both; sites that only set perms keep
  their `secureio.h` include). Locked by
  `tests/features/secureio_configbackup_split/` (13 invariants:
  file-content boundaries, non-copyable lock, every caller's
  include set). Pre-fix source fails 4; post-fix all 13 pass.

### Changed

- **Behavioural test coverage extended to the parse-failure mirror
  sites in ClaudeAllowlist + SettingsDialog.** The 0.7.12
  silent-data-loss-on-parse-failure refuse pattern shipped in
  three sites — `Config::load`, `ClaudeAllowlistDialog::saveSettings`,
  and `SettingsDialog::install{ClaudeHooks,ClaudeGitContextHook}` —
  but only Config had a regression test
  (`tests/features/config_parse_failure_guard/`). The other two
  were locked only by adjacent grep-style asserts inside other
  feature tests. Now they have their own dedicated test:
  `tests/features/settings_parse_failure_mirror/` (8 invariants:
  rotation call site, return-false-after-rotation gating,
  open-failure branch distinct from parse-failure branch, comment
  anchors explaining the clobber-risk reasoning). Closes the
  ROADMAP "Allowlist + settings-dialog feature-test analogs" item.
  Pre-fix source already passes — these tests lock previously
  shipped behaviour against future regressions, not catch a new
  bug.

## [0.7.30] — 2026-04-25

**Theme:** Session-file integrity. Three ROADMAP § 0.7.12 Tier 2 items
shipped together from the post-0.7.27 grouping plan, all in
`sessionmanager.cpp`: a SHA-256 envelope around the qCompress payload,
a pre-flight on qCompress's 4-byte uncompressed-length prefix, and
per-cell `QDataStream::status()` checks inside the decode loops.

### Added

- **V4 SHA-256 envelope around session-file payload
  (`SessionManager::serialize`, `SessionManager::restore`,
  `SessionManager::ENVELOPE_MAGIC`,
  `SessionManager::ENVELOPE_VERSION`).** Was: session files at
  `$XDG_DATA_HOME/ants-terminal/sessions/session_<tabId>.dat` were
  raw `qCompress` output with no payload integrity. Anyone with
  write access to that directory (the user's own UID, a compromised
  local process, a runaway `pip install` post-exec, an npm
  dependency) could plant a crafted session that fed arbitrary
  codepoints, fg/bg colors, and attribute flags into the grid on
  next restore — a render surface, not a sandbox. Now: serialize
  wraps the qCompress output in a V4 envelope
  `[SHEC magic (0x53484543)][envelope version=1][SHA-256 of payload
  (32 bytes)][payload length (uint32)][compressed payload]`; restore
  peeks the magic, verifies the hash, and refuses to restore on
  version mismatch, length disagreement, or hash mismatch. Inner
  `QDataStream` format unchanged (still V3) — the integrity layer is
  framing-only. Legacy V1-V3 files (no envelope) continue to load
  via the magic-peek fall-through; their next save writes them out
  as V4 organically. Regression test
  `tests/features/session_sha256_checksum` locks four invariants —
  serialize emits the envelope (INV-1), restore peeks the magic and
  verifies the hash with a return-false on mismatch (INV-2),
  ENVELOPE_MAGIC/ENVELOPE_VERSION declared on `SessionManager` with
  the agreed-upon magic value (INV-3), envelope version remains 1
  at this milestone (INV-4). Pre-fix source fails eight invariant
  assertions; post-fix all four pass. ROADMAP § 0.7.12 Tier 2 entry
  retired.

- **`qUncompress` length-prefix pre-flight
  (`SessionManager::restore`, `SessionManager::MAX_UNCOMPRESSED`).**
  Was: a crafted file claiming 500 MB uncompressed in qCompress's
  4-byte big-endian prefix triggered a 500 MB allocation that the
  post-hoc `raw.size() > 500MB` cap could only catch *after* the
  damage was done — and the on-disk payload could be tiny, so the
  allocation pressure showed up with no concomitant disk-space
  anomaly. Now: restore reads the first 4 bytes of the compressed
  payload, reconstructs the big-endian uint32, and rejects any
  claim above `MAX_UNCOMPRESSED` (500 MB) BEFORE qUncompress runs —
  constant-time, no allocator pressure. Short-payload guard
  (`compressed.size() < 4`) keeps the same path safe against
  truncated inputs that can't carry a length prefix at all. The
  post-decompression cap remains as a defense-in-depth backstop
  against payloads that under-claim and over-deliver. Regression
  test `tests/features/session_qcompress_length_guard` locks four
  invariants — pre-flight reconstruction precedes qUncompress
  (INV-1, INV-3), `MAX_UNCOMPRESSED` is the named constant (INV-2),
  short-payload guard exists (INV-4). Pre-fix source fails four
  invariant assertions; post-fix all four pass. ROADMAP § 0.7.12
  Tier 2 entry retired.

### Changed

- **`QDataStream::status()` checks inside cell-decode loops
  (`SessionManager::restore` `readCell` / `readCombining`).** Was:
  the `readCell` lambda was void; a stream truncated mid-cell still
  flowed default-constructed `QRgb` and `uint8_t` values through
  `QColor::fromRgba` etc. into the cell, silently writing
  uninitialized fg/bg/flags into the grid. The surrounding loops
  didn't check per-iteration status either, so the grid kept
  accepting cells from a stream already at `ReadPastEnd`. Now:
  `readCell` returns `bool` and short-circuits on `in.status() !=
  QDataStream::Ok`; every call site (scrollback cells, screen cells
  in range, screen cells skipped on width shrink, screen cells
  skipped on height shrink) is guarded by `if (!readCell(...))
  return false`. The combining-character helper checks status after
  each codepoint read, so a stream truncated mid-codepoint can't
  push default-constructed `0` into the combining map either. Pre-
  fix, a partial save (kernel crash mid-fsync, disk-full mid-write,
  hostile sender truncating the envelope payload) could materialize
  as garbage cells in the next restore — not a crash, but a
  corrupted scrollback. Regression test
  `tests/features/session_cell_loop_stream_status` locks three
  invariants — `readCell` returns bool with status check (INV-1),
  every call site uses `if (!readCell(...))` and at least four
  sites exist (INV-2), `readCombining` inner codepoint loop checks
  status (INV-3). Pre-fix source fails three invariant assertions;
  post-fix all three pass. ROADMAP § 0.7.12 Tier 2 entry retired.

## [0.7.29] — 2026-04-25

**Theme:** Audit pipeline II — output quality. Three ROADMAP § 0.7.12
items shipped together from the post-0.7.27 grouping plan, all in
`auditdialog.cpp`: SARIF v2.1.0 `result.suppressions[]` array, a
regex-DoS watchdog on user-supplied patterns, and a widened 96-bit
dedup key.

### Added

- **SARIF v2.1.0 `result.suppressions[]` array
  (`AuditDialog::exportSarif`, `AuditDialog::loadSuppressions`,
  `AuditDialog::saveSuppression`, `m_suppressionReasons` map,
  `Finding::suppressed`).** Was: SARIF export silently dropped
  suppressed findings (those whose dedup key matched
  `~/.audit_suppress`), producing a falsely-clean report that
  defeated the suppression-trend telemetry already computed for
  the in-app dashboard. Now: a parallel
  `QHash<QString, QString> m_suppressionReasons` map populated
  alongside `m_suppressedKeys` carries the user's free-text
  reason from the JSONL file into memory; the parse pipeline
  marks suppressed findings with `Finding::suppressed = true`
  instead of dropping them; render paths (UI, HTML, plain-text)
  continue to filter via `isSuppressed`; the SARIF export iterates
  ALL findings and attaches a `suppressions[]` block (kind:
  `external`, state: `accepted`, justification: the user's reason)
  per SARIF v2.1.0 §3.34. GitHub Code Scanning / SonarQube /
  VSCode SARIF Viewer now see the full audit picture and can
  compute fires-vs-suppressions ratios across export boundaries.
  Regression test `tests/features/audit_sarif_suppressions` locks
  five invariants — map declaration (INV-1), `loadSuppressions`
  populates + clears (INV-2), `saveSuppression` mirrors (INV-3),
  exportSarif emits the suppressions JSON property with
  `external`/`accepted` fields (INV-4), exportSarif no longer
  drops on `isSuppressed` (INV-5). Pre-fix source fails seven
  invariant assertions; post-fix all five pass. ROADMAP § 0.7.12
  Tier 2 entry retired.

- **Regex-DoS watchdog on user-supplied audit patterns
  (`AuditDialog::isCatastrophicRegex`,
  `AuditDialog::hardenUserRegex`, `applyFilter`,
  `loadAllowlist`).** Was: user patterns from
  `audit_rules.json` (`OutputFilter::dropIfMatches`) and
  `.audit_allowlist.json` (`AllowlistEntry::lineRegex`) flowed
  straight into `QRegularExpression` with no shape check and no
  match-time bound. A pathological pattern committed in either
  file could pin the GUI thread for seconds with classic
  catastrophic backtracking on adversarial scanner output —
  `(.+)+`, `(\w*)*`, `(.*)+` against long lines. Now: a static
  `isCatastrophicRegex` heuristic rejects nested-quantifier
  shapes (a quantified group whose body itself contains a
  quantifier) at compile time with a `qWarning` naming the
  offending pattern; the rule continues to run without the
  filter rather than refusing to load. Patterns that pass the
  shape check are wrapped in PCRE2's `(*LIMIT_MATCH=100000)`
  inline option via `hardenUserRegex` so even catastrophic
  shapes that slip past the heuristic have a bounded match-step
  budget — PCRE2 returns "no match" on overrun (fail-safe). 100
  k steps handles every sane pattern (typical match completes
  in < 1 k) and aborts adversarial patterns within
  milliseconds. Regression test
  `tests/features/audit_regex_dos_watchdog` locks four invariants
  — the helper exists and is invoked at both user-pattern entry
  points (INV-1), it recognizes nested-quantifier shapes
  (INV-2), the `LIMIT_MATCH=N` cap is in the [1k, 1M] sane
  range (INV-3), `loadAllowlist` emits a qWarning on rejection
  (INV-4). Pre-fix source fails four invariants; post-fix all
  four pass. ROADMAP § 0.7.12 Tier 2 entry retired.

### Changed

- **Widen `computeDedup` from 64 to 96 bits
  (`computeDedup`, `AuditDialog::isSuppressed`,
  `Finding::dedupKey`).** Was: SHA-256 truncated to 16 hex chars
  (64 bits) — same key serves as the SARIF `partialFingerprint`,
  the suppression-JSONL key, the rule-quality bucket, and the
  in-app "Suppress" anchor URL. The 64-bit collision threshold
  (~2³² entries at 50 % collision probability) was comfortable
  but tight given the multi-role usage; a single false-collision
  cost a wrong-finding suppression. Now: `.left(24)` (96 bits)
  raises the birthday threshold to ~2⁴⁸ for 8 extra bytes per
  stored key — well past any plausible project's lifetime
  collection. A new `bool AuditDialog::isSuppressed(const QString
  &dedupKey) const` helper encapsulates a backward-compat lookup:
  match either the full 24-char key OR the leading 16-char prefix,
  so existing pre-0.7.29 user `~/.audit_suppress` entries
  continue to suppress new 24-char findings without forcing a
  migration. Six render-pipeline call sites that previously read
  `m_suppressedKeys.contains(f.dedupKey)` now route through the
  helper. Regression test `tests/features/audit_dedup_96bit`
  locks four invariants — width ≥ 24 hex chars (INV-1),
  isSuppressed exists with `.left(16)` legacy path (INV-2),
  zero raw `m_suppressedKeys.contains(f.dedupKey)` sites
  remain (INV-3), `saveSuppression` mirrors into
  `m_suppressionReasons` (INV-4). Pre-fix source fails five
  invariant assertions; post-fix all four pass.

## [0.7.28] — 2026-04-25

**Theme:** Audit pipeline I — process-side robustness. Three ROADMAP
§ 0.7.12 Tier 2 items shipped together as bundle 0.7.28 from the
post-0.7.27 grouping plan: per-tool timeout overrides, incremental
QProcess output drain with a 64 MiB cap, and a distinct tool-crash
warning that no longer hides as "0 findings."

### Changed

- **`AuditCheck::timeoutMs` field — per-check QProcess timeout
  (`AuditCheck`, `AuditDialog::runNextCheck`, ctor lambda).** Was:
  `m_timeout->start(30000)` hard-coded inside `runNextCheck`, with
  the timeout-handler lambda printing a literal `"Timed out (30s)"`
  warning. Slow tools (cppcheck on a 500k-line tree, semgrep with
  rule-pack compile, osv-scanner rate-limited by OSV.dev,
  trufflehog over the full git history, clang-tidy / clazy on
  Qt-heavy code) routinely exceeded the 30 s cap and got demoted
  to tool-health warnings instead of producing findings. Now: a new
  `int timeoutMs = 30000;` trailing field on the `AuditCheck`
  aggregate (default preserves the pre-fix global so positional
  call sites stay correct), `m_timeout->start(check.timeoutMs)` at
  the use site, and the timeout-warning string formatted from the
  actual cap. `populateChecks` ends with a calibration loop that
  bumps known-slow tool IDs to 60 s (`cppcheck`, `cppcheck_unused`,
  `clang_tidy`, `clazy`), 90 s (`semgrep`), or 120 s (`osv_scanner`,
  `trufflehog`). Regression test `tests/features/audit_per_tool_timeout`
  locks four invariants — the field declaration (INV-1), the
  per-check use site with no hard-coded `30000` (INV-2), the
  parameterised warning message (INV-3), at least one calibration
  override above 30 s (INV-4). Pre-fix source fails all four;
  post-fix all four pass. ROADMAP § 0.7.12 Tier 2 entry retired.

- **Incremental QProcess output drain with 64 MiB cap
  (`AuditDialog::onCheckOutputReady`, `AuditDialog::onCheckErrorReady`,
  `AuditDialog::onCheckFinished`).** Was: `m_process->readAllStandardOutput()`
  called exactly once inside `onCheckFinished`. Until that moment,
  `QProcess` accumulated internal buffers without bound — a runaway
  `semgrep` against generated code or a buggy plugin emitting a
  tight `printf` loop could buffer hundreds of megabytes before the
  timeout fired, with the audit dialog showing a frozen progress
  bar the whole time. Now: the constructor wires
  `readyReadStandardOutput` and `readyReadStandardError` to new
  `onCheckOutputReady` / `onCheckErrorReady` slots that append
  incrementally to `m_currentOutput` / `m_currentError`; if the
  combined size exceeds `MAX_TOOL_OUTPUT_BYTES = 64 * 1024 * 1024`,
  the process is killed and `m_outputOverflowed` is flagged. On
  `finished()` the runner drains any tail data (Qt may buffer
  between the last `readyRead` and `finished()`), reads from the
  member buffers instead of the live process, and surfaces a
  distinct "Output exceeded N MiB cap" warning when overflow
  occurred. Buffers reset before each check so output never
  concatenates across checks. A small `connectProcessSignals()`
  helper centralises the three connections so the timeout-kill /
  cancel-kill / reconnect cycles never lose a drain slot.
  Regression test `tests/features/audit_incremental_output_drain`
  locks six invariants — both `readyRead*` connections (INV-1),
  buffer members declared (INV-2), `MAX_TOOL_OUTPUT_BYTES` cap in
  the 4 MiB ≤ cap < 1 GiB defensible range (INV-3), per-check
  reset (INV-4), `onCheckFinished` reads buffers not the live
  process (INV-5), overflow path kills the process (INV-6).
  Pre-fix source fails on the connect-grep alone; post-fix all six
  pass. ROADMAP § 0.7.12 Tier 2 entry retired.

### Fixed

- **Tool-crash distinct from "no findings"
  (`AuditDialog::onCheckFinished`).** Was: signature
  `void AuditDialog::onCheckFinished(int /*exitCode*/,
  QProcess::ExitStatus /*status*/)` — both parameters discarded
  via comments. A tool that segfaulted with empty stdout was
  indistinguishable from a clean run with zero findings, silently
  hiding both a real bug in the tool AND any findings the tool
  would have reported. Same went for non-zero-exit-with-stderr-only
  patterns (clang-tidy missing `compile_commands.json`, semgrep
  failing to parse a rule). Now: parameters named (`exitCode`,
  `exitStatus`); function branches on `QProcess::CrashExit` to
  emit a "Tool crashed (signal exit)" warning; on `exitCode != 0
  && stdout empty && stderr non-empty` to emit a "Tool exited N
  with no findings on stdout" warning. Both warnings demoted to
  `Severity::Info` so they don't sort to the top of the report
  next to real findings. The four exit modes (timeout, overflow,
  crash, non-zero-with-stderr-only) all funnel through a single
  small file-scope `makeToolHealthWarning()` helper that
  centralises the row shape (Info severity, warning flag, distinct
  message prefix). Regression test
  `tests/features/audit_tool_crash_distinct` locks four invariants
  — named parameters (INV-1), `CrashExit` branch (INV-2), warning
  emission with crash/exit-message (INV-3), `Severity::Info`
  demotion (INV-4). Pre-fix source fails all four; post-fix all
  four pass. ROADMAP § 0.7.12 Tier 2 entry retired.

## [0.7.27] — 2026-04-25

**Theme:** PTY-handler hardening sweep. Two ROADMAP § 0.7.12 Tier 2
items shipped together — child-side FD closure no longer relies on
a hard-coded `fd<1024` cap, and master-side writes no longer drop
data on EAGAIN.

### Security

- **PTY child uses `close_range(2)` with RLIMIT_NOFILE-bounded
  fallback (`Pty::start`).** Was: `for (int fd = 3; fd < 1024;
  ++fd) ::close(fd)` — silently leaked any inherited FD with index
  ≥ 1024 into the user's shell on systemd-service / container /
  hardened-server profiles where `rlim_cur` sits above the
  hard-coded ceiling. Qt's display socket, D-Bus session
  connection, plugin HTTP sockets, Lua VM eventfds, and the
  `remote-control` IPC socket are all valid leak candidates; a
  leaked AI HTTP socket is a credentials-exfiltration vector,
  a leaked D-Bus socket lets the shell impersonate the user's
  desktop session, a leaked remote-control socket is a UID-scope
  RCE-by-proxy vector. Now: post-fork child branch issues
  `::syscall(SYS_close_range, 3, ~0U, 0)` first — single
  signal-safe syscall on Linux 5.9+, atomic over the whole
  range, ignores the soft cap. If the syscall returns non-zero
  (kernel < 5.9, missing build-time `SYS_close_range`), the
  fallback path consults `getrlimit(RLIMIT_NOFILE)` and iterates
  up to the runtime soft cap, capped at 65536 to bound the
  worst-case syscall storm on profiles where `rlim_cur` is in
  the hundreds of thousands. Two new headers
  (`<sys/resource.h>`, `<sys/syscall.h>`) added to
  `ptyhandler.cpp`. Regression test
  `tests/features/pty_closefrom` locks five invariants —
  `SYS_close_range` referenced inside the post-fork child
  branch (INV-1), the hard-coded `fd<1024` loop is gone (INV-2),
  fallback consults `RLIMIT_NOFILE` (INV-3), required headers
  included (INV-4), fallback bound is capped to 65536 to avoid
  the unbounded-loop pathology (INV-5). Pre-fix source fails 4
  of 5; post-fix all 5 pass. CWE-403 reference; ROADMAP § 0.7.12
  Tier 2 entry retired.

### Fixed

- **`Pty::write` queues on EAGAIN instead of dropping bytes
  (`Pty::write`, new `Pty::onWriteReady`).** Was: the master FD
  is non-blocking, so a slow consumer on the slave side could fill
  the kernel PTY buffer; on EAGAIN the write loop's else-clause
  broke out with the comment `// EAGAIN or fatal error`, silently
  dropping the unwritten remainder. Behaviourally invisible during
  normal interactive use (the kernel buffer drains within
  microseconds) but provoked by realistic bursts — large pastes,
  AI-dialog command insertions, plugin-driven keystroke floods,
  or any write into a slave whose reader is suspended. Now: a new
  `m_pendingWrite` byte buffer and a `QSocketNotifier(
  QSocketNotifier::Write)` on `m_masterFd` (initially disabled
  because PTY masters are writable nearly continuously and a
  hot notifier would burn CPU). On EAGAIN the unwritten remainder
  is moved to the queue and the notifier is enabled; when the
  kernel signals writability, `onWriteReady` drains the queue and
  disables the notifier on completion. Fresh `write()` calls
  arriving while the queue is non-empty append rather than
  bypass, preserving FIFO ordering so newer keystrokes never race
  ahead of older ones. Queue capped at 4 MiB
  (`MAX_PENDING_WRITE_BYTES`) — large enough for realistic bursts,
  small enough that a permanently-stuck slave cannot OOM the GUI
  process. Regression test `tests/features/pty_write_eagain_queue`
  locks seven invariants — write-side notifier creation (INV-1),
  queue member declared (INV-2), notifier pointer member declared
  (INV-3), `onWriteReady` slot declared and connected (INV-4),
  EAGAIN handled distinctly inside `Pty::write` (INV-5), 4 MiB
  cap present (INV-6), direct-write path checks queue first for
  FIFO ordering (INV-7). Pre-fix source fails 5 of 7; post-fix
  all 7 pass. ROADMAP § 0.7.12 Tier 2 entry retired.

## [0.7.26] — 2026-04-25

**Theme:** menubar opacity — the actual root-cause fix. 0.7.25's
palette + widget-local-QSS belt-and-suspenders did not in fact
suspend any belt: the user reported the desktop wallpaper still
showing through the menubar strip on KWin + Breeze + Qt 6.

### Fixed

- **Menubar background now actually paints opaquely under
  `WA_TranslucentBackground` (`OpaqueMenuBar`, `MainWindow::applyTheme`).**
  User report 2026-04-25: "I can clearly see my desktop background
  behind it." Root cause: under `WA_TranslucentBackground` parent +
  `WA_OpaquePaintEvent` on the menubar, **none** of the conventional
  opaque-paint paths runs reliably on every WM/style stack. Specifically:
  `autoFillBackground` is suppressed by `WA_OpaquePaintEvent` (the
  contract is "the widget paints all pixels"), `QPalette::Window` is
  only consulted by `autoFillBackground` and inherits the suppression,
  and QSS `QMenuBar { background-color: … }` — which is supposed to
  draw via `QStyleSheetStyle::drawControl(CE_MenuBarEmptyArea)` —
  is silently skipped on KWin + Breeze + Qt 6 when
  `WA_OpaquePaintEvent` is set, because the QSS engine assumes the
  widget owns those pixels. Net effect: every safeguard 0.7.25 added
  was correctly installed and not painting anything; the menubar
  surface stayed cleared-to-transparent and the compositor showed the
  wallpaper through. New file `src/opaquemenubar.h` defines
  `OpaqueMenuBar`, a `QMenuBar` subclass whose `paintEvent` runs
  `QPainter::fillRect(rect(), m_bg)` with `CompositionMode_Source`
  *before* delegating to `QMenuBar::paintEvent`. That is the only
  path that actually keeps the `WA_OpaquePaintEvent` contract honest
  under translucent parents. `m_menuBar` is now an `OpaqueMenuBar` and
  `applyTheme` sets the fill colour via `setBackgroundFill(theme.bgSecondary)`.
  The 0.7.25 palette + widget-local QSS calls are kept, no longer load-
  bearing for the strip's opacity but useful for child-widget theme
  propagation and for scoping the `::item :hover/:selected/:pressed`
  rules on the menubar itself rather than relying on the top-level
  cascade. Regression test `tests/features/menubar_hover_stylesheet`
  extended with INV-8 — three new assertions that pin the
  `OpaqueMenuBar` construction site, the `setBackgroundFill` call in
  `applyTheme`, and the presence of `paintEvent` + `fillRect` inside
  `src/opaquemenubar.h`. The full spec is rewritten with the
  per-iteration history (0.6.42 → 0.7.26) and an explicit "manual
  verification" recipe (bright wallpaper + dark `bgSecondary` +
  ~0.85 opacity on KWin) so any future drift is reproducible by hand.

## [0.7.25] — 2026-04-24

**Theme:** memory-DoS hardening on OSC 8 hyperlinks and Kitty APC
graphics chunks, plus a menubar-opacity fix reported mid-session.

### Security

- **OSC 8 URI cap + Kitty APC chunk-buffer cap
  (`TerminalGrid`).** Two attacker-controlled accumulators used to be
  bounded only by the VT parser's per-envelope 10 MB ceiling, with no
  downstream ceiling of their own. A hostile program could emit one
  `\x1b]8;;<10MB-URI>\x07` sequence per scrollback line and wedge
  tens of GB of URI bytes into per-row `HyperlinkSpan` copies +
  scrollback; or keep sending `\x1b_G...,m=1,...\x07` frames without
  ever closing with `m=0`, growing `m_kittyChunkBuffer` without
  bound. `src/terminalgrid.h` now declares
  `MAX_OSC8_URI_BYTES = 2048` (real URLs are < 2 KiB; abuse is not)
  and `MAX_KITTY_CHUNK_BYTES = 32 MiB` (larger than any realistic
  chunked image upload — Kitty itself transmits in ~4 KiB frames).
  The OSC 8 open branch rejects oversized URIs down the same drop
  path as the existing invalid-scheme rejection — following text
  prints unlinked. The APC chunk path clears and `shrink_to_fit`s
  the staging buffer on cap breach, so a subsequent `m=0` sees an
  empty buffer rather than attacker-prepended garbage. Regression
  test `tests/features/osc8_apc_memory_caps` locks five invariants —
  OSC 8 happy path, oversized-drop, at-cap-accepted, APC single
  frame, APC overflow-dropped. Pre-fix source fails INV-OSC8-B and
  INV-APC-B; post-fix all five pass.

### Fixed

- **Menubar renders opaque under `WA_TranslucentBackground`
  (`MainWindow::applyTheme`).** User report 2026-04-24: "the
  background of the menubar is transparent." Root cause: the QSS
  cascade from the `QMainWindow`-level stylesheet can race with the
  compositor damage rect under `WA_TranslucentBackground` — on frames
  where the compositor invalidates the chrome region before QSS polish
  has produced a paint, the menubar shows through to whatever is
  behind the window. `applyTheme` now installs a belt-and-suspenders
  opaque fill on `m_menuBar`: `QPalette::Window = theme.bgSecondary`
  (so `autoFillBackground` lays down an opaque fill *before* QSS
  paints) plus a widget-local `setStyleSheet(QMenuBar ...)` block (so
  the menubar's own style engine polishes the QSS independently of
  the top-level cascade). Mirrors the same pattern already used on
  the custom title bar and status bar. `tests/features/menubar_hover_stylesheet`
  extended with INV-7 locking both the palette and widget-local-QSS
  calls.

## [0.7.24] — 2026-04-24

**Theme:** wide-char (CJK / wide emoji) overwrite no longer strands its
mate. 0.7.12 Tier 2 hardening item.

### Fixed

- **Wide-char overwrite now zeroes the stranded mate
  (`TerminalGrid`).** A wide character occupies two adjacent cells —
  a first half (`isWideChar=true`, codepoint=CP) and a continuation
  (`isWideCont=true`, codepoint=0). Before this release, three write
  paths in `src/terminalgrid.cpp` left half-pairs in an inconsistent
  state when a new write landed on only one half: (a) `handlePrint`
  narrow write over a continuation left the mate at `col-1` still
  claiming `isWideChar=true` with no neighbor (rendered with a gap);
  (b) `handlePrint` narrow write over a first half left the old
  continuation at `col+1` claiming `isWideCont=true` with no mate
  (blocked selection / copy); (c) `handlePrint` wide write shifted
  by one cell from an existing wide pair left the old continuation
  at `col+2` orphaned; (d) the SIMD-coalesced fast path
  `handleAsciiPrintRun` had the same left/right edge issues on its
  write span. Consolidated the policy into a single
  `breakWidePairsAround(row, startCol, endCol)` helper that runs
  before every write site: it blanks the stranded first half on the
  left edge and clears the stranded continuation on the right edge.
  Regression test `tests/features/wide_char_overwrite_mate` locks
  five subcases — pre-fix source fails four of them (INV-1, INV-2,
  INV-3, INV-4), post-fix all five pass.

## [0.7.23] — 2026-04-24

**Theme:** xterm-compatible Background Color Erase (BCE) across every
scroll and erase path. 0.7.12 Tier 2 hardening item.

### Fixed

- **BCE on scroll and insert/delete paths (`TerminalGrid`).** Apps
  that paint with a non-default SGR background (`\e[44m` etc.) and
  then scroll, insert lines, delete lines, delete chars, or insert
  blanks expect the newly-exposed cells to inherit the current bg —
  this is the xterm convention vim/less/tmux/mc/htop rely on for
  full-screen painted backgrounds. Previously `takeBlankedCellsRow()`
  hardcoded `m_defaultBg`, so `CSI L` / `CSI M` / `CSI S` / `CSI T`
  / LF-past-scroll-bottom all produced default-bg gaps through any
  painted region. `deleteChars` / `insertBlanks` used raw
  `m_currentAttrs.bg` without the `.isValid()` fallback that
  `clearRow` already had. Consolidated the policy into one
  `TerminalGrid::eraseBg()` helper; every erase/scroll callsite
  now reads from it. Regression test:
  `tests/features/bce_scroll_erase/spec.md` — 10 behavioral
  subcases (IL, DL, SU, SD, DCH, ICH, ED2, EL0, LF-scroll,
  SGR-reset-before-erase). Pre-fix source fails 5 subcases
  (IL/DL/SU/SD/LF-scroll); post-fix all 10 pass.


## [0.7.22] — 2026-04-24

**Theme:** user-visible About menu (user ask 2026-04-24 — "How do I
see what version of Ants Terminal I am running? Can you please add a
GUI version?").

### Added

- **Help → About Ants Terminal… menu.** New rightmost menu on the
  menubar (matching Linux desktop HIG: Help is always last). Two
  actions:
  - **About Ants Terminal…** — shows a rich-text dialog with the
    running `ANTS_VERSION` (read directly from CMake's
    project-wide single source of truth, no hardcoded literal),
    the Qt runtime version (via `qVersion()`), the Lua engine
    version (when compiled with `ANTS_LUA_PLUGINS`), a one-line
    summary, and a clickable GitHub homepage link
    (`TextBrowserInteraction` enabled so URLs in the dialog open
    in the user's browser).
  - **About Qt…** — the stock `QMessageBox::aboutQt` dialog.
    Inherits future Qt-version bumps automatically.

  Pre-fix, `ants-terminal --version` on the command line was the
  only path to read the running version. Regression test:
  `tests/features/help_about_menu/spec.md` — 6 invariants
  (Help-menu-last, About-action present, `ANTS_VERSION`
  referenced not hardcoded, `Qt::RichText` +
  `Qt::TextBrowserInteraction` set, About-Qt action routed to
  `QMessageBox::aboutQt`, no `"0.7."` literal inside the
  handler body).


## [0.7.21] — 2026-04-24

**Theme:** Lua sandbox hardening trio from the 0.7.12 /indie-review.
Three small defense-in-depth fixes in `LuaEngine`, behavioral +
source-grep regression test locked to all three.

### Security

- **`string.dump` removed from the plugin sandbox.**
  `string.dump(f)` returns the bytecode serialization of a Lua
  function. Lua 5.4 has no bytecode verifier — the loader parses
  any byte sequence beginning with `\x1b` as a binary chunk, and
  crafted bytecode can corrupt Lua's internal state and escape
  the sandbox. `load`/`loadstring`/`loadfile` are already nilled
  at init, so there is no supported round-trip from
  `string.dump` back to executing bytecode, but a future C API
  added to `ants.*` that wraps `luaL_loadbuffer` with plugin-
  supplied data would reopen the attack surface. Closing the
  primitive at the sandbox layer — `lua_setfield(m_state, -2,
  "dump")` scoped to the string table, not a blanket
  `lua_setglobal` — is cheaper than auditing every future C API
  for the same rule.
- **`LuaEngine::loadScript` forces `"t"` (text-only) load mode.**
  The pre-fix path `luaL_dofile` → `luaL_loadfile` →
  `luaL_loadfilex(L, path, nullptr)` accepted both text and
  binary chunks at the loader level. The 0x1b-first-byte peek in
  `loadScript` was the first gate; the loader call is now the
  second gate. A future refactor that drops the peek still gets
  rejection at the Lua level.
- **Instruction-count hook cleared before `lua_close` in
  `shutdown`.** `lua_close` runs every pending `__gc` metamethod
  in dependency order. Metamethods can execute arbitrary Lua
  code which the count hook observes. If the hook fires mid-
  close and walks back into registry data or the dying engine
  pointer via `__ants_engine`, we get a UAF window. Clearing
  the hook first with `lua_sethook(m_state, nullptr, 0, 0)`
  removes that window; the C-side cleanup proceeds without any
  Lua-level observer.

Regression test: `tests/features/lua_sandbox_hardening/spec.md` —
6 invariants, behavioral (string.dump is nil, valid-text loads,
0x1b-first-byte rejected) plus source-grep on the three fix
tokens (`luaL_loadfilex(..., "t")`, `lua_sethook(m_state, nullptr,
0, 0)` before `lua_close`, `lua_setfield(m_state, -2, "dump")`
scoped to the string table). Verified to fail against pre-fix
source via `git stash` — 5 of 9 invariants flip red on the
regression, all green after the fix lands.


## [0.7.20] — 2026-04-24

**Theme:** Tier 2 hardening sweep — three open 📋 items from the
0.7.12 /indie-review landed as one release. Each has a behavioral
regression test, source-grep on the fix's load-bearing tokens, and
was verified to fail against pre-fix source via `git stash` before
locking.

### Security

- **`debug.log` lands 0600 regardless of umask.**
  `~/.local/share/ants-terminal/debug.log` was being created with
  the process umask (typically 0644 under 0022). The log can
  include PTY keystrokes (via the `input` / `pty` categories), AI
  endpoint request+response bodies (`network`), OSC 133 HMAC digest
  material (`shell`), and Claude transcript parse state (`claude`) —
  every one of those is material that must not be world-readable.
  `DebugLog::setActive` now calls `setOwnerOnlyPerms` twice after
  the file opens: once on the `QFileDevice` to cover the just-opened
  fd, and once on the path string to narrow any pre-existing 0644
  file that append reused from a prior (pre-fix) run. Fix uses the
  project-standard `secureio.h` helper, not a raw `QFile::setPermissions`
  bitmask. Regression test: `tests/features/debuglog_perms/spec.md`
  (4 invariants: fresh-open perms, clear-then-reopen, pre-existing
  0644 narrowed to 0600, source uses the helper).
- **Audit path-traversal guard on findings' `file` field.** User-
  supplied audit rules, `audit_rules.json` in a cloned project, and
  external scanner regex outputs can all produce findings whose
  `file` field is e.g. `../../etc/passwd`. Pre-fix, six call sites
  across `AuditDialog` naively concatenated `m_projectPath + "/" +
  f.file` and passed the result to `readSnippet` / `lineIsCode` /
  comment scans / AI-triage POST bodies — a textbook CWE-22 +
  OWASP LLM06 (sensitive-information disclosure via LLM) chain, since
  the AI-triage surface exfiltrates snippet contents to the configured
  /v1/chat/completions endpoint. New `AuditDialog::resolveProjectPath`
  helper canonicalizes the candidate path (resolves `..` and
  dereferences symlinks in a single `QFileInfo::canonicalFilePath`
  call), requires the result to be anchored under the canonical
  project root (with a trailing-slash sentinel so `/proj-foo` can't
  escape from `/proj`), and returns `QString()` on any rejection.
  All six call sites migrated: `dropFindingsInCommentsOrStrings`,
  `inlineSuppressed`, the enrichment pass, single-finding AI-triage
  snippet fallback, batch AI-triage snippet fallback, and the
  `dropIfContextContains` regex-captured-relPath read. Regression
  test: `tests/features/audit_path_traversal/spec.md` — 5 invariants
  behavioral (via a byte-faithful reference reimpl — production
  helper is private on the heavy QDialog subclass) plus source-grep
  on the production code to confirm migration + helper structure
  (canonicalFilePath + anchored startsWith).

### Fixed

- **Settings dialog discarded on external config.json reload.**
  `MainWindow` caches the `SettingsDialog` across Preferences...
  opens; the dialog was constructed with `&m_config` and populated
  its widgets from the then-current values at construction. When
  `QFileSystemWatcher` fired `onConfigFileChanged` on an external
  edit, `m_config = Config()` reloaded from disk but the cached
  dialog still held pre-reload widget state. Next Preferences...
  open would show stale values, and clicking OK would replay them
  over the fresh Config — silently undoing the external edit.
  `onConfigFileChanged` now closes the dialog if visible, calls
  `deleteLater()`, and nulls the pointer, so the next open rebuilds
  from the freshly reloaded Config. Regression test:
  `tests/features/settings_dialog_config_reload/spec.md` —
  4 source-grep invariants (cache nulled, visible-close gate,
  deleteLater-not-delete, invalidation scoped to
  `onConfigFileChanged`).


## [0.7.19] — 2026-04-24

**Theme:** CI un-break + tab-rename persistence. CI had been red since
0.7.17 on the AppStream metainfo validation step — the 0.7.17 release
description embedded `git clone https://ghp_…@github.com/…` as an
example of what the new secret-redactor scrubs, and `appstreamcli`
correctly rejects plaintext URLs in `<description>` bodies. Rewrote
the example to use an inline non-URL form; local
`appstreamcli validate --explain` now exits 0. Separately, user asked
whether manual tab renames (right-click → "Rename Tab…") survive
Ants restart. They didn't — the pin map lived only in `MainWindow`
memory and `SessionManager` didn't serialize it. Fixed by bumping
the session-file schema to V3 with a trailing `pinnedTitle` field;
V2 files still load with the out-param defaulting to empty.

### Added

- **Manual tab renames persist across Ants Terminal sessions.** User
  ask 2026-04-24. The right-click "Rename Tab…" pin (`m_tabTitlePins`)
  is now written to and read from each per-tab session file, so a
  tab renamed to "Deploy" or "Prod DB" or "Claude #3" keeps that
  label after the app exits and relaunches. `SessionManager`
  schema bumped to V3: a trailing `QString pinnedTitle` field is
  appended after the V2 `cwd`. V2 files continue to load via the
  existing `in.atEnd()` gate — Ants 0.7.18 and earlier can't read
  V3 files, but since session files are a per-user cache (not an
  interchange format) that's by design. `MainWindow::saveAllSessions`
  threads `m_tabTitlePins.value(w)` (keyed by the outer tab widget,
  which may be a `QSplitter` for split tabs) into the save;
  `restoreSessions` pulls the pin back, populates the in-memory
  `m_tabTitlePins` map, and sets the tab label directly (pin takes
  precedence over the shell-derived window title from the saved
  grid's windowTitle()). Contract locked by
  `tests/features/tab_rename_persist/spec.md` — 20 invariants
  covering the V3 round-trip (three pin lengths including empty),
  V2 backward compat (hand-crafted V2 stream → restore leaves
  `pinnedTitle` out-param empty), and MainWindow source-grep that
  both save-side and restore-side wiring remain threaded. Verified
  to fail against pre-fix source before locking.

### Fixed

- **AppStream metainfo validation passes.** `appstreamcli
  validate --explain` failed on the 0.7.17 / 0.7.18 release
  descriptions because the 0.7.17 block contained
  `git clone https://ghp_…@github.com/…` as a scrubber example,
  and AppStream's `description-has-plaintext-url` rule rejects
  raw URLs in description bodies. CI's "Validate AppStream
  metainfo" step exited 3 on every push since 0.7.17. Rewrote the
  example as `<code>git clone</code> with an embedded
  <code>ghp_</code> token in the URL`. CI build-test step should
  be green again on this release.

### Changed

- **`SessionManager::restore` initializes optional out-params
  before reading.** Previously, callers that passed a
  pre-populated `QString *cwd` or `QString *pinnedTitle` would
  see their sentinel survive an older-format load (V2 files
  leaving `pinnedTitle` untouched, V1 files leaving `cwd`
  untouched). Fixed by clearing both out-params at function
  entry regardless of the stream's version. The version-gated
  read blocks still populate them only when the on-disk format
  actually has the field; the clear-first guarantees they read
  as empty rather than as whatever the caller happened to
  pre-fill. Locked by `tab_rename_persist` I3.


## [0.7.18] — 2026-04-24

**Theme:** post-release documentation + tech-debt sweep. Three parallel
audit agents (doc drift, source debt, spec-vs-code drift) inventoried
every doc-vs-code mismatch, stale TODO, dead config key, and spec line
number across the tree. The `shell_command` config key turned out to
be the only actual functional bug: Settings → General's Shell picker
persisted the user's choice to `config.json` but no code path ever
read it — every tab opened `$SHELL` regardless of what the user picked.
Two other deferred-but-flagged items from the 0.7.12 ROADMAP also
landed: `Qt::WA_OpaquePaintEvent` on the menubar (closes the
mouse-move-over-menubar dropdown flicker on KWin that the 0.7.4
QOpenGLWidget→QWidget refactor didn't fully eliminate), and
`background_alpha` config key removed as redundant after user
confirmed the `opacity` knob already does what they want.

### Removed

- **`background_alpha` config key + `Config::backgroundAlpha()` +
  `Config::setBackgroundAlpha()` + `TerminalWidget::setBackgroundAlpha()`
  + `TerminalWidget::backgroundAlpha()` + `m_backgroundAlpha` member.**
  The key had no paint-site consumer (the `fillRect` at
  `terminalwidget.cpp:605` always used `m_windowOpacity`, which is
  driven by the `opacity` config key) and no UI widget. User
  confirmed (2026-04-24) they use `opacity` ~0.9-0.95 to make the
  terminal area translucent while the chrome stays solid — which is
  exactly what the existing paint path does. Removing the key
  collapses two overlapping knobs into the one that works.
  Backwards-compat note: stale `"background_alpha"` entries in
  existing `config.json` files are harmlessly ignored on load (Qt's
  JSON parser tolerates unknown keys). README config-defaults table
  and example JSON both pruned; CLAUDE.md "Per-pixel bg alpha is
  independent of window opacity" bullet rewritten to describe the
  actual single-knob behaviour. `m_windowOpacity` member kept (name
  is historical — it drives per-pixel terminal-area fillRect alpha,
  not Qt's whole-window `setWindowOpacity`); header + paint-site
  comments clarify the misnomer.

### Fixed

- **`Config::shellCommand()` actually launches the user's shell.**
  Settings → General's Shell picker (Default / bash / zsh / fish /
  Custom…) has persisted the user's choice to `shell_command` in
  `config.json` since the original implementation, and the Settings
  dialog loads/re-displays it correctly. But `TerminalWidget::startShell`
  took only a working directory and invoked `VtStream::start` with a
  hardcoded empty shell arg — `Pty::start`'s empty-shell fallback
  always fired (`$SHELL` → `pw_shell` → `/bin/bash`), silently
  dropping the user's choice. Fix: `startShell` gains a `shell`
  parameter, the `QMetaObject::invokeMethod` call forwards it
  instead of `QString()`, and every production call site in
  `MainWindow` (`newTab`, `newTabForRemote`, `splitCurrentPane`,
  the `restoredTabs` loop in session restore) passes
  `m_config.shellCommand()`. Contract pinned by
  `tests/features/shell_command_wiring/spec.md` — 7 invariants
  covering the signature, the `Q_ARG` forward, the four wired call
  sites, a zero-orphan guard, and a Config round-trip. Verified to
  fail against pre-fix source (5 invariants fire on missing
  signature + hardcoded `QString()` + four orphan call sites)
  before locking.
- **Dropdown flicker on mouse-move-over-menubar fix
  (`Qt::WA_OpaquePaintEvent` on `m_menuBar`).** The menubar's
  `autoFillBackground` + stylesheet fully cover every pixel on every
  paint, so marking it opaque-on-paint stops Qt from invalidating
  the translucent parent's compositor region under it when it
  repaints. Without this, each mouse-move over the menubar item
  owning an open dropdown damages the popup above the menubar on
  KWin. `MainWindow` ctor now calls
  `m_menuBar->setAttribute(Qt::WA_OpaquePaintEvent, true)`; the
  QSS comment at the `:hover` rule always promised this was set at
  the construction site, but the call was missing. Flagged in 0.7.12
  Tier 3 hardening sweep as "Missing per
  `tests/features/menubar_hover_stylesheet/spec.md` INV-3b". Locked
  by the reinstated INV-3b assertion in
  `test_menubar_hover_stylesheet.cpp` (was previously punted to
  `terminal_partial_update_mode`, but that test covers an orthogonal
  fix — both now assert their own attribute).

### Changed

- **`README.md` config-defaults table corrected.** `session_persistence`
  default shown as `false` in both the example JSON block and the
  key-description table; actual default in `config.cpp:311` is `true`.
  `font_size` range shown as 8–32; actual range in `config.cpp:170`
  and `settingsdialog.cpp:206` is 4–48. Both corrected — the
  authoritative code defaults win.
- **`PLUGINS.md` Lua resource-limit docs.** Previously claimed limits
  are enforced "per plugin per event invocation" and that each event
  gets a fresh 10 M-instruction budget. The implementation sets
  `lua_sethook(LUA_MASKCOUNT, 10000000)` once at engine init; the
  counter accumulates across all handlers in the same VM until the
  hook fires, at which point it resets. Doc rewritten to describe
  the real contract: "≥10 M instructions anywhere in the VM aborts
  the current pcall; plugin is not unloaded, next event starts from
  wherever the hook last fired."
- **`tests/features/terminal_partial_update_mode/spec.md` rewritten
  to match the live test.** The `.cpp` test was updated in 0.7.4 to
  enforce the post-refactor invariant (TerminalWidget is a plain
  `QWidget`, no `QOpenGLWidget::` calls, no `makeCurrent()`), but
  the adjacent `spec.md` still documented the pre-0.7.4
  `setUpdateBehavior(PartialUpdate)` fix that didn't work. Spec now
  describes the real fix (base-class change), lists the 4
  invariants the test actually enforces, and preserves the
  PartialUpdate detour in a History section for archaeology.
  Directory name kept for CMake stability — rename would touch
  `add_executable` / `add_test` / label wiring.
- **Spec line-number drift corrected.** `osc133_hmac_verification`
  (1297 → 1722), `session_persistence_default` (config.cpp:218 → 310),
  `review_changes_click` (mainwindow.cpp:74 → 148, ~411 → ~595),
  `ai_context_redaction` (aidialog.cpp:114-136 → 115-137). Line-
  number drift is inherent to spec.md files; this sweep resets the
  ground state, no workflow change.


## [0.7.17] — 2026-04-24

**Theme:** hot-path parser optimization + stale-TODO sweep. VtParser
printable-ASCII runs now coalesce into a single `VtAction` carrying a
pointer+length slice of the feed buffer; `TerminalGrid` grows a
`handleAsciiPrintRun` fast path that batches per-row cell writes so
`markScreenDirty`, the combining-char erase, and the `cell()` clamp
all fire once per row instead of once per byte. Rebench on 8 MiB
corpora: `ascii_print` 24.4 → 30.7 MB/s (+26%), `ansi_sgr`
27.5 → 31.4 MB/s (+14%). Two Tier 2 TODOs also closed: `SIGPIPE`
was already shipped in 0.6.28 (bc97485) but never had a regression
test — now grep-locked; RIS (ESC c) preserved only 2 of 8 integration
callbacks, silently breaking notifications, progress, command-finished,
user-vars, and the OSC 133 HMAC forgery alarm after any `tput reset` —
now preserves all 8 plus the HMAC key.

### Added

- **Claude Code `UserPromptSubmit` git-context hook (user ask
  2026-04-24).** New Settings-dialog button "Install git-context
  hook" writes `~/.config/ants-terminal/hooks/claude-git-context.sh`
  and merges one entry into `~/.claude/settings.json` under
  `hooks.UserPromptSubmit`. The script prints a compact
  `<git-context>` block — branch, upstream, ahead/behind counts,
  staged/unstaged/untracked file counts — on every user prompt, so
  Claude Code sees repo state *up front* without spending tokens on
  a `Bash(git status)` round-trip. Global scope (the user's explicit
  ask "can this be available to all projects?"): installed in
  `~/.claude/settings.json` so it fires on every Claude Code session
  regardless of which project. No-op outside a git repo, no-op when
  `git` isn't on PATH, no-op when `CLAUDE_PROJECT_DIR` is unset and
  `$PWD` isn't a repo — silent exit-0 so non-repo sessions see zero
  behaviour change. Independent of the existing status-bar hook
  installer (different direction of data flow: this one is
  Claude→git, the status-bar one is Ants→Claude); users can opt
  into one and not the other. Installer is idempotent and preserves
  pre-existing user-added `UserPromptSubmit` entries (ripgrep
  cheatsheet injectors, TODO listers, etc.) via the same
  "append-only-if-not-already-present" loop used by the status-bar
  installer. Parse-error-refuse on a corrupt settings file matches
  the 0.7.12 /indie-review cross-cutting fix shape. Contract pinned
  by `tests/features/claude_git_context_hook/spec.md` + 10-invariant
  installer source-grep test + 5-scenario behavioral script test.
- **`src/secretredact.h` — OWASP LLM06 defense layer.** Header-only
  module exposing `SecretRedact::scrub(QString) → {text, redactedCount}`.
  Regex-scrubs 14 well-known secret shapes out of any string before it
  leaves the process: AWS access keys (AKIA/ASIA), GitHub classic /
  OAuth / app / fine-grained PATs, Anthropic `sk-ant-`, OpenAI
  `sk-proj-` + legacy `sk-`, Slack `xox[abpros]-`, Stripe `sk_live_` /
  `rk_live_`, JWTs, `Bearer <token>` headers, generic
  `api_key=`/`token=`/`password=`/`secret=` assignments, and
  multi-line PEM `-----BEGIN … PRIVATE KEY-----` blocks. Replacements
  are `[REDACTED:<kind>]` so the LLM still sees the surrounding
  command/variable shape. Priority-ordered rule set guarantees
  `sk-ant-…` is labelled `anthropic`, not mislabelled as legacy
  `openai`; overlap resolution drops lower-priority matches in the
  same range. Pure function, thread-safe, static-initialised once per
  process.
- **`AiDialog::sendRequest` now scrubs both outbound strings.** The
  terminal scrollback context (`m_terminalContext`) and the user's
  own question (`userMessage`) both pass through `SecretRedact::scrub`
  before either is interpolated into the system prompt or the user
  message body. Pre-fix, a single `cat .env`, `aws configure show`,
  or `git clone https://ghp_…@…` line in recent scrollback exfiltrated
  the credential on the next AI request. When the combined
  `redactedCount > 0`, the dialog appends a `System` chat-history
  note — "Note: N secret(s) redacted from outbound request (OWASP
  LLM06)" — so the user knows the payload differs from what they
  saw/typed. The chat history's "You:" display intentionally shows
  pre-redaction text (redaction is a network-boundary concern, not a
  UX one).
- **Feature test:** `ai_context_redaction` — 16 positive cases (one
  per secret shape) + 6 negative controls (UUID, commit SHA, plain
  URLs, prose mentioning "api token" without a value, empty string,
  `ls -la`). Asserts no raw secret substring survives, the expected
  `[REDACTED:<kind>]` label appears, surrounding text is preserved,
  cumulative `redactedCount` is accurate across multiple secrets,
  precedence orders `sk-ant-` above legacy `sk-`, and (source-grep)
  `AiDialog::sendRequest` wires `SecretRedact::scrub` to both
  inbound strings and gates the user-notice on `totalRedacted > 0`.
  Verified to fail against pre-fix source (5 grep invariants) before
  locking.
- **Feature test:** `tab_rename_pin` — source-grep regression guard
  that pairs the right-click rename handler with the pin-map
  consumer side. Asserts the rename lambda calls
  `setTabTitleForRemote(…)`, never calls `m_tabWidget->setTabText`
  directly, does not guard the call behind `!newName.isEmpty()`
  (empty-string clear is deliberate UX), and that the consumer-
  side `m_tabTitlePins.contains(...)` guards on both the
  `titleChanged` handler and `updateTabTitles` still exist — if
  either is deleted "to clean up," the fix silently regresses.
- **Feature test:** `vtparser_print_run_coalesce` — locks grid-state
  equivalence between the bulk-feed (SIMD + coalesce) path and the
  byte-by-byte scalar path across 8 invariants (ASCII, CSI at every
  lane boundary, wrap at right edge, cursor mid-row, mixed UTF-8,
  empty input, combining-mark clearing).
- **Feature test:** `sigpipe_ignore` — source-grep regression test that
  pins `std::signal(SIGPIPE, SIG_IGN)` in `main.cpp` before the
  `QApplication` constructor. Installing it after QApplication leaves
  any write path triggered by Qt resource loading exposed to
  termination.
- **Feature test:** `ris_preserves_callbacks` — exercises all 8
  `TerminalGrid` integration callbacks plus the `m_osc133Key` HMAC
  config before and after an `ESC c` reset and asserts every one
  survives. Pre-0.7.17 only `responseCallback` + `bellCallback` were
  preserved; the other 6 silently wiped.

### Changed

- **`VtAction` carries an optional coalesced Print run.** Two new
  fields: `const char *printRun` + `int printRunLen`. When set, they
  point into the caller's feed buffer (valid for the duration of the
  `ActionCallback` call only). `TerminalWidget`'s direct-callback path
  uses this to skip `wcwidth`, the combining-char check, the wide-char
  branch, and `std::clamp` inside `cell()` on every printable byte.
- **`VtStream::onPtyData` callback expands runs into per-byte Prints.**
  Its batch outlives the feed buffer, so it MUST materialize the run
  into per-byte `Print` actions before queuing for the GUI thread.
  Lifetime invariant documented in `vtparser.h` alongside the field.
- **`tests/features/vtparser_simd_scan` + `threaded_parse_equivalence`
  canonicalize runs before comparing.** Both tests establish action-
  stream equivalence across feed strategies; the expand-runs-to-per-
  byte canonicalization preserves the contract after coalescing.

### Security

- **Claude allowlist `settings.local.json` perms survive rename
  fallback (belt-and-suspenders).** `ClaudeAllowlistDialog::saveSettings`
  previously set 0600 only on the `QSaveFile` temp fd, relying on
  rename to carry the perms across to the final path. Most local
  filesystems (ext4/xfs/btrfs) preserve fd perms across `rename(2)`,
  but FAT/exFAT on removable media don't carry POSIX perm bits at
  all, some SMB/NFS servers apply umask to the rename destination,
  and Qt's `QSaveFile::commit` falls back to copy+unlink on
  cross-device renames — in all three cases the final file lands
  at the process umask (typically 0644), leaving it readable to
  every other UID on the host. Since `settings.local.json` can
  hold Claude Code bearer tokens in merged `env`/`model`/custom-hook
  keys, the widened perms are a credential-leak surface. Fix: after
  `file.commit()` returns true, call `setOwnerOnlyPerms(m_settingsPath)`
  on the final path too. The pre-commit fd-level chmod is retained
  (covers the open-to-commit window on filesystems that *do*
  preserve fd perms — neither call subsumes the other). Flagged by
  the 2026-04-23 `/indie-review` sweep. Locked by
  `tests/features/allowlist_perms_postcommit/spec.md` — runtime
  stat check under umask 0022 (the POSIX default) plus source-grep
  that both chmod calls remain and the post-commit call is
  sequenced after the `if (!file.commit()) return false;` gate so
  a failed commit can't chmod a path that may not exist.
- **AI context + user-message secret redaction (OWASP LLM06).** The
  AI dialog previously shipped recent scrollback verbatim to whichever
  endpoint the user had configured — OpenAI, Anthropic, an in-house
  gateway, a self-hosted Ollama, any third party. Anything on the
  user's screen at dialog-open time exfiltrated: a single
  `cat .env`, `env | grep`, `aws configure show`,
  `git clone https://ghp_…@github.com/…`, or freshly-pasted SSH key
  landed in the provider's request logs or training-data pipeline.
  The user's own pasted questions were equally exposed. Fix: both
  outbound strings now pass through a 14-shape regex scrubber before
  reaching the JSON body. Complements the 0.7.12 LLM01/LLM02 fix
  (`ai_insert_command_sanitize`) — that handled untrusted input from
  the AI to the PTY; this handles untrusted output from the PTY to
  the AI. Contract pinned by
  `tests/features/ai_context_redaction/spec.md` (9 invariants).

### Fixed

- **Right-click "Rename Tab…" pins the label.** User report
  2026-04-24: "I renamed a few tabs I had open (some with Claude
  Code running) and then Claude Code just renamed them again. Is
  this intentional?" It was not. The rename handler at
  `mainwindow.cpp:4284` wrote directly to `m_tabWidget->setTabText`
  without populating `m_tabTitlePins`; the per-shell
  `titleChanged` signal handler and the 2 s `updateTabTitles` tick
  both consult the pin map to decide whether to relabel, so any
  tab whose shell writes OSC 0/2 (Claude Code writes one every
  prompt iteration, ~3–5 s) had its manual name wiped within
  seconds. Rename now routes through the existing
  `setTabTitleForRemote` (the rc_protocol `set-title` path) so
  non-empty names pin, and empty names clear the pin and restore
  the format-driven / shell-driven label — gives the user an
  in-UI "un-rename" path that didn't exist before. Locked by
  `tests/features/tab_rename_pin` (4 invariants).
- **RIS (`ESC c`) now preserves all 8 integration callbacks +
  `m_osc133Key`.** Previously `notifyCallback`, `progressCallback`,
  `lineCompletionCallback`, `commandFinishedCallback`, `userVarCallback`,
  and `osc133ForgeryCallback` silently became empty functions after any
  `tput reset` / `reset(1)` / `stty sane`. Desktop notifications,
  progress bars, `command_finished` plugin events, user-var theming
  hooks (jj, starship), and the OSC 133 HMAC forgery alarm all died
  after a single RIS. Security impact for the forgery alarm: a
  well-timed `tput reset` silenced it permanently, enabling OSC 133
  forgery to proceed without user notification. `m_osc133Key` (read
  from `$ANTS_OSC133_KEY` at process start) also now survives RIS.

### Performance

- **VtParser printable-ASCII run coalescing.** The SIMD fast path
  (`scanSafeAsciiRun` on SSE2/NEON) now emits one `VtAction::Print`
  per run instead of one per byte. For a 10 KB TUI-repaint PTY read,
  the allocation count drops from ~10 000 to 1 and the dispatch count
  drops by the same factor. Bench deltas on 8 MiB corpora:
  `ascii_print` 327 → 260 ms (−20%), `ansi_sgr` 291 → 255 ms (−12%).
  The `newline_stream` and `utf8_cjk` corpora are bound by
  `newLine`/scroll and UTF-8 multibyte decoding respectively — no
  safe-ASCII runs to coalesce — so their numbers are flat.
- **`TerminalGrid::handleAsciiPrintRun` batches per-row writes.**
  Skips the per-byte `wcwidth`, combining-char branch, wide-char
  branch, and `std::clamp` inside `cell()`. Fires `markScreenDirty`
  once per row instead of once per byte.


## [0.7.16] — 2026-04-23

**Theme:** per-tab Claude Code activity indicator — flagship response to
the 2026-04-23 user request for at-a-glance multi-tab Claude visibility.
Users running Claude Code in several tabs now see per-tab state glyphs
(idle / thinking / tool-use / **Bash** / planning / compacting /
**awaiting-input**) so they can tell which tab is blocked on them
without cycling through tabs. Permission prompts route by `session_id`
(not active-tab) so a prompt from any tab lights up that specific tab.
Supporting refactor: `ClaudeIntegration::parseTranscriptForState` is now
a thin wrapper over a new static `parseTranscriptTail` helper so the
new per-tab tracker can share the exact parsing logic with zero drift.

### Added

- **Per-tab Claude Code activity indicator.** Each tab whose shell has a
  Claude Code child process now draws a small state-dependent dot at
  the leading edge of the tab chrome: muted grey for Idle / Thinking,
  blue for generic ToolUse, **green for Bash** (split out because it's
  the highest-signal, longest-running tool), cyan for Planning, violet
  for Compacting, and a bright orange dot with a white outline for
  AwaitingInput (permission prompt pending). Users running Claude
  across several tabs can see at a glance which one is waiting on
  them — and permission prompts route by the hook event's `session_id`
  (matched against the `<session-uuid>.jsonl` transcript filename) so
  a prompt emitted by Claude in tab 3 lights up **tab 3's** glyph, not
  the currently-focused tab. Hover tooltip on each tab shows
  "Claude: thinking…" / "Claude: Bash" / etc. so glyph colours can be
  disambiguated without switching tabs. Settings dialog → General
  adds a checkbox ("Show per-tab Claude activity dot …") that flips
  `claude_tab_status_indicator` (default on); the change takes effect
  on the next paint via the live config reload path, no restart
  needed. New `src/claudetabtracker.{h,cpp}` keys state by shell PID
  so tab reorder / close doesn't shift entries, and reuses the
  existing transcript parser via a new static
  `ClaudeIntegration::parseTranscriptTail` helper — no duplicated
  parsing logic. Contract + 11-invariant regression test at
  `tests/features/claude_tab_status_indicator/`. Roadmap item:
  2026-04-23 user request.

### Changed

- **`ClaudeIntegration::parseTranscriptForState` refactored to a thin
  wrapper around the new static `parseTranscriptTail` helper.** Pure
  function, no side effects — safe to call from per-shell trackers.
  Behavior unchanged; `claude_status_bar` and
  `claude_plan_mode_detection` regression tests still green against
  the same transcripts.

- **Hook events stash `session_id` in
  `ClaudeIntegration::lastHookSessionId()`.** Handlers of signals
  emitted by `processHookEvent` (e.g. `permissionRequested`) can now
  read the id to route the event per-session rather than treating
  every hook as "belongs to the active tab". Required for the
  per-tab activity indicator to flag the correct tab on
  PermissionRequest.


## [0.7.15] — 2026-04-23

**Theme:** fold-in of the 2026-04-23 re-review follow-up list. Six of
nine items had already shipped in 0.7.12 but were never crossed off the
ROADMAP (code was present, bullet was not flipped); the remaining three
are real new work — a `/tmp/kwin_*.js` orphan sweep, a behavioral
replacement for the fragile source-grep gate test, and a signal-count
assertion on the plan-mode tail-preservation path. Two new regression
tests lock existing invariants.

### Added

- **Startup sweep for orphan `/tmp/kwin_*_ants_*.js` script files.**
  The position-tracker + move/center helpers write KWin scripts via
  `QTemporaryFile(autoRemove=false)` and rely on a chained dbus-send
  callback to remove them after KWin unloads. A crash, SIGKILL, or
  dbus hang between write and unload orphans the file — no functional
  harm (KWin already loaded its copy), but the files accumulate in
  `/tmp`. `MainWindow` ctor now runs a once-per-process sweep of
  `kwin_{pos,move,center}_ants_*.js` older than one hour. The
  one-hour floor avoids racing an in-flight script another Ants
  instance just wrote.

### Changed

- **Remote-control opt-in gate test is now behavioral, not source-grep.**
  The 0.7.12 test grepped for `remoteControlEnabled()` within 400 chars
  of `m_remoteControl->start()`. A refactor that renamed the getter or
  hoisted the check into a helper would have silently defeated the
  gate while still passing (or failing) the grep. Replaced with a
  Config API round-trip assertion: default is false, setter persists
  across instances, value is readable post-reload. A getter rename
  now fails at compile-time in the test. The structural "start() must
  live inside a conditional" check is retained as a plain-substring
  lookback to catch a refactor that removes the gate entirely —
  without the catastrophic-backtracking risk that killed the initial
  regex attempt.
- **Plan-mode regression test now asserts emission count, not just
  final value.** `runToggleFreeTailPreservesState` confirms
  `planModeChanged` fires exactly once across both parse phases
  (seed toggles NotSet → plan = one emission; tail sees zero
  permission-mode events and must not re-fire the same state).
  Guards against a regression that re-emits on every parse.

### Tests

- **`config_parse_failure_guard` adds retention-cap invariant (INV-5).**
  Plants 8 stale `.corrupt-*` siblings with decreasing mtimes, trips a
  fresh parse-failure, asserts only the newest 5 (fresh + 4 old)
  remain. Verified to fail against a neutered prune loop (got 9,
  expected 5). Sets mtimes explicitly via `QFile::setFileTime` since
  `QDir::Time` ranks on mtime, not on the filename-embedded timestamp.
- **`remote_control_opt_in` adds behavioral Config round-trip section.**
  Sandboxed via `XDG_CONFIG_HOME` (not `setTestModeEnabled` — that
  routes to a Qt-test-specific path that has collided with real user
  configs in practice). Four invariants: fresh-default false, in-
  memory round-trip, cross-instance persistence, setter-back-to-
  false round-trip.

### Internal

- **ROADMAP re-review follow-up list reconciled with shipped code.**
  Config backup retention cap, remote-control gate cache, kwin
  TOCTOU fix header comment accuracy, spec.md timestamp format, and
  plugin/theme parse warnings were all landed by 0.7.12 but still
  listed as 📋 on the roadmap. Flipped to ✅ with "Shipped 0.7.12"
  notes. No code changes for these items — just a book-keeping
  reconciliation so the next review doesn't re-flag them.

## [0.7.14] — 2026-04-23

**Theme:** three Claude Code integration robustness fixes from the
0.7.12+1 re-review checkpoint. All three failed silently — no
errors, no degraded-mode logs — so the symptoms were "a transcript-
driven UI state that just stopped updating" or "a project that
appeared under the wrong name in the picker". All three now have
targeted fixture-based regression tests.

### Fixed

- **Claude transcript tail window drops events larger than 32 KB.**
  `parseTranscriptForState` read the final 32 KB of the JSONL
  transcript and skipped the first line after the seek as "likely
  truncated". When the final event was a user `tool_result` carrying
  inline file contents (routine for Read/Grep tool results on large
  files), the 32 KB window landed inside that one event, the
  firstLine-skip ate it, and the parser returned with zero events —
  state frozen at whatever it was before the turn. Replaced with a
  doubling-grow window (32 KB → 4 MiB cap) that only trims a
  potentially-partial prefix line when the buffer contains at least
  two newlines, guaranteeing real content remains for the parser.
- **Claude transcript dialog silently drops `thinking` content
  blocks.** Real assistant events on extended-thinking-enabled
  models lead with one or more `thinking` blocks followed by the
  final text response. `ClaudeTranscriptDialog::formatEntry` only
  rendered `text`, `tool_use`, and `tool_result` block types;
  `thinking` fell into an implicit else that discarded the block.
  Now rendered as italicized, dimmed `Thinking:` paragraphs so
  readers can distinguish chain-of-thought from the visible reply.
- **`decodeProjectPath` mangles project names with embedded
  hyphens.** Claude Code encodes absolute paths by replacing every
  `/` with `-` — lossy, since `my-project` (leaf name) and
  `my/project` (two segments) produce the same encoded string. The
  decoder replaced every `-` with `/` unconditionally, turning
  `~/my-project/sub-dir` into `~/my/project/sub/dir`. Rewrote as a
  greedy left-to-right filesystem-probing walker: at each hyphen,
  prefer the `/` form if that directory exists, fall back to `-`
  (folded into the segment name) if that exists instead, default
  to `/` otherwise. Legacy hyphen-free paths remain unchanged.
  Preferred source of truth is still `extractCwdFromTranscript`
  (used by `discoverProjects`); the probe-based decoder is the
  last-resort fallback.

### Tests

- **`claude_transcript_robustness`** (6 invariants) — tail window
  grows past a 40 KB trailing event (INV-1), tail-growth is bounded
  at the 4 MiB cap on pathological inputs (INV-2), leaf-name hyphens
  are preserved when the directory exists (INV-3), intermediate-
  segment hyphens are preserved when the full path exists (INV-4),
  missing filesystem hints fall back to the legacy all-slashes
  behavior (INV-5), and the canonical repo-style path still decodes
  correctly (INV-6). Verified to fail against the pre-fix code on
  INV-1/3/4, pass on INV-2/5/6 (which test the safety-cap and legacy
  paths).

### Changed

- No API or config changes. Pure bugfix release.

## [0.7.13] — 2026-04-23

**Theme:** scope-tightening pass on the audit rule-pack trust boundary
shipped in 0.7.12. The global `audit_trust_command_rules` bool
over-granted after one opt-in — trusting Ants's own repo implicitly
trusted every cloned repo the user later opened. 0.7.13 replaces the
global flag with a per-project hash-bound trust store so trust is both
narrow and self-invalidating.

### Security

- **Per-project audit rule-pack trust store.** Global
  `audit_trust_command_rules` bool is retired. Trust is now keyed on
  `(canonical projectPath, sha256(audit_rules.json bytes))` via three
  new `Config` methods: `isAuditRulePackTrusted`, `trustAuditRulePack`,
  `untrustAuditRulePack`. Consequences:
  1. Trusting one project no longer extends to siblings.
  2. Any edit to the rule pack invalidates trust (re-prompt on next
     open) — a silent post-trust modification is detected.
  3. `projectPath` is canonicalized via `QFileInfo::canonicalFilePath`,
     so symlinks / trailing slashes resolve to a single record.
  4. `ANTS_AUDIT_TRUST_UNSAFE=1` remains an escape hatch for CI that
     can't round-trip through the persisted store.
  Upgrade behavior: users who had the legacy bool set to `true` are
  re-prompted (fresh trust decisions per project). This is the safe
  default for a security feature.

### Changed

- **Audit dialog: "Untrusted rules: N" badge.** The skipped-rule count
  now appears on the Detected-types row alongside "User rules: N" and
  "Path rules: N", with a tooltip explaining the trust boundary and
  opt-in path. GUI users previously only saw the stderr `qWarning`,
  which was invisible outside a terminal-launched invocation. Stderr
  copy retained for headless / CI cases.
- **`CLAUDE.md` streamlined.** Compressed from 325 → 147 lines
  (19.5 KB → 6.5 KB, ~67% reduction) by dropping the full project-
  structure tree (derivable from `ls src/`), collapsing the verbose
  audit pipeline prose, retiring the per-dep install-command list
  (each probes `which <tool>` and self-disables), and dropping the
  Config-keys table (derivable from `config.cpp`). Retained: non-
  obvious gotchas, rationale for design decisions, and the release-
  file triad rule.

### Tests

- **`tests/features/audit_command_rule_trust/`**. Seven-invariant
  regression test against the new trust API: default-untrusted,
  round-trip, hash-bound invalidation, cross-project isolation (with
  byte-identical packs to rule out hash coincidence), path
  canonicalization through symlinks + trailing slashes, idempotent
  untrust, and cross-`Config`-instance persistence through
  `config.json`. Spec-first: contract written before the code, every
  invariant mapped to an assertion with a diagnostic label.

## [0.7.12] — 2026-04-23

**Theme:** independent-review sweep. Multi-agent `/indie-review` run
(14 subsystems, 60 HIGH findings), followed by a re-review of the
shipped Tier 1 batch that surfaced three more HIGH findings of the
same shape. Every fix carries a regression test anchored to an
external signal (published spec, CVE, real on-disk data) rather
than author reasoning — closes the self-graded-homework loop.
Final polish pass (this tag) handles the six mechanical follow-ups
from the re-review.

### Security

- **Remote-control (Kitty rc_protocol) opt-in default.** New config
  key `remote_control_enabled` (default `false`, matches Kitty's
  `allow_remote_control=no`). `send-text` payloads pass through a C0
  control-char filter stripping `{0x00..0x08, 0x0B..0x1F, 0x7F}`
  while preserving HT/LF/CR and UTF-8 multi-byte, unless the request
  carries `"raw": true`. Closes the UID-scope keystroke-injection
  chain — a same-UID attacker can no longer inject
  bracketed-paste-disable + newline payloads. Gate now snapshots
  once per process (static bool) so a second `MainWindow` reading
  the config after a runtime toggle doesn't try to bind a socket
  the first window isn't listening on — matches the "requires
  restart" documentation.
- **Config / Allowlist / Install-hooks silent-data-loss guard.**
  The same load-without-else pattern (read → parse → on failure
  `root` stays empty → next `save()` writes defaults over the
  user's bytes) affected `src/config.cpp`, `src/claudeallowlist.cpp`,
  and `src/settingsdialog.cpp` (Install Claude hooks path, which
  shares the `settings.json` file with the Allowlist dialog). All
  three now rotate the corrupt file aside via the shared
  `rotateCorruptFileAside()` helper in `src/secureio.h`
  (ms-precision timestamp + collision-retry up to 10 suffixes),
  log the backup path, and refuse to write further until the user
  intervenes. Settings dialog shows the backup path in a
  `QMessageBox` so the user has an actionable recovery.
  `rotateCorruptFileAside()` now also prunes older
  `<path>.corrupt-*` siblings beyond the newest five — keeps
  recovery material available without letting repeated launches
  against a broken file accumulate unbounded.
- **`std::rename` return checked in `Config::save`.** Failure logs
  `errno` via `DebugLog::Config` and removes the orphan tmp file
  instead of silently stranding it on disk.
- **SSH bookmark `extraArgs` sanitizer.** `SshBookmark::sanitizeExtraArgs`
  now rejects `-oProxyCommand=`, `-oLocalCommand=`,
  `-oPermitLocalCommand=yes`, `-oMatch=exec` (OpenSSH
  `Match exec` runs `/bin/sh -c` on CLI-supplied commands), and
  `-oKnownHostsCommand=` (OpenSSH 8.5+, same RCE surface), in both
  single-token and space-separated forms, case-insensitive per
  OpenSSH's own option parser. Closes the CVE-2017-1000117-adjacent
  RCE-via-bookmark-field surface.
- **AI `Insert Cmd` sanitize + confirm.** New
  `AiDialog::extractAndSanitizeCommand` extracts the fenced code
  block and strips C0 controls (minus HT/LF/CR), DEL, C1 controls
  (`0x80..0x9F` incl. NEL `U+0085`), line/paragraph separators
  (`U+2028`, `U+2029`), bidi overrides (`U+202A..E`, `U+2066..9` —
  Trojan Source, CVE-2021-42574), and zero-width codepoints
  (`U+200B..D`, `U+FEFF`). Capped at 4 KiB. The click handler now
  requires explicit confirmation via `QMessageBox::question` showing
  the literal bytes plus a "N bytes filtered" footer when non-zero;
  when the 500-char preview doesn't cover the full command, the
  dialog surfaces a truncation warning so an attacker can't hide a
  payload past the preview window. OWASP LLM01 + LLM02 mitigation.
  Language-hint heuristic now also requires the first line be a
  single unbroken token (no space / tab) before treating it as a
  language identifier — `foo bar\nls` no longer has its first line
  eaten.
- **Audit rule-pack `command` fields gated.** `audit_rules.json`
  entries that carry a `command:` field are now skipped unless
  `Config::auditTrustCommandRules()` is `true` (default `false`)
  or `ANTS_AUDIT_TRUST_UNSAFE=1` is set. A cloned-but-untrusted
  repo can no longer run arbitrary shell via the Audit dialog.
  Skipped count surfaces via `qWarning`.
- **`/tmp/kwin_*.js` migrated to `QTemporaryFile`.** The three
  callsites in `xcbpositiontracker.cpp` (window-position poll) and
  `mainwindow.cpp` (Quake-mode move + centre) used predictable
  filenames in `/tmp`, enabling a same-name symlink-swap TOCTOU and
  a collision when two Ants instances ran concurrently. All three
  now use `QTemporaryFile` with a `XXXXXX` template and
  `setAutoRemove(false)` so the async KWin `finished` callback can
  still read the script.

### Fixed

- **Claude plan-mode detection.** `claudeintegration.cpp` was reading
  `value("mode")` — a field that never appears in Claude Code v2.1.87
  JSONL transcripts. Real field is `permissionMode`. Fixed, and the
  toggle-free tail scan now preserves the latched plan-mode state
  instead of resetting to `false` (otherwise a 32 KB window that
  scrolls past the last explicit toggle silently loses the flag).
- **Combining-char sidetable reflow on resize.** Carried through in
  the cross-subsystem cleanup.
- **Plugin manifest + user theme parse warnings.** Both loaders now
  emit a `qWarning` with the `QJsonParseError` offset when a file
  is malformed, so users can find their own typos instead of
  silently dropping the plugin or theme from the list.

### Changed

- **`rotateCorruptFileAside` helper shared across three subsystems.**
  Before the re-review, each site had its own ~30-line backup-rotation
  block; the three reviewers independently flagged the duplication.
  Now a single inline helper in `src/secureio.h` with retention cap
  and ms-collision retry.
- **Remote-control startup gate snapshots once per process.** Caches
  `Config::remoteControlEnabled()` into a static local at first
  `MainWindow` construction and reuses across subsequent windows, so
  a runtime toggle of the config key during a session can't partially
  enable rc on a second window.
- **`filterControlChars` header comment corrected.** Previously said
  "C0 / C1 control bytes"; only C0 is stripped at this layer —
  C1 codepoints are UTF-8 continuation bytes and can't be
  byte-filtered without mangling multi-byte characters. Comment now
  names `aidialog.cpp` as the layer that does strip C1 (operating
  on `QChar`, not raw bytes).

### Tests

- Five new feature-test lanes in `tests/features/`:
  - `claude_plan_mode_detection/` — 8 scenarios against the JSONL
    transcript schema; each asserts behaviour against live on-disk
    bytes rather than a mock.
  - `remote_control_opt_in/` — 17 assertions covering the gate,
    `filterControlChars` strip set, `"raw": true` bypass, and the
    `stripped` response field.
  - `config_parse_failure_guard/` — 12 assertions covering the
    three load states (missing / valid / corrupt), backup rotation,
    save suppression, and `std::rename` failure path.
  - `ssh_extra_args_sanitize/` — 22 assertions covering all five
    dangerous `ssh_config` directives in both `-oKey=val` and
    `-o Key=val` forms plus the `toSshCommand()` end-to-end
    invariant.
  - `ai_insert_command_sanitize/` — 21 assertions covering the full
    strip set (C0 / DEL / C1 / NEL / line separator / bidi overrides
    / zero-width), UTF-8 preservation, 4 KiB length cap, and the
    language-hint heuristic regression (whitespace on first line
    is not a lang ID).


## [0.7.11] — 2026-04-23

**Theme:** housekeeping pass — two long-deferred 💭 items from the 0.8+ queue
and a ROADMAP correction. The perf item rewrites the combining-char map
shift in `deleteChars` / `insertBlanks` to use node-handle `extract()` +
key mutate + `insert()` instead of rebuilding the map. The security item
is a defence-in-depth stat-guard around the remote-control socket's
`/tmp` fallback cleanup. The correction promotes the 0.7.9-era text-run
`QString` accumulator work from 💭 to ✅ (it was shipped, the ROADMAP
entry just missed the update).

### Changed

- **Combining-char map shift without rebuild** (`src/terminalgrid.cpp`,
  `deleteChars` / `insertBlanks`). Old impl built a fresh
  `std::unordered_map<int, std::vector<uint32_t>>`, moved each surviving
  entry into it (re-hashing every key), then move-assigned it back.
  Rewrote to collect affected keys in two small local vectors
  (`toErase` / `toShift`), erase the deleted ones, then for each shift
  extract the node handle, mutate its integer key, and reinsert —
  zero bucket reallocation, zero re-hashing of the value vectors.
  `deleteChars` processes shifts in ascending order (leftward shift
  can't collide); `insertBlanks` in descending order (rightward shift
  can't collide). Empty-map fast-path skips the block entirely, so
  steady-state rows with no combining content (the vast majority) pay
  nothing. Tests `combining_on_resize` and `osc8_insert_delete_lines`
  still pass.

### Security

- **IPC-socket `/tmp` fallback stat-guard** (`src/remotecontrol.cpp`).
  `RemoteControl::start()` used to call `QLocalServer::removeServer(path)`
  unconditionally when `listen()` failed on a stale socket. The
  `XDG_RUNTIME_DIR` path (0700-owned) is safe, but the `/tmp/ants-terminal
  -<uid>.sock` fallback lives in shared-world-writable space where a
  symlink or a foreign file of the same name could trick us into
  unlinking something unrelated. Added a `safeToUnlinkLocalSocket()`
  helper that `lstat()`s the path and refuses removal unless it's an
  actual `S_ISSOCK` owned by `getuid()`. `ENOENT` passes through (nothing
  to remove anyway). On refusal the remote-control layer disables itself
  with a clear log message — no silent unlink of unknown files.

### Fixed

- **ROADMAP accuracy** — the 💭 entry for "Per-frame `QString` construction
  in the text-run accumulator" was actually shipped in 0.7.9 (both
  `TerminalWidget::paintEvent` and `GlRenderer::render` accumulate
  codepoints into a reusable `std::vector<char32_t>` on the `Run` struct
  and call `QString::fromUcs4(data, size)` once at run-push time). The
  stale entry is now promoted to ✅ with the same-style retrospective
  write-up the free-list entry got.

## [0.7.10] — 2026-04-23

**Theme:** follow-through on the 0.7.9 null-result. The `std::rotate` scroll
refactor was rejected because it paid for save/restore overhead without
touching the real bottleneck — the per-scroll `vector<Cell>(m_cols)` heap
allocation for the new bottom row. This release addresses that bottleneck
directly with a small free-list of `m_cols`-sized cell buffers fed by any
path that discards a row (scrollback eviction, `scrollDown`, `insertLines`,
`deleteLines`) and drained by paths that need a fresh blank row. Steady-state
scroll work on a full scrollback runs allocator-free.

### Changed

- **`TerminalGrid` — cell-buffer free-list for scroll paths.** New
  `m_freeCellBuffers` pool (cap 4 entries) + `takeBlankedCellsRow()` /
  `returnCellsRow()` helpers in `src/terminalgrid.{h,cpp}`. `scrollUp`,
  `scrollDown`, `insertLines`, `deleteLines` now recycle cell buffers
  instead of calling `makeRow(m_cols, …)` on every scroll. When
  scrollback hits `m_maxScrollback` and a `pop_front` evicts the oldest
  line, its cells vector is salvaged into the pool rather than freed;
  the next scroll's new bottom row pulls from the pool (blanked in
  place), skipping the allocator entirely. `resize()` clears the pool
  (stale-size entries would be wrong-width). Pool stays at 0–2 entries
  in normal use — memory footprint is bounded and negligible. All 49
  feature tests still pass; `scroll_region_rotate` (shipped 0.7.9 to
  lock the contract) verified the invariants held across the refactor.

### Performance

- **`bench_vt_throughput newline_stream`: 5.26 → 6.09 MB/s (+15.8 %).**
  Median of ten-run pairs before / after. Other corpora are flat to
  mildly improved — `ascii_print` ~23 MB/s (baseline 23), `ansi_sgr`
  ~16.5 MB/s (baseline 16.5), `utf8_cjk` 8.9 MB/s (baseline 7.9,
  +12 %; the CJK payload hits `scrollUp` too). This is the first perf
  result to validate the 0.7.9 investigation's thesis that the
  container shuffle was a red herring and the allocator was the actual
  hot spot.

## [0.7.9] — 2026-04-22

**Theme:** planned-work pass — three ROADMAP items from the 0.7.7 "deferred
to 0.8+" queue investigated and shipped or rejected with findings on record.
The investigations that didn't ship still left value behind (a feature-test
that locks scroll-region semantics for any future refactor), and the one
that did ship consolidates 13 path strings through one header and halves the
per-frame `QString::fromUcs4` call count in both render paths.

### Added

- **`scroll_region_rotate` feature-test.** Locks the observable behavior
  of `TerminalGrid::scrollUp` / `scrollDown` — row motion inside
  `[scrollTop, scrollBottom]`, outside-region invariance, main-screen
  scrollback push, alt-screen no push, hyperlink tracking, and
  `count > regionHeight` clamp. Six scenarios, eight invariants. Pins
  the contract so the 0.8 ring-buffer-screen work can refactor safely.
  See `tests/features/scroll_region_rotate/spec.md`.
- **`src/configpaths.h` — single source of truth for home-relative
  paths.** Inline namespace consolidating `~/.claude/projects`,
  `~/.claude/sessions`, `~/.claude/settings.json`,
  `~/.claude/projects/<enc>/memory/MEMORY.md`, and
  `~/.config/ants-terminal/hooks/claude-forward.sh`. 13 duplicated
  path-concat sites across `claudeintegration.cpp` / `settingsdialog.cpp`
  collapsed to one call each. Future XDG-respect pass now has a single
  update surface.

### Changed

- **Render path — per-run `QString::fromUcs4`.** Both the QPainter
  (`terminalwidget.cpp::paintEvent`) and GL (`glrenderer.cpp::render`)
  code paths now accumulate codepoints into a `std::vector<char32_t>`
  per run and build the `QString` once at flush/draw time, replacing
  the previous `runText += QString::fromUcs4(&cp, 1)` pattern that
  allocated one small `QString` per non-space cell and repeatedly
  reallocated the run string on append. Saves N per-frame small-QString
  constructions in the common ASCII TUI case. Behaviour identical;
  ligature shaping via `QTextLayout` still receives the run text at
  the same point.

### Investigated, not shipped (documented in ROADMAP)

- **`std::rotate` scroll refactor.** Measured on
  `bench_vt_throughput newline_stream`: the rotate path was 12–15%
  **slower** (5.2 → 4.6 MB/s) than the erase/insert loop it replaces,
  because the newline-stream case is always `count == 1` where rotate
  pays for a save/restore cycle that `vector::erase + push_back`
  avoids. Real bottleneck is the per-scroll `makeRow(m_cols, ...)`
  allocation + heap handoff to scrollback — not the container
  shuffle. Reverted. The `scroll_region_rotate` test stays so the next
  attempt can refactor safely. ROADMAP §0.7.7 updated with the
  scrollback-push cell-reuse approach as the real 0.8 candidate.
- **DialogBase.** Only `settingsdialog` and `claudeallowlist`
  actually use `QDialogButtonBox` with Ok/Apply/Cancel; four other
  dialogs listed in the ROADMAP have bespoke button layouts. Would
  save ~10 lines across 2 call sites — premature abstraction.
- **ManagedProcess.** `auditdialog.cpp` uses one shared `QProcess`
  member that runs checks serially (not six copies as the ROADMAP
  claimed). The timeout-and-cleanup dance already lives at one call
  site. No duplication to extract.


## [0.7.8] — 2026-04-22

**Theme:** audit fold-in — every regression guard the 0.7.6 / 0.7.7 hardening
pass left behind is now a repo-wide detector. Three new rules in the Project
Audit dialog turn the one-off sweeps into persistent guards, so the next
contributor that re-introduces the pattern gets caught at audit-time.

### Added

- **`ssh_argv_dash_host` audit rule (Security, Major).** Flags any
  `<< shellQuote(...host...)` argv construction without a preceding
  `args << "--"` argv terminator — the CVE-2017-1000117 class. Runtime
  filter drops findings when the guard appears within ±5 lines
  (spans the `if (!user.isEmpty()) / else` split in `sshdialog.cpp:67-71`).
  `tests/audit_fixtures/ssh_argv_dash_host/` provides the bad/good pair.
- **`qimage_load_without_peek` audit rule (Qt, Minor).** Flags
  `.loadFromData(` calls that aren't either tagged `// image-peek-ok`
  or preceded by a `QImageReader` size-peek within ±5 lines — the
  image-bomb DoS vector that the 0.7.6 hardening sweep fixed. Both
  shipped sites in `terminalgrid.cpp` (OSC 1337 PNG + Kitty graphics)
  carry the explicit reviewer-sign-off tag and drop cleanly.
- **`setPermissions_pair_no_helper` audit rule (Qt, Info).** Hygiene
  nudge toward `setOwnerOnlyPerms()` from `src/secureio.h` when a raw
  `setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)`
  bitmask appears. Pattern deliberately excludes the 0755 hook-script
  case (extra `|` flags after `WriteOwner`) so the one legitimate
  literal-bitmask site in `settingsdialog.cpp` doesn't false-fire. The
  helper itself is suppressed via `// ants-audit: disable-file` in
  `secureio.h` — it is the definition, not a call site.

### Changed

- **`tests/audit_self_test.sh`** — three new `run_rule` lines, coverage
  cross-check now sees the new rule ids automatically. 55/55 pass
  (up from 52/52).


## [0.7.7] — 2026-04-22

**Theme:** the 0.7.7 planned-work pass from the hardening four-dimensional
review — every item marked 📋 in ROADMAP.md's 0.7.7 section is now shipped.
Two de-duplication refactors, three perf hoists, four regression
feature-tests locking the 0.7.6 → 0.7.7 fixes.

### Added

- **Regression coverage for the four 0.7.7 hardening fixes** —
  `tests/features/osc8_insert_delete_lines/` (CSI L / CSI M sync
  invariant), `tests/features/hyperlink_resize_clamp/` (shrink-resize
  clamp on the active OSC 8 start coordinates),
  `tests/features/ssh_dash_host_rejected/` (the `--` argv terminator
  against CVE-2017-1000117 class hosts), and
  `tests/features/image_bomb_png_header_peek/` (static source-inspection
  guard ensuring every `loadFromData` is peeked first). Each test is
  designed to fail on pre-fix code and pass now. 49 feature/fast tests
  total.

### Changed

- **`setOwnerOnlyPerms` helper** (`src/secureio.h`) replaces the 11
  copies of `file.setPermissions(QFileDevice::ReadOwner |
  QFileDevice::WriteOwner)` that were scattered across `config.cpp`,
  `sessionmanager.cpp` (×2), `claudeallowlist.cpp`, `auditdialog.cpp`
  (×4), `claudeintegration.cpp` (×2), `remotecontrol.cpp`, and
  `auditrulequality.cpp`. One typo (`ReadOther`, `ReadGroup`) away
  from silently widening access to files that may hold an API key.
  Two overloads: one for open `QFileDevice&` handles, one for existing
  path strings. The single 0755 case in `settingsdialog.cpp` (hooks
  shell script — needs executable bit) stays literal by design.
- **`shellQuote` helper** (`src/shellutils.h`) replaces the previous
  static function in `sshdialog.cpp` and the *re-defined lambda* in
  `mainwindow.cpp::openClaudeProjectsDialog`. The two earlier impls
  differed on empty-string and no-special-chars handling; both call
  sites now share one inline implementation.
- **Paint-path hoists** — `QFontMetrics` is no longer constructed per
  underlined cell; `updateFontMetrics()` caches `underlinePos` and
  `lineWidth` into members refreshed on font change. Selection bounds
  are now normalised once per `paintEvent` into local scalars with an
  inline `cellInSelection` lambda, rather than running `std::swap` +
  `std::min`/`std::max` per cell. Measurable on search-highlighted +
  selection-active frames.
- **`isCellSearchMatch` — `std::lower_bound` instead of linear scan.**
  `m_searchMatches` is sorted by `globalLine`; the binary search jumps
  to the first match at or after the target line and iterates only the
  matches on that line. The previous implementation self-described as
  "binary search" but walked the whole vector from index 0 on every
  cell. Expected win: 5–20% of paint time on search-highlighted
  content in deep scrollback.


## [0.7.6] — 2026-04-22

**Theme:** close the 0.7.x remote-control arc and close the audit-signal
gap the 0.7.5 hygiene work opened. Seven rc_protocol commands now cover
the "drive the terminal from a script" story end-to-end (`ls`, `send-text`,
`new-tab`, `select-window`, `set-title`, `get-text`, `launch`), and the
audit gains two feature-coverage lanes that catch shipped features
without locking tests and spec prose that drifted past a rename.

### Added

- **Audit — feature-coverage lanes** (`src/featurecoverage.{cpp,h}`). Two
  new in-process audit checks that close the signal-loss frontier left
  by the 2026-04-21 RetroDB audit-hygiene work: scanner calibration was
  tightened, so the remaining noise was *upstream* of the scanners —
  features shipping without any locking test, spec prose referring to
  symbols that had since been renamed. The lanes are Info/Minor severity;
  the value is raising awareness, not gating releases.

  Lane 1 — **spec↔code drift.** For each `tests/features/*/spec.md`,
  extracts backtick-fenced identifier-shaped tokens (CamelCase,
  snake_case, kebab-case, scoped::names, dotted.ids) and reports the
  ones that no longer appear anywhere under `src/`. Scoped names fall
  back to their tail component (`Class::method` → `method`) so rename
  refactors that keep the leaf don't false-positive; a curated stopword
  list drops ubiquitous Qt / language keywords that always exist.

  Lane 2 — **CHANGELOG↔feature-test coverage.** Parses the top
  `## [x.y.z]` section of `CHANGELOG.md`, walks the Added/Fixed bullets,
  and fuzz-matches each against the set of feature-spec titles.
  Backtick-token match wins; ≥2 significant-word overlap falls back
  (stopword-filtered, ≥4 chars). Bullets with neither surface as
  coverage gaps — release-note claims that never got a locking test.

  Both lanes use the `AuditCheck::inProcessRunner` hook so the pure
  parsers plug into the same `parseFindings → suppress → render`
  pipeline as the shelled-out scanners, sidestepping QProcess. Pinned
  by `tests/features/feature_coverage/` (17 invariants across token
  extraction shapes, stopword filtering, top-section isolation,
  bullet-section tagging, and both fuzzy-match paths).
- **Remote-control — `launch` command.** Seventh and final command in
  the initial rc_protocol surface. Convenience wrapper that spawns a
  command in a new tab — sugar for the `idx=$(... new-tab) && ...
  send-text --remote-tab $idx --remote-text $cmd$'\n'` pattern,
  collapsed into one round-trip.

      {"cmd":"launch","cwd":"<path optional>","command":"<string required>"}
      -> {"ok":true,"index":<int>}

  `command` is required (empty / missing yields a documented error
  pointing at `new-tab` for the bare-shell case). Auto-appends `\n`
  to the command if not already present — the convenience
  differentiator from `new-tab`, which stays byte-faithful for
  scripts that compose multi-line input or send control sequences.

      ants-terminal --remote launch --remote-command 'htop'
      ants-terminal --remote launch --remote-cwd /tmp --remote-command 'tail -f log'

  Pinned by `tests/features/remote_control_launch/`.
- **Remote-control — `get-text` command.** Sixth rc_protocol command.
  Returns the trailing N lines of (scrollback + screen) joined with
  `\n` — so scripts can capture output, dispatch on visible state,
  or grab the last command's result.

      {"cmd":"get-text","tab":<int optional>,"lines":<int optional>}
      -> {"ok":true,"text":"<string>","lines":<int>,"bytes":<int>}

  Default 100 lines; capped server-side at 10 000 so a runaway
  `--remote-lines 1000000` against a million-line scrollback can't
  blow up the JSON envelope. Reuses
  `TerminalWidget::recentOutput(int)` — already the AI dialog's
  context-capture accessor, so format stays consistent across both
  consumers. Client CLI adds `--remote-lines <n>` (validated via
  `toInt(&ok)`; non-numeric or negative values reject before the
  socket call).

      ants-terminal --remote get-text                                   # default 100
      ants-terminal --remote get-text --remote-lines 24                 # screen-ish
      ants-terminal --remote get-text --remote-tab 0 --remote-lines 500 # specific tab

  Pinned by `tests/features/remote_control_get_text/` — 6 invariants
  including the 10 000-line cap, the response field set
  (`text`/`lines`/`bytes`), and the client-side `--remote-lines`
  validation.
- **Remote-control — `set-title` command.** Fifth rc_protocol command.
  Pins a tab label that survives both the per-shell `titleChanged`
  signal (OSC 0/2 from the inferior) and the 2 s `updateTabTitles`
  refresh — exactly the behaviour you want for "label this tab `prod`
  while I work on it" workflows.

      {"cmd":"set-title","tab":<int optional>,"title":"<string>"}
      -> {"ok":true,"index":<int>}

  Empty `title` clears the pin and the auto-title path resumes:
  under `tabTitleFormat == "title"` (default) the most recent
  shell-provided title is restored from `TerminalWidget::shellTitle()`
  immediately rather than waiting for the next OSC 0/2; under
  cwd / process formats `updateTabTitles()` rebuilds from current
  state. Pin storage is keyed by the tab's `QWidget*` and freed
  alongside `m_tabSessionIds` at tab-close time so the
  `QHash<QWidget*, QString>` doesn't accumulate dangling keys.

  Client CLI adds `--remote-title <str>` (`isSet()`-gated, so an
  empty string legitimately clears the pin):

      ants-terminal --remote set-title --remote-title '★ prod'
      ants-terminal --remote set-title --remote-tab 2 --remote-title 'logs'
      ants-terminal --remote set-title --remote-title ''   # clears pin

  Pinned by `tests/features/remote_control_set_title/` — 8 invariants
  including the dual signal + timer guards, the tab-close cleanup,
  and the empty-title-restores-via-shellTitle behaviour.
- **Remote-control — `select-window` command.** Fourth rc_protocol
  command. Switches the active tab to a given 0-based index.

      {"cmd":"select-window","tab":<int required>}
      -> {"ok":true,"index":<int>}

  `tab` is required — no implicit default-to-active, the caller
  always spells out which tab they mean. Validated via
  `QJsonValue::isDouble()` so a string-typed `tab` field doesn't
  silently become tab 0. After the switch, focus is restored to the
  new active terminal via `QWidget::setFocus()` so follow-up
  `send-text` calls without an explicit `tab` field land on the
  expected pane. Client CLI reuses the existing `--remote-tab` option:

      ants-terminal --remote select-window --remote-tab 2

  Pinned by `tests/features/remote_control_select_window/` — 6
  invariants including the required-tab contract, the isDouble
  check, and the setFocus-on-switch behaviour.
- **Remote-control — `new-tab` command.** Third rc_protocol command.
  Opens a fresh tab and returns its 0-based index, so callers can chain
  into `send-text --remote-tab <i>` without guessing.

      {"cmd":"new-tab","cwd":"<path optional>","command":"<string optional>"}
      -> {"ok":true,"index":<int>}

  `cwd` omitted = inherit from focused/current terminal (same default
  as the menu-driven `newTab()` slot). `command` written to the new
  shell after a 200 ms settle via `TerminalWidget::writeCommand`
  (matches `onSshConnect` timing — shells drop keystrokes written
  before their init settles). Caller owns the trailing newline,
  matching `send-text` semantics for one consistent rule across every
  rc_protocol write-path. Client CLI adds `--remote-cwd <path>` and
  `--remote-command <str>`:

      ants-terminal --remote new-tab
      ants-terminal --remote new-tab --remote-cwd /tmp
      ants-terminal --remote new-tab --remote-cwd /tmp --remote-command 'pwd'

  Idiomatic chaining:

      idx=$(ants-terminal --remote new-tab --remote-cwd /tmp | jq .index)
      ants-terminal --remote send-text --remote-tab "$idx" \
          --remote-text $'ls -la\n'

  Pinned by `tests/features/remote_control_new_tab/` — 7 invariants
  including the 200 ms settle, the `QPointer<TerminalWidget>` guard
  on the deferred lambda (tab-close between enqueue and fire), and
  the non-const `int newTabForRemote(...)` signature the dispatch
  layer depends on.
- **Remote-control — `send-text` command.** Second rc_protocol command
  on top of the scaffold shipped in the previous Unreleased entry.
  Writes a UTF-8 string byte-for-byte to a tab's PTY master — control
  chars and escape sequences pass through unchanged, matching
  [Kitty's rc_protocol `send-text`](https://sw.kovidgoyal.net/kitty/rc_protocol/#send-text).
  Request shape: `{"cmd":"send-text","tab":<int optional>,"text":"<string>"}`;
  `tab` omitted targets the active tab. Response on success:
  `{"ok":true,"bytes":<int>}`. Client CLI adds `--remote-tab <i>` and
  `--remote-text "<string>"`; when `--remote-text` is absent, the
  client reads stdin until EOF — enables shell piping without
  inline-arg quoting pain:

      ants-terminal --remote send-text --remote-text 'echo hi\n'
      ants-terminal --remote send-text --remote-tab 0 --remote-text ''
      printf 'ls\n' | ants-terminal --remote send-text

  Does *not* auto-append a newline (matches Kitty; auto-newline would
  surprise binary-stream callers). `tab` is disambiguated via JSON
  `isDouble()` rather than `toInt() == 0` so `--remote-tab 0` stays
  meaningful. Locked by new source-grep test
  `tests/features/remote_control_send_text/` (7 invariants including
  the no-auto-newline negative guard and the stdin-read ergonomic).
- **Remote-control protocol — first slice** (`src/remotecontrol.{h,cpp}`).
  Kitty-style JSON-over-Unix-socket RPC. A `QLocalServer` listens on
  `$ANTS_REMOTE_SOCKET` or, by default, `$XDG_RUNTIME_DIR/ants-terminal.sock`
  (falling back to `/tmp/ants-terminal-<uid>.sock` when XDG runtime is
  unset). Each connection handles one request / one response, LF-terminated.
  The only command in this slice is `ls`, which returns
  `{"ok": true, "tabs": [{"index", "title", "cwd", "active"}, ...]}`.
  Unknown commands return `{"ok": false, "error": ...}` with client exit
  code 2. The same binary doubles as the client via
  `--remote <cmd>` (`--remote-socket <path>` overrides the socket).
  Clean-foundation release — the remaining commands (`send-text`,
  `set-title`, `new-tab`, `select-window`, `get-text`, `launch`) and the
  X25519 auth layer land in follow-up commits against this scaffold.
  Locked against future drift by `tests/features/remote_control_ls/`
  (8 source-grep invariants including field-name stability and the
  `--remote`-before-`MainWindow`-construction ordering).
- **Main-thread stall detector** (Debug Mode → Perf category). A 200 ms
  heartbeat `QTimer` in `MainWindow` measures the gap between its own
  firings; when the wall-clock gap exceeds the scheduled interval by
  more than 100 ms the event loop was blocked and the log records
  the drift, cumulative count, and worst-case observed so far. Gated
  by `ANTS_DEBUG=perf` (or `all`) so the detector is only armed when
  debugging — zero overhead when off, zero log output when no stalls
  are happening. Addresses the 2026-04-20 follow-up user report —
  "slow down experienced at various times, when tab has been clear
  or with lots of text, not one specific scenario." Intermittent
  slowdowns that span both empty and full tabs point at a periodic
  background handler rather than the PTY hot path, and this detector
  will fingerprint the exact blockage when the user next reproduces
  the slowdown.
- **VT throughput benchmark harness** at `tests/perf/bench_vt_throughput.cpp`.
  Drives four fixed corpora (`ascii_print`, `newline_stream`, `ansi_sgr`,
  `utf8_cjk`) through `VtParser` → `TerminalGrid::processAction` at `-O2`,
  emits CSV per corpus (bytes, actions, wall-ms, MB/s, actions/s). First
  step of the 0.8.0 "Terminal throughput slowdowns" perf investigation
  (user report 2026-04-20). Registered under `ctest` label `perf` so it
  runs only on explicit `ctest -L perf`; default `fast` suite stays under
  two seconds. Baseline on dev laptop: `ascii_print` 23 MB/s,
  `newline_stream` 5 MB/s (4× slower — scroll/scrollback is the hotspot
  to profile next), `ansi_sgr` 17 MB/s, `utf8_cjk` 8 MB/s.

### Changed

- **Dropdown-flicker — extended intra-action suppression** to
  `QEvent::HoverMove` / `HoverEnter` / `HoverLeave` in
  `MainWindow::eventFilter`. The 0.7.5 `NoAnimStyle` fix only partially
  reduced the user-reported flicker (mouse-movement-triggered);
  Qt's style engine consults `WA_Hover` tracking — a separate channel
  from `QMouseEvent` — to re-evaluate `:hover` on every cursor tick.
  Without suppressing HoverMove the menubar was still repainting on
  every pixel of intra-item mouse motion. Cross-item motion
  (File → Edit) still passes through so menu switching works.
  **Residual flicker is still visible** (confirmed by the user after
  shipping this change) — pushed to the 0.8.0 roadmap's
  "Carried over from 0.7.x" section for a different-angle fix.

### Fixed

- **`new-tab` honours its documented "caller owns newline" contract.**
  The first ship of `new-tab` used `TerminalWidget::writeCommand` for
  the deferred command write, which always auto-appends `\n` —
  contradicting the documented send-text-style "caller owns the
  trailing newline" semantics. `MainWindow::newTabForRemote` now
  uses `sendToPty` (raw bytes) instead. The convenience case lives
  in the new `launch` command (which auto-appends explicitly), so
  callers who wanted "open + run" can switch with no behaviour
  change. Pinned by an `INV-5 (neg)` rule in
  `tests/features/remote_control_launch/` against any future
  re-introduction of `writeCommand` in `newTabForRemote`.


## [0.7.5] — 2026-04-20

**Theme:** same-day follow-up to 0.7.4 that resolves the dropdown-menu
flicker the 0.7.4 "Known Issue" section called out. Root cause finally
identified via the Debug Mode instrumentation that shipped in 0.7.4:
Fusion's `QWidgetAnimator` was creating a 60 Hz
`QPropertyAnimation(target=QWidget, prop=geometry)` cycle on the idle
window (1439 animation completions in a 23 s idle log). Each cycle
drove a full `LayoutRequest → UpdateRequest → Paint` cascade across
every widget, which the user saw as dropdown flicker on mouse hover.
Neither `QApplication::setEffectEnabled(UI_AnimateMenu, false)` nor
`QMainWindow::setAnimated(false)` covers the style-hint-driven cycle
— only overriding `QStyle::SH_Widget_Animation_Duration` does.

### Fixed

- **Dropdown-menu flicker (root cause)** — introduced a `NoAnimStyle`
  `QProxyStyle` in `src/main.cpp` that zeroes
  `SH_Widget_Animation_Duration` and `SH_Widget_Animate`. Wrapped
  around the Fusion style at application startup
  (`app.setStyle(new NoAnimStyle(QStyleFactory::create("Fusion")))`).
  Idle-log result: **QPropertyAnimation events per second:
  0** (was ≈62 Hz in 0.7.4). Total event volume dropped >99 %
  (29 096 → 62 log lines in 4 s idle). Pinned by the new
  `no_anim_style` feature test (`tests/features/no_anim_style/`).
- **Debug Mode — animation-creation hook** switched from
  `QEvent::ChildAdded` to `QEvent::ChildPolished`. At `ChildAdded`
  time the child's vtable still points at `QObject` (the derived
  constructor hasn't run), so `child->metaObject()->className()`
  returns `"QObject"` and the QPropertyAnimation filter never fires.
  `ChildPolished` fires after full construction. The old hook was
  silently useless on the 0.7.4 investigation — this one actually
  logs creation sites.

## [0.7.4] — 2026-04-20

**Theme:** a session's worth of user-facing UI fixes, the introduction
of a comprehensive debug-logging system, and the debt-cleanup that
came out of both. The centerpiece is the new **Debug Mode** (Tools
→ Debug Mode), which was what let us pin a ~54 Hz full-window
repaint cascade that had been driving the menubar / dropdown /
paint-dialog flicker everyone had been chasing for multiple
releases. Fixes to the Command Palette, Paste dialog, TitleBar,
QMainWindow animator, Qt UI effects, `QMenuBar`'s hover rule, and
several smaller items all landed here.

### Added

- **Debug Mode** (`src/debuglog.{h,cpp}` — ~200 lines). A single
  `ANTS_LOG(category, ...)` macro plus 14 independent category
  toggles (Paint, Events, Input, PTY, VT, Render, Plugins, Network,
  Config, Audit, Claude, Signals, Shell, Session). Categories can
  be enabled narrowly (one) or broadly (all) via either
  `ANTS_DEBUG=<list>` at launch (comma-separated names, or `all`,
  or `1` for legacy paint-only) or **Tools → Debug Mode →
  [checkable submenu]** at runtime. Output is timestamped and
  written to `~/.local/share/ants-terminal/debug.log` (append mode,
  one header per process start with PID + active category mask);
  stderr mirror only when the env var was used, so `2>file.log`
  redirection still works. Menu also offers **Open Log File** (xdg-
  opens the log in the system viewer) and **Clear Log File**.
  Hot-path cost when disabled is a single bit-test on a static
  `quint32`. Replaces the ad-hoc `ANTS_PAINT_LOG` diagnostic hack
  that was briefly landed mid-session.
- **OSC 133 prompt-on-new-line guard** in
  `TerminalGrid::processAction`'s OSC 133 `A` handler. When a shell
  emits `ESC ] 133;A ST` at the start of a new prompt, the terminal
  now nudges the cursor to column 0 of the next line first if the
  previous output didn't end with a newline (e.g.
  `cat file.json` whose last byte is `}`). Fixes the long-standing
  "prompt glued onto the end of the previous command's output"
  UX papercut. Equivalent to zsh's `PROMPT_SP` shell-side option,
  but works for bash / fish / any shell that emits OSC 133 A.
  Requires the shell-integration scripts at
  `packaging/shell-integration/ants-osc133.{bash,zsh}` to be
  sourced; without OSC 133, the terminal has no way to know a
  prompt is starting.

### Fixed

- **Menubar File / Edit / View hover still flashed** on mouseover
  despite the 0.6.42 focus-redirect guards and the 0.6.43
  `QMenuBar::item` base stylesheet rule. Two remaining leaks:
  (a) the QMenuBar inherits `Qt::WA_TranslucentBackground` from the
  MainWindow, which disables auto-fill for the widget tree, so each
  hover repaint briefly rendered over cleared-to-transparent pixels;
  (b) Qt's QStyleSheetStyle treats `QMenuBar::item:hover` as a
  distinct pseudo-state from `:selected`, and the Breeze / Fusion
  paths can hover-flash for one frame before selection engages —
  on that frame, only `:hover` styling applied, and `:hover` was
  never defined. Fix sets `autoFillBackground(true)` +
  `Qt::WA_StyledBackground` on the QMenuBar so the stylesheet
  background-color paints reliably, mirrors the `:selected`
  highlight into `:hover`, and makes `setNativeMenuBar(false)`
  explicit so DE global-menu integrations never hide the menubar
  in our frameless window.
- **Paste-confirmation dialog: Paste button swallowed mouse clicks**
  when focus wasn't already on it — only the `&Paste` Alt-mnemonic
  path worked. Root cause under a frameless + translucent main
  window on KWin: any dialog that goes through `QDialog::exec()`
  inherits Qt's `Qt::ApplicationModal` transition, which doesn't
  compose well with our `QApplication::focusChanged` redirect on
  the frameless parent — same incompatibility the Review-Changes
  dialog hit at 0.6.29. Rewrote `pasteToTerminal()` to use the
  Review-Changes shape exactly: heap-allocated `QDialog` with
  `Qt::WA_DeleteOnClose`, own `QHBoxLayout` of `QPushButton`s
  (Cancel default, Paste `setAutoDefault(false)`), `clicked`
  lambda calls `performPaste()` and closes. No blocking; caller
  returns immediately. Previous attempts via `QMessageBox::exec()`,
  custom `QDialog::exec()`, and `show() + QEventLoop` all hit the
  same modal-vs-frameless regression.
- **Command Palette QListWidget continuous-timer churn.** The
  hidden `QListWidget` inside `CommandPalette` was running its
  internal `QAbstractItemView` machinery (layout scheduler,
  selection animation, view-update timer) from MainWindow
  construction onward, even though the palette itself was hidden.
  Debug-log measurement showed the timer firing at ~50 Hz,
  cascading LayoutRequest → UpdateRequest → full widget-tree
  paint. Fix: lazy-create the list in `CommandPalette::show()`
  so the view doesn't exist until the user first presses
  Ctrl+Shift+P. Zero `QAbstractItemView` timers at idle.
- **QMainWindow's built-in `QWidgetAnimator`** was enabled by
  default (`setAnimated(true)`). It exists to animate dock-widget
  rearrangements; Ants has no dock widgets, but the animator
  still ran at idle. `setAnimated(false)` kills the continuous
  `QPropertyAnimation(target=QWidget, prop=geometry)` cycle that
  was cascading a LayoutRequest through the whole widget tree on
  every frame.
- **Qt UI effect animations** (`Qt::UI_AnimateMenu` /
  `UI_FadeMenu` / `UI_AnimateCombo` / `UI_AnimateTooltip` /
  `UI_FadeTooltip` / `UI_AnimateToolBox`) disabled globally at
  startup via `QApplication::setEffectEnabled(…, false)`. Menu
  show/hide, combobox dropdown, tooltip fade, and toolbox expand
  all went through `QPropertyAnimation(geometry)` at 60 Hz — each
  animation frame triggered a LayoutRequest cascade. We're a
  precision terminal, not a presentation app; these animations
  were pure cost with no user benefit.
- **TitleBar button tooltips** (Center / Minimize / Maximize /
  Close) removed. A Qt / KWin / Wayland quirk re-fired the
  `QTipLabel` show/hide cycle on every frame while the cursor was
  near a titlebar button, animating the tooltip's geometry at
  ~60 Hz, cascading LayoutRequest through the whole widget tree.
  Diagnosed via `ANTS_DEBUG=paint,events` — the log pinpointed
  `cls=QTipLabel name=qtooltip_label parent=QToolButton:closeBtn`
  as the repaint source. The button glyphs (✥ / ✕ / ⬜ / ⟩) + the
  hover-colour feedback give enough affordance without tooltip
  text.
- **`TerminalWidget` switched from `QOpenGLWidget` to plain
  `QWidget`.** The GL widget composites its FBO through the
  parent's backing store and the composition path always
  invalidates the top-level window — we chased this at length via
  `PartialUpdate`, `WA_OpaquePaintEvent`, `swapInterval(0)`,
  `setAlphaBufferSize` opt-out, etc. None fixed it. Switching the
  base class eliminates the GL composition path entirely. The
  default QPainter path — already ligature-aware via QTextLayout
  / HarfBuzz — is unaffected. The optional GL glyph-atlas
  renderer (`gpu_rendering` config) is dormant in 0.7.4 and
  returns in a future refactor as an embedded `QOpenGLWindow` via
  `createWindowContainer` (the only shape that cleanly decouples
  GL composition from the parent's backing store).

### Tests

- `tests/features/menubar_hover_stylesheet/` — pins the
  `QMenuBar::item:hover` rule, `autoFillBackground(true)`,
  `Qt::WA_StyledBackground`, the `QMenuBar::item` base rule, and
  `setNativeMenuBar(false)` for the menubar hover no-flash contract.
- `tests/features/paste_dialog_custom/` — pins the async
  `pasteToTerminal()` pattern: `new QDialog(this)` on the heap,
  `Qt::WA_DeleteOnClose`, `QDialogButtonBox::Ok+Cancel`, Cancel
  `setDefault(true)`, Paste `setAutoDefault(false)`, the
  `show() + raise() + activateWindow()` activation dance, explicit
  `setFocus` on a button, `QPointer<TerminalWidget>` tab-close
  guard, `performPaste()` helper, no `QEventLoop`, no
  `QDialog::exec()`. Forbids the old synchronous
  `bool confirmDangerousPaste(...)` API from reappearing.
- `tests/features/terminal_partial_update_mode/` — pins
  `TerminalWidget : public QWidget` (not QOpenGLWidget) and the
  absence of `QOpenGLWidget::*` / `makeCurrent` calls in the `.cpp`.

### Known issue

A residual dropdown-flicker cycle driven by a
`QPropertyAnimation(target=QWidget, prop=geometry)` still runs in
some idle states on KWin + Qt 6. The per-iteration fixes listed
above resolved identifiable sources (CommandPalette QListWidget,
TitleBar tooltip, QMainWindow animator, Qt UI effects); what
remains is a Qt-internal animation we have not yet pinpointed. The
new Debug Mode (Tools → Debug Mode) is the instrument to hunt it
with — enable `paint` + `events` and grep the log for
`QPropertyAnimation CREATED` lines.

## [0.7.3] — 2026-04-20

**Theme:** H6.1 of the 📦 distribution-adoption plan — Lua plugins
inside Flatpak. The 0.7.2 manifest shipped with plugin support
disabled because `org.kde.Sdk//6.7` does not carry `lua54-devel`
and `flathub/shared-modules` has no Lua 5.4 entry today. 0.7.3
closes that gap with an in-manifest Lua archive module so a
Flatpak build loads plugins the same way the openSUSE, Arch, and
Debian builds do.

### Added

- **Lua 5.4 archive module in the Flatpak manifest**
  (`packaging/flatpak/org.ants.Terminal.yml`). A dedicated `lua`
  module appears before `ants-terminal` so the CMake configure
  step sees `/app/include/lua.h` + `/app/lib/liblua.a` and takes
  the `LUA_FOUND=TRUE` branch. Built from
  `https://www.lua.org/ftp/lua-5.4.7.tar.gz` with a pinned
  `sha256` and the `linux-noreadline` target — the terminal only
  statically links `liblua.a`, so pulling readline into the
  sandbox would be pure bloat. `MYCFLAGS="-fPIC"` keeps the
  static library PIE-safe for linking into the `ants-terminal`
  executable. Installed to `/app` via
  `make install INSTALL_TOP=/app`, which is exactly where
  CMake's `FindLua` searches by default when
  `CMAKE_INSTALL_PREFIX=/app`.
- **Auto-refresh of the Lua tarball hash via
  `flatpak-external-data-checker`.** The Lua module carries an
  `x-checker-data:` stanza pointing at
  <https://www.lua.org/ftp/> with
  `version-pattern: lua-(5\.4\.\d+)\.tar\.gz` and
  `url-template: https://www.lua.org/ftp/lua-$version.tar.gz`.
  Flathub CI opens a PR against the Flathub repo with a
  refreshed `url` + `sha256` on each Lua 5.4.x point release —
  no manual hash churn per bump, and 5.5.x majors are correctly
  excluded (they would break the in-source `find_package(Lua 5.4)`
  floor).
- **Feature test `tests/features/flatpak_lua_module/`.** Source-grep
  regression against the manifest YAML pinning six invariants:
  Lua module precedes ants-terminal (order matters —
  flatpak-builder evaluates modules in sequence, and CMake's
  `FindLua` runs during ants-terminal configure), `type: archive`
  with a pinned `sha256:`, `MYCFLAGS="-fPIC"` in the build
  commands, `make install INSTALL_TOP=/app`, the `linux-noreadline`
  build target (guarding against the default `make linux` that
  aliases to `linux-readline`), and the `x-checker-data` stanza
  with the 5.4.x version-pattern. A regression that strips any
  of these fails at ctest time.

### Changed

- `packaging/flatpak/README.md`: the "Lua plugins — limitation"
  section is replaced with a "Lua plugins" section documenting the
  enabled path (archive-module build, `linux-noreadline` rationale,
  `x-checker-data` automation, success criterion of `PluginManager`
  loading a plugin inside the sandbox).
- Manifest header comment updated to describe the in-manifest
  module and the automated hash-refresh path, superseding the
  "not wired up" note that shipped in 0.7.2.
- `ROADMAP.md` H6.1 flipped from 📋 to ✅ (shipped in 0.7.3).

## [0.7.2] — 2026-04-19

**Theme:** H6 of the 📦 distribution-adoption plan — Flatpak packaging.
Ships the `org.ants.Terminal.yml` manifest and the PTY wiring Flatpak
needs so a second tab's shell actually sees the host's `$PATH` and
filesystem. One artifact that runs on every distro unblocks the
"packaged everywhere" gating item without per-distro maintenance.

### Added

- **Flatpak manifest** (`packaging/flatpak/org.ants.Terminal.yml`).
  Targets `org.kde.Platform//6.7` — the KDE SDK brings cmake, ninja,
  and every Qt6 component the build needs (Core, Gui, Widgets,
  Network, OpenGL, OpenGLWidgets, DBus) so the manifest's `modules`
  block is a single `buildsystem: cmake-ninja` entry. `finish-args`
  cover Wayland + fallback-X11 + DRI, `--share=network` (AI endpoint
  + SSH outgoing), portal access (`org.freedesktop.portal.*` covers
  global shortcuts + file dialogs), desktop notifications, and
  `xdg-config/ants-terminal:create` + `xdg-data/ants-terminal:create`
  so config and data land under standard XDG paths. The manifest is
  ready to re-point from `type: dir / path: ../..` to
  `type: git / url / tag` for Flathub submission — local `flatpak-
  builder --install --user` builds end-to-end against the in-tree
  source today. See [packaging/flatpak/README.md](packaging/flatpak/README.md).
- **Flatpak host-shell wiring in `ptyhandler.cpp`.** The forked
  child now probes `FLATPAK_ID` (set by the flatpak launcher) and
  `/.flatpak-info` (present in every sandbox regardless of launch
  path); either signal triggers a branch that exec's the user's
  shell via `flatpak-spawn --host` instead of direct `execlp`. The
  sandbox doesn't inherit env or cwd across the host boundary, so
  `TERM`, `COLORTERM`, `TERM_PROGRAM`, `TERM_PROGRAM_VERSION` (pulled
  from the `ANTS_VERSION` compile definition, not a literal), and
  `COLORFGBG` are forwarded explicitly via `--env=KEY=VALUE`; the
  requested working directory crosses via `--directory=<workDir>`
  gated on `!workDir.isEmpty()`. `_exit(127)` after `execvp`
  matches the direct-exec fallback's failure shape for a missing
  shell. This is the same PTY model Ghostty's Flathub build uses —
  the terminal emulator stays sandboxed (VT parser, renderer,
  plugin VM all inside the confined runtime); only the child shell
  escapes so the user's `$PATH`, `$HOME`, and tooling are
  reachable. Outside Flatpak the existing `execlp(shellCStr, argv0,
  nullptr)` path is hit byte-for-byte unchanged. Feature test
  `tests/features/flatpak_host_shell/` locks the branch shape:
  detection probes both signals in an OR, `--host` and `--`
  separators appear in argv, every TERM var is present as
  `--env=...`, workDir gating is correct, the direct-exec call site
  is preserved, `_exit(127)` is the post-exec fall-through.
- **`packaging/flatpak/README.md`.** Local build instructions
  (`flatpak-builder --install --user`), host-shell rationale,
  Flathub submission workflow (the tag-based manifest body differs
  from the in-tree `type: dir` shape), and the documented Lua-
  plugins-disabled limitation for the initial manifest — plugin
  support returns via a shared-modules Lua entry once the tarball-
  sha256 refresh cadence is wired up.
- **`packaging/README.md` Flatpak section.** Layout tree adds
  `flatpak/`; new "Flatpak — `org.ants.Terminal.yml`" section
  mirrors the openSUSE / Arch / Debian sections; shared-concerns
  header updated from "three recipes" to "four recipes".

### Changed

- `ROADMAP.md` H6 flipped from 📋 to ✅ (manifest shipped; Flathub
  submission is the residual 📋); distribution-adoption table
  entry updated to reflect the split shipped/remaining state.

## [0.7.1] — 2026-04-19

**Theme:** first of the 0.8.0 🎨 multiplexing items — SSH ControlMaster
auto-integration from the bookmark dialog. A second tab to the same
host now piggybacks on the first tab's session, skipping the full
auth handshake. Matches the precedent set by kitty's `ssh` kitten,
Warp's SSH blocks, and iTerm2's SSH profiles.

### Added

- **SSH ControlMaster auto-multiplexing**. Connects opened from the
  SSH Manager dialog now carry
  `-o ControlMaster=auto`,
  `-o ControlPath=$HOME/.ssh/cm-%r@%h:%p`, and
  `-o ControlPersist=10m` when the new `ssh_control_master` config
  key is true (default). Opt-out via
  `~/.config/ants-terminal/config.json` for sites that forbid
  lingering multiplex sockets by policy. `$HOME` is resolved in
  C++ via `QDir::homePath()` so the ControlPath works under dash /
  POSIX `sh` (neither does tilde expansion on command-arg
  `foo=~/…`); `%r@%h:%p` are OpenSSH ControlPath substitution
  tokens, not shell metacharacters, so the existing `shellQuote()`
  helper leaves them intact. The three `-o` options precede the
  `[user@]host` destination for parity with OpenSSH's own
  documentation examples. Feature test
  `tests/features/ssh_control_master/` pins six invariants:
  default + explicit-false produce zero `Control*` tokens,
  explicit-true emits all three, `$HOME` resolution replaces any
  literal tilde, `%r@%h:%p` survives shell quoting, the flags
  precede the destination arg, and they coexist with legacy
  `-p`/`-i`/extraArgs without interference. See
  [ROADMAP.md §0.8.0](ROADMAP.md#080--multiplexing--marketplace-target-2026-08)
  — the item is carried forward from the 0.7.0 "SSH
  ControlMaster" roadmap bullet and lands ahead of the larger
  mux-server / remote-control work.

## [0.7.0] — 2026-04-19

**Theme:** shell integration + triggers. Consolidates the 0.6.1 →
0.6.45 arc into the release the 0.7 ROADMAP plotted out: command
blocks as first-class UI, `.cast` recording, semantic history,
HMAC-verified OSC 133 markers, the full iTerm2-parity trigger set,
SIMD VT parsing, a dedicated PTY read + VT parse worker thread,
Wayland-native Quake mode (layer-shell + GlobalShortcuts portal),
and the H1–H4 distribution slice (SECURITY.md, AppStream metainfo,
man page, shell completions) that takes Ants from "side project" to
"distro-ready." 0.7.0 is the minor-release rollup of 30 patch
releases; the individual per-feature entries below 0.7.0 remain
authoritative for the SHAs they shipped under.

### Added

#### 🎨 Shell integration

- **Command blocks as first-class UI** (Warp parity,
  [docs](https://docs.warp.dev/terminal/blocks)). OSC 133 prompt →
  command → output grouping; Ctrl+Shift+Up / Ctrl+Shift+Down jump to
  prev/next prompt; collapsible output with "… N lines hidden"
  summary; duration + timestamp in the prompt gutter; 2px pass/fail
  status stripe; per-block right-click menu (Copy Command, Copy
  Output, Re-run, Fold/Unfold, Share as `.cast`). Shipped across
  0.6.x; consolidated in §0.6.10.
- **Asciinema `.cast` v2 recording**
  ([spec](https://docs.asciinema.org/manual/asciicast/v2/)).
  Full-session via Ctrl+Shift+R; per-block export via the
  command-block context menu. §0.6.10.
- **Semantic history.** Ctrl-click on a `path:line:col` capture in
  scrollback opens the file at the cited position. CWD resolution
  via `/proc/<pid>/cwd`, so relative paths Just Work without shell
  cooperation. Editor support broadened beyond VS Code + Kate to
  VS Code family (`code-insiders`, `codium`), vi-family (`nvim`,
  `vim`), `nano`, Sublime / Helix / Micro, and JetBrains IDEs.
  §0.6.12.
- **Shell-side HMAC verification for OSC 133 markers.** When
  `$ANTS_OSC133_KEY` is set, every OSC 133 marker must carry an
  `ahmac=` param computed as `HMAC-SHA256(key, <marker>|<promptId>
  [|<exitCode>])`. Forged markers are dropped and a status-bar
  counter surfaces the count with a 5-second cooldown. Bash + zsh
  integration scripts under `packaging/shell-integration/`. §0.6.31.

#### 🔌 Triggers + plugin events

- **Full iTerm2-parity trigger rule set** with `instant` flag:
  `bell`, `inject`, `run_script`, `notify`, `sound`, `command`
  shipped in §0.6.9; `highlight_line`, `highlight_text`, and
  `make_hyperlink` shipped in §0.6.13 via a new `TerminalGrid`
  line-completion callback so matches map to exact column ranges
  on a real row before the row scrolls into scrollback.
- **OSC 1337 SetUserVar + `user_var_changed` event.** §0.6.9.
  Disambiguated from inline images by the byte after `1337;`.
  NAME ≤ 128 chars; decoded value capped at 4 KiB.
- **Command-palette plugin registration** via
  `ants.palette.register({title, action, hotkey})`. Always-on (no
  permission gate); optional global QShortcut. §0.6.9.
- **New plugin events:** `command_finished` (exit + duration),
  `pane_focused`, `theme_changed`, `window_config_reloaded`,
  `user_var_changed`, `palette_action`. §0.6.9.

#### ⚡ Performance

- **SIMD VT-parser scan.** Ground-state hot path now scans 16 bytes
  at a time via SSE2 (x86_64) / NEON (ARM64) for the next
  non-printable-ASCII byte, then bulk-emits `Print` actions for the
  safe run. A signed-compare trick (XOR 0x80 → two `cmpgt_epi8`
  against pre-computed bounds) flags any interesting byte with a
  single `movemask`. Regression guard:
  `tests/features/vtparser_simd_scan/` asserts byte-identical
  action streams across whole-buffer, byte-by-byte, and
  pseudo-random-chunk feeds over a 38-case corpus. §0.6.23.
- **Dedicated PTY read + VT parse worker thread.** PTY read and
  parse run on a `VtStream` `QThread`; parsed `VtAction` batches
  cross to the GUI over `Qt::QueuedConnection` and apply on the
  main thread (paint stays where it was). Back-pressure: at most
  8 batches (≈128 KB) in flight before the worker disables its
  `QSocketNotifier` and kernel flow control takes over; GUI
  re-enables on drain. Resize goes over
  `Qt::BlockingQueuedConnection` so winsize is current before the
  next paint. `ANTS_SINGLE_THREADED=1` kill-switch was retired
  once the new path baked out. §0.6.34 / §0.6.37.
- **Incremental reflow on resize.** Standalone lines that fit the
  new width get an in-place `cells.resize()` with default-attr
  padding or trailing-blank trim, skipping the allocation-heavy
  `joinLogical` / `rewrap` round-trip. Multi-line soft-wrap
  sequences still go through the full logic so correctness is
  preserved. §0.6.15.

#### 🖥 Platform — Wayland native

- **Layer-shell Quake mode.** `find_package(LayerShellQt CONFIG
  QUIET)` wires `LayerShellQt::Interface` when the
  `layer-shell-qt6-devel` package is installed;
  `MainWindow::setupQuakeMode()` promotes the window to a
  `zwlr_layer_surface_v1` at `LayerTop`, anchored top/left/right
  with exclusive-zone 0. XCB path preserved for X11. §0.6.38.
- **Freedesktop GlobalShortcuts portal integration.**
  `GlobalShortcutsPortal` client wraps the
  `org.freedesktop.portal.GlobalShortcuts` handshake
  (CreateSession → BindShortcuts → Activated) behind a single Qt
  signal. Wires the `toggle-quake` id on KDE Plasma 6 and
  xdg-desktop-portal-hyprland / -wlr; the in-app `QShortcut`
  fallback stays active on GNOME Shell. 500 ms debounce prevents
  focused double-fire. §0.6.39.

#### 🔒 Security

- **Plugin capability audit UI.** Settings → **Plugins** renders
  every declared permission as a checkbox per plugin; revocations
  persist to `config.plugin_grants[<name>]` and take effect at
  next plugin reload. §0.6.11.
- **Image-bomb defenses.** New `TerminalGrid::ImageBudget` tracks
  total decoded image bytes across the inline-display vector +
  the Kitty cache; cap is **256 MB per terminal**. Sixel rejects
  up front from declared raster size; Kitty PNG / iTerm2 OSC 1337
  reject post-decode. Inline red error text surfaces the
  rejection. The per-image `MAX_IMAGE_DIM = 4096` dimension cap
  remains in place. §0.6.11.

#### 📦 Distribution readiness (H1–H4)

- **H1 — `SECURITY.md` + `CODE_OF_CONDUCT.md`.**
  Coordinated-disclosure policy with supported-versions table,
  reporting channel (GitHub Security Advisory + encrypted email),
  disclosure timeline, severity rubric, in/out of scope lists.
  Contributor Covenant 2.1 verbatim with maintainer email + the
  private GitHub Security Advisory listed as conduct reporting
  channels. Clears the Debian / Fedora / Ubuntu security-team
  review gate. §0.6.16.
- **H2 — AppStream metainfo + polished desktop entry.**
  `packaging/linux/org.ants.Terminal.metainfo.xml` (AppStream 1.0
  with summary / description / releases / categories / keywords /
  OARS content rating / supports / provides / launchable) and
  `packaging/linux/org.ants.Terminal.desktop` (reverse-DNS id,
  tightened Keywords, StartupWMClass, Desktop Actions for
  NewWindow + QuakeMode). CMake install rules via
  `GNUInstallDirs` cover desktop / metainfo / six hicolor icons;
  CI runs `appstreamcli validate --explain` +
  `desktop-file-validate` on every push. §0.6.17.
- **H3 — Man page.** `packaging/linux/ants-terminal.1` in
  `groff -man` covering synopsis, every CLI flag, environment
  variables, files, exit status, bugs, authors, and see-also.
  CMake installs to `${CMAKE_INSTALL_MANDIR}/man1/`; CI lints the
  source with `groff -man -Tutf8 -wall`. §0.6.18.
- **H4 — Shell completions (bash / zsh / fish).** Each shell
  completion installed to the conventional vendor path
  (`bash-completion/completions/`, `zsh/site-functions/`,
  `fish/vendor_completions.d/`); all three are auto-discovered on
  system-wide installs. CI lints each file with the matching
  shell's parse-only flag. Closes the H1–H4 distribution slice;
  remaining packaging work (H5 distro recipes landed in §0.6.20;
  H6 Flatpak, H7 docs site, H13 distro outreach) lives in 0.8.0.
  §0.6.19.

#### 🧰 Dev experience — Project Audit polish

- **Qt rule: unbounded callback payloads**
  (`unbounded_callback_payloads`, §0.6.8).
- **Qt rule: `QNetworkReply::connect` without context object**
  (`qnetworkreply_no_abort`, §0.6.8).
- **Observability rule: silent `catch(...)`** (`silent_catch`,
  §0.6.7).
- **Self-consistency: fixture-per-`addGrepCheck`**
  (`audit_fixture_coverage`, §0.6.6), CI-enforced via
  `tests/audit_self_test.sh`.
- **Build-flag recommender** (`missing_build_flags`, §0.6.7).
- **No-CI check** (`no_ci`, §0.6.7).
- **build-asan CI lane** — ctest + binary smoke under ASan/UBSan
  on every push. §0.6.7.
- **`CONTRIBUTING.md`** — derived from `STANDARDS.md`; covers
  build modes, test layout, adding an audit rule, version-bump
  checklist. §0.6.7.
- **`actions/checkout` v4.2.2 → v5.0.1.** Runs on Node 24,
  pre-empting GitHub's 2026-06-02 Node 20 deprecation. Both CI
  pin sites SHA-pinned with `# v5.0.1` humans-readable comment.

### Changed

- **ROADMAP 0.7.0 theme** — every 📋 item on the list moves to ✅.
  Deferred items (EGL swap-with-damage on GL, Domain abstraction,
  persistent workspaces, Kitty Unicode-placeholder graphics) are
  explicitly re-scoped to 0.8.0+ so the 0.7 minor bump reflects
  the work that actually shipped.

### Notes

- 0.7.0 carries no breaking API changes. Every plugin that loads
  against 0.6.45 loads against 0.7.0. The minor bump reflects the
  accumulated *theme* delta since 0.6.0 shipped five days ago —
  not a manifest or `ants.*` schema break.
- Because 0.7.0 is a rollup, this entry deliberately duplicates
  individual 0.6.x bullets rather than linking through. The
  0.6.x entries below remain authoritative for the SHAs +
  commits they landed in.

## [0.6.45] — 2026-04-19

**Theme:** Deferred VT-standards items from the 10th-audit research
report, plus atomic-write hardening for long-lived audit state.
11th audit (this release) returned 0/~103 actionable — baseline
remains clean.

### Added

- **OSC 10 / 11 / 12 colour queries.** Apps (delta, neovim, bat,
  lazygit, fzf, jj, and most `termenv`-based Go TUIs) probe the
  terminal for default fg / bg / cursor colour with `\e]10;?\e\\` etc.
  to auto-detect dark vs light themes without relying on the
  unreliable `COLORFGBG` env var. Response is the xterm 16-bit-
  per-channel form `\e]<n>;rgb:RRRR/GGGG/BBBB\e\\`. Only the query
  form (`?` payload) is honoured — the set form is deliberately
  silently dropped to keep default fg/bg theme-driven and prevent
  in-terminal theme injection.
- **DECRQSS (Request Status String).** DCS `$q` requests for
  `DECSTBM` (scroll-region, `r`), `SGR` (current attributes, `m`),
  and `DECSCUSR` (cursor shape, ` q`) now reply with `DCS 1 $ r …`
  / `DCS 0 $ r …` per the xterm spec. tmux, neovim, and kitty's
  kitten use these to save-and-restore state around their own
  output. Unknown settings reply "invalid" rather than dropping
  silently so callers fall back to defaults. Sixel DCS payloads
  continue to route to the image handler — the DECRQSS branch
  gates on the `$q` prefix before Sixel's `q` search.

### Changed

- **Atomic JSON writes via `QSaveFile`** for four long-lived files:
    - `audit_rule_quality.json` — per-fire/per-suppression history
      (written frequently; corruption loses every rule score).
    - `.audit_cache/trend.json` — cumulative per-run severity totals.
    - `.audit_cache/baseline.json` — baseline fingerprints anchoring
      the "new findings only" filter.
    - `.audit_suppress` v1 → v2 migration rewrite (the simple append
      path stays non-atomic on purpose — one-line writes; loader
      already skips malformed lines).
  Torn writes from crash / `kill -9` now can't corrupt any of these.
  `config.json` and session snapshots already had a tighter
  fsync-then-rename pattern; those were left in place.

### Tests

- **`osc_color_query`** — locks OSC 10/11/12 response shape + the
  query-only gate (set form MUST NOT produce a response, preventing
  theme-injection via in-terminal programs).
- **`decrqss`** — locks the DECSTBM / SGR / DECSCUSR replies plus
  the invalid-reply bytes and a Sixel-routing regression guard so
  the new DECRQSS branch can't eat image traffic.

## [0.6.44] — 2026-04-19

**Theme:** Project Audit dialog — self-review improvements sourced from
the 10th audit run (2026-04-19). That audit returned 0/55 actionable
findings, and the nature of the 55 pointed at three bits of friction
the dialog was creating for itself: the dialog was auditing its own
output, every "is this noise?" verdict needed an individual click, and
users had no summary-level readout for "how much of this run was
signal?". All three are closed.

### Changed

- **Signal ratio surfaced in the summary banner.** `renderResults()`
  now computes an "X% actionable" figure — signal / (signal + noise)
  where noise = suppressions + AI-verdict FALSE_POSITIVE — and
  prints it below the severity pills with colour-coded context
  (green ≥60%, amber 20–59%, red <20%). A 30-day rolling average
  pulled from `RuleQualityTracker` sits next to it as a peer
  comparison point ("30d avg X% actionable, n=Y fires"). Makes the
  "should I bother scrutinising this run?" question answerable at
  a glance instead of requiring the user to count suppressions
  against the severity rows.
- **Auto-skip of prior audit artifacts in `isGeneratedFile()`.** Three
  new built-in skip rules, no project configuration needed:
    - `docs/AUTOMATED_AUDIT_REPORT_*.{json,md,html,txt}` — the
      previous-run artifacts contain SHA-256 dedup keys that gitleaks
      consistently flagged as high-entropy secrets (19 of 23 findings
      in the last run).
    - `.audit_cache/` and `audit_rule_quality.json` — the dialog's
      own baseline + self-learning ledger; auditing them is circular.
    - `tests/audit_fixtures/**` — scaffold data for the regex-rule
      self-test; `bad.*` files intentionally embed strings that
      third-party scanners misread as real findings (gitleaks picked
      up 4 of these last run).

### Added

- **Batch AI triage.** A `🧠 Triage visible (N)` button sits next to
  the confidence-sort toggle in the filter bar. Click it and every
  visible, not-yet-triaged finding goes to the configured LLM in
  one JSON request (hard cap 20 per batch; above that it slices
  and dispatches sequentially). Each batch's response is an array
  of `{key, verdict, confidence, reasoning}` — verdicts are spliced
  back onto the right findings by dedup key so a reordering model
  can't misalign results. Prompt-injection hardening inherited from
  the single-finding path (4-backtick snippet fencing, literal
  `` ```` `` scrub). Confirms the exact count before the POST;
  hidden entirely when AI isn't configured. The button's label
  re-calculates on every render so it always shows the current
  visible-untriaged count.

### Notes

- No schema changes. `audit_rule_quality.json` and
  `audit_rules.json` are read-compatible with prior versions.
- The 30-day actionable% uses `fires` as the denominator rather
  than run count, because run count isn't directly recorded —
  fires are a good enough proxy and the label makes the
  denominator explicit (`n=<count> fires`).

## [0.6.43] — 2026-04-19

**Theme:** Two user-reported regressions closed, plus a quality-of-life
default flip. The 0.6.42 focus-redirect guards didn't cure the menubar
hover flash — the real culprit was a stylesheet gap, not focus churn.
And users coming back to the app expected their tabs to be waiting for
them; session persistence now ships default-on, matching what every
modern terminal (iTerm2, Kitty, WezTerm, Konsole) does.

### Fixed

- **Menubar hover still flashed after the 0.6.42 fix.** The 0.6.42 pass
  narrowed focus-redirect firing but didn't address the real root
  cause. The stylesheet defined `QMenuBar::item:selected` without a
  matching base `QMenuBar::item` rule, so Qt fell back to the native
  style for the non-selected state and composited native item drawing
  under the `:selected` overlay. On hover transitions the native and
  stylesheet layers raced — visible as the flash. Added an explicit
  transparent-background `QMenuBar::item` base rule (with matching
  padding and border-radius) plus a `:pressed` rule so every menubar
  item state is owned entirely by the stylesheet. The 0.6.42 focus
  guards remain as defense-in-depth.
- **Tabs not remembered between sessions.**
  `Config::sessionPersistence()` defaulted to `false`, so users who
  never toggled Settings → General → "Save and restore sessions"
  lost their tab set on every quit. Flipped the default to `true`;
  users with `session_persistence: false` explicit in `config.json`
  still get their opt-out honored (only the absent-key path reads the
  fallback). Matches iTerm2 / Kitty / WezTerm / Konsole default
  behavior. The 5-second uptime floor in `saveAllSessions()` still
  protects against crash-on-launch wiping real state.

### Changed

- **`Config::sessionPersistence()` default changed from `false` to
  `true`.** First-run users now get tab restore out of the box.
  Only affects users with no `session_persistence` key in their
  config; explicit values are respected unchanged.

### Tests

- `tests/features/session_persistence_default/` — three-invariant
  feature test (spec.md + test_session_persistence_default.cpp) that
  locks in the truth table: missing key → true; explicit true → true;
  explicit false → false. Linked against `src/config.cpp` only (no
  GUI dependency). Verified against the pre-fix default by temporary
  revert — test fails red on `(default false)`, passes green on
  `(default true)`. Brings the feature-test count to 25, and the
  total ctest count to 26.

### Internal

- Cleaned up a `-Wshadow` warning at `mainwindow.cpp:3921` — the
  Review-Changes finalizer lambda's `const Theme &th` shadowed the
  outer scope's `th`. Renamed the lambda-local to `lth`, silencing
  the build-time note without functional change.
- Dropped an unused `<cstdlib>` include from the new session-
  persistence test (clangd lint flagged it).

## [0.6.42] — 2026-04-19

**Theme:** Focus-redirect guard rails. The 0.6.26 auto-return-to-terminal
behavior was firing through two narrow windows where it shouldn't —
hover-across-menubar produced a visibly flashing highlight, and the
paste-confirmation dialog's "Paste" button silently swallowed mouse
clicks (only the `&Paste` keyboard shortcut worked). Both user-visible
regressions traced back to the same `QApplication::focusChanged`
handler. New guards short-circuit the redirect when a popup is open,
when the cursor is over a menu/menubar, or when any top-level
`QDialog` is visible — the last one covers the `QMessageBox::exec()`
modality-handshake window where `activeModalWidget()` can briefly be
null.

### Fixed

- **Menubar File / Edit / View hover flash.** Moving the mouse across
  the menubar caused the hover highlight to flicker on every motion
  tick. Root cause: Qt synthesizes brief `focusChanged` cycles during
  menubar hover; `now` could momentarily park on a chrome widget not
  covered by the parent-chain exempt list, which triggered the
  auto-return-to-terminal redirect and wiped the menubar highlight.
  The focus-redirect lambda now early-returns when
  `QApplication::activePopupWidget()` is non-null (covers QMenu /
  QComboBox popup / tooltip) and when
  `QApplication::widgetAt(QCursor::pos())` has a `QMenu` or
  `QMenuBar` ancestor (covers the menubar, which is NOT a popup).
  Both guards are mirrored in the deferred `QTimer::singleShot(0,…)`
  fire-time block so a menu that opens between queue and fire is
  still honored.
- **Paste-confirmation dialog swallowed mouse clicks on "Paste".**
  Multi-line paste (or paste containing `sudo`, `| sh`, control
  characters) triggered the "Confirm paste" `QMessageBox`, but
  clicking the Paste button did nothing — only pressing `P` (the
  `&Paste` keyboard mnemonic) worked. Root cause:
  `QMessageBox::exec()` enters modal state inside a brief `show()`
  handshake during which `QApplication::activeModalWidget()` can
  return null for a tick; if a focusChanged event fired in that
  window, the redirect stole the button's focus mid-click and
  cancelled `clicked()`. The redirect now walks
  `QApplication::topLevelWidgets()` at queue time and early-returns
  on any visible `QDialog`, mirroring the check that already existed
  at fire time.

### Tests

- **`tests/features/focus_redirect_menu_guard/`** — new source-grep
  feature test pinning six invariants on the focusChanged lambda in
  `src/mainwindow.cpp`: the popup and menu-hover guards (queue +
  fire time), the visible-dialog walk at queue time, and the five
  existing guards that must not regress (`activeModalWidget`,
  `QAbstractButton`, `mouseButtons`, `QDialog` parent-chain,
  `CommandPalette`). Uses the balanced-brace scanner pattern
  (linear-time, constant stack) introduced in 0.6.41's
  `command_mark_gutter` test to avoid `std::regex` blowing the stack
  on a 4000+ line `mainwindow.cpp`.

## [0.6.41] — 2026-04-19

**Theme:** Quake-mode + OSC 133 UX polish. Surfaces the out-of-focus
portal binding state so users can tell at a glance whether their hotkey
escapes focus, and adds a scrollbar-adjacent tick-mark gutter that
shows OSC 133 command boundaries across the full scrollback — jump
targets for `Ctrl+Shift+Up/Down` that were previously invisible.

### Added

- **Command-mark gutter (`show_command_marks`, default on).** A 4-px
  vertical strip just left of the scrollbar draws one tick per
  `PromptRegion` in the current scrollback. Each tick is color-coded
  by last exit status: green (success), red (non-zero exit), gray
  (in-progress, OSC 133 D not yet emitted). Ticks map linearly from
  `PromptRegion.startLine / (scrollbackSize + rows)` to y — so a
  50,000-line scrollback compresses into the widget height with every
  boundary still visible. No-op when `promptRegions().empty()`, so
  users without shell integration see nothing change.
- **Settings → Terminal toggle.** New "Show command markers in
  scrollbar gutter" checkbox (default on) under Scrollback, with a
  tooltip explaining the color states. Propagates to all live
  terminals via `applyConfigToTerminal` on apply — no restart needed.
- **Portal-binding status label in Settings → Quake groupbox.** A
  one-line QLabel under the hotkey field reports whether out-of-focus
  hotkeys are available on this session:
  - ✓ **Portal binding active** (green) — hotkey works when Ants is
    unfocused, via Freedesktop Portal GlobalShortcuts. KDE Plasma 6,
    Hyprland, wlroots.
  - ⚠ **Portal unavailable** (amber) — hotkey works only while Ants
    is focused, with a hint pointing at the three
    `xdg-desktop-portal-*` backends that provide the service.

  Static check based on `GlobalShortcutsPortal::isAvailable()` — the
  D-Bus service registration is a per-session property that doesn't
  change while the app runs, so a snapshot at dialog construction is
  accurate.

### Fixed (test infrastructure)

- **Feature-test regex stack overflow.** The first draft of
  `tests/features/command_mark_gutter/` used a `[\s\S]*?^\}` pattern
  with `std::regex::multiline` to extract `paintEvent`'s body. On a
  4500-line `terminalwidget.cpp`, libstdc++'s backtracking executor
  recursed 100,000+ frames into `_M_dfs` and segfaulted. Replaced
  with a linear-time balanced-brace scanner
  (`extractFunctionBody(src, signature)`) — same semantics, constant
  stack. Left as a guiding pattern for future source-grep feature
  tests against large files.

### Invariants

- `Config::showCommandMarks()` persists to `show_command_marks` with
  default `true`.
- `paintEvent` gates gutter drawing on `m_showCommandMarks` AND
  non-empty `promptRegions()`.
- Gutter positions itself via `m_scrollBar->width()` subtraction so
  ticks never overlap the scrollbar thumb.
- Three-state color branch must remain: success / failure /
  in-progress.
- `applyConfigToTerminal(t)` propagates the toggle so settings-apply
  updates all live terminals.
- Settings portal-status label branches on
  `GlobalShortcutsPortal::isAvailable()` with the exact strings
  "Portal binding active" and "Portal unavailable" so the test can
  lock them.

Locked by `tests/features/command_mark_gutter` (CTest labels
`features;fast`, part of the release gate).

## [0.6.40] — 2026-04-18

**Theme:** OSC 133 shell-integration polish. Adds two keyboard-driven
"last completed command" actions that complement the existing
block-at-cursor right-click menu (Copy Command / Copy Output / Re-run
Command / Fold / Share as .cast) with no-selection-needed top-level
equivalents. The iTerm2 ⇧⌘O / WezTerm `CopyLastOutput` convention,
surfaced through the View menu, the command palette, and configurable
keybindings.

### Added

- **`TerminalWidget::copyLastCommandOutput()` slot.** Walks
  `promptRegions()` backwards for the most recent region with
  `commandEndMs > 0 && hasOutput`, delegates to `outputTextAt(idx)`
  (unbounded), and places the result on the clipboard. Returns the
  number of characters copied, or `-1` when no completed command exists.
  The backwards walk is load-bearing: if the tail region is still
  running (user typed a command but it hasn't emitted OSC 133 D yet),
  the method skips it and uses the previous completed region — so
  Ctrl+Alt+O during `sleep 30` copies the *previous* command's output,
  never the partial in-progress one.
- **`TerminalWidget::rerunLastCommand()` slot.** Same backwards-walk
  skip-in-progress rule, then delegates to `rerunCommandAt(idx)` which
  writes the command text + `\r` to the PTY. Returns the re-run region
  index or `-1`. Bailing on in-progress tails prevents splicing text
  into whatever the user is typing.
- **View menu entries + configurable keybindings.** "Copy Last Command
  Output" (default `Ctrl+Alt+O`, config key `copy_last_output`) and
  "Re-run Last Command" (default `Ctrl+Alt+R`, config key
  `rerun_last_command`) live under the View menu next to the existing
  Previous/Next Prompt entries. Both surface in the command palette
  automatically (it collects all menu actions). The `Ctrl+Alt+*`
  modifier combo avoids the already-taken `Ctrl+Shift+O` (split pane)
  and `Ctrl+Shift+R` (record session).
- **Status-bar toasts for discoverability.** Success path shows
  "Copied N chars of last command output" for 3 s; failure path shows
  "No completed command to copy (enable shell integration)" — the
  "(enable shell integration)" hint is load-bearing UX guidance when
  the user doesn't know OSC 133 requires bash/zsh/fish hooks.

### Why not reuse `lastCommandOutput()`

The existing `lastCommandOutput()` helper caps output at 100 lines
because it feeds `commandFailed` triggers where huge stderr noise is
harmful. A user-initiated copy expects the full output or a clear
"nothing to copy" signal, not a silently truncated string. The new
action uses `outputTextAt(idx)` (unbounded) instead, and a feature
test enforces this choice.

### Invariants

- Both slots must walk `promptRegions()` backwards and gate on
  `commandEndMs > 0`.
- `copyLastCommandOutput` must call `outputTextAt`, never
  `lastCommandOutput` (100-line cap).
- MainWindow wires both actions through `config.keybinding(...)` so
  users can rebind them via `config.json`.
- Defaults don't collide: `Ctrl+Alt+O` / `Ctrl+Alt+R` must appear
  exactly once in `mainwindow.cpp`.
- Both action handlers emit at least one `showStatusMessage(...)`
  toast so the operation isn't silent.

Locked by `tests/features/osc133_last_command` (CTest labels
`features;fast`, part of the release gate).

## [0.6.39] — 2026-04-18

**Theme:** Wayland-native Quake-mode, part 2 of 2. Completes the 0.7.0
ROADMAP item: out-of-focus global hotkey registration via the
Freedesktop Portal `org.freedesktop.portal.GlobalShortcuts` D-Bus API.
Replaces the originally-planned KGlobalAccel-on-KDE + GNOME-dbus
branching with one compositor-agnostic portal API that upstream has
converged on since 2023 (KDE Plasma 6, xdg-desktop-portal-hyprland,
xdg-desktop-portal-wlr).

### Added

- **`GlobalShortcutsPortal` client class.** New
  `src/globalshortcutsportal.{h,cpp}` wraps the three-step handshake
  (CreateSession → Response with session_handle → BindShortcuts →
  Activated) behind a simple `activated(QString)` Qt signal. The class
  handles the portal's request-path prediction convention (subscribing
  to the `Response` signal before dispatching the method call so the
  portal's reply can't land in a window where no match rule is
  installed), Qt D-Bus custom meta-type registration for the
  `a(sa{sv})` shortcuts array, and session lifecycle.
- **Portal-aware `MainWindow` Quake setup.** When
  `GlobalShortcutsPortal::isAvailable()` returns true, `MainWindow`
  instantiates the portal, binds `toggle-quake` with
  `config.quake_hotkey` as the preferred trigger, and routes `Activated`
  signals through `toggleQuakeVisibility()`. The in-app `QShortcut`
  from 0.6.38 stays wired unconditionally as a defence-in-depth
  fallback — for GNOME Shell (portal service present but no
  GlobalShortcuts backend yet), for the window between CreateSession
  and BindShortcuts response, and for any future revocation / re-bind
  path.
- **Focused double-fire debounce.** Both the in-app `QShortcut` lambda
  and the portal `activated` lambda stamp `m_lastQuakeToggleMs` with
  `QDateTime::currentMSecsSinceEpoch()` and reject any toggle if the
  previous stamp is less than 500 ms old. Without the debounce, a
  focused system where both paths deliver the same key press would
  hide-then-show the window in one frame (visible flicker).
- **Qt → portal trigger translation.** `qtKeySequenceToPortalTrigger()`
  converts `"Ctrl+Shift+\`"` / `"Meta+F12"` into the freedesktop
  shortcut-syntax equivalents (`"CTRL+SHIFT+grave"`,
  `"LOGO+F12"`). Deliberately minimal — modifiers plus a handful of
  common punctuation → xkb-keysyms; full keysym coverage is
  xkbcommon's job. Unrecognised keys pass through unchanged, and
  the user adjusts in System Settings if needed (the portal prompt
  treats `preferred_trigger` as advisory anyway).

### Invariants

- New source-grep regression test
  `tests/features/global_shortcuts_portal/` pins: (1)
  `isAvailable()` gate on portal construction, (2) canonical
  `org.freedesktop.portal.Desktop` / `/org/freedesktop/portal/desktop`
  / `org.freedesktop.portal.GlobalShortcuts` / `.Request` literals in
  the impl, (3) bound id `"toggle-quake"` matches the Activated
  handler filter, (4) in-app `QShortcut` fallback stays wired, (5) both
  paths debounce via `m_lastQuakeToggleMs` (regex match count ≥ 2), (6)
  CMake lists the source + keeps `Qt6::DBus` linked, (7) header
  surfaces the expected public API (`isAvailable`, `bindShortcut`,
  `activated`).

## [0.6.38] — 2026-04-18

**Theme:** Wayland-native Quake-mode, part 1 of 2. The 0.7.0 ROADMAP
item is split into (a) layer-shell anchoring + wiring the
long-dead `quake_hotkey` config key — shipped here — and (b)
Freedesktop Portal GlobalShortcuts for out-of-focus hotkey
registration, shipping in 0.6.39. After this release, running
Ants Terminal in Quake mode on Wayland no longer depends on the
compositor's goodwill for positioning/stacking.

### Added

- **`LayerShellQt` integration for Wayland Quake-mode.** When the
  `layer-shell-qt6-devel` package is available at build time,
  `find_package(LayerShellQt CONFIG QUIET)` wires
  `LayerShellQt::Interface` in and defines `ANTS_WAYLAND_LAYER_SHELL`.
  At runtime on Wayland, `MainWindow::setupQuakeMode()` promotes the
  QWindow to a `zwlr_layer_surface_v1` at `LayerTop`, anchored
  top/left/right with exclusive-zone 0 and
  `KeyboardInteractivityOnDemand`. This is the Wayland equivalent of
  X11's `_NET_WM_STATE_ABOVE` + client-side `move()` — without it the
  compositor could place the dropdown anywhere and stack it below
  other surfaces.
- **Build-time + runtime fallbacks.** When the devel package is
  missing at build time, CMake logs `LayerShellQt not found — Quake-mode
  on Wayland falls back to the Qt toplevel path` and the binary still
  runs. When the binary is run on X11, the XCB path (pre-0.6.38
  behaviour) is taken — no regression.

### Fixed

- **`quake_hotkey` config key was saved but never read.** The
  `QLineEdit` in Settings → "Dropdown/Quake Mode" persisted the user's
  chosen sequence to `config.json`, but nothing consumed it — the
  value was inert. `MainWindow` now wires a `QShortcut` with
  `Qt::ApplicationShortcut` context to `toggleQuakeVisibility()`
  when Quake mode is active. The shortcut fires only while Ants has
  focus; true out-of-focus global hotkey registration goes through
  the Freedesktop Portal GlobalShortcuts API coming in 0.6.39.
- **Slide animation snap on Wayland.** The
  `QPropertyAnimation(this, "pos")` path in `toggleQuakeVisibility()`
  is a no-op on Wayland (the compositor owns position) and the
  animation's end-value `move()` would visibly snap. The Wayland
  branch now takes a plain `show()`/`hide()` toggle; the XCB slide
  animation is unchanged.

### Packaging

- **openSUSE spec:** added `BuildRequires: cmake(LayerShellQt) >= 6.0`.
- **Arch PKGBUILD:** added `layer-shell-qt` to `makedepends` and
  listed it under `optdepends` for runtime discoverability.
- **Debian control:** added `liblayershellqt6-dev` to
  `Build-Depends`. Older Debian releases packaged the devel headers
  as `liblayershellqtinterface-dev`; the CMake `QUIET` flag lets the
  build proceed either way.

### Invariants

- **`tests/features/wayland_quake_mode/`** — source-grep regression
  test pinning the 0.6.38 contract: (1) the `<LayerShellQt/Window>`
  include is guarded by `ANTS_WAYLAND_LAYER_SHELL`; (2)
  `setupQuakeMode()` does a runtime `QGuiApplication::platformName()`
  dispatch and preserves the X11 flags; (3) `toggleQuakeVisibility()`
  returns before the `QPropertyAnimation` setup on Wayland; (4) the
  constructor wires `quake_hotkey` to a `QShortcut` connected to
  `toggleQuakeVisibility`; (5) `CMakeLists.txt` uses
  `find_package(LayerShellQt CONFIG QUIET)` and links
  `LayerShellQt::Interface` when found.

## [0.6.37] — 2026-04-18

**Theme:** Phase 3 of the 0.7.0 threading refactor — retire the
`ANTS_SINGLE_THREADED=1` kill-switch and delete the legacy
single-threaded read/parse code paths from `TerminalWidget`. Bake
period (0.6.34 → 0.6.37) proved the worker path; the 0.6.35 hotfix
proved the kill-switch worked. Shipping on the worker is now the only
path.

### Removed

- **`ANTS_SINGLE_THREADED=1` kill-switch.** The env-var fork in
  `TerminalWidget::startShell()` and the legacy branch it guarded
  are gone. Every launch now goes through `VtStream` on
  `m_parseThread`.
- **Legacy `m_pty` / `m_parser` members and `onPtyData` slot.** The
  widget no longer owns a `Pty` or a `VtParser` directly; both live
  on the worker inside `VtStream`. `hasPty()` and `ptyChildPid()`
  simplified to worker-only checks; no behaviour change at the
  call sites.
- **`#include "vtparser.h"` and `#include "ptyhandler.h"` from
  `terminalwidget.h`.** Transitively pulled in via `vtstream.h` for
  callers that still need `VtAction` / `Pty` types.

### Changed

- **ROADMAP.md §0.7.0 threading entry** updated to note the 0.6.37
  kill-switch retirement and the four-test invariant set
  (parse equivalence, response ordering, resize synchronicity,
  ptyWrite gating).

## [0.6.36] — 2026-04-18

**Theme:** Tenth periodic audit (10th in the series, post-0.6.35).
Two actionable findings across ~110 raw — a 2% signal ratio consistent
with the codebase's audit-calibration anchor. Both fixed in a single
pass.

### Fixed

- **`RuleQualityTracker::prune()` dead code.** Declared in
  `src/auditrulequality.h:133` and defined in `.cpp:250`; never
  called. The retention-cutoff logic was inlined into `save()`
  (const-method) when that method needed to filter records without
  mutating. Two copies of the same logic would drift over time.
  Removed the dead function — `save()` is the single source of
  truth for the 90-day retention window. Flagged by cppcheck's
  `unusedPrivateFunction` rule.
- **`pipeRun` regex recompiled on every paste.**
  `src/terminalwidget.cpp:2129`
  constructed a fresh `QRegularExpression` with the
  CaseInsensitiveOption on every call to `confirmDangerousPaste()`
  — a hot path that fires on every user paste. Hoisted to a
  function-local `static const` so the pattern compiles once per
  process and is reused thereafter. Flagged by clazy's
  `use-static-qregularexpression` check. Safe because
  `QRegularExpression::match` is reentrant and const-safe since
  Qt 5.4.

## [0.6.35] — 2026-04-18

**Theme:** Hotfix for the 0.6.34 threaded-parse refactor. Keystrokes
typed into the terminal were silently dropped — the echo never
appeared on screen. Shipped 0.6.34, caught in minutes of dogfood, fix
is small and the regression test makes it impossible to come back
quietly.

### Fixed

- **0.6.34 regression: keystrokes not echoing.** Twelve PTY-write
  call sites in `TerminalWidget` (keystrokes, Ctrl modifiers, arrow
  keys, clipboard paste, focus reporting, mouse middle-click paste,
  input method commit, scratchpad, context-menu paste and clear,
  `writeCommand`, `rerunCommandAt`) still carried pre-0.6.34
  `if (m_pty) ptyWrite(...)` or `if (cond && m_pty) { ptyWrite(...) }`
  guards. Under the threaded parse path `m_pty` is null (the Pty
  lives on the worker thread inside `VtStream`), so the guard
  silently swallowed every write. Replaced each guard with either
  `hasPty()` (path-agnostic boolean — true for either the worker or
  the legacy path) or an unconditional `ptyWrite(...)` (the helper
  is already null-safe). The legacy path's `m_pty` lifecycle code
  in `startShell` / `recalcGridSize` / `~TerminalWidget` is
  unchanged.
- **`shellPid()` / `shellCwd()` / `foregroundProcess()`** were also
  gated on the bare `m_pty` pointer, returning empty on the worker
  path. Replaced with a new `ptyChildPid()` helper that reads from
  whichever path is active (`VtStream::childPid()` on the worker;
  `Pty::childPid()` on the legacy path). The PID is written once by
  `forkpty()` during `startShell`'s blocking-queued invocation, so
  the cross-thread read is synchronised and mutex-free.

### Added

- **`tests/features/threaded_ptywrite_gating/`** regression test.
  Source-grep inspection: no `if (m_pty)` / `&& m_pty)` / `!m_pty`
  gate may appear outside the four allowlisted functions
  (`ptyWrite`, `ptyChildPid`, `startShell`, `recalcGridSize`,
  `~TerminalWidget`). Ground-truth verified — re-injecting the
  regression makes the test fail at the offending line; restoring
  the fix makes it pass.

## [0.6.34] — 2026-04-18

**Theme:** Decouple PTY read + VT parse from the GUI thread. Ships the
first planned 0.7.0 performance item early since the architectural
change landed clean with three feature-conformance tests locking the
invariants.

Previously, every byte of PTY output — from a fast prompt echo to a
`find /` firehose — traversed the Qt GUI thread through
`QSocketNotifier → Pty::dataReceived → VtParser::feed →
TerminalGrid::processAction` and then on to paint. Heavy streams
blocked scroll and repaint. As of 0.6.34, read + parse run on a
dedicated worker `QThread` (`VtStream`); only batched `VtAction`
streams cross back to GUI for grid application and paint.

### Added

- **Threaded PTY read + VT parse (`VtStream`).** New worker-thread
  wrapper around `Pty` + `VtParser` (`src/vtstream.{h,cpp}`). The
  worker reads the PTY master via its own `QSocketNotifier`, feeds
  bytes through `VtParser`, accumulates emitted `VtAction`s into a
  `VtBatch`, and ships the batch to the GUI over
  `Qt::QueuedConnection`. GUI applies the batch to `TerminalGrid` and
  repaints — the grid and paint path do not move.
- **Back-pressure.** Worker buffers up to 8 batches (≈128 KB of
  unprocessed PTY bytes) before disabling its read notifier so the
  kernel applies flow control to the child process. GUI re-enables
  reads on drain via `VtStream::drainAck()`. Replaces "let RAM grow
  unbounded while GUI catches up" with kernel-natural
  back-pressure.
- **`Pty::setReadEnabled(bool)`** — hook to pause/resume the read
  notifier without tearing it down. Used by the back-pressure path.
  `Pty::write` and `Pty::resize` moved into `public slots:` so they
  can be invoked cross-thread via `QMetaObject::invokeMethod`.
- **Three feature-conformance tests** under `tests/features/`:
  - `threaded_parse_equivalence` — 11 input fixtures (plain ASCII,
    mixed ANSI, DCS Sixel, APC Kitty, OSC 52 long payload, UTF-8
    multibyte across chunk boundaries, DEC 2026 sync, OSC 133
    markers, CSI with 32 params, UTF-8 bulk, 50 KB Print run) × 6
    chunking strategies (whole, byte-by-byte, 16 KB worker flush,
    7-byte, 3-byte, two pseudo-random seeds). Asserts action-stream
    identity across strategies.
  - `threaded_response_ordering` — DA1 / CPR / DA2 / mode-996 /
    DSR-OK sequence asserted to fire in parse order across three
    chunking strategies. End-to-end write order across the worker
    boundary follows from this plus Qt's per-receiver FIFO.
  - `threaded_resize_synchronous` — source-level contract that
    every `invokeMethod(m_vtStream, "resize", ...)` uses
    `Qt::BlockingQueuedConnection`, and that `VtStream::resize` /
    `::write` are declared in a `public slots:` block.

### Changed

- **`TerminalWidget` PTY-write path unified through `ptyWrite()`**.
  ~30 call sites (keystrokes, bracketed paste, Kitty kbd protocol,
  response callback, snippets, scratchpad) now route through a single
  helper that branches on `m_vtStream ? invokeMethod(QueuedConnection)
  : m_pty->write()`. One-line diff per call site; correctness is
  centralised.
- **Resize path uses `Qt::BlockingQueuedConnection`.**
  `TerminalWidget::recalcGridSize` waits for the worker to finish
  `Pty::resize` before returning, so the next paint always sees
  consistent `ws_row`/`ws_col` vs. `m_grid->rows()`/`m_grid->cols()`.
  No 1-frame flicker at the new size with old PTY dims.
- **Session logging and asciicast recording timestamps** are now
  sampled on the worker at batch-flush time, which is more accurate
  than resampling on GUI (GUI paint latency previously skewed the
  recorded `elapsed` field). Log write granularity is now per-batch
  (coarser than per-PTY-read); users grepping the file minutes later
  see no difference.

Kill switch: set `ANTS_SINGLE_THREADED=1` in the environment to force
the legacy single-threaded path while the new code proves itself. It
will be removed in a follow-up release once no regressions surface.

## [0.6.33] — 2026-04-18

**Theme:** Nine long-standing user-reported bugs around the status bar,
scrollback integrity, session restore, and the Review Changes dialog —
fixed in one pass. The quiet centerpiece is the root-cause diagnosis of
the long tail of "Review Changes button does nothing" reports: a
0.6.26-era focus-redirect lambda was firing a `singleShot(0)` refocus
*between* a button's mousePress and mouseRelease, ripping focus off the
button mid-click and silently cancelling the `clicked()` signal that
`QAbstractButton` only emits when focus is continuous through release.
Also retires the `ElidedLabel + stylesheet chip padding` class of
status-bar bug (branch chip truncating to "…") by converting every
fixed-width status-bar chip to plain `QLabel` / `QPushButton` with
`QSizePolicy::Fixed`.

### Added

- **One-click Claude Code hook installer** in Settings > General.
  Writes `~/.config/ants-terminal/hooks/claude-forward.sh` and merges
  PreToolUse / PostToolUse / Stop / PreCompact / SessionStart entries
  into `~/.claude/settings.json`. Idempotent, preserves the user's
  existing hooks, safe to re-run.
- **Frozen-view scrollback snapshot.** When the user is scrolled up,
  `cellAtGlobal` / `combiningAt` now read from a snapshot of the screen
  taken at scroll-up time rather than the live grid. The viewport is
  completely immune to live screen writes, not just to deep-scroll
  rows. Locked by new `tests/features/scrollback_frozen_view/`.
- **Claude status vocabulary.** Per-tab status now distinguishes
  idle / thinking / bash / reading a file / editing / searching /
  browsing / planning / auditing / prompting / compacting, each with
  a distinct theme colour. Plan-mode and `/audit` are detected in the
  transcript parser; new `ClaudeIntegration::planModeChanged` and
  `auditingChanged` signals.
- **`refreshStatusBarForActiveTab()`** — single per-tab refresh entry
  point covering branch, notification, process, Claude status, Review
  Changes, and Add-to-allowlist. Replaces the scatter of direct calls
  in `onTabChanged` where one miss meant a stale chip for 2 s.
- **`notification_timeout_ms` config key** (default 5000) with a
  seconds spinner in Settings > General.
- **`tab_color_sequence` config key** — ordered colour fallback
  independent of session persistence. UUID-keyed `tab_groups` is
  retained for within-session drag-reorder stability.

### Changed

- **Branch chip colour moved from green (ansi[2]) to cyan (ansi[6])**
  so it is visually distinct from Claude-state chips and transient
  notification text.
- **Status-bar layout is now Fixed-sized.** Branch, process, Claude
  status, and all transient buttons use `QSizePolicy::Fixed` with
  plain `QLabel` / `QPushButton`. Only the transient notification
  slot is elastic and elides with "…". The
  `ElidedLabel + stylesheet chip padding` miscalculation class of bug
  is retired.
- **Review Changes button is now tri-state.** Hidden when not a git
  repo, **disabled** when clean and in-sync, enabled otherwise. The
  0.6.29 "hide on clean" contract was too aggressive — an unpushed
  commit is reviewable work too. Probe switched to `git status
  --porcelain=v1 -b` so ahead-of-upstream counts. The global
  `QPushButton:hover` rule is now gated with `:enabled` so disabled
  buttons no longer light up on hover. The dialog renders Status +
  Unpushed + Diff sections; spec and feature test updated to match.
- **Review Changes dialog is now fully async.** Opens instantly with
  a loading placeholder; git output streams in. The status-bar button
  disables while the dialog is open and re-enables on close.
  `WindowModal` was tried and reverted — non-modal with button-disable
  is the pattern that actually works.
- **Add to Allowlist** gains a 3-scan debounce against TUI-repaint
  flicker, a shared `toolFinished` / `sessionStopped` retraction path
  across scroll-scan and hook detections, and duplicate-rule feedback
  on prefill.

### Fixed

- **Focus-redirect race silently cancelled button clicks.** Root
  cause of the long tail of "Review Changes does nothing" reports.
  The 0.6.26 `focusChanged` lambda queued a `singleShot(0)` to refocus
  the terminal the moment focus touched status-bar chrome. That
  zero-delay timer could fire *between* `mousePress` and
  `mouseRelease` on a `QPushButton`, ripping focus away mid-click;
  `QAbstractButton` requires continuous focus through release to emit
  `clicked()`, so the click was silently dropped. Fix: the lambda
  now exempts `QAbstractButton` focus targets and defers while any
  mouse button is held down.
- **Branch label eliding to "…"** with ample room. `ElidedLabel`'s
  `sizeHint` fudge could not account for the chip stylesheet's
  padding + margin. Converted to a plain `QLabel` with Fixed
  `sizePolicy`.
- **Claude status label truncating and occasionally stale.** The
  new vocabulary fits the fixed chip width, and the consolidated
  `refreshStatusBarForActiveTab()` wiring ensures state never bleeds
  from a previous tab on `onTabChanged`.
- **Scrollback blank-line bloat on inactive tabs after session
  restart.** Two root causes compounded: (a) `recalcGridSize` ran on
  inactive tabs before their geometry was laid out, shrinking grids
  to 3×10 — fixed with a pre-layout guard; (b) `TerminalGrid::resize`
  pushed every reflowed row into scrollback indiscriminately — now
  skips entirely-blank rows.
- **Tab colours not persisting across restarts.** UUID-keyed
  `tab_groups` only worked when session persistence was enabled,
  because only the session-persist path re-used saved UUIDs. Added
  an ordered `tab_color_sequence` fallback applied at startup once
  tabs are constructed.

## [0.6.32] — 2026-04-18

**Theme:** Audit self-learning layer + contributor-facing docs + a
one-line CI hotfix for 0.6.31's OSC 133 verifier. The headline is a
**per-rule fire/suppression tracker** with an inline LCS-based
tightening suggester — the lightweight always-on counterpart to the
heavier weekly cloud routine documented in `RECOMMENDED_ROUTINES.md`.
Without instrumentation the 2026-04-16 one-shot FP-rate triage would
silently re-accrue as the codebase evolves; now each suppression is
recorded and the noisiest rules surface on demand in the audit dialog.

### Added — audit-tool self-learning layer

- **`RuleQualityTracker`** (`src/auditrulequality.{h,cpp}`) records every
  per-rule fire and user-suppression event from the project Audit
  dialog into `<project>/audit_rule_quality.json`. The JSON is bounded
  to a 90-day rolling window, capped at 50 000 records per category.
  Tracker writes back on dialog destruction (RAII finalisation), and
  force-flushes immediately on each suppression so user actions are
  durable even on a hard exit.
- **Rule Quality dialog** — new "📊 Rule Quality" button in the audit
  dialog opens a modal table of per-rule stats: 30-day fires,
  30-day suppressions, FP rate (capped 0-100 %), all-time fires, and a
  proposed `dropIfContains` tightening when the heuristic suggester
  has enough samples. Sorted by FP rate desc so the noisiest rules
  surface first; rows with FP ≥ 50 % are highlighted.
- **LCS-based tightening suggester** — when the user suppresses a
  finding, the tracker computes the longest common substring across
  the last 5 suppressed line texts for that rule. If the LCS is at
  least 6 chars AND contains a structural boundary character (space,
  paren, brace, bracket, semicolon, quote, angle bracket), the
  suggestion is surfaced inline in the status bar (`💡 <substring>
  looks like a common FP shape — consider adding it to <rule>'s
  dropIfContains in audit_rules.json`). Pure-identifier substrings
  are rejected so the suggester proposes rule-shape filters, not
  project-noun filters.
- **Test:** `tests/features/audit_rule_quality/` covers fire +
  suppression aggregation, the LCS suggester's positive and negative
  paths (too-few-samples, pure-identifier rejection), and JSON
  persistence round-trip. Headless model-only test, no Qt GUI.

### Added — contributor docs

- **`docs/RECOMMENDED_ROUTINES.md`** captures the four Claude Code
  routines that earn a slot on the shared account quota for this repo:
  nightly audit triage (diff-against-baseline, PR only NEW findings),
  PR security review on `main`, weekly audit-rule self-triage
  (cross-rule LLM analysis of `audit_rule_quality.json`, opens
  tightening PRs — the deeper counterpart to the in-process tracker
  above), and a monthly ROADMAP-item scoper. Each entry includes the
  trigger config, environment setup, and the exact prompt text. Total
  budget ~3-4 runs/day, leaving room for ad-hoc work and the Vestige
  repo's own routines.

### Fixed

- **CI build on Ubuntu runner.** `QMessageAuthenticationCode::addData`
  in older Qt 6.x does not accept `QByteArrayView`; the 0.6.31 OSC 133
  verifier used the view-taking overload and broke the Ubuntu job.
  Switched to the `(const char*, qsizetype)` overload present in every
  Qt 6.x version. Behaviour unchanged; the HMAC feature test still
  passes. Purely a build-compat fix — users on 0.6.31 are unaffected
  if they're already building locally.

## [0.6.31] — 2026-04-17

**Theme:** Security + audit signal-to-noise + two regressions. The
headline is **OSC 133 HMAC-signed shell integration** (a 0.7.0 ROADMAP
item shipped early): protects the command-block UI from forged shell-
integration markers emitted by untrusted in-terminal processes. Also
cuts the audit-tool false-positive rate from ~97 % to under 10 % via a
two-pass triage of the Ants and Vestige audit runs, fixes the user-
reported Review Changes click-does-nothing bug, and locks the 0.6.26
Shift+Enter "tab freezes" regression with a feature test.

### Added — Security

- **OSC 133 HMAC-signed shell integration.** When `$ANTS_OSC133_KEY`
  is set in the terminal's environment, every OSC 133 marker
  (`A`/`B`/`C`/`D`) MUST carry a matching
  `ahmac=<hex-sha256-hmac>` parameter computed as
  `HMAC-SHA256(key, "<marker>|<promptId>[|<exitCode>]")`. Markers
  without a valid HMAC are dropped (no UI side-effects) and a forgery
  counter increments, surfaced in the status bar via a throttled
  warning ("⚠ OSC 133 forgery detected") with a 5-second cooldown.
  The verifier closes the spoofing surface where any process running
  inside the terminal — a malicious TUI, `cat malicious.txt`, anything
  that writes to stdout — could otherwise mint OSC 133 markers and
  pollute prompt regions, exit codes, and the `command_finished`
  plugin event.

  When `$ANTS_OSC133_KEY` is not set, the verifier is silent and OSC
  133 behaves as in 0.6.30 (legacy permissive). No upgrade penalty
  for users who don't opt in.

  Shell hooks ship under
  `packaging/shell-integration/ants-osc133.{bash,zsh}` and install to
  `${datarootdir}/ants-terminal/shell-integration/`. The README in
  the same directory walks through key generation
  (`openssl rand -hex 32`), `~/.bashrc`/`~/.zshrc` setup, threat
  model, and verification steps.

  Headless feature test
  (`tests/features/osc133_hmac_verification/`) covers verifier OFF
  back-compatibility, verifier ON accepting valid HMACs (including
  uppercase-hex), and rejection of missing/wrong/promptId-mismatched/
  exit-code-mismatched HMACs. Implements ROADMAP §0.7.0 → 🔒 Security
  → "Shell-side HMAC verification for OSC 133 markers."

### Fixed

- **Review Changes button silently swallowed clicks on a clean repo.**
  `refreshReviewButton`'s clean-repo branch left the button
  `setEnabled(false); show()` "as a hint that Claude edited
  something." `QAbstractButton::mousePressEvent` drops clicks on
  disabled buttons, so `clicked()` was never emitted and the 0.6.29
  silent-return-with-flash guards inside `showDiffViewer` never
  fired. The global `QPushButton:hover` rule isn't `:enabled`-gated
  either, so the disabled button still highlighted on hover —
  advertising itself as actionable while doing nothing. Net symptom
  (user report 2026-04-17): "*the Review Changes button is showing
  and is active as it has an onmouseover event that highlights the
  button. When I click the button though, nothing happens.*" Fix:
  `hide()` on clean repo. The button reappears via the next
  2-second refresh / fileChanged tick when a real diff appears.
  New `tests/features/review_changes_clickable/` locks the regression
  with a source-grep guard against re-introducing the
  `setEnabled(false); ... show();` shape and a tripwire on the
  global hover stylesheet remaining un-`:enabled`-gated.

- **Shift+Enter wedged the terminal tab in bracketed-paste mode**
  (0.6.26 regression). `QByteArray("\x1B[200~\n\x1B[201~", 8)`
  truncated the 13-byte literal to 8 bytes, dropping the closing
  `[201~` end-paste marker and leaving an orphan `ESC` that kept
  the shell in bracketed-paste mode and ate the next keystroke.
  Switched to `QByteArrayLiteral(...)` so the size is derived from
  the literal at compile time — closes the entire class of
  "length argument drifts away from literal" bugs. New
  `tests/features/shift_enter_bracketed_paste/` locks the byte
  sequence AND source-grep-guards against the
  `QByteArray(literal, <num>)` shape recurring.

- **Status-bar allowlist button dedup** missed the hook-path
  container. The scroll-scan-path cleanup used
  `findChildren<QPushButton*>("claudeAllowBtn")`, but the hook-path
  permission handler creates the button as a `QWidget` container
  (with child controls) that has the same `objectName`. A
  `QPushButton`-typed `findChildren` skipped the container,
  letting both buttons stack when a scroll-scan detection fired
  while a hook-path container was already visible. Switched to
  `QWidget*` for parity with the `onTabChanged` and hook-path
  dedup lookups.

### Fixed — audit signal-to-noise overhaul (~97 % → <10 % FP rate)

Two-pass triage of the Ants audit (`docs/AUDIT_TRIAGE_2026-04-16.md`)
followed by the Vestige 3D engine triage. 137 of 141 findings in the
Ants run were FPs; 100 % of four `addFindCheck` rules in the Vestige
run were FPs. Targeted rule tightenings:

- **`memory_patterns`** regex inverted — only flags `new X()` /
  `new X(nullptr)` / `new X(NULL)` (i.e. NOT parented). Any
  identifier inside the parens is treated as a Qt parent and
  suppressed. Kills 30 FPs from `new QWidget(parent)` /
  `new QAction(this)` etc. that the previous substring blacklist
  could only suppress when the parent expression matched a hardcoded
  name.
- **`clazy --checks=`** drops `qt-keywords`. The project documents
  bare `signals:` / `slots:` / `emit` as house style; flagging
  them was ~48 FPs per run.
- **`clang-tidy`** auto-disables when no `compile_commands.json`
  exists; collapses `'QString' file not found` storms into a single
  banner line. Kills 34 near-identical driver-error FPs.
- **`cppcheck`** excludes every `build-*` variant by name
  (`build-asan`, `build-debug`, etc.). cppcheck was parsing
  `moc_*.cpp` files in `build-asan/` and tripping on their
  `#error "This file was generated using moc from 6.11.0"` banners
  — likely the source of the 30-second timeout on Compiler Warnings.
- **Context-window suppression** added to
  `qt_openurl_unchecked` (±5 lines for `startsWith("http"` /
  `QUrl("https"` / `// ants-audit: scheme-validated`),
  `insecure_http` (±5 lines for `startsWith` scheme gates),
  `unbounded_callback_payloads` (±10 lines for `.truncate(` /
  `.left(` / `.size() <=` / `constexpr int kMax`),
  `cmd_injection` (suppress on `, nullptr)` / `, NULL)`
  exec-family terminators), `bash_c_non_literal` (±5 lines for
  `m_config.` / `Config::instance` Config-trust-boundary references),
  `debug_leftovers` (±8 lines for `if (m_debug` / `if (on)` /
  `#ifdef DEBUG` debug-flag conditionals).
- **`secrets_scan`** RHS literal constraint — requires the right-
  hand side to be a `"…"` string of ≥16 chars (or `'…'`, or a
  ≥16-char unquoted token). Rejects variable-name LHSes with
  constructor / variable RHSes (`m_aiApiKey = new QLineEdit(tab)`,
  `m_apiKey = apiKey`).
- **Severity demotion**: tool timeouts → Info (was inheriting the
  check's severity, polluting the Major / Critical tiers with
  "(warning) Timed out (30s)"); `long_files` → Info; `missing_
  compiler_flags` → Info. The 2026-04-16 triage flagged these as
  "severity leak" — advisories that aren't bugs sharing a tier
  with real bugs.
- **Find/grep exclude lists** pick up `__pycache__/`, `.claude/`,
  `target/`, `.venv/`, `venv/`, `.tox/`, `.pytest_cache/`,
  `.mypy_cache/`. The previous static lists missed common
  ecosystem scratch dirs.
- **`// ants-audit: scheme-validated`** inline marker recognized
  in the context-window for `qt_openurl_unchecked` — closes the
  last MAJOR-tier FP from the Ants triage where the OSC 8
  `openHyperlink` `openUrl` was 35 lines below the OSC 8 ingestion
  scheme allowlist (different file; ±5 line context can't span).

Four small real findings from the prior Ants triage:

- `claudeallowlist.cpp:409` — `std::as_const(segments)` on the
  value-captured QStringList (clazy `range-loop-detach`).
- `auditdialog.cpp:3115` — hoisted `QStringList parts` out of the
  finding-render loop (clazy `container-inside-loop`).
- `elidedlabel.h:33`, `sshdialog.h:32` — getters return
  `const &` not by value (cppcheck `performance:returnByReference`).

Net measured effect on a clean re-scan: the same audit drops from
~141 to ≲10 findings, with no loss of real coverage.

### Fixed — audit rule noise follow-up (triage of the Vestige 2026-04-16 run)

Four `addFindCheck` rules produced 100/100 false positives when
`ants-audit` scanned the Vestige engine on 2026-04-16. Each is a
rule-shape bug rather than a find-site bug; all four are tightened
so the next scan of the same tree drops straight to zero noise for
these categories.

- **`file_perms` — World-Writable Files.** Dropped the `-perm -020`
  (group-writable) branch. Mode 664 is the default umask result on
  most Linux distros, so the check was flagging every normal file
  as a security finding. The rule now matches only true
  world-writable (`-perm -002`, CWE-732).
  `src/auditdialog.cpp:708-720`.
- **`header_guards` — Missing Header Guards.** Scan window
  widened from `head -5` to `head -30` (copyright blocks at the
  top of a header commonly pushed `#pragma once` past line 5, so
  valid guards were read as missing). The `_H`-suffix requirement
  on traditional `#ifndef` guards was also dropped — naming
  conventions vary (`FOO_HPP`, `FOO_GUARD`, `__FOO__`) and the
  suffix was over-fitted to a single house style.
  `src/auditdialog.cpp:1006-1026`.
- **`binary_in_repo` — Binary Files in Source.** Prefer
  `git ls-files` over `find .` when the project is a git checkout.
  `find` was matching gitignored paths (`__pycache__/*.pyc`,
  `.claude/worktrees/…`, `target/…`) because `kFindExcl` is a
  static allowlist and can't keep up with every project's
  `.gitignore`. The scan now falls through to the legacy `find`
  command only when `git rev-parse` fails.
  `src/auditdialog.cpp:582-597`.
- **`dup_files` — Duplicate File Detection.** Same fix as
  `binary_in_repo` — prefer `git ls-files`, fall back to `find`.
  `md5sum` runs only on git-tracked files ≥100 bytes, sidestepping
  the Claude-managed-worktree and build-artefact duplication that
  made the report useless.
  `src/auditdialog.cpp:561-579`.

Measured against the same Vestige tree:

| Rule              | Before | After |
|-------------------|:------:|:-----:|
| `file_perms`      | 30 FPs |   0   |
| `header_guards`   | 20 FPs |   0   |
| `binary_in_repo`  | 30 FPs |   0   |
| `dup_files`       | 30 FPs |   0   |

The `timeout`-as-tool-health and security-rule self-exclusion
fixes cited in the same Vestige triage report landed earlier —
see the 0.6.30 Fixed section (auditdialog.cpp:162-165 for
timeouts, `kGrepFileExclSec` for self-exclusion).

## [0.6.30] — 2026-04-16

**Theme:** Contributor-facing workflow infrastructure — project-level
Claude Code hooks + recipe-driven version bumping. **No runtime or
behavior changes for end users.** The `ants-terminal` binary is bit-
identical to 0.6.29 modulo the `ANTS_VERSION` macro string. The new
files are entirely tooling for contributors who clone the repo and
work on it through Claude Code.

### Added

- **`.claude/settings.json`** wires a `PostToolUse` hook on
  `Edit|Write` that delegates to
  `packaging/hook-on-cmakelists-edit.sh`. The script short-circuits
  unless the touched path ends in `CMakeLists.txt`, then runs
  `packaging/check-version-drift.sh` and surfaces drift via a
  `systemMessage`. Contributor benefit: the version-drift gap that
  let 0.6.23 ship with stale packaging files can no longer survive
  even an in-session edit — the agent learns about drift the moment
  it edits CMakeLists, not at commit time.
- **`.claude/bump.json`** — per-project recipe consumed by a
  user-level `/bump` skill (lives in `~/.claude/skills/bump/`, not
  in this repo). Lists the 6 version-bearing files with templated
  `{OLD}` / `{NEW}` / `{TODAY}` find/replace patterns, plus 4 todos
  for content the recipe can't synthesise (CHANGELOG body, AppStream
  `<release>` HTML, debian changelog block, build/test verification).
  `/bump 0.6.30` walks the recipe and runs `check-version-drift.sh`
  as its post-check.
- **`packaging/hook-on-cmakelists-edit.sh`** — small bash dispatcher
  the project hook calls. Stdin is the `PostToolUse` JSON payload;
  stdout is empty on a clean state or `{systemMessage:"…"}` on
  drift. Always exits 0 — never blocks an edit, only informs.

### Changed

- **`.gitignore` narrowed** `.claude/` → `.claude/*` with explicit
  `!.claude/settings.json` / `!.claude/bump.json` exceptions so the
  team-shared workflow files commit while developer-local
  `.claude/settings.local.json` and `.claude/worktrees/` stay
  ignored. Same pattern adopted in the Vestige 3D Engine repo.

## [0.6.29] — 2026-04-16

**Theme:** Status-bar reliability pass — nail down every user-reported
status-bar symptom in one go: git branch chip elided to "…" when the
statusbar had plenty of space, Review Changes button not appearing
until a tab-switch, Add-to-Allowlist button silently no-op on save
failure, Claude status label 2 s stale after tab-switch, Review
Changes click "does nothing" with no feedback.

### Added

- **Feature-conformance test: `status_bar_elision`**
  (`tests/features/status_bar_elision/`) — spec + test covering the
  ElidedLabel elision policy. Three assertions: short text never
  elides, over-cap text elides with a non-empty tooltip, and the
  statusbar layout's QBoxLayout squeeze never drops short chips to
  "…" (the user-reported regression vector). Fails against pre-fix
  `src/elidedlabel.h`, passes on fix.

### Changed

- **`ElidedLabel::minimumSizeHint` + `sizeHint` now compute against
  `m_fullText`**, not `QLabel::sizeHint()` which reports the
  *displayed* (possibly already elided) text. This is the root-cause
  fix for the user-reported "git branch shows `…` instead of `main`"
  regression: the previous 3-char minimum let QStatusBar squeeze the
  chip below what "main" required, `fontMetrics().elidedText()`
  returned "…" for the squeezed width, the shorter displayed text
  fed back into `sizeHint`, and the widget was stuck on a fixed
  point it couldn't escape. `minimumSizeHint` now returns
  `full-text-width + padding` capped at `maximumWidth`, so short
  text is guaranteed to fit and only genuinely over-cap text elides.
- **`refreshReviewButton()` now fires on every 2 s status-bar tick**
  via the shared `m_statusTimer`. Previously only `onTabChanged` and
  the Claude Code `fileChanged` hook triggered it, so a dirty repo
  at startup (no tab-switch, no hook fired) left the Review Changes
  button hidden. Also called once via `QTimer::singleShot(0)` at end
  of the MainWindow constructor so the button appears immediately
  on boot rather than 2 s later.
- **`ClaudeIntegration::setShellPid` calls `pollClaudeProcess()`
  immediately** after arming the timer. Previously the Claude status
  label was `NotRunning` for up to 2 s after every tab-switch to a
  tab where Claude was actually running, until the next poll tick
  caught up — user-visible as "status doesn't update right away."
- **Add-to-Allowlist button unified across both creation paths.**
  The hook-server button-group QWidget (Allow/Deny/Add-to-allowlist)
  now carries `objectName "claudeAllowBtn"` matching the scroll-scan
  path's bare QPushButton; `onTabChanged` now searches for
  `QWidget` (not just `QPushButton`) with that objectName so both
  paths are cleaned on tab-switch. Hook-server path also listens on
  *every* terminal for `claudePermissionCleared` instead of only
  `currentTerminal()`, so a brief visit to another tab doesn't
  orphan the button.
- **Both permission paths now set/clear `m_claudePromptActive`**
  so the Claude status label consistently reads "Claude: prompting"
  while a prompt is live, and `onTabChanged` resets the flag so the
  next tab doesn't inherit the previous tab's prompt state.
- **`showDiffViewer` no longer has silent `return` branches.** The
  user-reported "Review Changes shows but click does nothing"
  symptom was a combination of missing feedback when
  `focusedTerminal` returned null, empty `shellCwd`, or the
  underlying `git diff` returned non-zero. Each case now emits an
  explicit `showStatusMessage`. Also combined worktree + index diff
  into a single `git diff HEAD` (matches what `refreshReviewButton`
  uses for its enablement probe) instead of two sequential calls
  where the second was only reached on empty-worktree cases.

### Fixed

- **`ClaudeAllowlistDialog::saveSettings` now returns `bool`** and
  all three callers (prompt-prefill auto-save,
  `QDialogButtonBox::accepted`, Apply-button `onApply`) surface
  failure to the user: the prefill path shows a status message with
  the target path; OK/Apply surface an error in the dialog's
  validation label and refuse to `accept()` so the user can retry.
  Previously every `QSaveFile` open / write / commit failure was
  swallowed — the user saw the "Rule added to allowlist" toast even
  when the write had failed, then Claude Code re-prompted and the
  user reported "Add to allowlist does nothing."

## [0.6.28] — 2026-04-16

**Theme:** End-to-end feature review — same-class overflow sweep across
chrome widgets, quake-mode toggle fix, combining-character preservation
on resize, disk-write durability hardening, and auto-profile regex
caching.

### Added

- **`ElidedLabel` widget** (`src/elidedlabel.h`) — QLabel that
  truncates its full text with "…" to fit the current widget width,
  re-eliding on every resize and exposing the un-elided string via
  tooltip. Header-only; drop-in replacement wherever dynamic strings
  could outgrow a bounded slot.
- **Feature-conformance test: `combining_on_resize`**
  (`tests/features/combining_on_resize/`) — exercises the three
  invariants covering the new resize fix (I1 main-screen preservation,
  I2 alt-screen preservation, I3 shrink-range eviction). Confirmed to
  fail against pre-fix `src/terminalgrid.cpp` and pass against the fix.

### Changed

- **Status-bar text slots elide on overflow.** The three text widgets
  — git branch (`m_statusGitBranch`, cap 220 px, elide right),
  transient message (`m_statusMessage`, stretch, elide middle), and
  foreground process (`m_statusProcess`, cap 160 px, elide right) —
  now cap out and show "…" instead of growing unbounded. User report:
  when the transient-message slot showed "Claude permission:
  Bash(git log …)" it pushed the git-branch chip and process indicator
  off the right edge. Middle-elide keeps both the leading label and
  the trailing detail visible in the message slot.
- **Same treatment applied to the other overflow-risk labels:**
  - `TitleBar::m_titleLabel` — window title from OSC 0/2 is
    user-controlled and can push the minimize/maximize/close buttons
    off a frameless window. Middle-elide keeps "cmd" prefix and "cwd"
    suffix both visible.
  - `MainWindow::m_claudeStatusLabel` — "Claude: `<tool-name>`" in the
    ToolUse state accepts an arbitrary tool name from the transcript
    (MCP tools, custom tools); right-elide caps at 220 px.
  - `AuditDialog::m_statusLabel` — "Running: `<check-name>…`",
    "SARIF saved: `<path>`", and friends were unbounded; right-elide
    keeps the dialog footer stable.
- **Auto-profile rule regexes cached across poll ticks.**
  `checkAutoProfileRules` previously compiled a fresh
  `QRegularExpression` per rule on every 2 s tick — 10 rules × 30
  ticks/min = 300 wasteful JIT compiles/min. Cached keyed on pattern
  string in a function-local static `QHash`. Invalid patterns (from
  mistyped user config) are now detected via `QRegularExpression::isValid()`
  and surfaced via a one-shot status message instead of silently
  never matching.

### Fixed

- **Quake-mode window no longer hides itself 200 ms after a show
  toggle.** The `m_quakeAnim` `QPropertyAnimation` is reused across
  hide/show toggles. The hide branch connected `finished→hide()` with
  `Qt::UniqueConnection`, but Qt can't dedupe lambda-wrapped slots, and
  the stale connection from the previous hide fired at the end of the
  show slide-down — the window flashed in then vanished. Fix:
  `QObject::disconnect(m_quakeAnim, &finished, this, nullptr)` before
  every `start()`, and re-attach the `hide()` slot only on the hide
  branch. Show branch has no `finished` listener (the animation simply
  lands at its on-screen end value).
- **Terminal resize preserves combining characters on both main and
  alt screen.** The simple-copy path in `TerminalGrid::resize()`
  (taken when `cols` is unchanged or when the alt-screen buffer is
  being resized alongside main) walked `TermLine::cells` only and
  default-constructed the `combining` side table — stripping accents,
  ZWJ sequences, and variation selectors off every on-screen cell
  whenever the user dragged the window edge. Symptom: filenames like
  `résumé.pdf` or `naïve.cpp` in `ls` output mutated to
  `re?sume?.pdf` / `nai?ve.cpp` on every resize. Fix: copy
  `TermLine::combining` and `softWrapped` alongside cells; filter out
  combining entries whose column exceeds the new width (shrink case).
  Alt-screen TUIs (vim, less, htop) that render accented filenames
  see the same bug and the same fix.
- **Config + session disk writes `fsync()` before atomic rename.**
  `Config::save`, `SessionManager::saveSession`, and
  `SessionManager::saveTabOrder` each follow the write-to-`.tmp`
  + rename pattern for atomicity, but `QFile::close` flushes only
  userspace buffers. On ext4 `data=ordered` (the common default), a
  kernel crash or power loss between `close()` and `rename()` can
  leave a zero-sized file replacing the previous good one —
  silently losing config or a whole session's scrollback. Matches
  the write-rename-fsync pattern used by SQLite/Git.

### Dev experience

- Packaging files caught up with the CMakeLists version. Pre-0.6.28
  CI had been red since 0.6.27 landed without the five packaging
  files (`.spec`, `PKGBUILD`, `debian/changelog`, man page,
  `metainfo.xml`) being bumped in lockstep —
  `packaging/check-version-drift.sh` correctly flagged the drift and
  blocked the merge. All six files now agree at 0.6.28 and CI passes.

## [0.6.27] — 2026-04-15

**Theme:** `clear`-command scrollback fix, scroll-offset clamp,
tab-close visibility, Claude "prompting" status, and allowlist-button
lifecycle tightening.

### Fixed

- **`clear` now properly wipes scrollback.** On `TERM=xterm-256color`
  (set by `PtyHandler`), `ncurses` `clear` emits `\E[H\E[2J\E[3J`
  (verified via `clear | od -c`). The 0.6.26 Ink-overflow-repaint
  heuristic — "mode 3 within 50 ms of mode 2 = Ink frame-reset, preserve
  scrollback" — false-matched this byte-burst because the `2J`+`3J` pair
  is identical to Ink's `clearTerminal`. User-reported symptom: after
  `clear`, the scrollbar remained scrollable showing nothing, because
  `m_scrollback` still held the pre-`clear` lines.

  Disambiguator: cursor position at the moment `3J` arrives. `clear`
  emits `H` FIRST so cursor sits at (0,0); Ink emits `H` LAST so cursor
  is still wherever the overflowing output left it (bottom of the
  screen in practice). Treating cursor-at-origin as "user-initiated
  clear" wipes scrollback on `clear` while still preserving it for Ink
  — because Ink's overflow only triggers when output has filled past
  the viewport, by construction the cursor is never at (0,0) when Ink's
  `3J` fires. New `user-clear` scenario in
  `tests/features/scrollback_redraw/test_redraw.cpp` pins the
  behaviour; the existing `ink-overflow-repaint` scenario continues to
  pass.
- **Scroll-to-bottom button no longer lingers after scrollback
  shrinks.** `TerminalWidget::m_scrollOffset` is a widget-side value
  that wasn't re-clamped when `TerminalGrid::m_scrollback.clear()` ran
  (via `\E[3J`). The scrollbar's `setValue` clamp hid the symptom for
  the scrollbar itself, but `m_scrollOffset > 0` was still the
  show-button condition and the viewport-rendering base (`viewStart =
  scrollbackSize - m_scrollOffset` went negative, painting phantom
  blank rows). Clamping `m_scrollOffset` to `[0, scrollbackSize]` in
  `updateScrollBar()` — the single choke-point called after every
  output batch — keeps the widget state consistent with the grid.
- **Tab close (×) button is now always visible.** The theme
  stylesheet had `QTabBar::close-button { image: none; }`, which hid
  the × glyph entirely and left only an invisible hover hit-target. New
  users didn't discover the close affordance unless they happened to
  mouse over it. Removing the `image` rule lets Qt fall back to the
  platform close icon (`QStyle::SP_TitleBarCloseButton`), which adapts
  to the active palette. Hover still applies an ansi-red background
  (%7) for "will-click" feedback.
- **"Add to allowlist" button now also clears the accompanying status
  message on dismiss.** Clicking the × button cleared the message, but
  the `claudePermissionCleared` path (prompt disappears on
  approve/decline) just deleted the button and left the
  "Claude Code permission: …" text stuck in the status bar. Both paths
  now call `clearStatusMessage()` symmetrically.

### Added

- **Claude status bar says "Claude: prompting" while a permission
  prompt is waiting.** Previously the label kept showing "Claude: idle"
  because `ClaudeIntegration`'s transcript-driven state machine is
  unaware of the on-screen permission UI. For a user scrolled up in
  history — who can't see the prompt directly — "idle" was
  misleading. The prompt state is now tracked per-window as a boolean
  overlay on top of the `ClaudeState`: any detected prompt flips the
  label to yellow "Claude: prompting" regardless of the underlying
  state, and the prompt-cleared signal reverts it to the base state.
  Implementation consolidates the label-rendering switch into a single
  `MainWindow::applyClaudeStatusLabel()` method so prompt and state
  changes share one code path.

## [0.6.26] — 2026-04-15

**Theme:** UX polish — status bar consistency, focus handling, Ink
overflow-repaint fix, tab-colour gradient.

### Fixed

- **Ink overflow-repaint no longer wipes scrollback or duplicates
  the replay.** Claude Code's Ink/React TUI emits
  `CSI 2J + CSI 3J + CSI H + fullStaticOutput + output` whenever its
  rendered frame overflows the viewport (see `ink/build/ink.js:705`,
  `ansi-escapes/base.js:124` `clearTerminal`). Previously the `CSI 3J`
  part of that burst wiped the user's conversation history and the
  subsequent replay duplicated into scrollback. Now `CSI 3J` arriving
  within 50 ms of `CSI 2J` on the main screen is classified as Ink's
  frame-reset marker: scrollback is preserved, and the 0.6.22
  suppression window is armed for the replay so the scroll-off lines
  don't duplicate. Standalone `CSI 3J` (user-initiated `clear -x` or
  similar) still wipes scrollback as the user requested. Two new
  conformance scenarios — `ink-overflow-repaint` and `standalone-3J`
  — in `tests/features/scrollback_redraw/test_redraw.cpp` pin both
  behaviours.
- **Claude Code context progress bar no longer paints persistent
  "0%".** `ClaudeIntegration::setShellPid` emits
  `stateChanged(NotRunning)` + `contextUpdated(0)` on every tab
  switch. The `stateChanged` slot hid the bar; the `contextUpdated(0)`
  slot unconditionally re-showed it. Fresh tabs, or tabs where Claude
  was never started, now stay hidden until a real non-zero percent
  is emitted.
- **Review Changes button no longer renders in small-caps monospace.**
  The 0.6.26-in-progress "bold at 11 px to clear Gruvbox contrast"
  attempt combined with Qt's app-wide monospace font (main.cpp:150-153)
  and Fusion style produced squished lowercase letterforms that read
  as uppercase. Button now inherits the global `QPushButton`
  stylesheet so it matches the sibling "Add to allowlist" button.
  Disabled state gets its own distinctive visual (dashed border +
  italic + muted fg) so a clean-repo state reads clearly as
  non-actionable.
- **Status bar is now height-consistent.** `QStatusBar`'s size hint
  tracks its tallest child, so the bar jumped when the transient
  "Add to allowlist" button appeared and shrank when it went away.
  A 32 px `setMinimumHeight` floor covering the default
  `QPushButton` size keeps the bar stable regardless of transient
  widget presence.

### Added

- **Git-branch chip ↔ transient-status-slot divider.** A 1-px
  vertical `QFrame::VLine` painted in `textSecondary` (not `border`,
  because `border` is nearly invisible against `bgPrimary` on
  low-contrast themes like Gruvbox-dark and Nord). Gives a
  deterministic visual boundary every theme renders correctly.
- **Focus auto-return to active terminal.** Connected to
  `QApplication::focusChanged`. When focus lands on chrome widgets
  (`QStatusBar`, `QTabBar`, bare `QMainWindow`) with no active modal,
  keyboard focus is deferred-set back to the terminal one tick
  later. Dialogs, menus, command palette, `QLineEdit`/`QTextEdit`
  input widgets, and `TerminalWidget` descendants all retain focus
  legitimately — the whitelist is specifically tuned to avoid
  hijacking user-meant focus.
- **Tab-colour gradient.** Per-tab colour badge changed from a 3 px
  bottom strip to a vertical gradient — transparent at the top,
  140/255 alpha of the chosen colour at the bottom. Tab text stays
  readable across every theme while the colour reads as an
  intentional wash. The active-tab 2 px accent underline
  (`QTabBar::tab:selected { border-bottom }`) is preserved by
  excluding the bottom 2 px from the gradient area.

## [0.6.25] — 2026-04-15

**Theme:** a user-reported scrollback regression from 0.6.24's
CSI-clear window extension, plus the two packaging-audit follow-ups
from the ninth periodic audit (`audit_2026_04_15`).

### Fixed

- **Viewport no longer shifts when streaming content arrives while
  the user is scrolled up in history.** User report (2026-04-15):
  with ~500 lines in the terminal, scrolled up a few lines, with
  Claude Code actively streaming — the content at the viewport kept
  getting overwritten ("line 251 becomes line 250"). Root cause:
  the 0.6.21 scrolled-up pause and the 0.6.22 (extended 0.6.24)
  post-full-clear suppression window both skipped scrollback pushes
  to protect against TUI redraw pollution. Skipping the push kept
  `TerminalGrid::scrollbackPushed()` frozen, which kept
  `TerminalWidget::onOutputReceived`'s scroll anchor from advancing
  `m_scrollOffset`. The screen still scrolled on every LF, so the
  viewport rows overlapping the screen got overwritten in place,
  and — worse — the anchor never re-pinned to the user's reading
  position in scrollback, so every subsequent push (once the
  suppression let up) moved the viewport further from where the
  user wanted to be. The 0.6.24 broadening to `CSI 0J` / `CSI 1J`
  made the window arm during the far more common "home + erase to
  end" repaint idiom, so the regression surfaced as near-continuous
  viewport drift during Claude Code activity. Fix: when the user is
  scrolled up (`m_scrollbackInsertPaused == true`), always push to
  scrollback — even inside the CSI-clear window. The anchor then
  advances `m_scrollOffset` by the push count, keeping `viewStart`
  pinned to the user's reading position. The doubling-protection
  window keeps its teeth for the at-bottom case (the only case where
  the doubling symptom is observable). New feature test
  `tests/features/scrollback_redraw/test_viewport_stable.cpp`
  asserts content at viewport rows is invariant across streaming
  for both the pause-only path and the CSI-clear path, across scroll
  offsets from barely-scrolled-up to deep-in-history. `spec.md`
  gains a §Viewport-stable companion contract documenting the
  invariant and its scope (must-hold when viewport is entirely in
  scrollback; partial guarantee when viewport overlaps the screen,
  since screen writes are inherently visible there).

### Changed

- **`packaging-version-drift` rule now scans AppStream metainfo.** The
  ninth audit noted that
  `packaging/linux/org.ants.Terminal.metainfo.xml` couples to every
  release — its `<release version="X.Y.Z">` list has to advance on
  each bump — but the drift rule was not scanning it. Added to the
  rule's coverage list; the grep extracts the newest declared release
  (metainfo lists releases newest-first, so `head -1` captures the
  latest) and diffs it against CMakeLists.txt's `project(VERSION)`.
  Verified end-to-end: a contrived CMake bump to `0.9.99` now emits
  six findings (five packaging recipes + the metainfo entry) instead
  of five.
- **Drift-check logic extracted into
  `packaging/check-version-drift.sh`.** The check used to live as a
  ~20-line bash string embedded in `src/auditdialog.cpp`. That meant
  CI couldn't reuse it without duplicating the logic and risking a
  new axis of drift. The script is now the single source of truth;
  `auditdialog.cpp` invokes it with a minimal `[ -x … ] && bash … ||
  exit 0` wrapper (graceful no-op when the tree is missing the
  script), and CI invokes it directly so a non-zero exit fails the
  job. Audit dialog's parser continues to ignore exit codes — same
  as before — so the CI-friendly non-zero-on-drift exit semantics
  don't break the in-app view.
- **CI gate: `Check packaging version drift` step in
  `.github/workflows/ci.yml`.** Runs the script after every push and
  pull request. A PR that bumps `CMakeLists.txt` without updating
  every coupled file — spec, PKGBUILD, debian/changelog, man page,
  AppStream metainfo, README — now fails CI before merge. Closes
  the honor-system gap that let 0.6.23 ship with four stale
  packaging files (and would have let this very release ship with
  drifted metainfo if the new metainfo coverage had not been added
  in the same commit).

## [0.6.24] — 2026-04-15

### Fixed

- **Status-bar "Add to allowlist" button now persists for the life of
  the permission prompt.** The previous lifecycle retracted the button
  on the next `TerminalWidget::outputReceived` signal after a 1 s grace
  — but Claude Code v2.1+ repaints its TUI continuously (cursor blink,
  spinner animation) even while the prompt is still visible, so the
  button vanished within seconds of appearing. Introduced a stricter
  `claudePermissionCleared` signal that fires only when the grid scan
  detects the prompt footer is no longer on screen, and rewired both
  the grid-scan and hook-stream button paths to retract against that
  signal instead. The button now stays visible for as long as the
  prompt is on screen.
- **"Review Changes" button legible on every theme, including those
  with deliberately-muted `textSecondary`.** The 5e2ac58 fix gave the
  disabled state an explicit `textSecondary` color — which works for
  themes where textSecondary is ~70% luminance, but still fails WCAG
  AA on themes like One Dark (`#5C6370` on `bgSecondary #21252B` ≈
  3:1 contrast). Switched to `textPrimary` + italic + dissolved
  border: guarantees legibility across all 11 themes while the italic
  still communicates "nothing to review right now."
- **Scrollback-doubling suppression window now covers all full-clear
  shapes, not just `CSI 2J`.** The 0.6.22 fix armed the 250 ms
  suppression window only on `eraseInDisplay` mode 2; a ninth-audit
  probe found that `CSI H; CSI 0J` and `CSI N;M H; CSI 1J`
  (with `(N,M)` at the bottom-right corner) produce the same
  post-state — every visible cell cleared — and therefore exhibit
  the same doubling bug on subsequent redraw. The window is now
  armed for all three equivalent-effect shapes. Added two regression
  scenarios to `tests/features/scrollback_redraw/` (`identical-repaint-0J`
  and `identical-repaint-1J`) that reproduce the bug pre-fix and pass
  post-fix. Spec §Contract updated to document the broader invariant.

### Changed

- **Packaging files caught up to 0.6.24.** The 0.6.23 release tag
  shipped with `CMakeLists.txt` at 0.6.23 but the openSUSE spec,
  Arch PKGBUILD, Debian changelog, and man page still reading 0.6.22
  — the exact drift the `packaging-version-drift` audit rule (0.6.22)
  was written to catch. Rule wasn't run on the 0.6.23 release commit.
  Also added `org.ants.Terminal.metainfo.xml` release entries for
  0.6.23 and 0.6.24 (AppStream metadata is not currently checked by
  the audit rule — follow-up item).

## [0.6.23] — 2026-04-15

**Theme:** status-bar event-driven contract + three user-reported UX
fixes (allowlist subsumption false-positive, tab-colour picker had no
visible effect, Shift+Enter paste-image regression) and the first
0.7.0 performance item — SIMD VT-parser scan.

### Fixed

- **Allowlist "already covered" check no longer blocks legitimate
  compound rules.** User-reported via screenshot: clicking
  **Add to allowlist** for `Bash(make * | tail * && ctest * | tail *)`
  opened the dialog with the right compound rule, but a pink warning
  said *"Already covered by existing rule: Bash(make *)"* and refused
  to add it — so the next compound invocation re-prompted. Root
  cause: `ClaudeAllowlistDialog::ruleSubsumes` did a flat
  `narrowInner.startsWith(broadPrefix + " ")` check, which was true
  for any compound command whose first segment matched. Fix: split
  the narrow rule on shell splitters (`&&`, `||`, `;`, `|`) and
  require every segment to start with the broad prefix before
  declaring subsumption. Matches the per-segment semantics
  `generalizeRule` has used since 0.6.22. A compound broad rule
  deliberately returns `false` rather than risk a false positive —
  a duplicate rule is benign, a false-positive warning is not.
  `ruleSubsumes` promoted from `private` to `public` so the feature
  test can exercise the contract directly. Feature test in
  `tests/features/allowlist_add/` adds 16 cases including the exact
  reproduction + simple-prefix regression guards + all four
  splitters. See `tests/features/allowlist_add/spec.md` §C.
- **Right-click tab → choose colour now renders visibly.** The
  context-menu's "Red / Green / Blue / …" picker stored the choice
  in `m_tabColors[idx]` and called `setTabTextColor`, but the
  window-level stylesheet rule `QTabBar::tab { color: %6; }` beat
  `setTabTextColor` on QSS specificity, so the colour was stored
  and never rendered. Fix: new `ColoredTabBar` subclass (storage
  via `QTabBar::tabData` — survives drag-reorder and auto-drops on
  close, unlike an index-keyed map) + a trivial `ColoredTabWidget`
  wrapper that installs it at construction (QTabWidget::setTabBar
  is protected). `paintEvent` overlays a 3-px bottom strip in the
  chosen colour *after* the base class paints, outside stylesheet
  influence. MainWindow's context-menu handler now calls
  `m_coloredTabBar->setTabColor(idx, color)` and the stale
  `m_tabColors` map is removed. New feature test
  `tests/features/tab_color/` asserts round-trip through
  `setTabColor`/`tabColor`, colour-follows-tab across `moveTab`
  (drag reorder), and cleanup on `removeTab` with no index leak
  into a newly-inserted tab. See `tests/features/tab_color/spec.md`.
- **Status bar now clears event-scoped transients on tab switch.**
  User-stated contract: the status bar is event-driven — state /
  location widgets (git branch, foreground process, Claude state,
  context %, Review Changes button) are always visible and reflect
  the active tab; transient notifications carry a timeout; widgets
  tied to a specific event are visible only while the event is live
  on its originating tab. `MainWindow::onTabChanged` now calls
  `clearStatusMessage()`, hides `m_claudeErrorLabel`, and
  `deleteLater`'s every `QPushButton` on the status bar with
  `objectName == "claudeAllowBtn"` at the top of the handler —
  before state-bar refresh (`updateStatusBar`, `refreshReviewButton`,
  `setShellPid`) paints the new tab's state. Without the cleanup,
  switching from "tab A asked permission" to tab B left the Allow
  button visible inviting approval of the wrong prompt, and
  theme-change notifications from tab A lingered for five seconds
  on tab B. Contract codified in
  `tests/features/claude_status_bar/spec.md` §D (new section).
- **Shift+Enter no longer pastes the clipboard image.** User-reported:
  with a picture on the clipboard, Shift+Enter inserted the image
  (saved to disk, filepath pasted) instead of the literal newline.
  The keypress was being routed through the Kitty keyboard-protocol
  encoder when `kittyKeyFlags() > 0`, which emits `CSI 13;2u` for
  Shift+Enter; some TUIs (Claude Code v2.1+ among them) treat that
  sequence as "paste clipboard" when an image is present. Fix: move
  the Shift+Enter literal-newline block to fire *before* the Kitty
  encoder so the sequence Ants puts on the wire is always
  `\x16\n` (readline quoted-insert + LF) regardless of the
  negotiated Kitty flags. Ctrl+Shift+Enter's scratchpad continues
  to take precedence; the new guard
  `!(mods & Qt::ControlModifier)` makes that explicit.
  `src/terminalwidget.cpp` keyPressEvent reorder.

### Added

- **SIMD VT-parser scan (0.7.0 Performance ROADMAP item).** The
  Ground state now fast-paths runs of printable-ASCII bytes (`0x20
  ..0x7E`) via a 16-byte-wide SIMD lane, avoiding per-byte
  state-machine work. Implementation in `src/vtparser.cpp` uses
  SSE2 on x86_64 and NEON on ARM64 with a scalar tail / fallback;
  a signed-compare trick (XOR 0x80 → two `cmpgt_epi8`s against
  pre-computed bounds) flags any byte outside the safe range in
  one `movemask` + `ctz`. TUI repaints (the workload that drove
  the 0.6.22 scrollback-doubling fix) are the biggest winner —
  thousands of printable bytes at a time now go through a tight
  SIMD loop instead of four function calls per byte. New feature
  test `tests/features/vtparser_simd_scan/` asserts the emitted
  action stream is byte-identical across whole-buffer, byte-by-byte,
  and pseudo-random-chunk feed strategies over a 38-case corpus
  including every offset-mod-16 alignment for an interesting byte
  (regression guard against scan-boundary off-by-ones). See
  `tests/features/vtparser_simd_scan/spec.md`.

## [0.6.22] — 2026-04-15

**Theme:** root-cause fix for the main-screen TUI "scrollback doubling"
regression, plus packaging catch-up (four recipes bumped to 0.6.22, three
missing AppStream `<release>` entries, openSUSE `%post`/`%postun`
scriptlets, Debian `postinst`/`postrm`). The scrollback fix is the
headline — Claude Code v2.1+ repaints its entire TUI on *every* state
update (not just `/compact`), and each repaint was pushing a full-screen
worth of duplicate content into scrollback. 0.6.21's scrollback-insert
pause only triggered while the user was scrolled up; this release adds
a sliding time window anchored to main-screen `CSI 2J`, so the
suppression works at the bottom too.

### Fixed

- **"Review Changes" button is now state-reactive.** User-reported:
  clicking the button sometimes produced a "No changes detected" toast
  and did nothing else, because the button was shown unconditionally
  on every `fileChanged` signal without checking whether a diff
  actually existed by the time the user clicked. Three-state design:
  `git diff --quiet HEAD` (async, off the UI thread) determines
  (a) **hidden** — no active terminal, no cwd, or cwd is not a git
  repo; (b) **visible + disabled** — clean git repo (Claude edited
  something but diff is gone); (c) **visible + enabled** — real diff
  present. Re-checked on `fileChanged`, on tab switch, and after the
  diff viewer finds empty output.
- **Add-to-Allowlist rule generalisation now covers every shell
  splitter.** `generalizeRule()` previously only handled `cd X && cmd`
  (and even that dropped the `cd` prefix, producing rules that Claude
  Code's allowlist matcher rejected on the next compound invocation).
  User feedback: the "Add to allowlist" button would add a rule that
  re-prompted on the very next turn. Rewritten to split on `&&`, `||`,
  `;`, `|` while preserving compound structure: `Bash(cd /x && make y)`
  now generalises to `Bash(cd * && make *)` (matching the convention
  already in the user's `settings.local.json`), `Bash(cat f | grep x)`
  to `Bash(cat * | grep *)`, etc. Safety denylist retained and
  extended to ALL segments (any `rm`/bare-`sudo`/`SUDO_*` anywhere in
  the chain blocks generalisation). Feature test in
  `tests/features/allowlist_add/` exercises 23 cases covering every
  splitter variant and the denylist.
- **Claude Code status bar no longer shows stale state after tab switch.**
  User-reported: "Claude status indicator doesn't work half the time."
  Root cause: `ClaudeIntegration::setShellPid()` re-pointed the watcher
  to the new tab's shell PID but left the cached `m_state` /
  `m_currentTool` / `m_contextPercent` from the previous tab intact
  until the next poll tick (~1 s later). Tab A's "Claude:
  thinking..." bled into tab B's status until polling caught up. Fix:
  when `setShellPid()` receives a PID different from the current one,
  immediately clear the cached state, drop the transcript watch, and
  emit `stateChanged(NotRunning, "")` + `contextUpdated(0)` so the UI
  reflects the switch within the current event-loop iteration. Paired
  with a new feature test
  (`tests/features/claude_status_bar/spec.md`) that verified the
  fix both ways — fails against the un-fixed `setShellPid`, passes
  with the one-line state-clear.
- **Visual artifacts from leaked SGR decorations.** User-reported: TUI
  apps (Claude Code v2.1+ is the motivating case) leave strikethrough /
  plain-single underline active across rows, producing horizontal
  dashed lines on otherwise-blank rows and leaked decorations on
  pending task-list items. Two-layer fix: (a) render-side filter skips
  strikethrough and plain single-color underline draws on empty-glyph
  cells (space / null codepoint) — the attribute is still recorded,
  just not painted where it has no glyph to decorate, so "underlined
  text with a space in the middle" still draws correctly per-glyph;
  (b) SGR 8 / 28 (conceal / reveal) and SGR 53 / 55 (overline /
  no-overline) now have explicit handlers instead of falling through
  to the default branch silently. Hovered-URL underline hint always
  draws regardless of cell content. See
  `tests/features/sgr_attribute_reset/spec.md` for the invariant
  asserted by CTest.
- **Scrollback no longer accumulates duplicates during main-screen TUI
  repaints.** On main-screen full-display erase (`CSI 2J`,
  `eraseInDisplay(2)`), open a 250 ms sliding window during which
  `scrollUp()` drops the push-to-scrollback step. Each scrollUp during
  the window extends it, so the window survives the entire repaint
  burst regardless of length, but closes promptly when the app goes
  quiet. Alt-screen bypasses scrollback already and is unaffected;
  mode-3 erase (which clears scrollback by request) is also unaffected.
  Defensively cleared on alt-screen entry. The 0.6.21 `m_scrollOffset
  > 0` pause (for users actively reading history) is retained — the
  two triggers compose (either one suppresses). File: `terminalgrid.cpp`
  `eraseInDisplay()` + `scrollUp()`; header additions in
  `terminalgrid.h` (`m_csiClearRedrawActive`, `m_csiClearRedrawTimer`,
  `kCsiClearRedrawWindowMs`).

### Added — Audit tooling

- **Trivy filesystem-scan lane in Project Audit.** New "Trivy
  Filesystem Scan" check runs `trivy fs` with the `vuln`, `secret`,
  and `misconfig` scanners in one invocation, severity-floor
  MEDIUM. Output is piped through `jq` into the standard
  `path:line: severity: scanner/rule-id: title` shape so
  `parseFindings()` consumes it directly. Self-disables when
  either `trivy` or `jq` is missing (the description text
  explains how to install). Auto-selected when both are present.
- **Three new audit rules.** `cmake_no_version_floor` (find_package
  REQUIRED with no version constraint), `bash_c_non_literal`
  (`bash -c <ident>` injection-sink heuristic, restricted to lines
  containing `bash` / `sh` so it doesn't flag `grep -c`), and
  `packaging_version_drift` (cross-file consistency: extracts
  CMakeLists.txt project(VERSION X.Y.Z) and diffs it against
  every packaging recipe). Both regex-based rules ship with
  bad/good fixture pairs under `tests/audit_fixtures/`; the
  drift check is exercised by the audit dialog itself.

### Added — Test infrastructure

- **Feature-conformance test harness (`tests/features/`).** New test
  lane alongside the existing `audit_rule_fixtures`. Each feature
  subdirectory ships a `spec.md` (human contract — reviewed like
  code) and a C++ test executable that exercises the feature through
  its public API and asserts the contract holds. GUI-free, fast,
  labelled `features` so CI and local `ctest -L features` pick them
  up. First test landed: `scrollback_redraw` — the regression guard
  for this release's headline fix. Verified that it FAILS against
  0.6.21 code (growth=77 vs. threshold=34, reproducing the doubling)
  and PASSES against 0.6.22 — i.e. it *would have caught the
  incomplete 0.6.21 fix at commit time*. See `tests/features/README.md`
  for the authoring guide and rationale. CLAUDE.md updated.

### Changed — Packaging

- **openSUSE spec, Arch PKGBUILD, Debian changelog, AppStream metainfo
  all bumped to 0.6.22.** Prior releases had drifted: spec and PKGBUILD
  still showed 0.6.20, metainfo's most recent `<release>` was 0.6.17
  (so 0.6.18/.19/.20/.21 were invisible to GNOME Software / KDE
  Discover users reading the metainfo catalogue).
- **openSUSE spec gets `%post`/`%postun` scriptlets** (`update-desktop-
  database`, `gtk-update-icon-cache`). Without these, minimal
  Tumbleweed images won't see the launcher in the application menu
  until next session restart. Matches Fedora/openSUSE packaging
  guidelines for any package shipping a `.desktop` entry + hicolor
  icons.

## [0.6.21] — 2026-04-15

**Theme:** audit-tool noise reduction, dialog theming, and a long-
standing main-screen-TUI scrollback corruption fix. Self-audit triage
turned up a GNU grep 3.12 argument-ordering quirk that silently
disabled the `--include` file-type filter, producing the bulk of the
CRITICAL/MAJOR false positives. Same release extends the QSS cascade
so every popup dialog inherits the terminal's active theme, and adds
a scrollback-insert pause that stops TUI redraws from interleaving
intermediate frames into the history the user is trying to read.

### Changed

- **Scroll-lock on scrollback read (main-screen TUI fix).** When a
  TUI program runs on the main screen (Claude Code v2.1+ is the
  flagship example — it renders its approval prompts and context bar
  via cursor movement + overwrite, not alt-screen) and the user has
  scrolled up to read history, every cursor-up-and-rewrite cycle that
  extends past the scroll region used to push the overwritten top
  line into scrollback. With a redraw-heavy TUI that fires many
  frames per second (spinner, context growth, expanding tool blocks),
  this interleaved duplicate frames into the scrollback the user was
  reading — the user's perceived position stayed stable (viewport
  anchor shifts with `scrollbackPushed`), but the history below their
  read point filled with duplicated content and the "scrolled" view
  drifted. Fix: `TerminalGrid::scrollUp()` now honours a new
  `scrollbackInsertPaused` flag, set by `TerminalWidget::onPtyData()`
  while `m_scrollOffset > 0`. Lines that would have been pushed into
  scrollback during a paused window are simply dropped (they were
  about to be overwritten in-place anyway). Flag auto-clears on the
  next PTY packet once the user returns to the bottom.
- **Dialog theming cascade.** `MainWindow::applyTheme()` now ships
  a comprehensive QSS covering `QDialog`, `QLabel`, `QPushButton`,
  `QLineEdit`, `QTextEdit` / `QPlainTextEdit` / `QTextBrowser`,
  `QCheckBox`, `QRadioButton`, `QComboBox`, `QSpinBox` /
  `QDoubleSpinBox`, `QGroupBox`, `QListWidget` / `QTreeWidget` /
  `QTableWidget`, `QHeaderView`, `QScrollBar` (v+h), `QToolTip`,
  `QProgressBar`, and `QDialogButtonBox`. Every pop-up created with
  the main window as parent (Settings, Audit, AI, SSH, Claude
  Projects/Transcript/Allowlist, homograph-link warning, permission
  prompts, `QMessageBox`, `QInputDialog`, `QFileDialog`, `QColorDialog`,
  …) now paints with the terminal's active theme colors — no more
  light-gray system defaults leaking through. Cached dialogs are
  re-polished on theme switch so live widgets pick up new colors
  without re-instantiation.

### Fixed

- **Audit: grep argument-ordering bug (major noise source).** GNU grep
  3.12 silently drops the `--include=<glob>` filter whenever a file-level
  `--exclude=<name>` appears earlier on the command line — every file
  under the search root is scanned regardless of extension. Our
  `addGrepCheck()` builder put `kGrepExclSec` (which carried
  `--exclude=auditdialog.cpp --exclude=auditdialog.h`) before
  `kGrepIncludeSource`, so security scans that should have been scoped
  to source files were also matching `.md`, `.xml`, `CMakeLists.txt`,
  and the audit tool's own pattern-definition strings. Split the
  exclude-dir and file-exclude portions, append file-excludes AFTER all
  `--include` flags (including caller-supplied extras). Cuts the
  CRITICAL `cmd_injection` false-positive count from 8 → 4 (remaining
  hits are either real — `execlp` in the forkpty child, by design — or
  intentional test fixtures).
- **Audit: `lineIsCode()` over-counted string-only lines as code.**
  The comment/string-aware filter flagged continuation lines like
  `"system(), popen()…", "Security",` as real code because the
  string-delimiter character itself was treated as a code token.
  Tightened to require an identifier- or operator-class character
  outside strings/comments before a line counts as code. Drops the
  remaining self-referential matches in `auditdialog.cpp`'s own
  pattern-definition arguments without affecting legitimate mixed
  code+string lines like `int n = f("foo");`.
- **Audit: cppcheck `invalidSuppression` noise.** cppcheck's own
  `--inline-suppr` parser misreads documentation comments that mention
  the literal `cppcheck-suppress` token (e.g. the passthrough-marker
  docs in `auditdialog.cpp`) as actual suppression directives and
  emits `invalidSuppression` errors. Added `--suppress=invalidSuppression`
  to both cppcheck invocations — the category catches only meta-parsing
  failures, never real code bugs, so losing it has no downside. Also
  restructured the offending docs (`auditdialog.{h,cpp}`) so the token
  no longer appears as the first word of a comment body.

## [0.6.20] — 2026-04-15

**Theme:** ROADMAP §H5 (Distribution-readiness bundle 5) — ready-to-submit
packaging recipes for the three mainstream non-Flatpak distros. Each
recipe drives the existing CMake install rules (no build-system forks,
no per-distro patches) and runs the audit-rule regression suite in its
`%check` / `check()` / `dh_auto_test` step. Fully additive, no runtime
code changes.

### Added

- **`packaging/opensuse/ants-terminal.spec`** — openSUSE RPM spec
  targeting Tumbleweed. Uses the core [openSUSE CMake macros][suse-cmake]
  (`%cmake`, `%cmake_build`, `%cmake_install`, `%ctest`, `%autosetup`)
  so the file is close to portable to Fedora. BuildRequires declared
  via `cmake(Qt6*)` pkgconfig-style entries so OBS resolves them
  against whichever Qt6 stack the target project carries. `%files`
  enumerates all fifteen install paths explicitly so a missing or
  relocated artefact fails the OBS build instead of producing a
  silently-incomplete package.
- **`packaging/archlinux/PKGBUILD`** — [AUR][aur] recipe on the release
  track (package name `ants-terminal`). `check()` runs ctest. `sha256sums`
  is `SKIP` in the in-tree recipe with a comment pointing packagers at
  `updpkgsums` since the upstream tarball doesn't exist until the tag
  is pushed. A rolling `-git` variant is documented in
  `packaging/README.md` (three-line diff: `pkgname`, `source`,
  `pkgver()`).
- **`packaging/debian/`** — Debian source tree: `control`, `rules`,
  `changelog`, `copyright`, `source/format`. `debhelper-compat 13`
  drives `dh $@ --buildsystem=cmake` with Ninja as the backend; DEP-5
  `copyright` carries the full MIT license text.
  `DEB_BUILD_MAINT_OPTIONS = hardening=+all` stacks dpkg-buildflags'
  hardening wrappers on top of our CMake hardening flags. Suitable
  for `debuild -uc -us`, a Launchpad PPA, or an eventual Debian ITP.
- **`packaging/README.md`** — one-page build / submission guide for
  all three recipes: local-build recipe, OBS / AUR / PPA submission
  flow, `dch` / `osc vc` / `updpkgsums` version-bump recipes, and
  the `-git` variant diff for Arch.

### Changed

- `CMakeLists.txt` `project(... VERSION 0.6.19)` → `0.6.20`. The
  single-source-of-truth macro `ANTS_VERSION` propagates to
  `setApplicationVersion`, the SARIF driver, the dialog title badge,
  and the HTML-export metadata on rebuild.
- `packaging/linux/ants-terminal.1` `.TH` line now reads
  `ants-terminal 0.6.20` (was stuck at `0.6.18` — missed the 0.6.19
  bump). groff ships the version string in its header/footer, so
  `man ants-terminal` now matches `ants-terminal --version` again.
- README current-version line bumped to 0.6.20. The **Install
  footprint** table didn't need to change — H5 is pure packaging
  metadata, not new installed files.
- ROADMAP §H5 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.8.0 📦 narrative section, with a link
  back to this CHANGELOG entry. H1–H5 distribution slice now five
  bundles deep; remaining 0.8.0 packaging work is H6 (Flatpak) and
  H7 (docs site).

[suse-cmake]: https://en.opensuse.org/openSUSE:Packaging_for_Leap
[aur]: https://wiki.archlinux.org/title/Arch_User_Repository

## [0.6.19] — 2026-04-15

**Theme:** ROADMAP §H4 (Distribution-readiness bundle 4 of 4) — shell
completions for the current `QCommandLineParser` surface, installed to
the conventional vendor locations for bash, zsh, and fish so distro
packages don't have to relocate them. Closes the H1–H4 distribution
slice; remaining packaging work (H5 spec/PKGBUILD/debian, H6 Flatpak,
H7 docs site) lives in 0.8.0+. Fully additive, no runtime code changes.

### Added

- **`packaging/completions/ants-terminal.bash`** — [bash-completion][bash-completion]
  function `_ants_terminal` registered via `complete -F`. Suggests the
  full long/short option set; suppresses suggestions after `--new-plugin`
  (the next token is a freeform plugin name with no useful enumeration).
- **`packaging/completions/_ants-terminal`** — [zsh `#compdef`][zsh-compsys]
  function using `_arguments`, with mutually-exclusive groups so `--quake`
  and `--dropdown` only offer the unspecified alias, and `-h` / `-v`
  short-circuit further completion (`(- *)` exclusion).
- **`packaging/completions/ants-terminal.fish`** — [fish `complete`][fish-complete]
  declarations, one per flag, with descriptions that mirror the manpage
  one-liners. `--new-plugin` declared `-r -x` so fish knows it consumes
  the next token but doesn't try to file-complete it.
- **CMake install rules** for all three files at the GNU vendor
  locations: `${datarootdir}/bash-completion/completions/ants-terminal`,
  `${datarootdir}/zsh/site-functions/_ants-terminal`, and
  `${datarootdir}/fish/vendor_completions.d/ants-terminal.fish`. All
  three are auto-discovered without the user sourcing anything by hand.
- **CI lint job** — `.github/workflows/ci.yml` runs `bash -n` on the
  bash file, `zsh -n` on the zsh file, and `fish --no-execute` on the
  fish file so syntax regressions fail the build the same way the H2
  AppStream / desktop-entry validators and H3 groff lint do.

### Changed

- README **Install System-wide** table gains three rows for the
  completion files so the install footprint stays self-documenting.
- ROADMAP §H4 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.7.0 📦 narrative section, with links back
  to this CHANGELOG entry. Closes out the H1–H4 distribution slice.

[bash-completion]: https://github.com/scop/bash-completion
[zsh-compsys]: https://zsh.sourceforge.io/Doc/Release/Completion-System.html
[fish-complete]: https://fishshell.com/docs/current/cmds/complete.html

## [0.6.18] — 2026-04-15

**Theme:** ROADMAP §H3 (Distribution-readiness bundle 3 of 4) — a
spec-compliant `man 1 ants-terminal` page so distro packagers and
users alike get the standard Unix discovery experience (`man
ants-terminal`, `apropos terminal`, `whatis ants-terminal`). Fully
additive, no runtime code changes.

### Added

- **`packaging/linux/ants-terminal.1`** — [groff -man][groff-man]
  source covering `NAME` / `SYNOPSIS` / `DESCRIPTION` / `OPTIONS`
  (`-h`/`--help`, `-v`/`--version`, `--quake`/`--dropdown`, and
  `--new-plugin <name>` with its full validation contract) /
  `ENVIRONMENT` (`SHELL`, `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`,
  `ANTS_PLUGIN_DEV`, `QT_QPA_PLATFORM`) / `FILES` (config /
  themes / plugins / sessions / recordings / logs / `audit_rules.json`
  / `.audit_suppress`) / `EXIT STATUS` (the four `--new-plugin` codes)
  / `BUGS` / `AUTHORS` / `SEE ALSO` (cross-refs to xterm, konsole,
  gnome-terminal, tmux, ssh, forkpty(3), appstreamcli,
  desktop-file-validate). Renders cleanly under both `groff -man` and
  `man -l`.
- **CMake install rule** for the man page — `install(FILES … DESTINATION
  ${CMAKE_INSTALL_MANDIR}/man1)`. DESTDIR-staged install verified to
  drop the page at `…/share/man/man1/ants-terminal.1`, which `man-db`
  picks up automatically once the prefix is on `MANPATH` (true for
  `/usr` and `/usr/local` out of the box).
- **CI lint job** — `.github/workflows/ci.yml` installs `groff` and
  runs `groff -man -Tutf8 -wall packaging/linux/ants-terminal.1
  >/dev/null` so syntax regressions in the man source fail the build
  the same way `appstreamcli` / `desktop-file-validate` do for the
  desktop entry and metainfo XML.

### Changed

- README **Install System-wide** table gains a row for the man page so
  the install footprint stays self-documenting alongside the binary,
  desktop entry, metainfo, and icons.
- ROADMAP §H3 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.7.0 📦 narrative section, with links back
  to this CHANGELOG entry.

[groff-man]: https://man7.org/linux/man-pages/man7/groff_man.7.html

## [0.6.17] — 2026-04-14

**Theme:** ROADMAP §H2 (Distribution-readiness bundle 2 of 4) —
AppStream metainfo and a spec-compliant desktop entry so Ants appears
in GNOME Software / KDE Discover catalogues once a distro packages it.
Fully additive, no runtime code changes.

### Added

- **`packaging/linux/org.ants.Terminal.metainfo.xml`** — [AppStream 1.0][as1]
  descriptor: `id` / `name` / `summary` / multi-paragraph
  `description` / `categories` / `keywords` / `url` (homepage,
  bugtracker, vcs, help, contribute) / `provides/binary` /
  `supports/control` / `content_rating` (OARS 1.1, all-none) /
  `launchable` tying to the `.desktop` id / `releases` with 0.6.14–
  0.6.17 changelog summaries. `metadata_license` CC0-1.0,
  `project_license` MIT. Validated by `appstreamcli validate`
  (pedantic clean aside from the reverse-DNS uppercase hint, which
  matches the convention used by `org.gnome.Terminal` and
  `org.kde.Konsole`).
- **`packaging/linux/org.ants.Terminal.desktop`** — reverse-DNS app
  id so it round-trips with the metainfo `launchable`. Fields:
  `Type=Application`, `Terminal=false`, `Categories=Qt;System;Terminal-
  Emulator;` (exactly one main category — avoids the multi-listing
  menu hint), `Keywords=` tightened, `StartupWMClass=ants-terminal`,
  `TryExec=ants-terminal`. Two `Desktop Action` entries — `NewWindow`
  and `QuakeMode` (wires `--quake`) — for right-click launcher
  integration in most DEs. Validated by `desktop-file-validate` clean.
- **CMake install rules** (`include(GNUInstallDirs)`) for the desktop
  file (`share/applications/`), metainfo XML (`share/metainfo/`), and
  the six hicolor icons renamed to `ants-terminal.png` under
  `share/icons/hicolor/<size>x<size>/apps/`. `DESTDIR=…` staging works
  out of the box for distro build roots.
- **CI validation job** — `.github/workflows/ci.yml` installs
  `appstream` + `desktop-file-utils` and runs `appstreamcli validate
  --explain` + `desktop-file-validate` on every push so schema drift
  in the packaging files fails the build instead of landing in a
  release tarball.

### Changed

- README **Install System-wide** section replaces the bare `make
  install` with `cmake --install build`, adds a table of every path
  the install rule lays down (bin/ desktop/ metainfo/ hicolor icons),
  and notes `DESTDIR=…` support for packagers.
- README **Desktop Entry** section split into two paths: the dev
  workflow (`ants-terminal.desktop.in` + `launch.sh` for running
  uninstalled from the build tree) and the distro workflow (the new
  `packaging/linux/` files installed via CMake). Dev path unchanged;
  packaged path is new.
- README **Project Structure** map mentions `packaging/linux/`.
- ROADMAP §H2 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.7.0 📦 narrative section, with links to
  this CHANGELOG entry.

[as1]: https://www.freedesktop.org/software/appstream/docs/

## [0.6.16] — 2026-04-14

**Theme:** ROADMAP §H1 (Distribution-readiness bundle 1 of 4) —
community and security policy docs that distro packaging teams look
for before accepting a package. Fully additive, docs-only.

### Added

- **`CODE_OF_CONDUCT.md`** — [Contributor Covenant 2.1][cc21] verbatim
  with two reporting channels: a dedicated maintainer email and the
  private GitHub Security Advisory (tagged `[conduct]` for triage) for
  reporters who prefer GitHub's private-report flow. Email is listed
  first per CoC convention — no GitHub account required. Ships
  ROADMAP §H1.
- **`SECURITY.md`** — coordinated-disclosure policy: supported-versions
  table, reporting channels (GitHub Security Advisory preferred;
  signed encrypted email for reporters who need it), 48h / 7d / 30d /
  90d disclosure timeline, severity rubric (Critical / High / Medium /
  Low), in-scope / out-of-scope lists, and an acknowledgement of the
  hardening already in the tree (image-bomb budgets, URI scheme
  allowlist, plugin sandbox, OSC 52 quotas, multi-line paste
  confirmation, compile-time hardening flags, ASan + UBSan CI).
  Completes ROADMAP §H1. (The file itself landed in commit
  `4599813`; this release changelogs it alongside the CoC.)
- **Distribution-adoption plan in `ROADMAP.md`** — new 📦 theme plus a
  rollup section covering bundles H1–H16 (security/CoC, AppStream +
  desktop, man page, shell completions, distro packaging for
  openSUSE / Arch / Debian, Flatpak, docs site, macOS, accessibility,
  i18n, reproducible builds, SBOM, distro submissions, deb/rpm
  signing). Nothing ships from this plan yet except H1; it exists so
  later releases have a single source of truth for packaging work.

### Changed

- ROADMAP transition: §H1 status 📋 → ✅ in both the
  distribution-adoption overview table and the §0.7.0 📦 narrative
  section, with links to this CHANGELOG entry.
- README's **Contributing** section now links `CONTRIBUTING.md`,
  `CODE_OF_CONDUCT.md`, and `SECURITY.md` before the step-by-step
  fork/PR instructions, so first-time contributors see the policy
  docs before they see the mechanics.
- `CONTRIBUTING.md` intro references the CoC (participation
  agreement) and `SECURITY.md` (private channel for sensitive
  reports) so the rules aren't buried.

[cc21]: https://www.contributor-covenant.org/version/2/1/code_of_conduct/

## [0.6.15] — 2026-04-14

**Theme:** the **incremental reflow** optimization from ROADMAP §0.7.0
Performance. Resizing a terminal with a long scrollback used to re-join
every line into a logical-line buffer and re-wrap every logical line
back into TermLines, allocating scratch vectors and copying cells
cell-by-cell — O(scrollback) allocation churn on every width change.
This release adds a fast path for the common case: standalone lines
(not part of a soft-wrap sequence) whose content fits the new width
get resized in place, skipping join/rewrap entirely.

### Changed

- **Incremental scrollback reflow** in `TerminalGrid::resize()`. The
  new algorithm walks scrollback once:
  - **Fast path** (standalone line, `softWrapped == false`, not mid-
    sequence, content width ≤ new cols): `tl.cells.resize(cols)`
    with default-attr padding when growing, or trim trailing blanks +
    drop combining-char entries at now-removed columns when shrinking.
    No allocation of scratch TermLines. No cell-by-cell copy.
  - **Slow path** (soft-wrap sequence OR standalone that doesn't fit):
    the existing `joinLogical` + `rewrap` round-trip, unchanged. This
    preserves correctness for trailing-space trim, combining-char
    position merging, and wide-character boundary handling.
  Typical scrollback is dominated by standalone lines ≤ the new width,
  so the fast path handles most rows. Multi-line soft-wrap sequences
  (long commands that wrapped at the prompt's edge) still round-trip
  through the full logic so nothing regresses there.
- ROADMAP transition: §0.7.0 → ⚡ Performance → "Incremental reflow on
  resize" 💭→✅ with a link to this CHANGELOG section. See
  [ROADMAP.md](ROADMAP.md#070--shell-integration--triggers-target-2026-06).

### Notes

- **No behavior change on correctness.** The fast path only applies
  when the line's logical-line representation would be a single
  TermLine identical (up to trailing blanks) to the one we produce by
  direct resize. For every case the slow path is still reachable via
  the existing code.
- **Screen-line reflow is unchanged.** Screen rows are already
  small-N (typically ≤ 50) so the O(N) savings aren't worth the
  added branch complexity; the joinLogical/rewrap loop runs on
  `m_screenLines` as before.
- **Alt-screen and scrollback-hyperlink reflow paths unchanged.**
  Alt-screen is explicitly `simple copy` today; scrollback hyperlink
  spans are not re-wrapped at all (pre-existing behavior) — both
  outside the scope of this optimization.
- **No plugin API change, no config schema change, no UI change.**
  `PLUGINS.md` does not need a bump.

## [0.6.14] — 2026-04-14

**Theme:** small security follow-up to 0.6.13 — catch a URI-scheme
injection vector in the new `make_hyperlink` trigger action that
self-review after shipping turned up. No ROADMAP transition; this is
a hardening patch on an already-shipped feature.

### Security

- **URI scheme allowlist in `TerminalGrid::addRowHyperlink()`**. The
  `make_hyperlink` trigger action expands `$0..$9` backrefs from the
  matched PTY output into the URL template — and PTY output is
  attacker-controllable (SSH sessions, remote shells, any inner-tty
  process). Without a scheme check, an innocent-looking template
  like `https://example.com/$1` could be weaponized when the
  pattern matches an attacker-supplied string that expands to
  `javascript:alert(1)` or `data:text/html,...`. The new check
  mirrors the existing OSC 8 allowlist (`http`, `https`, `ftp`,
  `file`, `mailto`); hyperlinks with any other scheme are silently
  dropped, matching OSC 8's drop-on-bad-scheme semantics — text
  still prints, it just isn't clickable.

### Notes

- Same allowlist used by OSC 8 remote hyperlink parsing in
  `handleOsc()`; kept in lockstep so both sources share the same
  threat model.
- No config schema change, no plugin API change, no UI change.
- `QSet` added to `terminalgrid.cpp` includes for the allowlist
  storage.

## [0.6.13] — 2026-04-14

**Theme:** close out the last deferred bullet from the 0.6.9 trigger-system
bundle — the iTerm2 **HighlightLine**, **HighlightText**, and
**MakeHyperlink** actions, which needed grid-cell mutation surgery that
chunk-level matching (checkTriggers) couldn't provide. This release adds
a new line-completion callback on `TerminalGrid` that fires from
`newLine()` with the row the cursor is leaving, and `TerminalWidget`
runs the grid-mutation subset of trigger rules against the finalized
line text there — so matches map to exact col ranges on a real row, and
mutations land before the row scrolls into scrollback.

### Added

- **`highlight_line` trigger action** — when the pattern matches in
  finalized output, recolors the **entire line** the match landed on.
  Action Value format: `"#fg"` (fg only), `"#fg/#bg"` (both), or
  `"/#bg"` (bg only). Empty-string slots mean "leave unchanged" —
  so partial overrides compose cleanly with the line's existing SGR
  attributes.
- **`highlight_text` trigger action** — same color-string format as
  `highlight_line`, but recolors **only the matched substring** using
  the regex's capture-0 span as the col range.
- **`make_hyperlink` trigger action** — turns the matched substring
  into a clickable OSC 8-equivalent hyperlink. Action Value is a URL
  **template** with `$0..$9` backrefs expanded against regex capture
  groups (`$0` = whole match, `$1..$9` = groups). Example: pattern
  `#(\d+)` + template `https://github.com/milnet01/ants-terminal/issues/$1`
  → a clickable issue link on every `#123` reference in output. Matches
  iTerm2's MakeHyperlink contract.
- **`TerminalGrid::setLineCompletionCallback(…)`** — new public API.
  Fired from `newLine()` **before** any cursor/scroll motion with the
  screen row the cursor was leaving. Used by TerminalWidget to run
  grid-mutation trigger rules; any other consumer can hook in for
  per-line post-processing (e.g., future trigger-system actions that
  need line-level context).
- **`TerminalGrid::applyRowAttrs(row, startCol, endCol, fg, bg)`** —
  overrides per-cell fg/bg on an inclusive col range of a screen row.
  Invalid QColor() leaves that channel untouched so callers can write
  fg-only, bg-only, or both. Marks the row dirty for re-paint.
- **`TerminalGrid::addRowHyperlink(row, startCol, endCol, uri)`** —
  pushes a hyperlink span onto the screen row's per-line
  `m_screenHyperlinks` vector with the same shape the OSC 8 parser
  uses. Auto-grows the vector if the row index is past the current
  end. Rows scrolling into scrollback preserve the span via the
  existing scrollUp move-semantics path.
- **`TerminalWidget::onGridLineCompleted(int screenRow)`** — dispatch
  handler installed as the grid's line-completion callback. Iterates
  rules, filters to the three new grid-mutation types, runs
  `globalMatch()` on the finalized line text, and invokes the
  appropriate `applyRowAttrs` / `addRowHyperlink` for each match (so
  multiple hits on one line all apply).

### Changed

- **Settings → Triggers dropdown** now exposes all action types. The
  hardcoded `{"notify", "sound", "command"}` list was hiding the
  `bell` / `inject` / `run_script` types shipped in 0.6.9 from the
  UI; 0.6.13 adds those plus the three new grid-mutation types so
  every trigger rule supported by the backend is reachable from
  Settings. The trigger-tab description was also expanded to document
  the Action Value format for each type.
- `TerminalWidget::checkTriggers()` now **skips** the three new
  grid-mutation types — those dispatch through `onGridLineCompleted`
  instead. Routing them through both paths would double-fire and
  match on raw PTY byte chunks (which don't map to grid cells)
  instead of finalized line text.
- ROADMAP transition: §0.7.0 → 🔌 Plugins trigger system — the
  "HighlightLine / HighlightText / MakeHyperlink actions from the
  iTerm2 reference are deferred" caveat on the 0.6.9 bullet is now
  resolved; the bullet is marked fully complete.

### Notes

- **Grid-mutation triggers fire on line completion only** — i.e.,
  when `newLine()` is called after a `\n`. The `instant` flag is not
  honored for these three types (it wouldn't make sense — the mid-
  line text isn't final, and half-matched mutations would flash
  then get overwritten). Dispatch types (notify/sound/etc.) still
  honor `instant` via the existing chunk-level matcher.
- **No config schema change.** Same `{pattern, action_type,
  action_value, instant, enabled}` rule shape from 0.6.9; the new
  types just teach both `checkTriggers` and `onGridLineCompleted`
  how to interpret `action_type` and `action_value`.
- **Multi-match support** — `highlight_text` and `make_hyperlink`
  both use `globalMatch()` so every occurrence on a line is
  decorated (not just the first).
- **No plugin API surface change.** `PLUGINS.md` does not need a
  bump; trigger rules live in config, not in the `ants.*` API.

## [0.6.12] — 2026-04-14

**Theme:** close out the **semantic-history** ROADMAP item (which was
already implemented but never promoted on the roadmap) and round out
its editor coverage so the line/column jump works for editors beyond
just VS Code and Kate.

### Added

- **Editor jump support for nvim, vim, sublime, JetBrains IDEs,
  helix, and micro** in `TerminalWidget::openFileAtPath()`. Ctrl-click
  on a `path:line:col` capture in scrollback (compiler / linter /
  stack-trace output) now opens the file at the cited location for:
  - **VS Code family**: `code`, `code-insiders`, `codium`, `vscodium`
    via `--goto <path>:<line>:<col>`
  - **Kate**: `-l <line>` + `-c <col>` (col was previously ignored)
  - **Sublime / Helix / Micro**: `<path>:<line>:<col>` argv shape
  - **Vi family**: `nvim`, `vim`, `vi`, `ex` via `+<line> <path>`
  - **Nano**: `+<line>,<col> <path>`
  - **JetBrains IDEs**: `idea`, `pycharm`, `clion`, `goland`,
    `webstorm`, `rider`, `rubymine`, `phpstorm`, `datagrip` via
    `--line <line>` + `--column <col>`
  - **Anything else**: best-effort (open the path, no jump). Users
    can wrap their editor in a small shell script if it isn't on the
    list yet.
  Editor name match uses `QFileInfo(editor).fileName()` so absolute
  paths (`/usr/local/bin/code`) and bare names (`code`) both work.

### Changed

- ROADMAP transition: §0.7.0 → 🎨 Features → "Semantic history"
  💭→✅. The OSC 7-less implementation was already shipped (uses
  `/proc/<pid>/cwd` to resolve relative paths); this release just
  documents the existing behavior and broadens the editor list. See
  [CHANGELOG.md §0.6.12](CHANGELOG.md#0612--2026-04-14).

### Notes

- **No new config keys, no new permissions, no plugin API change.**
  PLUGINS.md does not need a bump.
- The path-detection regex is unchanged from 0.6.x — the existing
  `(?:^|[\s("'])((\.{0,2}/)?[a-zA-Z0-9_\-./]+\.[a-zA-Z0-9_]+(?::(\d+)(?::(\d+))?)?)`
  pattern in `detectUrls()` already captures `path:line:col` and was
  the basis for the original semantic-history routing.
- Line jump is suppressed when the captured line is 0 (unparseable)
  so editors don't see a bogus `+0` / `--line 0` arg.

## [0.6.11] — 2026-04-14

**Theme:** ship the **0.7.0 security pair** — a UI for auditing and
revoking plugin capabilities, plus image-bomb defenses for the inline
graphics decoders. Both items were the smallest, lowest-risk slice of
the remaining 0.7.0 work; bundling them keeps the security theme
closing out before the riskier threading/platform work begins.

### Added

- **Plugin capability audit UI.** Settings → **Plugins** lists every
  discovered plugin with its name, version, author, description, and
  the full set of permissions declared in its `manifest.json`. Each
  permission renders as a checkbox whose initial state reflects the
  current grant in `config.plugin_grants[<name>]`. Unchecking + Apply
  persists the revocation; **takes effect at the next plugin reload**
  (matches the existing first-load permission prompt's grant
  semantics). When no plugins are loaded — either Lua is not
  compiled in or the plugin directory is empty — the tab shows a
  guidance message pointing at `PLUGINS.md`. Permission descriptions
  match the first-load dialog wording so users see the same
  language across both surfaces. Closes
  [ROADMAP.md §0.7.0 → "Plugin capability audit UI"](ROADMAP.md#070--shell-integration--triggers-target-2026-06).
- **Image-bomb defenses.** New `ImageBudget` class (private to
  `TerminalGrid`) tracks total decoded image bytes across every
  inline-display entry (Sixel + Kitty `T`/`p` + iTerm2 OSC 1337) and
  the Kitty graphics cache. Cap = **256 MB per terminal** — when a
  decode would push the total past the cap, the decoder rejects the
  payload up front (Sixel) or post-decode (Kitty PNG / iTerm2
  OSC 1337 — those don't carry pre-decode dimensions in the params
  block) and surfaces an inline error in the warning color
  (`[ants: <kind> WxH (NN MB) exceeds 256 MB image budget]`). Every
  eviction site (inline-image FIFO drain, Kitty cache eviction, Kitty
  delete-by-id, Kitty delete-all) releases bytes back to the budget.
  Alt-screen swap calls `recomputeImageBudget()` so the counter stays
  accurate when the active and saved containers cross paths. Existing
  per-image dimension cap (`MAX_IMAGE_DIM = 4096`) remains in force —
  ROADMAP specified 16384 as the upper bound but our existing 4096 is
  stricter. Closes
  [ROADMAP.md §0.7.0 → "Image-bomb defenses"](ROADMAP.md#070--shell-integration--triggers-target-2026-06).
- **`SettingsDialog::PluginDisplay`** — lightweight POD struct
  decoupling the Settings dialog from `pluginmanager.h` (and from the
  Lua-gated build chain). MainWindow translates each `PluginInfo` into
  this form before handing the list to the dialog via `setPlugins()`,
  so the Plugins tab compiles and runs even without Lua support.
- **`TerminalGrid::writeInlineError(QString)`** — emits a short red
  ASCII diagnostic line at the cursor (CR+LF after) for surfacing
  image-bomb rejections without the disruption of a desktop
  notification. Color matches the command-block status stripe red.
- **`TerminalGrid::recomputeImageBudget()`** — bulk-recompute helper
  for the alt-screen swap path where individual add/release tracking
  would be brittle. O(N) over the inline + cache vectors.

### Changed

- `SettingsDialog` constructor now wires a "Plugins" tab between
  "Profiles" and the OK/Cancel button row. `applySettings()` collects
  checked permissions per plugin and calls
  `Config::setPluginGrants(name, granted)` for every plugin in the
  audit list — including the "all unchecked" case so revocation
  persists as an empty grant array.
- `MainWindow::settingsAction` lambda now gathers the current plugin
  snapshot from `m_pluginManager->plugins()` (or an empty list when
  `ANTS_LUA_PLUGINS` is undefined) and calls `setPlugins()` on the
  dialog every time it opens — so hot-reloads / new installs are
  reflected without recreating the dialog.
- `TerminalGrid` Sixel / Kitty / iTerm2 decode paths and every
  image-eviction site now consult / update the per-terminal
  `ImageBudget`. RIS (`ESC c`) replaces the entire `TerminalGrid`
  via `*this = TerminalGrid(...)`, which naturally resets the budget
  alongside the containers; no special-case code needed.
- ROADMAP transitions: "Plugin capability audit UI" 📋→✅ and
  "Image-bomb defenses" 📋→✅ in §0.7.0 → 🔒 Security, both linked
  back to this CHANGELOG section.

### Notes

- **Plugin permissions storage on disk is unchanged** — same
  `config.plugin_grants[<name>] = [...]` shape from manifest v2
  (0.6.0). The new UI is additive: it reads what the first-load
  permission prompt already wrote and exposes it for editing.
- **Image budget is conservative.** Same `QImage` stored in both
  `m_inlineImages` and `m_kittyImages` is counted twice even though
  Qt's COW means the underlying bitmap is allocated once. We may
  reject earlier than strictly necessary; we never reject too late.
- **No plugin API surface changed** — `PLUGINS.md` does not need a
  bump. The audit UI sits entirely outside the `ants.*` API.
- No new permissions were introduced; the descriptions for
  `clipboard.write`, `settings`, and `net` mirror the first-load
  dialog text in `mainwindow.cpp`.

## [0.6.10] — 2026-04-14

**Theme:** ship the **command-block UI bundle** from 0.7.0 §Features
(shell integration). OSC 133 prompt regions already carried the data;
this release wires the UI that makes blocks first-class citizens —
per-block context menu, pass/fail status stripe, asciicast export of
a single block. Also formalizes asciinema recording (the recorder
itself has shipped — it's now marked ✅ on the roadmap).

### Added

- **Command-block context menu.** Right-click inside an OSC 133
  prompt region surfaces block-scoped actions without requiring a
  selection: **Copy Command** (extracts just the command text via the
  OSC 133 B cursor column so PS1 is stripped), **Copy Output**
  (extracts the range between the C marker and the next region),
  **Re-run Command** (writes the captured command + `\r` to the PTY —
  bypasses paste confirmation since it's your own history, not
  external input), **Fold / Unfold Output** (toggles the existing
  fold triangle for this specific region rather than the cursor
  region), and **Share Block as .cast…** (exports one block to a
  standalone asciicast v2 file). The menu header shows
  `Command Block (exit N)` when the block has finished so users can
  see pass/fail before acting. Actions are gated on block state —
  Re-run requires `commandEndMs > 0`, Copy Output requires
  `hasOutput`, and so on.
- **Per-block exit-code status stripe.** A 2px vertical bar painted
  on the far-left gutter of every finished prompt line — green for
  `exit_code == 0`, red otherwise. Unlike the fold triangle it shows
  on the most recent finished block too (no successor region
  required). Gives at-a-glance scannability of long scrollback for
  pass/fail.
- **`exportBlockAsCast(int, QString)`** — writes a standalone
  asciicast v2 file with a single command block's output. Header =
  current grid dimensions + `commandStartMs` timestamp. Event stream
  = two records: the command echo at t=0.0, and the captured output
  at t=duration (synthesized; we don't have per-byte timing for
  historical blocks). Players replay as a prompt followed by the
  output dump — honest about what we recorded. Shared via the
  **Share Block as .cast…** context menu action; file extension
  `.cast` per the [asciicast v2 spec](https://docs.asciinema.org/manual/asciicast/v2/).
- **Block-text extraction helpers** on `TerminalWidget` —
  `promptRegionIndexAtLine(int)`, `commandTextAt(int)`,
  `outputTextAt(int)`, `rerunCommandAt(int)`, `toggleFoldAt(int)`.
  Public so plugin code and future UI (marketplace, block sharing
  service) can consume the same semantics.

### Changed

- **`PromptRegion` now carries three new fields** — `exitCode`
  (stored in the OSC 133 D handler alongside `m_lastExitCode`; lets
  block UI paint per-region pass/fail instead of relying on the
  grid-global most-recent value), `commandStartCol` (cursor column
  at OSC 133 B fire — lets `commandTextAt` strip PS1 from the
  extracted command), and `outputStartLine` (global line where OSC
  133 C fired, so output extraction doesn't guess based on
  `endLine + 1`). All additive; existing consumers unaffected.
- **ROADMAP 0.7.0 §Features.** "Command blocks as first-class UI"
  and "Asciinema recording (`.cast` v2)" move from 📋 (planned) to
  ✅ (shipped in 0.6.10). Four bullets consolidated under the
  command-block item — navigation (`Ctrl+Shift+Up`/`Down`) was
  already wired in an earlier release, metadata display (duration
  + timestamp + exit stripe) completes here, per-block menu ships
  here, asciinema recorder was already implemented (now marked
  shipped). The "suppress output noise" sub-bullet from the
  original 0.7.0 design was deferred — it's ill-defined without a
  noise heuristic and not worth blocking the bundle on. Semantic
  history (Cmd-click on path:line:col) and OSC 133 HMAC remain as
  future work.

### Notes

- No plugin API changes in this release. `PLUGINS.md` not bumped.
- No new permission gates. The context menu is client-side UI;
  `rerunCommandAt` uses the same `m_pty->write` path the user's own
  keystrokes use.
- Multi-line wrapped commands are captured correctly — `commandTextAt`
  follows the region from `endLine` through `outputStartLine - 1`
  (or the next region's `startLine - 1` when no output was captured),
  joining lines with `\n`. Trailing-space padding is stripped per
  line.

## [0.6.9] — 2026-04-14

**Theme:** ship the **trigger system bundle** from 0.7.0 §Plugins —
four items that share LuaEngine / PluginManager / OSC-dispatch
plumbing and are cleaner to land together than to drip-feed across
four releases. Sets up shell-aware plugin workflows for 0.7.

### Added

- **Trigger system extensions.** `TriggerRule` JSON schema gains an
  `instant` boolean (iTerm2 convention — `true` fires on every PTY
  chunk for in-progress matches like password prompts; `false`
  defaults to firing only on newline-terminated chunks so settled
  output doesn't double-fire mid-line) and three new `action_type`
  values: `bell` (alias for `sound` — explicit per iTerm2's vocabulary),
  `inject` (writes `action_value` directly to the focused PTY — useful
  for "yes\n" auto-confirm rules), and `run_script` (broadcasts to
  plugins as a `palette_action` event with payload
  `"<action_id>\t<matched_substring>"`). Existing `notify` / `sound` /
  `command` actions unchanged.
- **OSC 1337 SetUserVar channel.** Parses
  `ESC ] 1337 ; SetUserVar=NAME=<base64-value> ST` (iTerm2 / WezTerm
  convention) and fires a `user_var_changed` plugin event with payload
  `"NAME=value"`. Disambiguated from inline images by the byte after
  `1337;` (`S` for SetUserVar, `F` for File). NAME capped at 128 chars,
  decoded value capped at 4 KiB — this is a status channel, not a
  transport. Lets a shell hook pipe state (git branch, k8s context,
  virtualenv) into plugins without prompt-text scraping.
- **`ants.palette.register({title, action, hotkey})`.** Lua API for
  plugins to inject Ctrl+Shift+P palette entries. `title` and `action`
  required; `hotkey` optional (when set, registers a global QShortcut
  that fires the same action). Entries appear as `"<plugin>: <title>"`
  to keep the palette scannable across plugins. Triggering an entry
  fires a `palette_action` event with `action` as payload, scoped to
  the registering plugin only — broadcast events use a different
  payload format (`"<id>\t<match>"` from `run_script` triggers).
- **Five new plugin events:**
  - `command_finished` — fires on OSC 133 D; payload
    `"exit_code=N&duration_ms=N"` (URL-form so plugins can split on
    `&` without escaping).
  - `pane_focused` — fires on tab switch; payload = tab title. Today
    fires on tab changes; will cover within-tab pane focus when split-
    pane focus tracking lands.
  - `theme_changed` — fires after `applyTheme()` completes; payload =
    theme name. Lets plugins swap palette/icon assets to match.
  - `window_config_reloaded` — fires after Settings → Apply; payload
    empty (plugins re-read settings via `ants.settings.get` on demand).
  - `user_var_changed` — fires on OSC 1337 SetUserVar (see above).
  - `palette_action` — fires when a registered palette entry is
    triggered, or when a `run_script` trigger matches.
- **`TerminalGrid` callbacks.** New `setCommandFinishedCallback`,
  `setUserVarCallback`. Wired by `TerminalWidget` to corresponding
  Qt signals (`commandFinished`, `userVarChanged`, `triggerRunScript`)
  which `MainWindow` forwards into `PluginManager::fireEvent`.

### Changed

- **Command palette is now dynamic.** `MainWindow::rebuildCommandPalette()`
  collects menu actions and plugin-registered entries on every change
  (plugin load/reload/unload, individual `register` calls). On
  `pluginsReloaded` signal, all plugin entries are torn down so a
  removed plugin's entries don't survive across reloads — init.lua
  re-runs on reload and re-registers anything that should still appear.
- **PLUGINS.md** — documents the six new events, the `palette` API,
  the `instant` flag, the new trigger action types. The 0.7 roadmap
  section in PLUGINS.md is updated to reflect what's now shipped vs
  what remains.
- **ROADMAP.md** — moves all four trigger-system items from 📋 to ✅
  in 0.7.0 §Plugins. Remaining 0.7.0 work is shell-integration UI
  (command blocks, asciinema), performance (SIMD VT-parser scan,
  decoupled threads), platform (Wayland Quake mode), and security
  (plugin capability audit UI, image-bomb defenses).

### Notes

- The trigger system's `HighlightLine` / `HighlightText` /
  `MakeHyperlink` actions from the original ROADMAP item are deferred
  — they require grid-cell mutation (per-cell color overrides + OSC 8
  hyperlink injection from outside the parser) which is a bigger
  surgery than the dispatch-level actions shipped here. The actions
  shipped (`notify`, `sound`/`bell`, `command`, `inject`, `run_script`,
  `PostNotification` via `notify`) cover the iTerm2 trigger doc's
  most-used cases. Grid-mutation actions are tracked as a follow-up.
- `pane_focused` fires on tab switches today; once split-pane focus
  tracking lands, the same event covers within-tab pane changes —
  plugin handlers don't need to change.

## [0.6.8] — 2026-04-14

**Theme:** close the two remaining Qt-specific audit rules from
0.7.0 §Dev experience. With these in place, the entire 0.7.0
"Project Audit tool" lane is shipped — every motivating case
surfaced by the 0.6.5 audit pass now has automated coverage.

### Added

- **Audit rule `unbounded_callback_payloads`.** Same-line regex flags
  PTY / OSC / IPC byte buffers forwarded straight into a user-supplied
  `*Callback(…)` without a length cap (`.left()` / `.truncate()` /
  `.mid()` / `.chopped()` / `.chop()`). Motivating case: pre-0.6.5
  OSC 9 / OSC 777 notification body shovelled the entire escape payload
  (potentially MB) into the desktop notifier, which then crashed the
  notification daemon and/or amplified a malformed-OSC DoS. Fix shipped
  in 0.6.5; the rule prevents the regression. Severity Major.
  `terminalgrid.cpp` is the canonical safe call site (filtered at
  runtime by `.left(kMaxNotifyBody)`); rule produces zero findings on
  our tree.
- **Audit rule `qnetworkreply_no_abort`.** Detects the dangerous shape
  from the pre-0.6.5 AiDialog incident: a 3-arg `connect()` to a
  QNetworkReply signal whose third argument is a bare lambda. With no
  context object, Qt cannot auto-disconnect when the lambda's captured
  `this` is destroyed — owner closed mid-flight → reply fires later →
  use-after-free. Pattern enforces the 4-arg form (sender, signal,
  context, slot), which Qt protects via auto-disconnect on context
  destruction. Severity Major. Covers `readyRead`, `finished`,
  `errorOccurred`, `sslErrors`. Single-line only — multi-line connect
  formatting is a known false-negative (rare in practice). Ants's own
  AiDialog and AuditDialog use the safe 4-arg shape and are not flagged.
- **Fixtures for both new rules.** `bad.cpp` with three `@expect`
  markers each, `good.cpp` with the corresponding safe shapes that must
  not match the regex. Wired through `audit_self_test.sh` so CI catches
  regressions on every push (now 14 rules, 46 total checks pass).

### Changed

- **ROADMAP** — moved the two remaining 0.7.0 §Dev experience
  audit-rule items (`unbounded_callback_payloads`,
  `qnetworkreply_no_abort`) from 📋 to ✅. The Project Audit tool
  lane in 0.7.0 is now fully shipped; remaining 0.7.0 work is
  features (command blocks, asciinema), the trigger system, and the
  performance / platform / security items.

## [0.6.7] — 2026-04-14

**Theme:** close the remaining grep-shaped items from 0.7.0 §Dev experience
in one sweep, plus fix the CI workflow file which — it turns out — had
never parsed (unquoted colon in a step `name` rejected the entire YAML).

### Added

- **Audit rule `silent_catch`.** Flags empty same-line `catch (...) {}`
  handlers that swallow exceptions without logging or rethrow. Conservative
  first cut (same-line empty body only); extending to multi-line / single-
  statement trivial bodies needs `grep -Pzo` plumbing and is deferred.
- **Audit rule `missing_build_flags`.** Nudges projects toward better
  compile-time coverage by flagging absence of `-Wformat=2`, `-Wshadow`,
  `-Wnull-dereference`, and `-Wconversion` in the top-level `CMakeLists.txt`.
  Strips comment-only lines before matching so a CMakeLists that *discusses*
  a flag in a comment (e.g. why it's disabled) doesn't cause a false negative.
  Severity Minor — this is a nudge, not a bug. Correctly reports one finding
  on our own tree: `-Wconversion`, which is intentionally off (noisy in
  combination with Qt), documented in `CMakeLists.txt`.
- **Audit rule `no_ci`.** Detects projects missing CI configuration
  (`.github/workflows/`, `.gitlab-ci.yml`, `.circleci/`, `.travis.yml`,
  `Jenkinsfile`). Severity Major — regressions ship silently without a CI
  safety net.
- **Sanitizer CI job (`build-asan`).** New parallel GitHub Actions job
  builds with `-DANTS_SANITIZERS=ON` (ASan + UBSan), runs ctest under
  sanitizers, and smoke-tests the binary (`--version`, `--help`) with
  `QT_QPA_PLATFORM=offscreen` so initialization-path bugs surface on every
  push. `detect_leaks=0` for now — Qt global singletons on fast-exit paths
  look like leaks to LSan; re-enable once we have a real unit-test suite
  exercising full app lifecycle.
- **`CONTRIBUTING.md`.** Short actionable guide derived from `STANDARDS.md`:
  build modes, where tests live, step-by-step for adding an audit rule,
  version-bump checklist, commit conventions, what-not-to-send.
- **Fixtures for `silent_catch`** — `bad.cpp` with three `@expect` markers,
  `good.cpp` with logging/rethrow/return-with-value catch bodies that must
  not match.

### Fixed

- **CI workflow was never parsing.** `name: Run cppcheck (audit rule:
  Qt-aware static analysis)` had an unquoted `:` inside the scalar, which
  GitHub Actions' YAML parser rejected as "mapping values are not allowed
  here" — the workflow file was dropped entirely and no jobs ran for 0.6.5
  or 0.6.6. Fixed by double-quoting the step name. The parse error was
  silent on GitHub's side (workflow-file runs showed 0 jobs with a
  one-line "workflow file issue" notice); caught here by running PyYAML's
  strict parser over the file locally.

### Changed

- **ROADMAP** — moved four items from 0.7.0 §Dev experience to 0.6.7
  shipped (silent-catch rule, build-flag recommender, no-CI check,
  sanitizer-in-ctest hookup, CONTRIBUTING.md).

## [0.6.6] — 2026-04-14

**Theme:** close the first Dev-experience item queued in 0.7 after the 0.6.5
audit. One new audit rule + matching CI enforcement, backfill the two
fixtures it immediately surfaced.

### Added

- **Audit self-check: fixture-per-`addGrepCheck`.** New `audit_fixture_coverage`
  rule in the Project Audit dialog enumerates every `addGrepCheck("id", …)`
  call in `src/auditdialog.cpp`, dedups by id, and reports any id without a
  matching `tests/audit_fixtures/<id>/` directory. Catches the exact gap we
  hit in 0.6.5 — four rules shipped a cycle without regression fixtures.
  Silent no-op on projects that don't follow this convention (grep yields no
  ids). Output uses the parseFindings-friendly `file:line: message` shape so
  findings link directly to the registration site.
- **CI-enforced fixture-coverage cross-check** in `tests/audit_self_test.sh`.
  Belt-and-suspenders: the runtime check surfaces findings to the dev on next
  audit run; the test-harness assertion blocks merges in CI when a new rule
  id lacks either a fixture dir OR a `run_rule` line in the script. Mirrors
  the runtime check exactly. Added to ctest output as
  `PASS/FAIL: fixture-coverage <id>`.
- **Fixture coverage for `memory_patterns` and `qt_openurl_unchecked`** —
  the two real gaps the new rule just surfaced. Each gets `bad.cpp` with
  `@expect` markers and a `good.cpp` that avoids matching tokens (including
  in comments — the pattern is case-sensitive and line-based, so comment
  wording had to use UPPER-CASE or non-literal phrasing).

### Changed

- **ROADMAP** — moved the "Self-consistency: fixture-per-`addGrepCheck`" item
  from 0.7.0 §Dev experience to 0.6.6 shipped.

## [0.6.5] — 2026-04-14

**Theme:** second audit of the day. Ran the 5-phase prompt
(`/mnt/Storage/Scripts/audit_prompt.md`) against the post-0.6.4 tree. Five
subagent reports converged on three real High findings, three Medium, and a
batch of noise. Five subagent claims were rejected on Phase 4 verification
(documented in the commit body) — keeping the signal honest matters more than
the count.

### Security

- **Cap OSC 9 / OSC 777 desktop-notification payloads at 1024 bytes (title
  at 256).** The VT parser caps an OSC payload at 10 MB so inline-image
  escapes round-trip cleanly; `handleOsc()` was forwarding that whole
  payload to the notification callback as a QString. A program inside the
  terminal could spam the freedesktop notification daemon with multi-MB
  titles. `src/terminalgrid.cpp:761-790` now `.left(N)`-clamps before the
  callback, matching the OSC 52 clipboard-cap pattern.
- **Cap accumulated SSE response buffer in AiDialog at 10 MB.**
  `m_sseLineBuffer` already had this cap on the pipe side (`src/aidialog.cpp:179`);
  `m_streamBuffer` — which holds *parsed* content — did not. A misbehaving
  or hostile endpoint could stream past `max_tokens` indefinitely. Once
  capped, we append a single `[response truncated]` marker and drop
  subsequent chunks.
- **AiDialog now has a destructor that aborts any in-flight reply.** Qt
  auto-disconnects signals from a destroyed receiver, but there's a narrow
  window during member destruction where a reply can still fire
  `readyRead` / `finished` on a partially-destructed AiDialog. Explicit
  `abort() + deleteLater()` closes the window.

### Changed

- **Stricter compile flags.** Added `-Wformat=2`, `-Wshadow=local`, and
  `-Wnull-dereference` to the default warning set. `-Wshadow=local` was
  picked over the full `-Wshadow` deliberately: the project uses
  Qt-idiomatic `void setData(QByteArray data)` patterns where parameter
  names legitimately match member accessors; flagging those would drown
  the real `int x = …; if (cond) { int x = …; }` shadowing that the flag
  exists to catch. Surfaced two legitimate local-vs-local shadows
  (`mainwindow.cpp:1170` and `auditdialog.cpp:3062`), both renamed.
- **Silent `catch (…)` blocks in OSC handlers now route to the existing
  `DBGLOG` channel.** Three sites in `src/terminalgrid.cpp` (OSC 133 D,
  OSC 9;4, OSC 1337 param parse) write a diagnostic line when their
  `std::stoi` throws, guarded by `m_debugLog` so there's zero cost when
  debug isn't enabled. The two Kitty-graphics `safeStoi`/`safeStoul`
  helpers stay intentionally silent — documented with a comment — because
  they fire per-chunk in the APC parameter loop and logging would flood.

### Added

- **`.github/workflows/ci.yml`.** Runs `ctest` plus the `cppcheck` Qt-aware
  pass on every push to `main` and every PR. `actions/checkout` pinned by
  40-char SHA (not tag) per STANDARDS.md §Supply chain. Workflow token
  scoped to `contents: read`.
- **Four new audit-rule regression fixtures** under
  `tests/audit_fixtures/`: `todo_scan`, `format_string`, `hardcoded_ips`,
  `weak_crypto`. These four rules ship in `auditdialog.cpp` but had no
  `bad/good` fixture pair, so regex drift would have shipped silently.
  `tests/audit_self_test.sh` now covers 9 of the 9 unconditional
  `addGrepCheck()` rules; `ctest` exercises 27 assertions (up from 23).
- **`launch.sh` now starts with `set -eu`.** Three-line wrapper, but free
  hardening against silent failures if a typo lands in a future edit.
- **`-Wl` cert-err33-c fix.** `src/ptyhandler.cpp:112`'s argv0
  `::snprintf` return value was being ignored; now `(void)`-cast with a
  comment explaining the truncation is a known-acceptable loss (the
  written buffer is a login-marker, not a lookup key).

## [0.6.4] — 2026-04-14

**Theme:** Project Audit signal-to-noise. Addresses a reviewer-provided brief
after a 2026-04-14 audit run surfaced 26 BLOCKER findings that were all false
positives from ASCII section underlines in a vendored single-header library,
and 30 MAJOR world-writable-file findings that were a FUSE-mount artefact.

### Fixed

- **Selection auto-scroll direction was inverted.** Dragging a selection
  past the bottom edge of the viewport scrolled *up* into older scrollback
  instead of revealing newer lines below (and symmetrically at the top
  edge). The auto-scroll timer in `TerminalWidget` applied
  `m_scrollOffset - delta` with a signed direction, which inverted the
  intent; flipped to `+ delta` so the direction convention now matches
  `wheelEvent` / `smoothScrollStep` — `+offset` reveals older content,
  `-offset` reveals newer.

- **Conflict-marker regex no longer matches ASCII underlines.** The
  `conflict_markers` rule required exactly 7 sigil chars at start-of-line
  but had no trailing anchor, so `===========` (and longer banners in
  vendored headers) still matched. Tightened to
  `^(<{7}|\|{7}|={7}|>{7})(\s|$)`. Also added `|{7}` for the diff3-style
  merge-base marker that the old pattern missed entirely. A
  `conflict_markers` self-test fixture now carries eight ASCII-underline
  canaries in `good.cpp` so regressions are caught by the existing CTest
  driver.

- **World-writable check on non-POSIX mounts.** On filesystems that don't
  enforce POSIX permission bits (FUSE, NTFS, vfat/exfat, CIFS, 9p) every
  file reports as world-writable because the mount maps all Unix perms
  to 0777. `AuditDialog` now probes `stat -f -c %T <project>` at
  construction; when the result is in a known non-POSIX list the
  `file_perms` check is replaced with an INFO note explaining the skip,
  instead of producing ~every file in the tree as a finding.

### Changed

- **Default exclude list broadened.** `kFindExcl` and `kGrepExcl` now
  exclude `external/` and `third_party/` in addition to the existing
  `vendor/`, `build/`, `node_modules/`, `.cache/`, `dist/`, `.git/`,
  `.audit_cache/`. Vendored code is not project-maintained, so findings
  there aren't actionable and used to drown out real signal.

- **Category headers surface multi-tool corroboration.** The HTML and
  plain-text renders now append `(N corroborated)` to each category
  header when one or more of its findings are flagged by ≥2 distinct
  tools on the same file:line — the same signal that drives the ★ badge
  and boosts the confidence score. Lets the downstream consumer
  prioritise triage at a glance instead of having to scan every finding's
  inline tags.

- **Confidence legend in the plain-text header.** The `conf N` and ★
  tags were opaque without documentation. A four-line legend now sits
  in the handoff header explaining what the score means and how
  corroboration feeds into it.

### Added

- **5-phase workflow scaffold in the Claude Review handoff.** The
  `/tmp/ants-audit-*.txt` export used by the "Review with Claude"
  button now carries a BASELINE → VERIFY → CITATIONS → APPROVAL →
  IMPLEMENT+TEST prompt between the project-docs block and the
  findings list. Stops the downstream session from plunging straight
  into fixes without verifying findings are real, cross-checking CVE
  version ranges, or committing a regression test per fix. Aligns with
  the project's existing no-workarounds rule (documented in
  `CLAUDE.md`).

## [0.6.3] — 2026-04-14

**Theme:** Claude Code status responsiveness. Replaces the 2-second transcript
poll with an inotify-driven event pipeline; state transitions now surface in
~50ms instead of up to 2s, with lower steady-state CPU.

### Changed

- **Transcript state updates are event-driven.** `ClaudeIntegration` already
  had a `QFileSystemWatcher` on the active transcript but bypassed it with
  an unconditional per-poll parse ("QFileSystemWatcher can miss rapid
  changes on Linux"). That was over-defensive — QFSW's only real miss
  modes are atomic rename-over (already handled by the re-add in
  `parseTranscriptForState`) and full file replacement (now handled by a
  slow backstop). The watcher's `fileChanged` signal now drives a 50ms
  single-shot debounce timer that coalesces streaming-output bursts
  (during an assistant turn Claude writes dozens of JSONL lines per
  second) into at most ~20 parses/sec. Net effect: "Claude:
  thinking…/compacting…/idle" transitions flip in ~50ms instead of up to
  2000ms, and the CPU no longer re-parses 32KB of JSON every 2 seconds
  when nothing has changed.

- **Process-presence poll kept at 2s, with a 20s transcript backstop.**
  Polling `/proc` for claude-code starting/stopping under our shell has
  no clean event equivalent (we're not the direct parent, and netlink
  proc-connector needs elevated caps), but it's cheap — ~40 bytes read
  per cycle. The poll now also re-parses the transcript once every 10
  cycles (~20s) as a defensive net for the rare file-replaced case where
  QFSW loses its watch; the re-parse also re-arms the watch via the
  existing `addPath`-if-missing check.

## [0.6.2] — 2026-04-14

**Theme:** Claude Code status readability. Adds a dedicated "compacting" state
so the status bar doesn't flatten a multi-second `/compact` into generic
"thinking…".

### Added

- **"Claude: compacting…" status.** New `ClaudeState::Compacting` surfaces
  when a `/compact` is in flight, rendered in magenta so it's visually
  distinct from idle (green), thinking (blue), and tool use (yellow).
  Detection is transcript-driven: the parser walks the existing 32KB tail
  window looking for a `user` event whose string content contains
  `<command-name>/compact</command-name>`, and only fires while no
  subsequent `isCompactSummary:true` user event (the condensed-history
  marker Claude Code writes when compaction finishes) has appeared. The
  `PreCompact` hook path now routes through the same state, so terminals
  that have the hook server wired up get the same indicator without the
  2-second poll latency.

## [0.6.1] — 2026-04-14

**Theme:** scroll-anchor correctness. One-liner fix for a long-standing drift
bug that only surfaced with a full scrollback buffer.

### Fixed

- **Scroll anchor drifts when scrollback is at its cap.** While scrolled up
  reading history, bursts of new output (e.g. Claude Code redrawing its
  prompt / spinner) appeared to "redraw text at the current history
  position" — actually the pinned viewport was sliding by one content-line
  per pushed line. Root cause: the anchor math in `onPtyData` diffed
  `scrollbackSize()` before/after parsing, but once the buffer hits
  `scrollback_lines` (default 50k) each push also pops a stale line from
  the front, so the size delta is always zero and the anchor thinks
  nothing happened. `TerminalGrid` now exposes a monotonic
  `scrollbackPushed()` counter that keeps incrementing through the cap;
  `TerminalWidget` diffs that instead. Below-cap behavior is unchanged.

## [0.6.0] — 2026-04-14

**Theme:** make the two features that already make Ants distinctive
(plugins + audit) production-grade. Ships the full context polish + plugin v2
arc from the 0.6 roadmap — scrollback regex search, OSC 9;4 progress,
multi-line paste confirmation, OSC 8 homograph warning, LRU glyph-atlas
eviction, cell-level dirty tracking, per-plugin Lua VMs, manifest v2 with
declarative permissions + capability-gated APIs, plugin keybindings, and
hot reload.

### Added

#### 🎨 Features

- **Scrollback regex search**. Ctrl+Shift+F search bar gains three toggle
  buttons: `.*` (regex mode via `QRegularExpression`), `Aa` (match case),
  and `Ab` (whole word, wraps the pattern in `\b…\b`). Alt+R / Alt+C /
  Alt+W flip toggles without leaving the input. Invalid regex patterns
  render the input with a red border and show the error in the match-
  counter tooltip (`!/!`). Zero-width matches (`\b`, `^`) are detected
  and skipped to avoid infinite loops. Matches Kitty / iTerm2 / WezTerm
  behavior; closes the Ghostty 1.3 gap.
- **OSC 9;4 progress reporting**. Parses ConEmu / Microsoft Terminal
  `ESC]9;4;state;percent ST` sequences. State 0 clears, 1 shows a normal
  blue progress bar, 2 shows a red error bar, 3 shows an indeterminate
  (full-width) lavender bar, 4 shows a yellow warning bar. Renders as a
  3-pixel strip along the bottom edge of the terminal (above the
  scrollbar) and as a small colored dot next to the tab title. Coexists
  with the existing OSC 9 desktop-notification handler via payload
  disambiguation (`9;4;…` vs. `9;<body>`). Spec:
  [MS Terminal progress sequences](https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences).
- **Click-to-move-cursor on shell prompts** (previously in code, now
  documented). When the cursor is in an OSC 133 prompt region with no
  output yet, clicking on the same line sends the appropriate run of
  `ESC [ C` / `ESC [ D` arrow sequences to reposition the cursor. Capped
  at 200 arrows to guard against runaway sends.

#### ⚡ Performance

- **LRU glyph-atlas eviction**. Replaces the old "clear everything on
  overflow" behavior with a warm-half retention policy: entries touched
  within the last 300 frames are kept, colder ones are re-rasterized
  into a fresh atlas. Median-based fallback handles cold-boot overflow.
  Eliminates the paint stall on long ligature-heavy sessions. See
  `glrenderer.cpp:compactAtlas`.
- **Per-line dirty tracking**. `TermLine` gains a `dirty` flag set by
  every grid mutation (print, erase, scroll, alt-screen swap, resize).
  `TerminalWidget::invalidateSpanCaches()` now does targeted eviction —
  only URL / highlight caches for dirty lines get dropped, not the whole
  map. Big win on high-throughput output that only touches a few lines.
  (Full cell-level render-path partial-update is deferred; the dirty
  primitive is in place for that work.)

#### 🔒 Security

- **Multi-line paste confirmation**. Dialog appears when the pasted
  payload contains a newline, the string `sudo `, a curl/wget/fetch
  piped into `sh`/`bash`/`python`/etc., or any non-TAB/LF/CR control
  character. Policy is independent of bracketed paste — iTerm2 /
  Microsoft Terminal got bitten by conflating the two
  ([MS Terminal #13014](https://github.com/microsoft/terminal/issues/13014)).
  Config key: `confirm_multiline_paste` (default `true`). UI:
  Settings → Behavior → "Confirm multi-line / dangerous pastes".
- **Per-terminal OSC 52 write quota**. 32 writes/min + 1 MB/min per
  grid, independent of the existing 1 MB per-string cap. Protects
  against drip-feed exfiltration.
- **OSC 8 homograph warning**. When an OSC 8 hyperlink's visible label
  encodes a hostname-looking token (`github.com`) that doesn't match
  the actual URL host (`github.evil.example.com`), a confirm dialog
  shows both and requires explicit opt-in. Strips leading `www.` for
  fair comparison.

#### 🔌 Plugins — manifest v2 + capability model

- **Per-plugin Lua VMs**. Each plugin gets its own `lua_State`, heap
  budget (10 MB), and instruction hook (10 M ops). One VM stalling or
  leaking globals can no longer affect others. `PluginManager` owns
  a map of `(name → LuaEngine *)` and fans out events to each in turn.
- **Declarative permissions in `manifest.json`**. Plugins declare a
  `"permissions": ["clipboard.write", "settings"]` array. On first
  load, a permission dialog lists each requested capability and lets
  the user accept / un-check individual grants. Grants persist in
  `config.json` under `plugin_grants`; subsequent loads don't re-prompt
  unless the manifest requests a new permission. Un-granted permissions
  surface as *missing API functions* (not `nil` stubs) so plugins can
  feature-detect with `if ants.clipboard then … end`.
- **`ants.clipboard.write(text)`** — gated by `"clipboard.write"`
  permission. Writes to the system clipboard via QApplication.
- **`ants.settings.get(key)` / `ants.settings.set(key, value)`** — gated
  by `"settings"` permission. Per-plugin key/value store, persisted
  under `plugin_settings.<plugin_name>.<key>` in `config.json`. Manifest
  can declare a `"settings_schema"` JSON-Schema subset (plumbing in
  place; schema-driven Settings UI is a follow-up item).
- **Plugin keybinding registration** via manifest `"keybindings"` block:
  `{"my-action": "Ctrl+Shift+K"}`. Firing the shortcut dispatches a
  `keybinding` event to the owning plugin with the action id as the
  payload. `ants.on("keybinding", function(id) ... end)` listens. The
  shortcut lives on the MainWindow; invalid sequences log a warning.
- **Plugin hot reload** via `QFileSystemWatcher`. Watches `init.lua`,
  `manifest.json`, and the plugin directory itself; on change, debounced
  150 ms, the plugin VM is torn down (fires `unload` event) and
  re-initialized (fires `load` event). Enabled only when
  `ANTS_PLUGIN_DEV=1` — idle cost is zero otherwise.
- **`ants._version`** exposes `ANTS_VERSION` string so plugins can
  feature-detect without hardcoding.
- **`ants._plugin_name`** exposes the plugin's declared manifest name.
- **New events**: `load`, `unload` (lifecycle), `keybinding` (manifest
  shortcuts). Added to `eventFromString()` in `luaengine.cpp`.

#### 🧰 Dev experience

- **`ants-terminal --new-plugin <name>` CLI**. Creates a plugin skeleton
  at `~/.config/ants-terminal/plugins/<name>/` with `init.lua`,
  `manifest.json` (empty `permissions` array), and `README.md` templates.
  Validates the name against `[A-Za-z][A-Za-z0-9_-]{0,63}`.
- **`ANTS_PLUGIN_DEV=1`** env var enables verbose plugin logging on
  scan/load, plus hot reload. Cached per-process via a static.
- **`--help` / `--version`** flags wired through `QCommandLineParser`
  (previously `--version` set via `setApplicationVersion` but no CLI
  handler existed).

### Changed

- `README.md` — navbar now links to `PLUGINS.md`, `ROADMAP.md`,
  and `CHANGELOG.md`. Plugins section points to PLUGINS.md as the
  source of truth; keeps a quick-start summary table inline.
- `CLAUDE.md`, `STANDARDS.md`, `RULES.md` — cross-linked to the new
  docs; plugin-system sections defer to PLUGINS.md for the author
  contract and document internal invariants separately.
- `PluginManager` no longer exposes a single `engine()` accessor;
  callers use `engineFor(name)` to get the per-plugin VM. Event
  delivery fans out over all loaded engines.
- Paste path now runs through `TerminalWidget::confirmDangerousPaste`
  before the bracketed-paste wrap — confirmation policy is
  independent of PTY-side behavior.

## [0.5.0] — 2026-04-13

### Added

**Project Audit — context-aware upgrade** (this release's headline feature).
The audit pipeline now collects and applies context at every stage, closing
the gap with commercial tools (CodeQL, Semgrep, SonarQube, DeepSource).

- **Inline suppression directives.** Findings can be silenced directly in
  source via `// ants-audit: disable[=rule-id]` (same line),
  `disable-next-line`, and `disable-file` (first 20 lines). Foreign markers
  are also honored for cross-tool compatibility: clang-tidy `NOLINT` /
  `NOLINTNEXTLINE`, cppcheck `cppcheck-suppress`, flake8 `noqa`, bandit
  `nosec`, semgrep `nosemgrep`, gitleaks `#gitleaks:allow`, eslint
  `eslint-disable-*`, pylint `pylint: disable`. Rule-list parsing and
  glob matching (`google-*`) are supported.
- **Generated-file auto-skip.** Findings in `moc_*`, `ui_*`, `qrc_*`,
  `*.pb.cc`/`*.pb.h`, `*.grpc.pb.*`, flex/bison outputs, `/generated/`
  paths, and `*_generated.*` files are dropped before rendering.
- **Path-based rules** in `audit_rules.json`. A new `path_rules[]` block
  takes entries of `{glob, skip, severity_shift, skip_rules[]}` to
  configure per-directory severity shifts and skips (e.g. `tests/**`
  shifts everything down one severity band; `third_party/**` skips
  entirely). Glob syntax supports `**`, `*`, `?`.
- **Code snippets** (±3 lines) attached to every `file:line` finding,
  cached per-file, rendered with the finding line highlighted. Exposed
  via SARIF `physicalLocation.contextRegion`, embedded in the HTML
  report's expandable `[details]` panel, and included in the Claude
  review handoff.
- **Git blame enrichment.** Author, author date, and short SHA shown
  next to every finding. Cached per `(file, line)` with a 2 s timeout;
  auto-disabled for non-git projects. Exported via SARIF
  `properties.blame` bag (sarif-tools de-facto convention).
- **Confidence score 0-100.** Replaces the binary `highConfidence` flag
  with a weighted sum: severity × 15 + 20 cross-tool agreement + 10
  external AST tool − 20 test-path, clamped, adjusted by explicit AI
  verdicts. Displayed as a coloured pip in the UI; exported in SARIF
  (`properties.confidence`) and HTML.
- **Severity pill filter + live text filter** in the dialog, matching the
  HTML export's UX. Filters across file name, message, rule id, and
  author (via blame). Re-renders instantly.
- **Sort by confidence** toggle reorders every check's findings by
  confidence descending — the "which should I look at first" view.
- **Collapsible `[details]`** per finding. Click to reveal snippet
  context and an inline "🧠 Triage with AI" action. Expanded state
  persists across re-renders.
- **Semgrep** integration (optional). When `semgrep` is available, a
  Security-category check runs `p/security-audit` plus language packs
  (`p/c`, `p/cpp`, `p/python`, `p/javascript`, `p/typescript`). Picks
  up project-local `.semgrep.yml` automatically. Complements clazy's
  Qt-aware AST lane.
- **AI triage per finding.** Click "🧠 Triage with AI" inside an
  expanded finding to POST rule + message + snippet + blame to the
  project's configured OpenAI-compatible endpoint. Response parsed
  as `{verdict, confidence, reasoning}`; verdict badge rendered
  inline, confidence score adjusted accordingly. Opt-in per click,
  respects `Config::aiEnabled`.
- **Version stamp on all audit outputs.** Dialog title, dialog types
  label, SARIF `driver.version`, HTML report meta line, and plain-text
  Claude handoff now all carry `ants-audit v<PROJECT_VERSION>` pulled
  from a single CMake `add_compile_definitions(ANTS_VERSION=…)` source
  of truth.
- **Single-source-of-truth versioning infrastructure.**
  `CMakeLists.txt` `project(... VERSION 0.5.0)` propagates via
  `add_compile_definitions(ANTS_VERSION=…)` to every caller. Four
  previously-hardcoded `"0.4.0"` literals retired: `main.cpp`
  (`setApplicationVersion`), `auditdialog.cpp` (SARIF driver),
  `ptyhandler.cpp` (`TERM_PROGRAM_VERSION` env), and
  `claudeintegration.cpp` (MCP `serverInfo.version`).

### Changed

- **`audit_rules.json` schema** extended with `path_rules[]`. Existing
  `rules[]` entries load unchanged; loader documents both blocks.
- **SARIF export** now emits `physicalLocation.contextRegion` with the
  full ±3 snippet for every finding that has a `file:line` location,
  plus `properties.blame` and `properties.confidence`. `properties`
  also carries `aiTriage` when the user triaged that finding.
- **HTML report** template upgraded with confidence pips, blame tags,
  verdict badges, and per-finding expandable snippet panels. Added
  CSS classes (`.conf-pip`, `.blame`, `.verdict`, `.snippet`, `.hit`).
  `DATA.version` now propagated through the inline payload.
- **Plain-text Claude handoff** body lines carry `[conf N · blame · AI]`
  tags and a 3-line indented snippet per finding. Header gains a
  `Generator:` line.
- **`dropFindingsInCommentsOrStrings`** still opt-in per check via the
  existing `kSourceScannedChecks` set, but is now augmented by the
  inline-suppression scan (applied to *every* finding with a
  `file:line`). Pipeline order documented in `STANDARDS.md`.

### Security

- Inline suppression scan reads files through a bounded per-file cache
  (4 MB cap) — runaway check on a huge file cannot stall the dialog.
- Git blame shells out with a 2 s timeout and is auto-disabled when
  not in a git repository.
- AI triage respects the project's existing `ai_enabled` config and
  refuses non-http/https endpoints.

### Tests / Tooling

- `tests/audit_self_test.sh` extended with 18 suppression-token
  assertions covering every honored marker (ants-native,
  NOLINT/NOLINTNEXTLINE, cppcheck-suppress, noqa, nosec, nosemgrep,
  gitleaks-allow, eslint-disable-*, pylint: disable) plus negative
  cases. Total: 23 rule tests passing.
- `ANTS_VERSION` compile definition wired via CMake
  `add_compile_definitions`. `main.cpp` and `auditdialog.cpp` no
  longer carry hardcoded version strings.

### Docs

- `README.md` — expanded "Project Audit Dialog" section with examples
  of inline suppression directives and `audit_rules.json` schema
  including `path_rules`; semgrep added to optional dependencies.
- `CLAUDE.md` — pipeline order, confidence formula, generated-file
  patterns, new design decisions around context-awareness.
- `STANDARDS.md` — context-awareness pipeline order invariants, blame
  cache rules, AI triage config sourcing.
- `RULES.md` — three new audit rules covering inline-suppression
  preference, `path_rules` vs. hardcoded paths, and "confidence is
  display-only, not a gate".

## [0.4.0] — 2026-04-13

### Added

**Project Audit dialog — full feature arc.** Over the course of eight
incremental commits, the audit panel evolved from a regex-heavy scanner
into a structured, AST-aware, cross-validated tool.

- **SonarQube-style taxonomy** — every finding carries a type
  (Info / CodeSmell / Bug / Hotspot / Vulnerability) and a 5-level
  severity (Info / Minor / Major / Critical / Blocker).
- **Per-finding dedup keys** (SHA-256 of `file:line:checkId:title`).
- **Baseline diff** — save current findings to
  `.audit_cache/baseline.json`; later runs highlight only new findings.
- **Trend tracking** — severity counts persisted at
  `.audit_cache/trend.json` (last 50 runs) and rendered as a delta
  banner against the previous run.
- **Recent-changes scope** — restrict findings to files touched in
  the last N commits, or (stricter) to exact diff hunk line ranges
  via `git diff --unified=0 HEAD~N`.
- **Multi-tool correlation** — ★ badge on findings flagged at the
  same `file:line` by ≥2 distinct tools.
- **clazy integration** — Qt-aware AST checks (`connect-3arg-lambda`,
  `container-inside-loop`, `old-style-connect`, `qt-keywords`, etc.)
  via `clazy-standalone`, reading the project's
  `compile_commands.json`. Retires three FP-prone regex checks.
- **Comment/string-aware filtering** — per-check opt-in state-machine
  scan that drops matches inside `//`, `/* */`, or `"strings"`.
- **SARIF v2.1.0 export** — OASIS-standard JSON consumed by GitHub
  Code Scanning, VSCode SARIF Viewer, SonarQube, CodeQL. Includes
  per-rule catalogue, `partialFingerprints`, and per-finding
  properties bag.
- **Single-file HTML report** — no external assets, embedded JSON
  payload + vanilla JS. Severity pills, text filter, collapsible
  check cards.
- **Interactive suppression** — click any dedup hash to prompt for
  a reason and append to `.audit_suppress` (JSONL v2: `{key, rule,
  reason, timestamp}` per line).
- **User-defined rules** via `<project>/audit_rules.json`. Flat
  schema mirrors the internal `AuditCheck` struct. User rules run
  through the full filter / parse / dedup / suppress pipeline.
- **CTest harness** — `tests/audit_self_test.sh` with per-rule
  `bad.*`/`good.*` fixtures using `// @expect <rule-id>` markers;
  count-based assertion.
- **Review with Claude** handoff — emits a plain-text report with
  `CLAUDE.md`, `STANDARDS.md`, `RULES.md`, and `CONTRIBUTING.md`
  prepended so Claude can weigh findings against documented rules.

### Changed

- `.audit_suppress` upgraded from v1 plain-key-per-line to v2 JSONL;
  v1 still loads, first write converts in place with a migration
  marker.
- Audit rule pack format: JSON (`audit_rules.json`), not YAML —
  `QJsonDocument` is built-in; flat schema's readability gap with
  YAML is small enough that a parser dependency isn't worth it.

### Security

- `audit_rules.json` is a trust boundary — its `command` field runs
  through `/bin/bash` unconditionally. Documented analogous to
  `.git/hooks`: your repo, your commands, no sandbox.

## [0.3.0]

### Added

- Claude Code deep integration: live status bar, project/session
  browser (Ctrl+Shift+J), permission allowlist editor (Ctrl+Shift+L),
  transcript viewer, slash-command shortcuts.
- 12 professional UX features: hot-reload config, column selection,
  sticky headers, tab renaming/coloring, snippets, auto-profile
  switching, badge watermarks, dark/light auto-switching, Nerd Font
  fallback, scrollbar, Ctrl+arrow word movement, scroll anchoring.
- Session persistence: save/restore scrollback via `QDataStream` +
  `qCompress` binary serialization.
- AI assistant dialog: OpenAI-compatible chat completions with SSE
  streaming, configurable endpoint/model.
- SSH manager: bookmark editor, PTY-based `ssh` connection.
- Lua 5.4 plugin system: sandboxed `ants` API, instruction-count
  timeout, event-driven handlers.
- GPU rendering path: QOpenGLWidget with glyph atlas, GLSL 3.3
  shaders.

### Fixed

- Six rounds of comprehensive security + correctness audits covering
  PTY FD leaks, bracketed-paste injection, SIGPIPE, Lua coroutine
  escape, hardcoded colours, atomic config writes, and more.

## [0.2.0]

### Added

- Full C++ terminal emulator rewrite: custom VT100/xterm state-machine
  parser, `TerminalGrid` with scrollback + alt screen, PTY via
  `forkpty`, QPainter rendering with `QTextLayout` ligature shaping.
- Mouse reporting (SGR format), focus reporting (mode 1004),
  synchronized output (mode 2026).
- OSC 8 hyperlinks, OSC 52 clipboard write, OSC 133 shell-integration
  markers, OSC 9 / 777 desktop notifications.
- Sixel graphics (DCS), Kitty graphics protocol (APC), iTerm2 images
  (OSC 1337).
- Kitty keyboard protocol with progressive enhancement (push / pop /
  query / set).
- Combining characters via per-line side table.
- Command palette (Ctrl+Shift+P), tab management with custom frameless
  titlebar, split panes via nested `QSplitter`s, 11 built-in themes.

## [0.1.0]

### Added

- Initial release: basic terminal emulator prototype.
