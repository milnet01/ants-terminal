<!-- ants-spec-format: 1 -->
# ANTS-4272 — Deliver the output, line, prompt and keypress plugin events

**Status:** accepted (2026-10-14).

**Kind:** implement.

**Source:** ROADMAP.md ANTS-4272 (cold-eyes-2026-05-21; the user's split
and decisions of 2026-10-13 are in the item's body).

**Pairs with:** ANTS-1750 (the plugin worker-thread contract this builds on).

**Layman:** Plugins can react to what the terminal prints, to each finished
line, to shell prompts and to keys you press. Seeing output or keys needs
your permission once per plugin, and keys are never passed on while you
type a password.

## 1. Problem

`ants.on()` accepts `output`, `line`, `prompt` and `keypress`
(`eventFromString` in `src/luaengine.cpp`) and `PLUGINS.md` documents
them, but nothing fires them: `PluginEvent::Output`, `Line`, `Prompt` and
`KeyPress` have no `fireEvent` call site in `src/`. A plugin written to the
documentation silently never runs.

Wiring them naively has three costs:

1. **Privacy.** `ants.get_output()` reads `LuaEngine::m_recentOutput`, and
   `PluginManager::setRecentOutput` has no caller in `src/`. These events
   would hand every installed plugin the screen and every keystroke,
   passwords included. `TerminalWidget::keyPressEvent` already refuses to
   log typed text for that reason (ANTS-5077).
2. **Rate.** Output arrives as one `VtBatch` per parse, and
   `TerminalGrid::newLine()` runs once per line, so `cat` of a large file
   drives thousands of events a second into a plugin's queue.
3. **Freeze.** ANTS-1750 § 2.4 forbids the GUI thread from blocking on a
   worker. A key veto needs exactly that (ANTS-1750 § 2.6's bounded wait).

### 1.1 Decided by the user (2026-10-13)

Recorded in ANTS-4272's body:

- **Permission.** `output` and `line` need a new manifest permission,
  `terminal.read`; `keypress` needs `input.keys`. The user grants each
  once per plugin, through the existing first-load prompt. `prompt`
  carries no terminal content and needs none.
- **Passwords.** No `keypress` event is delivered while a password is
  being typed, or while the terminal cannot tell. § 2.7 says how a
  password prompt is detected.
- **Watch only.** A `keypress` handler cannot swallow the key. ANTS-1750
  § 2.6's bounded veto is not built.

Every other choice below follows from the three costs above.

## 2. Surface

### 2.1 Permissions

`knownPermissions()` in `src/pluginmanager.cpp` gains `"terminal.read"`
and `"input.keys"`, so the manifest parser keeps them and the load-time
prompt asks for them.

`PluginManager::fireEvent` adds one filter beside `handlesEvent`:

```cpp
// PluginManager — the permission an event needs, or empty.
// Output, Line -> "terminal.read"; KeyPress -> "input.keys"; else "".
static QString permissionFor(PluginEvent e);
```

An engine is skipped when `permissionFor(event)` is non-empty and
`hasPermission` of it is false.
`ants.on()` still accepts the name, so a plugin can register before the
user grants; its handler simply never runs.

### 2.2 No work when nobody listens

```cpp
// PluginManager — true when a healthy engine handles `e` and, where
// permissionFor(e) is non-empty, holds that permission.
bool wants(PluginEvent e) const;
// TerminalWidget — installed by MainWindow::connectTerminal under ANTS_LUA_PLUGINS.
void setPluginEventFilter(std::function<bool(PluginEvent)> wants);
```

Each emit site in § 2.3–2.6 asks the filter first. When it returns false
or is unset, the site skips the plugin emit and its decoding, text
extraction or formatting; the host function's other work runs as before. A build
without `ANTS_LUA_PLUGINS` never installs it.

Each site emits a `TerminalWidget` signal; `MainWindow::connectTerminal`
connects it to `m_pluginManager->fireEvent`, as it does for
`commandFinished`.

### 2.3 `output`

`TerminalWidget::onVtBatch` appends `batch->rawBytes` to a
`PluginOutputCoalescer` (new, `src/pluginoutputcoalescer.h`):

```cpp
class PluginOutputCoalescer {
public:
    static constexpr int kMaxChars = 16384;
    void append(const QByteArray &bytes); // stateful UTF-8 decode
    QString take();                       // pending text; empties the buffer
    bool hasPending() const;
};
```

A split multi-byte character waits for its second half. When pending text
exceeds `kMaxChars`, only the last `kMaxChars` are kept. A single-shot
100 ms timer, started on an append when it is not already running, emits
`pluginOutput(take())` when it fires. So one terminal emits at most ten
`output` events a second, each at most `kMaxChars` long. The payload is
the decoded text, escape sequences included.

### 2.4 `line`

When the filter wants `Line`, `TerminalWidget::onGridLineCompleted` reads
the row's text and `TerminalGrid::screenLineWrapped(row)` before its
`m_triggerRules.empty()` early return. `newLine()` also runs on a soft
wrap, so the payload is the logical line, as `PLUGINS.md` promises:

```cpp
// src/pluginoutputcoalescer.h — one per terminal.
class PluginLineJoiner {
public:
    // Holds a soft-wrapped row; on a row that is not, returns the held
    // rows plus this one. Held text past PluginOutputCoalescer::kMaxChars
    // keeps its head.
    std::optional<QString> addRow(const QString &rowText, bool softWrapped);
};
```

A returned line is emitted as `pluginLine(text)` if a new per-terminal
budget allows it.
`takeEventBudget` takes the cap as a parameter, defaulting to
`kEventsPerSecond`; the line budget's cap is
`kPluginLinesPerSecond = 200`. Lines past the cap in a one-second window
are dropped.

### 2.5 `prompt`

`TerminalGrid` gains a callback that fires for every OSC 133 marker that
passed `verifyOsc133Hmac`:

```cpp
using PromptMarkerCallback = std::function<void(char marker, int exitCode)>;
void setPromptMarkerCallback(PromptMarkerCallback cb);
```

It is called after the existing per-marker state change, in the same
branch. A forged marker returns before that branch's `switch`, so it never
reaches the callback. `exitCode` is meaningful for `D` only.
`TerminalWidget` emits `pluginPrompt` with `marker=A`, `marker=B`,
`marker=C` or `marker=D&exit=N`, under its own budget at
`kEventsPerSecond`.

### 2.6 `keypress`

`TerminalWidget::sendKeyData` is the one point after which a key is sent
(ANTS-5077). After `ptyWrite`, it emits `pluginKey(name)` when all hold:

- the filter wants `KeyPress`;
- `event` is non-null;
- the PTY is known not to be at a password prompt (§ 2.7);
- a per-terminal budget at `kPluginKeysPerSecond = 100` allows it.

`name` is
`QKeySequence(event->keyCombination()).toString(QKeySequence::PortableText)`,
for example `Ctrl+C` or `Shift+A`. The handler's return value is ignored:
the key has already been written.

An IME commit (`TerminalWidget::inputMethodEvent`) and a paste
(`TerminalWidget::performPaste`) do not pass through `sendKeyData`, so
they fire no `keypress`.

### 2.7 Password prompts

A shell at its prompt turns echo off, so echo alone does not mark a
password. Measured 2026-10-13 by reading each program's termios through
the pty master (`pty.fork`, then `tcgetattr` after two seconds):

| Program | `ECHO` | `ICANON` |
|---|---|---|
| `bash --norc --noprofile -i`, at its prompt | off | off |
| `zsh -f -i`, at its prompt | off | off |
| `bash -c "read -s"` | off | on |
| Python `getpass.getpass()` | off | on |
| `cat` | on | on |

So a password prompt is **`ECHO` off with `ICANON` on**: line mode with
no echo.

```cpp
// Pty — true only when tcgetattr(fd) succeeds and the PTY is not
// at a password prompt (ECHO clear with ICANON set).
static bool keysShareable(int masterFd);
```

On Linux a pty master reports the slave's termios, so the master fd
answers for the shell side; the table above was read that way.

The fd is used only on the parse worker thread. `Pty::onReadReady`
samples `keysShareable(m_masterFd)` on every read, beside its existing
`tcgetpgrp` sample, and stores it in a `std::atomic<bool>` that `VtStream`
owns and `TerminalWidget` reads. It starts false, so no key is delivered
before the first read. A password prompt is output, so the read carrying
it also samples the prompt's termios.

### 2.8 Failure modes

- **A handler is slow.** Events queue on its worker. ANTS-1750's health
  tick demotes a plugin whose handler outruns its budget; the GUI never
  waits. The caps in § 2.3–2.6 bound how fast a queue can grow.
- **The sample is stale.** A program that enters a password prompt
  without printing anything gives no read to sample, so keys typed then
  are delivered. This is the residual risk of sampling on reads; § 2.10
  says why sampling per key was rejected.
- **A full-screen program asks for a password.** It reads keys with
  `ICANON` off, which looks like a shell prompt, so its keys reach a
  plugin holding `input.keys`.
- **`tcgetattr` fails** (the fd closed in teardown). `keysShareable`
  returns false and keys are withheld.
- **A permission is never granted.** The handler is registered and never
  runs. No error is raised, as an un-granted `ants.clipboard` is simply
  absent.
- **Output overflows.** The oldest text in the window is lost; the
  plugin sees the newest `kMaxChars` characters.

### 2.9 Reload story

Plugins already reload without a relaunch. A plugin that adds a handler
or a permission picks it up on its next load, and the grant prompt runs
then. The terminal code itself arrives in a new build, which needs a
relaunch like any other build.

### 2.10 Alternatives rejected

- **No permission, like the existing events.** Rejected by the user: any
  installed plugin could record passwords.
- **A bounded key veto (ANTS-1750 § 2.6).** Rejected by the user for now:
  it adds a GUI-thread wait per key, and no plugin needs to swallow keys
  yet.
- **One `output` event per `VtBatch`.** Rejected: unbounded rate under
  bulk output (§ 1 item 2).
- **Withhold keys whenever `ECHO` is off.** Rejected: § 2.7's table
  shows bash and zsh clear `ECHO` at every prompt, so almost no key would
  be delivered.
- **Sample the termios per key from the GUI thread.** Rejected: `Pty`'s fd lives
  on the parse worker, and a GUI-thread `tcgetattr` races its close in
  teardown.
- **Strip escape sequences from `output`.** Rejected: `line` already
  gives clean text, and stripping is a second parser.

## 3. Invariants

- **INV-1** — `fireEvent` delivers `Output` and `Line` only to an engine
  holding `terminal.read`, and `KeyPress` only to one holding
  `input.keys`; `Prompt` and every older event need no new permission.
  Breaks when `fireEvent` filters on `handlesEvent` alone.
  *Test:* `tests/features/plugin_io_events`: two plugins each register
  `line`, `output` and `keypress`, and only the one granted each
  permission receives its events.

- **INV-2** — A manifest naming `terminal.read` or `input.keys`, with the
  grant store already granting it, loads with `hasPermission` true for
  it. Breaks when `knownPermissions()` omits a string, which the parser
  then drops silently.
  *Test:* `tests/features/plugin_io_events`.

- **INV-3** — Each emit site in § 2.3–2.6 calls the plugin filter before
  decoding, reading a row or formatting a key name, and skips the emit,
  and only the emit, when the filter is unset or false. Breaks when an
  emit site decodes or reads text before asking, or returns from its host
  function.
  *Test:* `tests/features/plugin_io_events` source-scrape of `onVtBatch`,
  `onGridLineCompleted`, `sendKeyData` and the prompt callback.

- **INV-4** — `PluginOutputCoalescer` returns appended text in order,
  joins a UTF-8 character split across two appends, and keeps only the
  last `kMaxChars` characters of an overflow. Breaks when the decoder is
  stateless, or an overflow keeps the head.
  *Test:* `tests/features/plugin_io_events` appends `"é"` split across
  two calls, then `kMaxChars + 10` characters.

- **INV-5** — One terminal emits at most one `output` event per 100 ms.
  Breaks when `onVtBatch` emits per batch.
  *Test:* `tests/features/plugin_io_events` source-scrape: the flush is a
  single-shot 100 ms timer, started only when it is not running.

- **INV-6** — `onGridLineCompleted` emits `pluginLine` before its
  trigger-rules early return, under a budget capped at
  `kPluginLinesPerSecond`, and only for a row that ends a logical line.
  Breaks when the emit sits after the early return, shares the trigger
  budget, or fires on a soft-wrapped row.
  *Test:* `tests/features/plugin_io_events` source-scrape of the order;
  `takeEventBudget` with cap 200 refusing the 201st take in one window;
  `PluginLineJoiner` given `"ab"` soft-wrapped then `"c"` returns nothing,
  then `"abc"`.

- **INV-7** — With `ANTS_OSC133_KEY` set, a marker with a wrong `ahmac`
  reaches no prompt callback; a correct one does, as does every marker
  with the key unset, and `D` carries its exit code. Breaks when the
  callback runs before `verifyOsc133Hmac`.
  *Test:* `tests/features/plugin_io_events` feeds a `TerminalGrid`
  `OSC 133;A`, `B`, `C` and `D;3`, then a forged `A`.

- **INV-8** — `Pty::keysShareable` returns false for a pty whose slave
  has `ECHO` clear with `ICANON` set, true with both clear, and false for
  an fd `tcgetattr` rejects; `sendKeyData` emits no `pluginKey` unless the
  sampled state is true. Breaks when the helper tests `ECHO` alone, or
  fails open.
  *Test:* `tests/features/plugin_io_events` opens a pty with `openpty`,
  sets each of the two slave states and checks
  `keysShareable(master)`, then passes `-1`; a source-scrape finds
  `sendKeyData`'s `pluginKey` emit inside a test of the sampled flag.

- **INV-9** — `sendKeyData` writes the key before it emits `pluginKey`
  and consumes no result; `inputMethodEvent` and `performPaste` emit
  nothing. Breaks when the write waits on a plugin, or an IME commit or
  a paste fires `keypress`.
  *Test:* `tests/features/plugin_io_events` source-scrape of the three
  bodies.

## 4. RAM / build cost

Per terminal: one coalescer, at most `kMaxChars` (16384) characters plus
a decoder's state; one line joiner, holding at most `kMaxChars`; three
new `EventBudget`s; one 100 ms timer. No new
build target outside the feature test, and no new dependency.

## 5. Out of scope

- Wiring `ants.get_output()`, which still returns an empty string, and
  gating it on `terminal.read` — deferred; not yet queued. ANTS-2001 is
  closed, and `PluginManager::setRecentOutput` still has no caller.
- A key veto — deferred; not yet queued.
- `keypress` for IME commits and pastes — deferred; not yet queued.

## 6. Tests

Feature test: `tests/features/plugin_io_events/` (`spec.md`,
`test_plugin_io_events.cpp`), in the bundle `build_target_for` names.
Covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8 and INV-9.
Label `features;fast`. Each test is seen to fail
against the pre-change source before it passes.

## 7. Cross-doc impact

- `PLUGINS.md`: drop the † from the four rows; mark `keypress` not
  cancellable; document the payloads and rates; add `terminal.read` and
  `input.keys` to the permission table.
- `docs/specs/ANTS-1750.md` § 2.6: a pointer that ANTS-4272 wired
  `KeyPress` watch-only, so its veto contract stays unbuilt.
- `src/luaengine.h`: the `KeyPress` comment.
- `CHANGELOG.md`.

## What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `tests/features/plugin_io_events`, permission delivery |
| INV-2 | `tests/features/plugin_io_events`, manifest test |
| INV-3 | `tests/features/plugin_io_events`, source-scrape |
| INV-4 | `tests/features/plugin_io_events`, coalescer tests |
| INV-5 | **Partial:** a source-scrape sees the timer's shape, not its rate in a running terminal |
| INV-6 | `tests/features/plugin_io_events`, order scrape, budget and joiner tests |
| INV-7 | `tests/features/plugin_io_events`, OSC 133 test |
| INV-8 | `tests/features/plugin_io_events`, pty test and `sendKeyData` scrape |
| INV-9 | `tests/features/plugin_io_events`, source-scrape |
| § 2.8 stale sample | **nothing** — a limit of sampling on reads, accepted in § 2.10 |
| § 2.8 full-screen password | **nothing** — a raw-mode password prompt looks like a shell prompt; not yet queued |

## Cold-eyes loop log

Rows live in `../reviews/ANTS-4272-plugin-io-events-loop-log.md`.
