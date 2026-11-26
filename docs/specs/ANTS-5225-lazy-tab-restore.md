<!-- ants-spec-format: 1 -->
# ANTS-5225 — Restore a saved tab's history when the tab is first needed

**Status:** spec draft (2026-11-26).
**Kind:** perf.
**Source:** ROADMAP ANTS-5225 (code-quality-review-2026-09-11 perf pass,
lane mainwindow-a; picked up by the user 2026-11-26).

**Layman:** After a restart, Ants shows every saved tab at once, but a tab
loads its history and starts its shell only when you open it, so the
window appears without waiting for tabs you may never look at.

## 1. Goal

At startup, `MainWindow::restoreSessions` reads only each saved tab's
title, pinned title and working directory. A tab's scrollback is restored,
and its shell started, the first time the tab is needed: when it becomes
the current tab, or when a remote-control command or MCP verb names it by
index, or when it is renamed. Until then the tab is **asleep**. A sleeping
tab's session file is never overwritten and never aged out.

## 2. Problem

`MainWindow::restoreSessions` calls `SessionManager::loadSession` for every
tab in `tab_order.txt` before the window shows. `SessionManager::restore`
checks the envelope's SHA-256, unpacks the `qCompress` payload, parses every
scrollback line into `TermLine`s and pushes them onto the grid.

Measured 2026-11-26 on `bench_session_save`'s grid (50000 lines × 80
columns; file 878026 bytes, 52353196 bytes unpacked), with a scratch program
linking `ants_core_lib` and `ants_vt_lib`, mean of three runs:

| Step | ms |
|---|---|
| SHA-256 of the payload | 0.4 |
| `qUncompress` | 29.7 |
| Walk the stream to the title, cwd and pin, skipping cell bytes | 4.3 |
| Full `SessionManager::restore` | 402.8 |

So each long-history tab costs about 0.4 s of startup, and reaching the
three fields a tab needs at startup costs about 35 ms of it. The tab title,
pinned title and cwd sit at the end of the stream (`serializeStream` writes
them after the screen lines), so they cannot be read without unpacking.

## 3. Scope decisions (agreed with the user)

Both made by the user on 2026-11-26, choosing the recommended option.

- **A sleeping tab starts no shell.** The rejected option loaded every tab
  in the background after the window showed; it keeps every tab's history
  in memory, as today.
