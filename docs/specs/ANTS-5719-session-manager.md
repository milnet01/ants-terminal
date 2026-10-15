<!-- ants-spec-format: 1 -->
# ANTS-5719 — Claude Code session manager: see, park and resume sessions by memory

**Status:** spec draft (2026-10-15).
**Kind:** feature.
**Source:** ROADMAP ANTS-5719 (user request 2026-10-15).
**Blocker for:** ANTS-5720 (heavy jobs take turns).

**Layman:** One window lists every Claude session in your tabs, what it is
doing and how much memory it uses; idle sessions can be parked to free
memory and brought back in the same tab with the conversation intact.

## 1. Goal

A user running many Claude Code sessions in Ants tabs can see, in one
window, each session's state, how long it has been idle and how much memory
its whole process tree uses. They can park an idle session, which ends its
`claude` process and frees that memory, and resume it later in the same tab
with the same conversation. Ants warns when free memory runs low, naming
what to park. When memory becomes critical, Ants parks the longest-idle
sessions by itself, one at a time, and says which.

## 2. Problem

1. **Memory runs out with many sessions open.** Measured 2026-10-15 on the
   user's 32 GB host with `ps -eo rss,comm` summed per command: 18 `claude`
   processes held 5.4 GB (191–435 MB each), 18 `ants-mcpd` 0.9 GB and 4
   `clangd` 2.3 GB, with about 9 GB available. Sessions also start builds
   and test suites that spike memory.
2. **Nothing in Ants measures memory.** No code under `src/` reads RSS,
   PSS, `smaps_rollup` or `MemAvailable` (search for those names across
   `src/`, 2026-10-15, found none).
3. **An idle session costs memory for nothing, and freeing it by hand is
   risky.** Quitting `claude` and resuming it later works today:
   `MainWindow::openClaudeProjectsDialog` writes
   `cd <path> && claude --resume <id>` into a tab. But the user has to find
   which session is idle and large, remember its id, and must not quit one
   that is mid-task.
4. **The system's last resort is a kill.** `earlyoom` runs with `-m 6 -s 10`
   (read from `/etc/sysconfig/earlyoom`, 2026-10-15): when available memory
   is under 6 % (about 1.9 GB here) and free swap under 10 %, it kills a
   process, which may be a Claude session mid-task.

## 3. Scope decisions (agreed with the user)

All made by the user on 2026-10-15.

- **Parking is the user's click, except at critical memory.** Below the
  warning threshold Ants only suggests. Below the critical threshold it
  parks by itself (§ 4.6).
- **Only sessions in Ants tabs.** Those are the ones Ants can resume in
  place. Sessions in other terminals are not listed.
- **Heavy-job turn-taking is a second step**, ANTS-5720, not this spec.
- **The warning threshold is 4 GB available**, adjustable in Settings.

Set in this spec (not asked): the critical threshold is 2.5 GB available,
above earlyoom's 6 % line (about 1.9 GB here), so parking acts before
earlyoom kills. Adjustable in Settings.

## 4. Surface

### 4.1 Where a session's facts come from

`ClaudeTabTracker` already finds each tab's `claude` process
(`ClaudeIntegration::findClaudeChildPid`), its transcript path, and keeps
`ShellState` with `state`, `awaitingInput` and `idleSinceMs`. The session
id is the transcript's basename without `.jsonl`
(`ClaudeTabTracker::shellForSessionId` maps the other way). A new accessor exposes what the tracker holds
privately:

```cpp
// claudetabtracker.h
struct SessionFacts {
    pid_t shellPid = 0;
    pid_t claudePid = 0;        // 0 when no claude is running
    QString sessionId;          // transcript basename minus .jsonl; may be empty
    QString transcriptPath;
};
SessionFacts sessionFacts(pid_t shellPid) const;
```

### 4.2 Memory sampling — new module `src/sessionmemory.{h,cpp}`

```cpp
namespace sessionmemory {
struct TreeSample {
    qint64 pssKb = 0;        // sum of Pss over the tree
    qint64 cpuTicks = 0;     // sum of utime+stime over the tree
    int processes = 0;
    bool capped = false;     // the walk hit kMaxTreePids
    bool approximate = false; // some process fell back to VmRSS (§ 6)
};
constexpr int kMaxTreePids = 256;
// Walk /proc/<pid>/task/<tid>/children breadth-first from root, root
// included, then add every process whose /proc/<p>/stat ppid is already
// in the set (a child whose forking thread exited is in no children
// file; ClaudeIntegration::findClaudeChildPid falls back the same way).
// Read /proc/<p>/smaps_rollup (Pss) and /proc/<p>/stat.
TreeSample sampleTree(pid_t root, const QString &procRoot = "/proc");
// MemAvailable from <procRoot>/meminfo, in kB; -1 when unreadable.
qint64 memAvailableKb(const QString &procRoot = "/proc");
}
```

