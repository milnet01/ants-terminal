# ANTS-2049 — End-to-end harness to drive Ants Terminal as a user and observe the result

**Status:** accepted — cold-eyes loops 1–6 folded clean, user sign-off 2026-06-10.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-2049 (user-request-2026-06-10).
**Blocker for:** ANTS-2050 (feature-covering test-case suite runs on this harness).
**Pairs with:** ANTS-2051 part 2 (id-less flip targeting — deferred "pending the ANTS-2049/2050 behavioral harness").

## 1. Problem

The project's automated tests (ctest: unit, feature-conformance, audit-fixture)
exercise code paths in isolation. There is no way for the agent to *use the real
application the way a person would* — launch it, type into the terminal, click a
button, open the roadmap/audit/settings dialogs, resize the window, run Claude
Code in a tab — and then check that it looks and behaves right. Whole classes of
behaviour are therefore unverifiable except by asking the user to relaunch and
eyeball: dialog geometry (`docs/standards/dialogs.md` D1–D4), status-bar widgets,
theme/opacity rendering, alt-screen apps, paste, session persistence.

The building blocks already exist but are not composed into a usable loop:

1. **Observation** verbs are built and **socket-dispatchable** (hyphenated keys
   in the `dispatch()` `if (cmd == ...)`-chain in `src/remotecontrol.cpp`):
   `get-text`
   (`cmdGetText`), `tab-list` (`cmdTabList`), `ls` (`cmdLs`). Note
   `current_state` / `last_selection` / `get_scrollback` are **MCP-only** — they
   have no socket branch and answer only on the in-process MCP server (the
   user's instance), so the harness cannot use them against the test socket.
2. **Terminal input** is built — `cmdSendText` (declared in `remotecontrol.h`,
   socket key `send-text`) writes bytes to the PTY master (types into the
   shell); `cmdLaunch` (`remotecontrol.h`, key `launch`) launches a program
   in a tab; `cmdNewTab` (`remotecontrol.h`, key `new-tab`) opens a tab.
3. **Multi-instance** is supported — `RemoteControl::defaultSocketPath()`
   (`src/remotecontrol.cpp`) honours `ANTS_REMOTE_SOCKET` unconditionally
   ("lets the user script multi-instance setups without touching the
   source").

What is missing, and why a harness cannot be assembled today:

1. **No GUI-level input injection.** `send-text` drives the *terminal* (PTY),
   not the *GUI*. There is no verb to post a synthetic mouse click, a GUI key
   event (a dialog's Esc/Enter, a `Ctrl+Shift+T` shortcut), a window resize, or
   to capture the rendered window to an image. Dialogs, buttons, menus, and
   chrome are undrivable.
2. **No way to pass structured arguments to an arbitrary verb over the CLI.**
   The `--remote` client hand-codes *argument shaping* for a few verbs —
   `send-text` (`main.cpp:307`), `new-tab`/`launch` (`:342`), `set-title`
   (`:354`), `get-text` (`:360`) — and falls through to
   `runClient(cmd, args, socketPath)` (`:374`) for any other `cmd`. So a
   socket-dispatchable arg-less observer (`tab-list`, `ls`) is already reachable
   *by name* against any socket via `ANTS_REMOTE_SOCKET`. What is **not**
   expressible is a verb carrying a *new argument shape*: the
   inject verbs below need `key` / `modifiers` / `widget` / `x` / `y` / `w` /
   `h` / `path`, and the CLI has no `--remote-*` flag for those. A generic
   raw-JSON passthrough is the missing piece (§2.4).
3. **The MCP verbs the agent already has hit the wrong instance.** The Ants MCP
   server is hosted *inside the user's running instance* and answers on the
   *default* socket. So `mcp__ants__get_text` observes the user's instance, not
   a test instance. The harness must talk to the test instance's socket
   directly via the CLI, never via the agent's MCP tools.
4. **No launch/teardown discipline.** Relinking or disturbing the user's
   `build/` instance is forbidden (ANTS-2025, CLAUDE.md). A test instance must
   use an isolated binary (`build-fast/`), an isolated socket, and a throwaway
   config profile, and be reaped cleanly.

**Layman:** Right now I can run the automated tests, but I can't "sit down and
use" the actual terminal the way you would — type, click buttons, open dialogs,
resize, run Claude Code — and check it looks right. This sets up a safe,
repeatable way to do exactly that against a throwaway copy of the app, without
ever touching the one you're using.

## 2. Surface

### 2.1 Shape — a throwaway second instance, driven over its own socket

The harness launches a *dedicated* Ants instance, drives it, observes it, and
kills it. It never binds the user's socket, reads the user's config, or relinks
`build/`.

```
                 ┌─────────────────────────────────────────────┐
   harness  ───▶ │  build-fast/ants-terminal --e2e             │
  (shell +       │    ANTS_REMOTE_SOCKET=/tmp/ants-e2e-PID.sock │
   --remote      │    XDG_CONFIG_HOME=<tempdir>                 │
   client)       │    (on-screen window by default)             │
        ▲        │  ┌───────────────────────────────────────┐  │
        │  JSON  │  │ RemoteControl::dispatch()             │  │
        └────────┼─▶│   observe: get-text/tab-list/ls       │  │
   reply (JSON)  │  │   inject: inject-key/inject-click/... │  │ (gated: --e2e)
                 │  └───────────────────────────────────────┘  │
                 └─────────────────────────────────────────────┘
```

The test instance runs **on-screen** (a real mapped window) by default — a
throwaway window briefly appearing on the user's desktop is acceptable, and
`grab-image` (§2.3) captures it via the widget's own paint path regardless.
Running headless via `QT_QPA_PLATFORM=offscreen` is an opt-in the harness
*may* set, but whether the full live-PTY + dialog-render path works offscreen
is **not** a contract of this spec — it is to be confirmed during
implementation (a smoke run under `offscreen`); the default and the guaranteed
path is on-screen.

### 2.2 `--e2e` launch flag (the security gate)

Add an `--e2e` boolean option to the `QCommandLineParser` in `main.cpp`
(near `:222`). The flag threads through to the RemoteControl start gate via the
exact wiring below — this is load-bearing because, by default, **the control
socket never opens**: `MainWindow` only calls `m_remoteControl->start()` when
`m_config.remoteControlEnabled()` is true (`mainwindow.cpp:1100-1102`), and that
config key defaults to **false** (`config.cpp:794`). On a throwaway profile the
gate is therefore closed, so without an override the harness's "wait for socket"
(§2.5) would time out before any verb runs.

Wiring (each step names its exact site):

1. `main.cpp` parses `--e2e` into a local `bool e2eMode` and passes it to a new
   `MainWindow` constructor parameter — `MainWindow(bool quakeMode, bool e2eMode
   = false, QWidget *parent = nullptr)` (currently `MainWindow(bool quakeMode =
   false, QWidget *parent = nullptr)`, `mainwindow.h:53`; built at
   `main.cpp:393`). Inserting `e2eMode` before `parent` is safe: the only two
   construction sites — `MainWindow window(quakeMode)` (`main.cpp:393`) and
   `new MainWindow()` (`mainwindow.cpp:1241`) — pass ≤1 positional arg, so no
   positional `parent` caller rebinds.
2. `MainWindow::MainWindow` (`mainwindow.cpp:244`) initialises the `m_e2eMode`
   member from the param near the top of the ctor, then — at
   `m_remoteControl = new RemoteControl(this, this)` (`mainwindow.cpp:1082`,
   inside the same ctor body, which runs 244→1104) — calls a new
   `m_remoteControl->setE2eMode(m_e2eMode)`. (Secondary `MainWindow`s — e.g.
   File→New Window at `mainwindow.cpp:1241`, `new MainWindow()` — default
   `e2eMode=false`, so their inject gate stays closed; only the `main.cpp:393`
   primary threads `--e2e`. The default-false carries this; no socket-lock
   reasoning needed.)
3. The `if (remoteControlGate)` at `mainwindow.cpp:1101` becomes
   `if (remoteControlGate || m_e2eMode)` (the `static const bool
   remoteControlGate` cache at `:1100` and the `start()` call at `:1102` are
   unchanged) — so `--e2e` forces the socket open past the
   `remoteControlEnabled()` default-false gate without changing it for normal
   runs.

`setE2eMode` is called on **every** `MainWindow` ctor with that window's
`m_e2eMode` — which is **true only** for the `--e2e`-launched primary, never
from a config key, env var, or a literal `true`. It is the **sole** enabler of
the inject verbs (INV-1, INV-2): when the gate is false the inject branch in
`dispatch()` returns `{ok:false, code:"e2e_disabled"}` and posts no event. `--e2e` widens the *verb
set* only; it does **not** alter rendering, theming, or any user-visible
behaviour — a test must observe the same app the user runs.

Profile isolation is achieved by the *harness* exporting **three** XDG dirs to
throwaway paths at launch, not by `--e2e`: `XDG_CONFIG_HOME` (config —
`~/.config/ants-terminal/config.json`), **`XDG_DATA_HOME`** (session/tab
persistence — `sessionmanager.cpp:22` uses `QStandardPaths::GenericDataLocation`),
and **`XDG_CACHE_HOME`** (MCP-state / codebase-index / ledger caches). Isolating
`XDG_CONFIG_HOME` alone is **insufficient** — verified at implementation, a test
instance with only config isolated restores the user's saved tabs from
`XDG_DATA_HOME`, violating INV-3.

### 2.3 Inject verbs (new RemoteControl commands)

Four new private `cmd*` handlers on `RemoteControl`, wired into `dispatch()`
behind the `RemoteControl::m_e2eMode` gate (set by `setE2eMode`, §2.2). Their
**on-wire `cmd` keys are hyphenated** — `inject-key`, `inject-click`,
`resize-window`, `grab-image` — added as new branches to the `dispatch()`
`if`-chain (`src/remotecontrol.cpp`), matching the existing socket convention
(`send-text`, `get-text`, `tab-list`); the underscore
form is reserved for MCP tool names, which these verbs deliberately are **not**. They are **socket-dispatch-only** — not registered as MCP tools (so
`inject-*` can never be called against the user's live instance through the
agent's MCP toolset; a security property, not an oversight). They therefore skip
the MCP response-wrap (ANTS-1294) and the caller_cwd contract (ANTS-1401/1404),
which apply only to MCP-registered tools.

**Target resolution (shared by all four verbs).** A single **new** helper
`e2eResolveTarget(const QString &objectName) -> QWidget*` picks the widget to
act on. It must **not** use `QApplication::activeWindow()` — that is unreliable
on offscreen/some-WM platforms (the codebase already avoids it for exactly this
reason; see the rationale at `mainwindow.cpp:1011-1019`). It uses the same
`topLevelWidgets()+isVisible()` traversal *pattern* the codebase already uses
(`mainwindow.cpp:918-921` — note that site is a dialog *detector* that returns
on the first visible `QDialog`, so `e2eResolveTarget` adds the
return-the-widget step it doesn't have). Order: (1)
`QApplication::activeModalWidget()` if a modal dialog is up; else (2) the first
visible non-`this` top-level in `QApplication::topLevelWidgets()`; then (3) if
`objectName` is given, `target->findChild<QWidget*>(objectName)` within that
top-level. An `objectName` that resolves to nothing → `widget_not_found`
(no event posted, no crash); no live top-level at all → `bad_args`. The helper
returns that target *widget*; each verb then applies its own hop on it —
`inject-key` posts to `target->focusWidget()` (falling back to `target`),
`inject-click` maps coordinates onto `target`, `grab-image`/`resize-window` act
on `target` directly.

```cpp
// All execute on the GUI thread: dispatch() runs inside the QLocalServer
// readyRead handler (onNewConnection), which is owned by the GUI thread,
// so postEvent to widgets is main-thread-safe (verified — no QtTest needed).

// cmd "inject-key" — post a synthetic key event to the resolved target (default:
// the resolved top-level's focus widget; `widget` overrides). A plain printable
// chord posted to the focused terminal grid reaches its keyPressEvent (→ PTY),
// which is the robust, accelerator-independent path the smoke suite uses to
// prove inject-key (case 2). Window-context QAction/QShortcut accelerators can
// also be driven by posting to the top-level, but whether a focused grid yields
// the chord first is platform-dependent and not relied on by any smoke case.
//   { cmd:"inject-key", key:<Qt::Key int or "Return"/"Escape"/...>,
//     modifiers?:["ctrl","shift","alt","meta"], text?:"<chars>",
//     widget?:"<objectName>" }
// Builds QKeyEvent press+release, posts via QCoreApplication::postEvent.
QJsonDocument cmdInjectKey(const QJsonObject &req);

// cmd "inject-click" — post a synthetic mouse press+release.
//   { cmd:"inject-click", widget?:"<objectName>",
//     x?:<int>, y?:<int>, button?:"left"|"right"|"middle" }
// If `widget` given and x/y omitted, clicks the widget's rect center; x/y are
// relative to the target widget (or the resolved top-level if no widget).
QJsonDocument cmdInjectClick(const QJsonObject &req);

// cmd "resize-window" — resize the resolved top-level window.
//   { cmd:"resize-window", w:<int>, h:<int> }
// Clamped to [minimumSize, screenAvailable]; calls QWidget::resize. The reply
// echoes the post-clamp size: { ok:true, w:<applied>, h:<applied> } — so a test
// can assert the result WITHOUT current_state (which is not socket-reachable).
QJsonDocument cmdResizeWindow(const QJsonObject &req);

// cmd "grab-image" — render the resolved widget (default: top-level) to a PNG.
//   { cmd:"grab-image", path:"<path>", widget?:"<objectName>" }
// QWidget::grab() → QPixmap::save(path,"PNG") — captures via the widget's own
// paint path (no compositor / Spectacle needed). Guard order:
//   (1) path missing/empty → bad_args  (validatePath returns bad=false for an
//       empty rawPath — pathvalidation.cpp:55 — so the handler must reject it
//       itself before QPixmap::save("")).
//   (2) ANTS_E2E_ARTIFACT_DIR unset/empty → bad_args ("no artifact dir"): a
//       clearer message than validatePath's own empty-root reject (which fails
//       CLOSED with bad_path "no project root to anchor against",
//       pathvalidation.cpp:84-89 / ANTS-1805 — so this guard is for message
//       clarity, not because validatePath is unsafe).
//   (3) PathValidation::validatePath(path, canonical(ANTS_E2E_ARTIFACT_DIR),
//       "grab-image", "path") — a path escaping that root → bad_path.
QJsonDocument cmdGrabImage(const QJsonObject &req);
```

Refusal envelopes follow `docs/standards/mcp-error-codes.md`. Reused codes:
`bad_path` (grab path escapes the artifact root), `bad_args` (missing/invalid
key/coords/dims, no artifact dir, or no live top-level). **New codes** —
`e2e_disabled` (inject verb called without `--e2e`) and `widget_not_found`
(objectName resolves to nothing) — must be added to the taxonomy in the
implementation commit (§8); the taxonomy is scoped to MCP refusal envelopes,
but these socket-only verbs emit the same `{ok:false, code}` shape and the
harness dispatches on `code`, so they share the code space.

### 2.4 Generic raw-JSON passthrough (`--remote-json`)

Add a `--remote-json '<json-object>'` option to the `main.cpp` `--remote`
client path. The object must carry a `cmd` field; the client extracts `cmd` and
passes the remaining fields as the `args` object to
`runClient(command, args, socketPath)` (which itself does `QJsonObject env =
args; env["cmd"] = command;` in `src/remotecontrol_state.cpp` — so a `cmd` key
inside the body is redundant with the extracted one, not a second channel). It sends to the socket
named by `ANTS_REMOTE_SOCKET` (or default) and prints the JSON reply to stdout.
It **reuses `runClient` (`src/remotecontrol_state.cpp`) unchanged**, so it
inherits its exit semantics: **0** on `ok:true`, **2** on `ok:false`, **1** on a
transport failure (no/oversized response). A verb-level refusal is therefore
exit 2 with the `{ok:false, code}` body on stdout — *expected*, not a transport
error — so the smoke suite (§6) parses the JSON `ok`/`code` fields and never
relies on the exit code alone to tell a refusal from a crash.

This is the **single** CLI surface the harness uses, for two reasons: the inject
verbs carry argument shapes (§1 missing-piece 2) the hand-coded `--remote` flags can't express,
and routing every call — observers included — through one passthrough keeps the
driver uniform. (Arg-less observers like `tab-list` are *also* reachable by name
via the `main.cpp:374` fall-through, but `--remote-json` is the one path that
also carries the inject args.) The existing `runClient` SO_PEERCRED
server-credential check is preserved unchanged.

### 2.5 Harness driver + smoke suite (files)

```
tools/e2e/
  run.sh        # launch_e2e / call_e2e / teardown_e2e shell functions:
                #   launch_e2e [tag] -> mktemp a per-tag profile+artifact dir +
                #     socket path, pick binary ($ANTS_E2E_BIN, default
                #     build-fast/ants-terminal), export ANTS_REMOTE_SOCKET +
                #     XDG_CONFIG_HOME + XDG_DATA_HOME + XDG_CACHE_HOME (all 3 —
                #     session lives in XDG_DATA_HOME) + ANTS_E2E_ARTIFACT_DIR,
                #     spawn `--e2e`.
                #     Re-entrant: a [tag] arg gives a 2nd isolated instance
                #     (case 5's non-e2e gate test) its own socket/profile.
                #     Readiness = poll `call_e2e '{"cmd":"tab-list"}'` until
                #     ok:true or a ~10 s deadline (socket-file existence alone
                #     is not enough — the server must accept + answer);
                #     tab-list is used because it IS socket-dispatchable.
                #   call_e2e '<json>' -> $BIN --remote-json '<json>' on the
                #     test socket; echoes reply. A transport failure (exit 1)
                #     AFTER readiness — e.g. the instance crashed mid-run —
                #     aborts the current case with a diagnostic; the EXIT trap
                #     still reaps every spawned instance + temp dir.
                #   teardown_e2e -> kill all spawned instances, rm -rf the temp
                #     dirs (trap EXIT, covers every launch_e2e tag).
  smoke.sh      # the 8 smoke cases (see §6); sources run.sh; exit 0/non-zero.
docs/qa/e2e/
  README.md     # how to run the harness by hand + the ANTS-2050 case format.
```

The harness driver is shell + the `--remote-json` client; no new C++ executable.

## 3. Invariants

- **INV-1** — Inject verbs (`inject-key`, `inject-click`, `resize-window`,
  `grab-image`) are refused with `code:"e2e_disabled"` and post **no** event /
  perform **no** resize/grab unless the instance was launched with `--e2e`.
  *Test:* `smoke.sh` calls `inject-key` against a non-`--e2e` instance → refusal;
  against an `--e2e` instance → `ok:true`, and `resize-window`'s reply echoes
  the applied `{w,h}` (the socket-observable result, since `current_state` is
  MCP-only).
- **INV-2** — The inject gate `RemoteControl::m_e2eMode` becomes **true only**
  through the `--e2e` CLI flag: `setE2eMode` is called from every `MainWindow`
  ctor with that window's `m_e2eMode`, which is `true` only for the
  `--e2e`-launched primary — no config key, env var, or default path sets it
  true. *Test:* source-grep that every `setE2eMode(` argument and every
  `m_e2eMode =` write traces to the `--e2e`-derived `e2eMode` value (no literal
  `true`, no config/env read); `smoke.sh` confirms a plain launch leaves inject
  verbs refused.
- **INV-3** — The harness launches the test instance with an `ANTS_REMOTE_SOCKET`
  distinct from the default and `XDG_CONFIG_HOME` + `XDG_DATA_HOME` +
  `XDG_CACHE_HOME` all under a fresh `mktemp` directory, so it never binds the
  user's socket nor reads/writes the user's config, **session, or caches**.
  *Test:* `smoke.sh` asserts the live socket path ≠ `defaultSocketPath()`, the
  profile dir equals the harness's recorded `mktemp` path, and a freshly
  launched test instance's `tab-list` shows a clean session (not the user's
  saved tabs) — the regression that proved `XDG_CONFIG_HOME` alone is
  insufficient.
- **INV-4** — all four e2e verbs (`inject-key`, `inject-click`, `resize-window`,
  `grab-image`) resolve their target via the offscreen-safe helper
  (`activeModalWidget` → first-visible `topLevelWidgets()` →
  `findChild(objectName)`; never `QApplication::activeWindow()`); an
  `objectName` that resolves to nothing returns `code:"widget_not_found"` with
  no event posted and no crash. *Test:* `smoke.sh` sends a click to a bogus
  objectName → `widget_not_found`; source-grep asserts no `activeWindow()` in
  the resolve helper.
- **INV-5** — `grab-image` writes only to a path resolving under the
  harness-provided `ANTS_E2E_ARTIFACT_DIR`. It refuses `bad_args` for a
  missing/empty `path` arg and for an unset/empty artifact dir (both guarded in
  the handler before `validatePath`), and `bad_path` for a path escaping the
  (set) root via `PathValidation::validatePath`. *Test:* `smoke.sh` (a) grabs to
  an in-dir path (PNG exists, non-zero size); (b) grabs to `/etc/x.png` with the
  var set → `bad_path`; (c) launches without `ANTS_E2E_ARTIFACT_DIR` and grabs
  → `bad_args`; (d) `grab-image` with empty `path` → `bad_args`.
- **INV-6** — Teardown kills the test instance and removes the temp profile +
  artifact dirs; no `ants-terminal --e2e` process and no temp dir survive a
  completed run. *Test:* `smoke.sh` EXIT trap asserts `pgrep` empty and dirs
  gone.
- **INV-7** — The harness defaults its binary to `build-fast/ants-terminal`
  (never `build/`) and never invokes a build/relink while an instance runs
  (ANTS-2025). *Test:* `run.sh` review + grep: default `ANTS_E2E_BIN` is
  `build-fast/…`; no `cmake --build` inside `run.sh`/`smoke.sh`.
- **INV-8** — Inject verbs execute on the GUI thread and use raw Qt event
  posting (`QCoreApplication::postEvent` with `QKeyEvent`/`QMouseEvent`), adding
  **no** QtTest link dependency to the main binary. *Test:* source-grep: no
  `QtTest`/`QTest::` in `src/remotecontrol_terminal.cpp` (the inject verbs'
  TU); CMake `ants-terminal` target
  gains no `Qt6::Test` link.
- **INV-9** — `--remote-json` sends only to the socket the client already
  resolves (`ANTS_REMOTE_SOCKET` or default) and preserves the existing
  `runClient` server-credential (SO_PEERCRED) check unchanged. *Test:*
  source-review of the new branch; `smoke.sh` asserts the socket-targeting
  property distinctly from case 1's content echo — a `--remote-json` call with
  `ANTS_REMOTE_SOCKET` pointed at a non-existent path fails as a *transport*
  error (exit 1), confirming the client honours the resolved socket rather than
  silently hitting the default.

## 4. RAM / build cost

- **Runtime:** one transient Ants process (~150 MB resident, the app's normal
  footprint) alive only for a harness run; PNG artifacts (a few hundred KB each)
  in a temp dir. **Eviction:** the EXIT trap kills the process and `rm -rf`s the
  temp profile + artifact dirs — no persistent growth, nothing survives the run
  (INV-6).
- **Build:** no new C++ executable; four small `cmd*` handlers + one CLI option
  added to existing translation units (`remotecontrol.cpp`, `main.cpp`). One new
  ctest target wraps `smoke.sh` under label `e2e`, **excluded from the default
  ctest set** (needs a GUI/offscreen platform + a real shell+PTY) — opt-in via
  `ctest -L e2e`.
- **External libs:** none. Reuses `QWidget::grab`, raw `QEvent` posting, and the
  existing `QLocalSocket` client. No QtTest dependency in the shipped binary
  (INV-8).

## 5. Security boundaries

Injecting keystrokes into a terminal is arbitrary shell execution; injecting
clicks is arbitrary GUI control. The defence is layered:

1. Inject verbs exist in `dispatch()` but are gated behind
   `RemoteControl::m_e2eMode`, settable only via `setE2eMode` on the `--e2e`
   path (INV-1, INV-2). The user's normal binary never sets it, so the verbs are
   inert there.
2. Inject verbs are **not** MCP-registered, so they are unreachable through the
   agent's MCP toolset (which targets the user's instance). They exist only on
   the raw socket of an `--e2e` instance.
3. The RemoteControl socket keeps its existing defences unchanged: 0700 socket
   dir / `UserAccessOption` (`start()` in `src/remotecontrol.cpp`), the server-side
   SO_PEERCRED same-UID check on accept, and the client-side server-credential
   check inside `runClient` (`src/remotecontrol_state.cpp`). `--e2e` widens the
   *verb set*, not the *trust model* — a peer must still be same-UID.
4. `grab-image` writes only under the harness artifact dir (INV-5) via
   `PathValidation` (ANTS-1295), so a compromised caller cannot scribble PNGs
   across the filesystem.

## 6. Tests

Smoke suite: `tools/e2e/smoke.sh` (sources `tools/e2e/run.sh`), run as a ctest
target under label `e2e` (opt-in, excluded from the default run). Each case is
`drive → observe → assert`, exit 0/non-zero with diagnostic output. Cases cover
the invariants and prove each harness capability end-to-end:

1. **Terminal echo** — `send-text "echo ANTS_E2E_OK\n"` → `get-text` contains
   `ANTS_E2E_OK`. (Proves launch + terminal drive + observe; INV-3, INV-9.)
2. **GUI key event** — `inject-key` a sentinel string (e.g. `ANTS_E2E_KEY`) into
   the focused terminal grid → `get-text` contains it. This proves the synthetic
   `QKeyEvent` path end-to-end (a real Qt key event reaching a widget's
   `keyPressEvent`), distinct from case 1's `send-text` PTY-write path and
   independent of any accelerator/menu — so the assertion proves `inject-key`
   itself, not a fallback verb. (Proves GUI key injection; INV-1.)
3. **Dialog open + grab** — open the roadmap dialog by `inject-click` on the
   Roadmap chrome button (resolve by objectName; the dialog is the
   `RoadmapDialog` created in `MainWindow::showRoadmapDialog()`,
   `mainwindow.cpp:6261`), poll `topLevelWidgets` for a visible `RoadmapDialog`
   through a short-deadline loop (within ~3 s), then `grab-image` to the
   artifact dir → PNG exists, non-zero. (Proves chrome click drive +
   dialog-visibility wait + image observe; INV-5.) Runs **on-screen by
   default**; under `QT_QPA_PLATFORM=offscreen` (§2.1) the case is **skipped, not
   failed** if no visible dialog materialises, so the suite stays green headless.
4. **Window resize** — `resize-window {w:1000,h:700}` → the verb's own reply
   echoes the applied `{w,h}` (case asserts on that, since `current_state` is
   not socket-reachable). (Capability: positive path of INV-1 for
   `resize-window`.)
5. **Gate negative** — a *second* isolated, plain (non-`--e2e`) instance —
   launched via `launch_e2e <tag>` with its own socket + profile — refuses
   `inject-key` with `e2e_disabled`. (INV-1, INV-2.)
6. **Bad widget** — `inject-click widget:"nope-xyz"` → `widget_not_found`, app
   still alive (a following `tab-list` still answers `ok:true`). (INV-4.)
7. **Grab path escape** — with `ANTS_E2E_ARTIFACT_DIR` set, `grab-image
   path:/etc/x.png` → `bad_path`; with it unset → `bad_args`. (INV-5.)
8. **Teardown** — EXIT trap asserts no surviving process / temp dir. (INV-6.)

These are shell + ctest-`e2e` cases driving a live instance — deliberately
**not** `tests/features/<name>/` C++ conformance tests (`specs.md` §3.6), because
the contract here is an end-to-end user-drive loop, not an in-process unit
behaviour. Per the project test convention, verify the negative-path cases
(5–7) fail against a build that omits the guard before restoring it — produced
by locally reverting the relevant guard (the `m_e2eMode` check for case 5, the
`findChild`-miss branch for case 6, the path/dir guards for case 7), rebuilding
to `build-fast/`, and confirming the case no longer refuses.

## 7. Out of scope

- **Real OS-level input** (Wayland `wtype`/`ydotool`, Spectacle compositor
  capture) — deterministic socket injection is the chosen mechanism; real-input
  coverage is deferred. Tracked by a follow-up if a case ever needs the real
  input stack (none in the ANTS-2050 skeleton).
- **The full feature-covering case suite** — only the 8 smoke cases ship here;
  the exhaustive per-subsystem suite is **ANTS-2050**, which runs on this
  harness.
- **CI wiring** — the `e2e` label exists; whether it runs in CI (needs an
  offscreen/GUI runner) is a later call, not part of this foundation.

## 8. Cross-doc impact

- **CLAUDE.md** — add a one-line pointer under "Test harnesses" to `tools/e2e/`
  + the `--e2e` flag and the `e2e` ctest label.
- **`docs/standards/mcp-error-codes.md`** — register the two new codes
  `e2e_disabled` and `widget_not_found` in the same commit as the refusal sites
  (the taxonomy's standing rule; §2.3), with a one-line scoping note that they
  are emitted by socket-only `--e2e` verbs (not MCP tools) sharing the code
  space — so the taxonomy's "every MCP tool" opening reads honestly.
- **CHANGELOG.md** — new entry for the E2E harness + `--e2e` flag.
- **ROADMAP.md** — flip ANTS-2049 to ✅ on ship; ANTS-2050 stays 📋.
- No README / PLUGINS.md change (no user-facing feature, no `ants.*` Lua surface
  change).

## Cold-eyes loop log

Two independent cold reviewers per loop, briefed against the spec + cited code,
no prior-loop findings carried forward. Every finding verified against live
source before fixing; unverified claims dismissed with the contradicting grep.

- **Loop 1** — 2 CRITICAL/HIGH + MED/LOW. Fixed: the RC `start()` gate is at the
  `mainwindow.cpp:1100-1102` call-site (default-false `remoteControlEnabled()`),
  not in `start()` — pinned the `--e2e` → ctor-param → `setE2eMode` wiring;
  inject targeting must avoid offscreen-unreliable `activeWindow()`; corrected
  the `runClient` exit-code contract; honest framing of the `--remote` surface;
  defined the `ANTS_E2E_ARTIFACT_DIR` validation root; flagged the two new error
  codes for taxonomy registration; line-number + `$TMPDIR` drift.
- **Loop 2** — 2 CRITICAL + HIGH/MED. Fixed: readiness poll + observers used
  MCP-only `current_state`/`last_selection` (no socket branch) → switched to
  socket-dispatchable `get-text`/`tab-list`/`ls` + verb-reply envelopes;
  underscore vs hyphenated verb keys → all on-wire keys + new verbs hyphenated;
  INV-2 grep made provenance-based (not a vacuous literal match); artifact-dir
  guard precedes `validatePath`; `resize-window` returns its post-clamp size;
  second-instance isolation + `launch_e2e` re-entrancy; crash-mid-run handling;
  case-3 dialog open + visibility wait.
- **Loop 3** — 1 CRITICAL + HIGH/MED. Fixed: the `validatePath` empty-root
  rationale was **inverted** — code fails closed (`pathvalidation.cpp:84-89`,
  ANTS-1805) — so the dir guard is for message clarity, not safety; missing/empty
  `path` arg needs its own `bad_args` guard; `--remote-json` must extract `cmd`
  (runClient overwrites it); ctor/`:1082` same-body clarification + New-Window
  note; `.h` decl line refs; INV-4 scope; taxonomy scoping note.
- **Loop 4** — 0 CRITICAL/HIGH; MED/LOW. Fixed: `:918-921` is a dialog detector
  not a target resolver (`e2eResolveTarget` is new code adding the return step);
  `inject-key` post target specified; `runClient` line `:8142`; ctor
  mid-signature insertion safety; case-3 dialog opener concretized.
- **Loop 5** — 0 CRITICAL/HIGH; MED/LOW. Fixed: dropped a socket-lock
  non-sequitur; reconciled "`setE2eMode` called every ctor, true only via
  `--e2e`"; INV-9 now asserts socket-targeting distinctly from the echo;
  **case 2 redesigned** to prove `inject-key` independently (sentinel char into
  the grid → `get-text`), not via an `inject-click` fallback; cross-ref labels;
  exact case count; case-3 offscreen skip-not-fail.
- **Loop 6 (confirming)** — 0 CRITICAL/HIGH. One verified MED: `dispatch()` is an
  `if`-chain, not a `switch` (fixed in §1.1/§2.3); range anchor tidied;
  focus-widget hop clarified. Two LOW line-claims (`defaultSocketPath:1244`,
  `showRoadmapDialog:6263`) dismissed — greps confirm `:1204` / `:6261`. Both
  reviewers: "a mature, well-cited spec; no logic or contract defects." Clean.
- **Implementation phase (Task 1, 2026-06-10)** — runtime observation caught
  what static review could not: a test instance with only `XDG_CONFIG_HOME`
  isolated **restored the user's saved tabs** from `XDG_DATA_HOME`
  (`sessionmanager.cpp:22`, `GenericDataLocation`). INV-3 + §2.2 + §2.5 updated
  to isolate all three XDG dirs (config + data + cache). An early proof of the
  harness's value — the gap was invisible to six doc-review loops.