- **No file-format change.** Startup reads the three fields from the
  existing session file (§2's 35 ms). The rejected option wrote titles into
  `tab_order.txt` as well, for near-zero startup cost, at the price of an
  on-disk format change.

## 4. Surface

### 4.1 Reading the three fields

```cpp
// sessionmanager.h
struct SessionPeek { QString title; QString cwd; QString pinnedTitle; };
// Reads tabId's file as loadSession would, but builds no lines and
// touches no grid. Returns false on any file restore() would refuse;
// does not move the file aside (the wake's loadSession does that).
static bool peekSession(const QString &tabId, SessionPeek *out);
```

`restore()` and `peekSession` share one decoder for everything before the
line records: the size cap, the envelope and its SHA-256 check, the
uncompressed-length guards, `qUncompress`, magic, version and dimensions.
The peek then walks each line record with the same per-record caps
`restore()` applies (`kMaxRestoreScrollbackLines`, `kMaxRestoreLineCells`,
`kMaxRestoreCombiningCells`, `kMaxRestoreCombiningMarks`,
`kMaxRestoreRows`), skipping cell bytes with `QDataStream::skipRawData`,
then reads the title, cwd (version ≥ 2) and pin (version ≥ 3) exactly as
`restore()` does.

### 4.2 Startup

`restoreSessions` keeps its once-per-process guard, its tab-order load and
its default-tab removal. Per saved tab it calls `addShellTab`,
`applyPersistedTabColor` and `peekSession`:

- **Peek succeeds:** record the tab in `m_sleepingTabs`
  (`QHash<TerminalWidget *, SleepingTab>`, holding the tab id and saved
  cwd). Set the label from the pin (also written to `m_tabTitlePins`), or
  else from the saved title.
- **Peek refuses:** the tab is not asleep; the deferred block wakes it
  like the active tab, and `loadSession` moves the file aside as today.

The deferred block still drains events, then calls `setCurrentIndex(idx)`.
It then wakes the tab at `idx` explicitly, since `setCurrentIndex` emits
nothing when `idx` is already current. It then wakes every tab whose peek
refused. No other tab is woken.

### 4.3 Waking

```cpp
// mainwindow.h — wakes t if it is asleep; a no-op otherwise.
void wakeRestoredTab(TerminalWidget *t);
// Wakes the tab at index, then returns terminalAtTab(index).
TerminalWidget *wakeTerminalAtTab(int index);
```

`wakeRestoredTab` removes `t` from `m_sleepingTabs` first, so a re-entrant
call is a no-op. It then runs today's per-tab order:

1. `loadSession(tabId, grid, &cwd, &pin)`. A refusal moves the file aside
   (`session_restore_refusal`) and the tab carries on empty.
2. `clearScreenContent()`.
3. `forceRecalcSize()`.
4. `startShell(startDir, m_config.shellCommand())`, where `startDir` is
   the saved cwd when that directory exists. On failure, stop here.
5. `update()` and `trackTerminalShell(t)`.
6. If `t` is `focusedTerminal()`: `m_claudeIntegration->setShellPid`
   with its PID (ANTS-5733).

It is called from:

- `onTabChanged`, before anything else in it reads the terminal. That
  covers a click, the tab switcher, the tab list panel, `select-window`,
  and Qt choosing a neighbour when the current tab closes.
- `setTabTitleForRemote`, which both the Rename Tab menu and `set-title`
  reach, so a pin set on a sleeping tab is saved by its next blob write.
- `RemoteControl`, through `wakeTerminalAtTab`, at every site that
  resolves an explicit `tab` index: `send-text`, `get-text`,
  `recent_errors` and `last_selection`. `RemoteControl` keeps no
  `terminalAtTab` call.

`terminalAtTab` itself does not wake: the Claude tab-dot indicator
provider, which the coloured tab bar asks about each tab it paints,
resolves tabs through it.

### 4.4 Saving, closing and ageing

- **`saveAllSessions`** (forced or not) keeps a sleeping tab's id at its
  place in `tabOrder` and writes no blob for it. `sessionTabIds` lists
  sleeping tabs as it lists any other.
- **`updateTabTitles`** skips sleeping tabs.
- **`performTabClose`** on a sleeping tab removes its session file and
  releases nothing (it has no shell), as today. The undo-close record
  takes the saved cwd, since `shellCwd()` is empty.
- **`cleanupOldSessions`** reads the ids in `tab_order.txt` and deletes no
  `session_<id>.dat` named there, whatever its age. A sleeping tab's file
  keeps its old mtime across restarts, so the age sweep would otherwise
  delete it 30 days after its last write.

## 5. Invariants

- **INV-1** — For any file `loadSession` accepts, `peekSession` returns
  the title, cwd and pinned title that `loadSession` returns for it,
  version 1, 2 and 3 streams included.
  *Test:* `tests/features/lazy_tab_restore` saves grids with a title, a cwd
  and a pin, and V2 and V1 streams built by hand, then compares the two
  readers.
  *Breaks when:* the walk reads the cwd before skipping the screen lines,
  or reads a pin from a V2 stream.

- **INV-2** — `peekSession` refuses every file `restore()` refuses and
  accepts every file it accepts.
  *Test:* the same test runs both readers over a corpus with one case per
  refusal in `restore()`: wrong envelope version, SHA mismatch,
  length-prefix claims past each guard, out-of-range dimensions, each
  per-record cap exceeded by one, and a stream cut mid-cell.
  *Breaks when:* the peek skips the SHA check, or skips cell bytes past a
  cell count over `kMaxRestoreLineCells`.

- **INV-3** — `restoreSessions` loads no tab's history and starts no
  shell itself. `loadSession` and `startShell` are reached on restore only
  through `wakeRestoredTab`.
  *Test:* scrape: the `restoreSessions` body contains `peekSession(` and
  `wakeRestoredTab(`, and neither `loadSession(` nor `startShell(`; the
  `wakeRestoredTab` body contains `loadSession(` before `startShell(`,
  before `trackTerminalShell(`, before `setShellPid(`.
  *Breaks when:* a later edit restores history eagerly in the startup
  loop.

- **INV-4** — A sleeping tab wakes when it becomes current, when it is
  renamed, or when a remote-control command names its index.
  *Test:* scrape: `onTabChanged` calls `wakeRestoredTab(` before its first
  `focusedTerminal()`; `setTabTitleForRemote` calls `wakeRestoredTab(`;
  `src/remotecontrol*.cpp` contains `wakeTerminalAtTab(` and no
  `terminalAtTab(`.
  *Breaks when:* a new remote-control command resolves a tab with
  `terminalAtTab`, and sends text to a tab with no shell.

- **INV-5** — `saveAllSessions` writes no blob for a sleeping tab, and its
  id stays in the saved tab order at its position.
  *Test:* scrape: `saveAllSessions` checks `m_sleepingTabs` after
  `tabOrder.append(` and before the first `SessionManager::saveSession(`.
  *Breaks when:* the check moves above `tabOrder.append(`, which drops the
  tab from `tab_order.txt`, or is removed, which overwrites the history
  with an empty grid on the first 30 s tick.

- **INV-6** — `cleanupOldSessions` deletes no `session_<id>.dat` whose id
  `tab_order.txt` names, whatever its age, and still deletes an unlisted
  one past the cutoff.
  *Test:* `tests/features/lazy_tab_restore` writes two files 40 days old,
  lists one in `tab_order.txt`, runs `cleanupOldSessions(30)` under a
  temporary `XDG_DATA_HOME`, and checks which file survives.
  *Breaks when:* the sweep matches by age alone.

- **INV-7** — A sleeping tab's label is the saved pin, or else the saved
  title, until it wakes.
  *Test:* scrape: `updateTabTitles` skips `m_sleepingTabs` entries.
  *Breaks when:* a non-default `tab_title_format` relabels the tab
  "Shell" every 2 s.

## 6. Failure modes

- **The file changes or vanishes between peek and wake.** The wake's
  `loadSession` refuses or finds nothing; the tab opens empty with a
  fresh shell, and a refused file is kept aside. That is today's outcome
  for a refused file.
- **The saved cwd no longer exists at wake.** `startShell` gets an empty
  start directory, as today.
- **The shell fails to start at wake.** The tab is no longer asleep and
  has no shell, as a failed start leaves a tab today. It is not retried.
- **The process exits with tabs still asleep.** Their files and their
  places in `tab_order.txt` are kept; the next start peeks them again.
- **A crash before the 5 s uptime floor.** No save runs, as today.
- **Two Ants processes.** Unchanged: a second window restores nothing
  (ANTS-5032).

## 7. Tests

- `tests/features/lazy_tab_restore/` (new, label `features`): INV-1, INV-2
  and INV-6 as behavioural gtest cases on `SessionManager`; INV-3, INV-4,
  INV-5 and INV-7 as scrapes over the `MainWindow` and `RemoteControl`
  sources. Its `spec.md` cites this document's invariants rather than
  restating them. Each case is seen to fail against the pre-change code,
  or against a mutant where the pre-change code has no such surface.
- `tests/perf/bench_session_save.cpp` gains a `peek` phase reporting
  `session.peek_ms` beside `session.restore_ms`, so §2's figures come from
  a shipped bench rather than the scratch program.
- The existing scrape tests whose anchors move are in §11.

## 8. Alternatives considered (and rejected)

- **Load every tab in the background after the window shows.** Rejected by
  the user (§3): every tab's history is held in memory whether or not it is
  opened.
- **Write titles and cwd into `tab_order.txt`.** Rejected by the user
  (§3): an on-disk format change, for about 35 ms per long-history tab.
- **Start every shell at startup and add the history above it on first
  show.** The live grid would need history inserted in front of lines the
  shell has already written. That shifts every global line number
  (prompt regions, selections, hyperlink spans) and needs a reflow from the
  saved width. Far more risk for no outcome the user asked for.
- **Wake inside `terminalAtTab`.** The tab-dot provider resolves every
  tab through it, so every tab would wake when the tab bar first paints.

## 9. Out of scope

- `tab_list` and `ls` report a sleeping tab with an empty cwd and shell
  PID 0, as for any tab without a shell. A field marking it asleep would
  be a new response field — deferred; not yet queued.
- `caller_cwd` routing (`tabForCwd`) never matches a sleeping tab. It has
  no shell, so no Claude session can be calling from it.
- An idle tab in a process that runs longer than 30 days can have its file
  aged out by the 24 h sweep. INV-6 fixes this too, since such a tab is in
  `tab_order.txt`.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `tests/features/lazy_tab_restore` (peek equals load) |
| INV-2 | `tests/features/lazy_tab_restore` (refusal corpus) |
| INV-3 | `tests/features/lazy_tab_restore` (scrape) |
| INV-4 | Partial: `tests/features/lazy_tab_restore` (scrape) checks the named call sites; a new waking route that bypasses them is caught only for `RemoteControl` |
| INV-5 | `tests/features/lazy_tab_restore` (scrape) |
| INV-6 | `tests/features/lazy_tab_restore` (cleanup case) |
| INV-7 | `tests/features/lazy_tab_restore` (scrape) |
| Startup cost per tab | Partial: `bench_session_save` reports `session.peek_ms`; it runs under the `perf` preset, outside the push gate |

## 11. Cross-doc impact

Test contracts whose clauses name the moved calls. They are amended in the
implementation commit, together with the scrape code that reads them, so
no test checks a clause its own spec no longer states:

- `tests/features/crash_safe_session_persist/spec.md` INV-10:
  `loadSession` moves from `restoreSessions` to `wakeRestoredTab`; the
  "removes no session file" half stands.
- `tests/features/claude_dot_restored_tabs/spec.md` WI-1 and WI-3: the
  `startShell` → `trackTerminalShell` → `setShellPid` order moves to
  `wakeRestoredTab`.
- `tests/features/tab_rename_persist/spec.md` Invariant 5: the pin comes
  from `peekSession` at startup.
- `tests/features/shell_command_wiring/spec.md` Invariant 3: the restore
  `startShell` call is in `wakeRestoredTab`.

Also: `docs/subsystems.md` and `.indie-review/partition.json` for the new
test directory; a CHANGELOG entry under `[Unreleased]`.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-5225-lazy-tab-restore-loop-log.md`. No review
has run: no file format or contract that other code binds to changes, so
none is owed.

## 13. Resource cost

A peek unpacks one file at a time into a buffer freed before the next
(52353196 bytes for §2's grid). A sleeping tab holds an empty grid and no
shell, so memory for the tabs a user never opens falls from a full grid
each to nearly nothing. `m_sleepingTabs` holds one entry per sleeping tab
and only shrinks.

## 14. Reload

This changes what a terminal does at launch, so it reaches a user at their
next launch, by nature: a running terminal has already restored its tabs.
The code runs in the terminal process, because it builds the terminal's own
tabs. It holds no state beyond `m_sleepingTabs`, which empties as tabs wake,
so it has no stop/start control.