- **PSS, not RSS.** RSS counts shared pages once per process, so 18 node
  processes sharing a runtime would be over-counted. PSS divides shared
  pages between their users, so tree sums add up to real use.
- **The tree is rooted at the tab's shell**, so it includes `claude`, its
  MCP servers, `clangd`, and any build or test it started.
- `procRoot` is a test seam: a fixture directory stands in for `/proc`.
- A process that exits mid-walk is skipped, not an error.

A `SessionMonitor` (`QObject`, in the same module) samples every tracked
shell plus `MemAvailable` every 5 seconds on a `QTimer`, only while the
manager window is open or a threshold is armed (always, while
`session_manager_enabled` is true). It keeps the last 12 samples per shell
(one minute) and emits `sampled()`.

### 4.3 The manager window — new `SessionManagerDialog`

Non-modal. Opened from the Claude menu, "Session Manager…", shortcut key
`session_manager` (no default; Ctrl+Shift+M is the "Center window"
action in `src/mainwindow_menus.cpp`). One row
per tab whose shell has a running or parked Claude session:

| Column | Source |
|---|---|
| Tab | tab title (the full title, as `TabListPanel` shows) |
| Project | `claude`'s cwd basename |
| State | `claudestate::display()` mapped to a `ClaudeTabIndicator::Glyph` as `src/claudestatuswidgets.cpp` does, in words (`TabListPanel::stateLabel`) and its colour; "Parked" for a parked tab |
| Idle | now − `idleSinceMs`, when the state is Idle |
| Memory | tree PSS from § 4.2, in MB |
| Action | **Park**, **Resume** or **Go to tab** |

A header line shows available memory and the total of all listed trees.
Rows sort by memory, largest first. **Park** is enabled only when the row
passes § 4.4's manual check; its tooltip says why when it is not.

### 4.4 Who may be parked

One pure function decides, so the window, the warning and auto-park agree:

```cpp
enum class ParkVerdict { Ok, NotRunning, NoSession, Busy, WaitingOnUser,
                         DraftInComposer, ChildrenBusy, TooRecent,
                         Focused };
struct ParkInput {
    ClaudeState state; bool awaitingInput;
    bool resumableId;         // sessionId matches § 4.5's pattern
    qint64 idleMs;            // now - idleSinceMs; meaningless unless Idle
    bool composerEmpty;       // lastUserKeystrokeMs < idleSinceMs
    qint64 cpuMsLastMinute;   // tree cpu over the last 12 samples
    bool focusedTab;
};
ParkVerdict parkVerdict(const ParkInput &in, bool automatic,
                        qint64 minIdleMs);
```

In order: no `claude` → `NotRunning`; no resumable session id →
`NoSession`; awaiting input → `WaitingOnUser`;
state not Idle → `Busy`; composer not empty → `DraftInComposer`; tree CPU
over the last minute ≥ 1000 ms → `ChildrenBusy` (a background build or test
is still running); then, for automatic parking only, idle under `minIdleMs`
→ `TooRecent` and the focused tab → `Focused`. Otherwise `Ok`.

### 4.5 Parking and resuming

**Park** (manual or automatic), on a session whose verdict is `Ok`:

1. Record `{sessionId, cwd, flags}` in `SessionMonitor`, keyed by shell
   pid (`parkedRecord(shellPid)` reads it; every consumer reads it there). `cwd` is
   `/proc/<claudePid>/cwd`. `flags` are the arguments of
   `/proc/<claudePid>/cmdline` that are on an allowlist:
   `--model <v>`, `--permission-mode <v>`, `--dangerously-skip-permissions`,
   `--add-dir <v>`. Anything else is dropped.
2. Send `SIGTERM` to `claudePid` only, never the shell, through

   ```cpp
   enum class ParkResult { Exited, StillRunning, NoSuchProcess };
   // SIGTERM, then poll until the pid is gone or timeoutMs passes.
   ParkResult sessionmemory::terminateAndWait(pid_t pid, int timeoutMs);
   ```

   with `timeoutMs` 10000. The transcript is already on disk: Claude Code
   appends each message as it is written.
3. While a record exists, the tab-dot provider in
   `src/claudestatuswidgets.cpp` shows `Glyph::Idle` (grey) for that shell
   instead of hiding the dot, and the manager row reads "Parked".

On `StillRunning` the park is reported as failed and the record is dropped.
Ants never sends `SIGKILL`.

