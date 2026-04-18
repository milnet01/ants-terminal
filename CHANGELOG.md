# Changelog

All notable changes to Ants Terminal are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Sections use the standard categories — **Added** for new features, **Changed**
for changes in existing behavior, **Deprecated** for soon-to-be-removed features,
**Removed** for now-removed features, **Fixed** for bug fixes, and **Security**
for security-relevant changes.

## [Unreleased]

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