**Resume**, from the row's button or from the tab's context menu: write
`cd <cwd> && claude --resume <sessionId> <flags>` into that tab with
`TerminalWidget::writeCommand`, every value through `shellQuote`, as
`openClaudeProjectsDialog` does. A session id that does not match
`^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$` is refused.
Resume clears the record.

A parked record lives in memory only. After Ants restarts, a parked tab is
a plain shell; the session is still resumable from Projects & Sessions
(`ClaudeProjectsDialog`).

### 4.6 Warning and automatic parking

A state machine fed one `MemAvailable` sample per tick:

```cpp
enum class MemLevel { Ok, Low, Critical };
// Hysteresis 512 MB: Low starts below warn, ends above warn + 512 MB;
// Critical starts below critical from any level (Ok may go straight to
// Critical), ends above critical + 512 MB, returning to Low or Ok.
MemLevel nextLevel(MemLevel current, qint64 availKb,
                   qint64 warnKb, qint64 criticalKb);
```

- **Entering Low**: one status-bar message naming the three largest
  parkable sessions (verdict `Ok` with `automatic = false`) and a button
  that opens the manager. Not repeated until the level has left Low.
- **While Critical**, and `session_manager_auto_park` is true: every 10 s,
  park the parkable session (verdict `Ok` with `automatic = true`) that has
  been idle longest. One per step. Each park is announced in the status bar
  with the tab's name. It stops when the level leaves Critical or nothing
  is parkable; "nothing parkable" is announced once.

### 4.7 Configuration and hot reload

| Key | Default | Meaning |
|---|---|---|
| `session_manager_enabled` | `true` | sampling, warning and auto-park on |
| `session_manager_warn_mb` | `4096` | Low below this many MB available |
| `session_manager_critical_mb` | `2560` | Critical below this |
| `session_manager_auto_park` | `true` | park by itself while Critical |
| `session_manager_auto_park_min_idle_min` | `10` | auto-park only after this idle time |

Values are read when `MainWindow::onConfigFileChanged` and the Settings
dialog's Apply path run, so a change applies without a relaunch. The rest
reaches a running terminal only through a new binary, on the next launch.

## 5. Invariants

- **INV-1** — `sampleTree` returns the PSS sum of the root and every
  descendant, each process once.
  *Test:* `tests/features/session_manager/test_session_manager.cpp`
  `SessionManager.Inv1TreeSumsEveryDescendantOnce`, against a fixture
  `procRoot` with a three-level tree whose `smaps_rollup` files carry known
  Pss values, one process listed as a child of two parents, and one whose
  only link is its `stat` ppid (in no `children` file).
  *Breaks when:* the walk skips grandchildren or the ppid-only process, or
  counts the twice-listed process twice.

- **INV-2** — `sampleTree` stops at `kMaxTreePids` and sets `capped`.
  *Test:* same file, `Inv2WalkIsCapped`, fixture tree of 300 processes.
  *Breaks when:* the walk follows a cycle or runs unbounded.

- **INV-3** — `parkVerdict` returns the first failing reason in § 4.4's
  order, and `Ok` only when every check passes.
  *Test:* same file, `Inv3VerdictTable`, a table with one row per verdict
  and one `Ok` row, for both `automatic` values.
  *Breaks when:* a busy, waiting or drafting session can be parked, or
  `TooRecent`/`Focused` block a manual park.

- **INV-4** — Parking signals only the `claude` pid, with `SIGTERM`, and
  never `SIGKILL`.
  *Test:* same file, `Inv4ParkSignalsClaudeOnly`, calling
  `terminateAndWait`: a fake shell with a child `sleep 300` returns
  `Exited`, the child gone and the shell alive; a child that ignores
  `SIGTERM` (`trap '' TERM`) with a 500 ms timeout returns `StillRunning`
  and is still alive afterwards.
  *Breaks when:* the shell or a sibling is signalled, or a stubborn child
  is killed.

- **INV-5** — The resume command is
  `cd <cwd> && claude --resume <id> <flags>` with every value shell-quoted,
  flags limited to § 4.5's allowlist, and a malformed id refused.
  *Test:* same file, `Inv5ResumeCommand`: a cwd holding a space and a
  quote, a cmdline holding an allowlisted flag and `--print`, and an id
  of `x; rm -rf ~`.
  *Breaks when:* a value is unquoted, a non-allowlisted flag survives, or
  the bad id produces a command.

- **INV-6** — `nextLevel` applies § 4.6's thresholds with 512 MB
  hysteresis.
  *Test:* same file, `Inv6LevelHysteresis`, a sample series crossing each
  threshold down, wobbling inside the band, then rising above it.
  *Breaks when:* the level flaps inside the band, or a single sample below
  critical taken from Ok does not return Critical.

- **INV-7** — While Critical with auto-park on, each 10 s step parks
  exactly one session, the longest-idle with an `Ok` automatic verdict;
  with auto-park off it parks none.
  *Test:* same file, `Inv7AutoParkPicksLongestIdle`, with the monitor's
  park action replaced by a recorder.
  *Breaks when:* two sessions park in one step, the focused tab parks, or
  the config key is ignored.

- **INV-8** — The five config keys exist with § 4.7's defaults and are
  applied on both reload paths.
  *Test:* same file, `Inv8ConfigKeysAndReload`: round-trip through
  `Config`, and a source check that `onConfigFileChanged` and the Settings
  apply path both call the monitor's `applyConfig`.
  *Breaks when:* a key is misspelt, a default differs, or one reload path
  is missed.

- **INV-9** — Trust boundary: the values in a parked session's resume
  command come only from `/proc` and the transcript basename, and reach
  the shell only through INV-5's builder.
  *Test:* same file, `Inv9OnlyTheBuilderResumesParked`: a source check
  that the park and resume code calls `writeCommand` only with the
  builder's result.
  *Breaks when:* the parked-session resume formats its own command.

## 6. Failure modes

- **`smaps_rollup` unreadable** (older kernel, permission): fall back to
  `VmRSS` from `/proc/<p>/status` and mark the figure "approx." in the
  window.
- **The tracker has no session id yet** (transcript not created): the
  verdict is `NoSession`; the tooltip reads "No conversation to resume yet".
- **`claude` ignores `SIGTERM`**: reported after 10 s, record dropped,
  nothing killed (§ 4.5).
- **A child outlives `claude`** (an MCP server or `clangd` that does not
  exit with it; unverified on this host): Ants does not kill it.
- **The user types into a parked tab** before resuming: that is a normal
  shell; the Resume button stays until used or the tab closes.
- **A parked session misses peer messages.** `session_message` mail waits
  in the store and is read on resume; a `SendMessage` to a parked session
  finds no live session. The status bar names parked sessions so the user
  knows.
- **Auto-park cannot free enough** (every session busy): the warning stays
  and "nothing parkable" is announced once; earlyoom remains the backstop.
- **Many tabs**: a sample is two small file reads per process; at 18
  sessions of about 10 processes, 360 reads every 5 s.

## 7. Tests

All in `tests/features/session_manager/` with a `spec.md` pointing here,
compiled into the bundle `build_target_for` names, label `features`. Each
test is seen failing against a build without the feature before it is
counted. INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7 and INV-8 each
have the test named in § 5, and so does INV-9.

## 8. Alternatives considered (and rejected)

- **Freeze idle sessions with `SIGSTOP`.** Frees no memory: a stopped
  process keeps its pages until the kernel swaps them, which is the
  slowness the user wants to avoid.
- **Quit by typing `/exit` into Claude.** Depends on the composer being
  empty and the UI accepting it; `SIGTERM` does not depend on screen state.
- **Extend the Tab List Panel instead of a window.** The panel is a narrow
  tab switcher; memory figures and actions would crowd it.
- **RSS instead of PSS.** Over-counts shared pages (§ 4.2).
- **Auto-park on idle time alone.** The user chose click-only except at
  critical memory (§ 3).

## 9. Out of scope

- Heavy jobs taking turns under load — tracked by ANTS-5720.
- Sessions outside Ants tabs (§ 3).
- Keeping parked records across an Ants restart — deferred; not yet queued.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `SessionManager.Inv1TreeSumsEveryDescendantOnce` |
| INV-2 | `SessionManager.Inv2WalkIsCapped` |
| INV-3 | `SessionManager.Inv3VerdictTable` |
| INV-4 | `SessionManager.Inv4ParkSignalsClaudeOnly` |
| INV-5 | `SessionManager.Inv5ResumeCommand` |
| INV-6 | `SessionManager.Inv6LevelHysteresis` |
| INV-7 | `SessionManager.Inv7AutoParkPicksLongestIdle` |
| INV-8 | `SessionManager.Inv8ConfigKeysAndReload` |
| INV-9 | `SessionManager.Inv9OnlyTheBuilderResumesParked` |
| § 4.3 window layout | **nothing** — visual; checked by hand when built |

## 11. Cross-doc impact

- `docs/subsystems.md`: add `sessionmemory` and `SessionManagerDialog`.
- `docs/standards/mcp-config-keys.md` is for MCP keys only; the five keys
  go in the user-facing config documentation.
- CHANGELOG under `[Unreleased]` when it ships.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-5719-session-manager-loop-log.md`.

## 13. Resource cost

Per tracked shell: 12 samples of three integers. One `QTimer` at 5 s.
No new dependencies.
