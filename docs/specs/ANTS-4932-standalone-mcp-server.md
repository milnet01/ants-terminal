# ANTS-4932 — Serve the project-scoped MCP verbs from a standalone process

**Status:** accepted (2026-09-24).
**Kind:** refactor.
**Source:** ROADMAP.md ANTS-4932 (user-request-2026-09-07; measurements and
four design decisions recorded on the item 2026-09-21; two further forks
settled by the user 2026-09-22).
**Blocked by:** none.
**Pairs with:** ANTS-1253 (tool-provider registry), ANTS-2132 (off-GUI-thread
dispatch), ANTS-5144 (shared socket listener).
**Supersedes:** no document. It *amends* the clauses named in § 8.

## 1. Problem

Every Ants MCP verb is a `RemoteControl` method registered by
`MainWindow::setupClaudeMcpProviders()`, and the socket is bound from inside
the GUI process by `ClaudeIntegration::startMcpServer()`. Nothing is loaded at
runtime. So changing one verb costs a rebuild of `ants-terminal` **and a hand
relaunch of the terminal**.

That relaunch is not a few seconds. `~Pty` (`src/ptyhandler.cpp`) kills every
child on teardown, and `SessionManager` persists scrollback and cwd, not
processes. The terminal hosts the user's Claude Code sessions in its tabs, so a
relaunch destroys every in-flight session across every project. The project
`CLAUDE.md` makes this a standing design rule; this item is the structural
instance it names.

Three consequences, in the order they bite:

1. **A verb fix cannot be exercised in the session that wrote it.** Recorded on
   the roadmap item: a shipped `spec_lint` change could not be tested over MCP
   without a relaunch.
2. **The registration list is a GUI method.** `setupClaudeMcpProviders()` is a
   `MainWindow` member, so no non-GUI host can enumerate the verbs at all.
   Most of its `registerToolProvider` calls wrap a `RemoteControl::cmd*`
   method through `rcDelegate`; the rest are inline handlers.
3. **The library carries the GUI.** `ants_core_lib` links `Qt6::Gui`,
   `Qt6::Widgets`, `Qt6::Network`, `Qt6::DBus` and `util` **PUBLIC**, and every
   `remotecontrol_*.cpp` compiles into it.

**What makes this tractable.** Every `MainWindow` read in the
`remotecontrol_*` surface is tab or terminal state. It arrives three ways, and
each has its own search:

- **A direct call in a verb body** — `m_main->` in `src/remotecontrol*.cpp`.
- **A call inside the root resolver** — `main->` in
  `resolveRootCanonical` and `resolveCallerCwdRoot`
  (`src/remotecontrol_feedback.cpp`).
- **A refusal when there is no window** — `if (!m_main)` guards returning
  `no_main` or `no_window`. `roadmap_log`'s `append` is one.

A grep for only the first form misses the other two.

**And a transport already exists.** Claude Code does not speak to the socket
directly. It launches `tools/mcp-bridge.py` as a stdio MCP server
(`claude mcp add ants -- …/tools/mcp-bridge.py`), which forwards each request
to `/tmp/ants-terminal-mcp-<pid>`. That bridge is a process the **client**
already starts, already in the project's working directory. It is the slot this
work drops into, which is why § 2.1 is smaller than the roadmap item predicted.

## 2. Surface

### 2.1 Topology — one door, stdio at the client edge

```
TODAY
  Claude Code --stdio--> tools/mcp-bridge.py --unix socket--> ants-terminal
                                                              (every verb)

SPECIFIED
  Claude Code --stdio--> ants-mcpd --unix socket--> ants-terminal
                         (serves project-scoped     (still serves every verb;
                          verbs; forwards the        ants-mcpd only asks it for
                          terminal-scoped ones)      the terminal-scoped ones)
```

`ants-mcpd` is a new executable. It takes `mcp-bridge.py`'s place in the
client registration; the Python bridge is retained unchanged for one release as
the fallback (§ 8).

Three properties fall out of the client launching it, rather than being
designed:

- **Its process cwd is the client's own working directory.** Observed on a
  running bridge: its `/proc/<pid>/cwd` equals its `claude` parent's. That may
  be a subdirectory of the project. § 2.4 resolves it exactly as it would a
  `caller_cwd` naming that directory.
- **It is the single front door.** The client connects to nothing else.
- **No discovery problem.** `ants-mcpd` finds the terminal the same way the
  bridge does — `$ANTS_MCP_SOCKET` if set, else the newest live-PID
  `/tmp/ants-terminal-mcp-*` owned by the same uid (the picker in
  `tools/mcp-bridge.py::pick_socket()`, reimplemented in C++, including its
  ANTS-1322 liveness check and its uid check).

**What this does NOT achieve, stated as a bound.** An MCP client caches
`tools/list` at connect. **Adding** a verb or changing its arguments still
needs a client reconnect. This removes the *terminal relaunch*, not every
restart. Changing a verb's **behaviour** needs neither.

### 2.2 The library split

A new `ants_mcpcore_lib` holds the project-scoped verb bodies. It follows the
pattern this CMakeLists already establishes twice — `ants_roadmapparse_lib`
(`PUBLIC Qt6::Core`) and `ants_roadmapstore_lib` (`PUBLIC Qt6::Core Qt6::Sql`,
kept headless by design for ANTS-3794's publish path) — and a third time in an
executable: `ants-helper`, built from `ants_helper_lib` (`PUBLIC Qt6::Core`)
under `ANTS_ENABLE_HELPER_CLI`.

**What moves into it.**

- **The `remotecontrol_*` TUs**, except `src/remotecontrol_terminal.cpp`, which
  is the tab-verb file and stays GUI-side. `src/remotecontrol.cpp` holds the
  `RemoteControl` class and moves too. `dispatch()` and the `--remote` socket
  members move to the head of `remotecontrol_terminal.cpp`, because
  `dispatch()` routes to its members such as `cmdGetText` and `cmdTabList`.
  That TU's window-free tail (the `roadmap_log` helpers) moves to the head of
  `remotecontrol_roadmap_query.cpp`, and `enrichLikelyFixes` to beside
  `cmdBuildStatus`. No TU is added, so every `TU k/N` ordinal holds. `ants-mcpd`
  reaches `RemoteControl` only through the registered handlers.
- **The engine sources those TUs call** — `speclint`, `codebaseindex`,
  `config`, `pathvalidation` and the rest. They compile into `ants_core_lib`
  today. Checked: none of them includes a Qt GUI header.
- **`ClaudeIntegration` itself** (`src/claudeintegration.cpp`), which holds
  the `tools/call` pipeline and the `tools/list` builder. It is window-free, so
  it moves whole rather than piecemeal, and `ants-mcpd` hosts it.

`ants_audit_lib` and `ants_lua_lib` stay separate libraries and link
`ants_mcpcore_lib` instead of `ants_core_lib`; both are window-free.
`ants-mcpd` links the three in one `--start-group`, which resolves their
references to each other.
- **`src/mcptoolregistry.cpp`** (§ 2.3).

The build is the completeness check: `ants-mcpd` fails to link while any
dependency is left behind.

**How the list is written.** `ANTS_RC_SOURCES_REL` stays the one place a
`remotecontrol` TU is named (ANTS-3833 INV-11). It splits into two lists in
that block: the moved TUs, which `ants_mcpcore_lib` consumes, and
`remotecontrol_terminal.cpp`, which `ants_core_lib` consumes. `ants_core_lib`
then links `ants_mcpcore_lib`.

`ants_mcpcore_lib` links `Qt6::Core`, `Qt6::Network`, `ants_roadmapparse_lib`
and `ants_roadmapstore_lib`. `Qt6::Network` stays because it is the socket.
`Qt6::Gui`, `Qt6::Widgets` and `Qt6::DBus` must not appear. INV-1 enforces
this.

`ants-mcpd` is `src/mcpdmain.cpp` linked against `ants_mcpcore_lib`.

**The coupling to sever is every `MainWindow` read in the moved TUs**, in all
three forms § 1 names. Pointer mentions are harmless. `MainWindow *` as a
member or parameter type needs only a forward declaration. What must go is
every dereference and every `#include "mainwindow.h"`, `"terminalwidget.h"` or
`"roadmapdialog.h"` in a moved TU. § 2.4 says what replaces them. A verb that
cannot lose its read joins the terminal-scoped set instead (§ 2.3).

### 2.3 One registration list, consumed by both hosts

This is the item's stated top drift risk and the design's load-bearing part.
Today three symbols are involved, and they must not be conflated:

| Role | Symbol | Home |
|---|---|---|
| Registry container | `ClaudeIntegration::m_toolProviders` | `src/claudeintegration.h` |
| Registrar | `ClaudeIntegration::registerToolProvider()` | `src/claudeintegration.h` |
| The registration **list** | `MainWindow::setupClaudeMcpProviders()` | `src/mainwindow.cpp` |
| The schema (`tools/list`) | a hand-built local `QJsonArray tools` | `src/claudeintegration.cpp` |

The list and the schema are two hand-maintained sets bound only by
ANTS-1253 INV-8's cross-grep. A second host would make them three.

**The change:** extract a window-free registration function into a new
`src/mcptoolregistry.cpp`:

```cpp
namespace mcp {
// Registers every verb that does not require a terminal, against any host.
// The ONLY enumeration of the project-scoped verb set.
// `rc` is read on every call: the terminal registers before it has built its
// RemoteControl. `host` carries what the inline handlers need.
void registerProjectScopedVerbs(ToolSink &sink, RemoteControlGetter rc,
                                RegistryHost host = {});

// The verbs that DO require a terminal. The GUI host registers these
// directly; ants-mcpd registers a forwarder for each (§ 2.5).
const QVector<QString> &terminalScopedVerbNames();
}
```

**`ToolSink` carries all three `registerToolProvider()` overloads** — the
`ToolHandler`, `RcHandler` and `DeferredToolHandler` forms. The types they name
(`CallerCwdContract`, `RcHandler`, `DispatchLane` and the handler aliases) move
out of `ClaudeIntegration` into a header in `ants_mcpcore_lib`.
`ClaudeIntegration` keeps aliases, so existing call sites compile unchanged.
`rcDelegate`, today a `MainWindow` member, becomes a free function in
`mcptoolregistry.cpp`.

`MainWindow::setupClaudeMcpProviders()` keeps its name and its terminal-scoped
registrations, and calls `mcp::registerProjectScopedVerbs()` for the rest. So
the terminal still serves every verb.

**A verb is terminal-scoped when its handler reads tab or terminal state.**
Today every registration sits in `mainwindow.cpp`, so location decides nothing.
Read each handler's body. An `rcDelegate` handler is decided by its `cmd*`
body; an inline `mainwindow.cpp` lambda is decided by the lambda itself.
Assigned by this spec:

- **Terminal-scoped:** every `cmd*` in `remotecontrol_terminal.cpp` except
  `cmdFindSources`, which reads no tab state and moves to the workspace TU; the
  inline `get_cwd`, `get_scrollback`, `get_last_command`, `get_environment`,
  `get_git_status` and `tab_list` lambdas; `token_usage` (it reads the terminal's own counters,
  so its body moves from `remotecontrol_review.cpp` to the GUI side);
  `get_session_info`, dispatched inline by the pipeline.
- **Served by `ants-mcpd`:** `caller_cwd_info`, whose lambda moves to
  `mcptoolregistry.cpp` so it can report `ServerCwd` (§ 2.4).
- **Served by both hosts:** `tool_info`, which the shared pipeline answers.
- **Every other inline lambda** is classified by the same reading when it is
  moved. A lambda that reads no tab or terminal state moves to
  `mcptoolregistry.cpp`.

`token_usage` counts only the calls the terminal served. Calls `ants-mcpd`
serves are not in it, nor in the status-bar savings. Deferred — tracked by
ANTS-5311, whose contract is `docs/specs/ANTS-5311-mcpd-usage-snapshots.md`.

**Both hosts run one `tools/call` pipeline.** It is extracted from
`ClaudeIntegration` into `ants_mcpcore_lib` as-is: the `claude.mcp_enabled`
master gate (ANTS-1901), the `caller_cwd` and TabSpecific gates in ANTS-1415's
order, rate limit, cache, ETag, `fields=`, the response wrap, the off-thread and
`Bulk` workers, and `initialize`. `ants-mcpd` does not write its own. A second
pipeline would drift from the first, and clients bind to its envelope. So
`ants-mcpd` reads `~/.config/ants-terminal/config.json` and refuses
`mcp_disabled` when the integration is switched off.

**The `Required` gate runs unchanged in `ants-mcpd`.** A `Required` verb with no
`caller_cwd` still refuses `caller_cwd_required`. So § 2.4's fallback is
reached only by verbs the gate lets through without one: the `Optional` ones
(`read_spill`, `caller_cwd_info`) and the `ProcessGlobal` ones (`tab_list`,
`token_usage`, `mcp_trace`).

**A forwarded verb runs the gates and nothing after them.** For a
terminal-scoped name, `ants-mcpd`'s pipeline runs the master gate and the
`caller_cwd` and TabSpecific gates. It then hands the whole request to the
forwarder (§ 2.5) and emits the terminal's reply unchanged. It skips rate limit,
cache, ETag, `fields=` and the wrap, because the terminal's own pipeline has
already applied them. The pipeline's inline `get_session_info` branch forwards
in `ants-mcpd`.

**The schema builder moves with the pipeline.** `tools/list` is built in
`ClaudeIntegration`, which both hosts run (§ 2.2), so one builder answers it in
both. This takes up ANTS-1253 § 9's explicitly deferred "auto-generation of
`tools/list` from registry" only so far as *sharing* it; the schema text stays
hand-written, not generated from the registry.

### 2.4 Root resolution without a window

`ants::resolveCallerCwdRoot` (`src/resolvedroot.h`) takes `MainWindow *` and,
on an absent `caller_cwd`, returns `Source::EmptyFallback` = the focused tab's
root. A standalone host has no focused tab.

Introduce a root provider seam:

```cpp
namespace ants {
class RootProvider {                       // implemented by both hosts
public:
    virtual ~RootProvider() = default;
    virtual QString fallbackRoot() const = 0;   // no caller_cwd supplied
    virtual QString fallbackRoadmapPath() const = 0;
    virtual std::optional<int> fallbackTab() const = 0;
    virtual ResolvedRoot::Source fallbackSource() const = 0;
    virtual std::optional<int> tabForCwd(const QString &canonical) const = 0;
};
}
```

**Every `MainWindow` read in a moved TU goes through it.**
`RemoteControl` holds a `RootProvider *`. The resolver takes it in place of
`MainWindow *`. The three forms in § 1 map as follows:

- A focused-terminal cwd fallback (`workspace_search`, `cited_by`, the
  resolver's `EmptyFallback`) → `fallbackRoot()`.
- `roadmap_query`'s `m_main->roadmapPathForRemote()` → `fallbackRoadmapPath()`.
- An `if (!m_main)` refusal → a refusal on a null provider. `ants-mcpd` always
  has one, so these verbs run there.

The `ants::onGuiThread` marshals around those reads move into the GUI host's
provider. `ants-mcpd`'s provider reads nothing on another thread.

- **GUI host:** `fallbackRoot()` returns today's focused-tab answer,
  `fallbackTab()` the focused tab's index, and `fallbackSource()`
  `EmptyFallback`. `fallbackRoadmapPath()` returns `roadmapPathForRemote()`.
  Behaviour unchanged.
- **`ants-mcpd`:** `fallbackRoot()` returns `QDir::currentPath()`, the
  server's own process cwd (§ 2.1). `fallbackTab()` is empty and
  `fallbackSource()` is `ServerCwd`. `fallbackRoadmapPath()` is
  `findRoadmapUnder(fallbackRoot())`, the lookup a `caller_cwd` gets.

A new `Source::ServerCwd` is added to the `ResolvedRoot` enum so the two are
distinguishable in diagnostics. `caller_cwd_info` reports it as
`"source":"ServerCwd"`, the same enum-name form `sourceToString` uses for the
others.

**This is strictly better than today's behaviour for the standalone host.** The
focused-tab guess can silently answer with a *different project*; the process
cwd cannot.

**The hard boundary, and it is a security one.** This fallback applies to
**project-scoped verbs only**. The `CallerCwdContract::TabSpecific` verbs
(`get_text`, `recent_errors`, `last_selection`, `get_scrollback`,
`get_last_command`, `get_environment`, `get_cwd`) keep ANTS-1415's gate
unchanged. With no `caller_cwd`, the gate refuses `tab_or_cwd_required` unless
the verb accepts a `tab` index and an integer `tab` was supplied
(`tabSpecificAcceptsTabIndex`, ANTS-1415 § 4 invariants 2, 6 and 7). `ants-mcpd`
runs that same gate, from the shared pipeline (§ 2.3), before forwarding.
`ants-mcpd` must never synthesise a `caller_cwd` before forwarding: doing so
converts a mandated refusal into a silent cross-tenant read, which is the leak
ANTS-1404/ANTS-1415 closed. INV-7 and INV-8 lock this.

### 2.5 Forwarding the terminal-scoped verbs

`ants-mcpd` registers a forwarder for each name in
`mcp::terminalScopedVerbNames()`. A forwarder opens a fresh connection to the
terminal socket, relays the request verbatim, and relays the reply verbatim.

**Verbatim matters.** The forwarder does not parse, re-wrap, re-cap or
re-sanitise. The terminal's reply is already a complete envelope.

**No terminal running** — the case of a Claude Code session in a plain console:

```json
{ "ok": false, "code": "no_terminal",
  "error": "no Ants Terminal is running",
  "hint": "start Ants Terminal, or pass caller_cwd to a project-scoped verb" }
```

The verbs stay **present in `tools/list`** either way. They are not hidden,
because the client caches that list at connect: hiding them would mean starting
the terminal later did not bring them back without a reconnect, which is a
worse failure than an honest refusal. `no_terminal` joins the taxonomy in
`docs/standards/mcp-error-codes.md`.

**Peer credentials on the second hop.** ANTS-5144 INV-7's accept-time checks
stay on the terminal's listener, unchanged. There the peer is now `ants-mcpd`
rather than the bridge. Both run as the user, so the terminal's same-uid check
still holds.

`ants-mcpd` accepts nothing (§ 2.6), so it has no accept-time checks. As a
client it checks two things before sending a request:

- the picked socket file is owned by its own uid;
- the connected peer's `SO_PEERCRED` uid is its own uid.

A foreign-owned socket is skipped by the picker, as the bridge skips it. A
failed peer check closes the connection. Either way, with no acceptable
terminal left the forwarded verb answers `no_terminal`, and its `error` names
the uid check.

### 2.6 Socket ownership

ANTS-5144 INV-3 means a live path is never taken over: whichever process binds
second gets nothing. So the two servers must not contend for one path.

- The terminal keeps `/tmp/ants-terminal-mcp-<pid>` and keeps exporting
  `ANTS_MCP_SOCKET`. Its call site in `mainwindow.cpp` is **unchanged**, which
  keeps ANTS-5144 INV-8, ANTS-1901 INV-2 and ANTS-1897 INV-14 intact.
- `ants-mcpd` binds **no listening socket at all**. It speaks stdio to the
  client and is a *client* of the terminal's socket.

This is why decision 3's "one door" costs nothing here: the door is stdio, and
the existing socket keeps its single server.

### 2.7 Store writes

`ants-mcpd` opens `~/.local/share/ants-terminal/roadmap.sqlite` and writes it
**directly** — not read-only, not proxying writes to the terminal.

It finds the store through `RoadmapStore::defaultPath()`, which uses
`QStandardPaths::GenericDataLocation`. That location follows `XDG_DATA_HOME`,
so a test sets `XDG_DATA_HOME` in the spawned process's environment. The store
and the lock directory below both move with it. No new flag is added.

The store is already built for this. `RoadmapStore::applyPragmas()` sets
`busy_timeout`, `foreign_keys`, `synchronous = FULL` and `journal_size_limit`
per connection, and `RoadmapStore::enableWal()` carries a measured account of
the one contended statement: switching to WAL takes an EXCLUSIVE lock acquired
below the busy handler, so **only creation contends** — on an existing WAL
store the pragma is a no-op. No new mechanism is needed for the common case.

**What is genuinely new, and this spec owns it.** ANTS-2132 § 2.10 states that
the `roadmap_busy` hold registry is *process-wide* `static` state in
`RemoteControl`, and that a second Ants process sharing the machine-global
store *is not covered*. `ants-mcpd` is that second process. So:

- SQLite's `busy_timeout` and single-writer WAL lock span both hosts. A
  concurrent write is delayed, then applied, never interleaved. That rules out
  corruption.
- It does not cover the hold. A migration reads `ROADMAP.md` outside any store
  transaction and commits its plan later. ANTS-2132 § 2.10 records that a write
  landing between those two steps is silently reverted. The hold is what stops
  it, and a process-wide hold cannot see the other host.

**So the hold becomes cross-process.** It is an advisory `flock` on a per-root
lock file, `roadmap-holds/<sha1 of the canonical root>.lock`, in the store's
directory. Both hosts take and test the same file. The file is opened
close-on-exec. Then the kernel releases the lock when its holder dies, so a
crash leaves no stale hold. Without close-on-exec, a child the host spawns
(`git`, `rg`, `ctest`) inherits the lock and keeps it alive. Both behaviours
were observed with `flock(1)` on this machine. The `roadmap_busy` refusal and
its fields are unchanged.

INV-9 locks the writes and INV-13 locks the hold.

## 3. Invariants

- **INV-1** — `ants-mcpd` links no GUI library. Its transitive link closure
  contains none of `Qt6::Gui`, `Qt6::Widgets`, `Qt6::DBus`. *Test:*
  `tests/features/standalone_mcp_server/` runs `readelf -d` on the built
  `ants-mcpd` and asserts no `NEEDED` entry names `libQt6Gui`, `libQt6Widgets`
  or `libQt6DBus`. The same command on `ants-terminal` lists all three, which
  is the test's positive control. Broken by: adding a source to
  `ants_mcpcore_lib` that pulls in a widget.
- **INV-2** — There is exactly ONE enumeration of the project-scoped verb set.
  `registerToolProvider` is called for a non-terminal verb from
  `src/mcptoolregistry.cpp` and from no other translation unit. *Test:*
  `tests/features/standalone_mcp_server/` runs
  `mcp::registerProjectScopedVerbs` against a recording `ToolSink` and asserts
  none of the recorded names is in `mcp::terminalScopedVerbNames()`. It then
  scrapes `mainwindow.cpp`'s `registerToolProvider("<name>"` calls and asserts
  every name is in that set. Broken by: registering a project-scoped verb in
  `mainwindow.cpp` again.
- **INV-3** — Both hosts answer `tools/list` from the one builder in
  `ClaudeIntegration`, and
  the two lists agree on every shared verb's name and input schema. *Test:*
  `tests/features/standalone_mcp_server/` spawns `ants-mcpd`, sends
  `tools/list`, and compares the reply field-by-field with the GUI pipeline's
  `tools/list` reply built in-process. Broken by: a second schema builder in
  either host.
- **INV-4** — `mcp::terminalScopedVerbNames()` is exactly the terminal-scoped
  set as § 2.3 defines it: the verbs the GUI host registers itself or
  dispatches inline for terminal state. *Test:* the existing
  `tests/features/mcp_dispatch_forward_completeness/` is extended to assert the
  name set equals the handlers registered in `mainwindow.cpp` outside
  `mcp::registerProjectScopedVerbs`, plus `get_session_info`, which the pipeline
  dispatches inline. Broken by: moving a verb between halves and
  updating one side only.
- **INV-5** — With no terminal socket reachable, every call to a verb in
  `mcp::terminalScopedVerbNames()` that passes the pipeline's gates returns
  `{ok:false, code:"no_terminal"}`, and every project-scoped verb still
  succeeds. *Test:* `tests/features/standalone_mcp_server/` runs `ants-mcpd`
  with `ANTS_MCP_SOCKET` pointed at a path with no listener; asserts
  `tab_list` → `no_terminal` and `spec_lint` with a `caller_cwd` → `ok:true`.
  Broken by: a forwarder that hangs or returns a transport error instead of the
  code.
- **INV-6** — On a verb the gate lets through with no `caller_cwd`, `ants-mcpd`
  resolves the root to its own process cwd and reports `Source::ServerCwd`.
  *Test:* same directory; run `ants-mcpd` with cwd set to a fixture project,
  call `caller_cwd_info` with no `caller_cwd`, assert `"source":"ServerCwd"`
  and the fixture's root. Broken by: reinstating a focused-tab guess.
- **INV-7** — On a `CallerCwdContract::TabSpecific` verb with no routing key
  (§ 2.4), `ants-mcpd` refuses `tab_or_cwd_required` and forwards nothing.
  Given an integer `tab` on a verb that accepts one, it forwards. *Test:*
  `tests/features/mcp_tabspecific_contract/` gains `ants-mcpd` cases for every
  TabSpecific verb, on both sides of the `tab` rule. Broken by: applying § 2.4's
  fallback to a TabSpecific verb.
- **INV-8** — `ants-mcpd` never adds, rewrites or synthesises a `caller_cwd`
  key in a request it forwards. *Test:* a recording stub listener captures the
  forwarded bytes; assert the `caller_cwd` key is byte-identical to the one
  received, and absent when it was absent. Broken by: "helpfully" filling it in.
- **INV-9** — Concurrent `roadmap_log` writes from `ants-mcpd` and from a
  terminal against one project neither corrupt the store nor lose a committed
  item. *Test:* `tests/features/standalone_mcp_server/` forks N appenders across
  both hosts against a temp store. It asserts that each host reported at least
  one `ok:true`, that the item count equals the number of `ok:true` calls, and
  that `PRAGMA integrity_check` → `ok`. Broken by: dropping `busy_timeout`, a
  write path that reads outside its transaction, or an `append` that still
  refuses `no_main` in `ants-mcpd`.
- **INV-10** — Rebuilding `ants-mcpd` changes a verb's answer with no terminal
  restart. *Test:* manual recipe, § 6.2 — this is the item's whole purpose and
  the one clause a unit test cannot hold.
- **INV-11** — `ants-mcpd` refuses a terminal socket owned by another uid, and
  a connected peer whose uid differs (§ 2.5). *Test:* both checks are
  functions that take the expected uid. `tests/features/standalone_mcp_server/`
  calls each with a uid other than its own and asserts refusal, then with its
  own and asserts acceptance. Broken by: dropping either check.
- **INV-12** — `mainwindow.cpp`'s `startMcpServer` call site and its
  `qputenv("ANTS_MCP_SOCKET", …)` export are unchanged by this work. *Test:*
  `McpMasterToggle.INV2_StartupGate` and `McpOrientation_Inv14.MainWindowExportsSocket`
  pass unmodified (ANTS-5144 INV-8's own test surface). Broken by: moving the
  bind to `ants-mcpd`.
- **INV-13** — While one host holds a root's migration hold, a roadmap write to
  that root from the other host refuses `roadmap_busy` (§ 2.7). *Test:*
  `tests/features/standalone_mcp_server/` takes the root's lock file in the test
  process, calls `roadmap_log` through `ants-mcpd` and asserts `roadmap_busy`,
  then releases it and asserts `ok:true`. Broken by: a hold that is process-local
  again.

## 4. RAM / build cost

**New build targets:** one static library (`ants_mcpcore_lib`) and one
executable (`ants-mcpd`). No new external dependency — `ants-mcpd` links
`Qt6::Core`, `Qt6::Network`, `Qt6::Sql` only, all already required.

**Build time.** The verb sources move rather than duplicate, so total
compilation is roughly unchanged; `ants-mcpd` adds one small TU and one link.
The link is cheap: no widgets.

**Memory.** `ants-mcpd` is a per-project, per-client process. Budget: **under
40 MiB RSS steady-state**, measured with no request in flight. It holds one
SQLite connection (interactive profile — SQLite's 2 MiB default page cache, per
`RoadmapStore::applyPragmas()`, which gives the larger `kBulkCacheKiB` only to
`Access::Bulk`), the codebase index cache it already builds today, and no
scrollback. The existing caches keep their existing caps; this work introduces
no new unbounded structure.

**Eviction.** None added. The process is short-lived — it exits with the client
session.

## 5. Out of scope

- **Hot-reloading a verb with no client reconnect at all.** The Lua-sandbox
  route is the only one that reaches zero restarts; the user ruled on
  2026-09-21 that it is not also specced. Permanent exclusion for this
  contract, not a deferral — it is a second authoring model.
- **Moving the tab/terminal verbs out of the GUI process.** They are tab and
  terminal state; there is nowhere else for them to live. Permanent exclusion.
- **Generating `tools/list` from the registry.** ANTS-1253 § 9 defers it; this
  spec shares the hand-written builder rather than replacing it. Deferred —
  tracked by ANTS-1253 § 9, which already carries it.
- **Narrowing `ants_core_lib`'s own PUBLIC Widgets surface.** The CMakeLists
  comment above `ants_audit_lib` names this split as the structural
  prerequisite; the narrowing itself is separate work. Deferred — tracked by
  ANTS-5309.
- **Retiring `tools/mcp-bridge.py`.** Kept one release as the fallback (§ 8).
  Deferred — tracked by ANTS-5308.
- **A second Ants *terminal* process serving remote control.** Unchanged by
  this work and already out of scope in ANTS-5144 § 5. Permanent exclusion.

## 6. Tests

### 6.1 Feature test

`tests/features/standalone_mcp_server/`, label `features;fast`. Covers INV-1,
INV-2, INV-3, INV-5, INV-6, INV-8, INV-9, INV-11, INV-13.

Extended existing directories — these gain cases rather than new dirs, because
each already owns the contract being extended:

- `tests/features/mcp_dispatch_forward_completeness/` — INV-4.
- `tests/features/mcp_tabspecific_contract/` — INV-7. Its existing ordering
  assertion is preserved; see § 8.
- `tests/features/shared_socket_listener/` — INV-12, unmodified.

Per the project convention, each new test is verified to **fail against
pre-change source** before the change is restored. The bundle that owns the new
directory is resolved with `build_target_for`, not guessed — a wrong target
builds green and runs the old binary.

### 6.2 Manual recipe — INV-10, the one that matters

1. Launch Ants Terminal. Open a Claude Code session in a tab. Note its pid.
2. Edit a project-scoped verb's reply — e.g. add a field to `spec_lint`'s
   envelope. Do **not** change its input schema.
3. `cmake --build build --target ants-mcpd`
4. In the *same* Claude Code session, start a new MCP connection (a `/mcp`
   reconnect, no terminal restart) and call `spec_lint`.
5. **Expected:** the new field is present. The terminal was never relaunched;
   the session's pid from step 1 is unchanged; sibling sessions in other tabs
   are untouched.

Step 5's pid check is the assertion. Without it the recipe cannot tell a
successful hot path from a relaunch nobody noticed.

## 7. Cross-doc impact

- **`CLAUDE.md`** — the hot-reload section names ANTS-4932 as its structural
  instance; update it to point at this spec and at `ants-mcpd`. The module map
  gains `mcpcore` / `mcpd`.
- **`docs/standards/mcp-error-codes.md`** — add `no_terminal`.
- **`docs/standards/mcp-tools.md`** — a new verb is registered in
  `mcptoolregistry.cpp`, not `mainwindow.cpp`. This is the instruction most
  likely to be followed from memory, so it changes in the same commit.
- **`README.md`** — the MCP setup line changes from `tools/mcp-bridge.py` to
  `ants-mcpd`. `tools/check-readme-claims.sh` runs in the pre-push hook.
- **`tests/features/ci_workflow_deps`** — unchanged. Every carrier builds
  `ants-mcpd` from source, so there is no tool to declare. INV-1's `readelf`
  comes with binutils, which each carrier's compiler already pulls in.
- **Packaging** — the RPM spec, Arch PKGBUILD and Debian control each gain the
  new binary.
- **CHANGELOG** — one entry stating what shipped, not the defect.

## 8. Migration / compatibility

**The clauses this spec amends.** Each is an accepted or shipped contract
that the design contradicts as written. None is silently overridden:

| Clause | What it says | What changes |
|---|---|---|
| ANTS-1253 INV-1 | every tool is registered exactly once **in `MainWindow::setupClaudeMcpProviders`** | the project-scoped verbs move to `mcp::registerProjectScopedVerbs`; "exactly once" is preserved and strengthened by INV-2 |
| ANTS-1253 INV-8 | cross-grep binds the `tools/list` builder to registrations **in `mainwindow.cpp`** | the grep's registration side becomes `mcptoolregistry.cpp` **and** `mainwindow.cpp`, which keeps the terminal-scoped registrations; the binding itself is kept, by INV-3 |
| ANTS-3833 INV-11 | a `remotecontrol` TU is named only in the `ANTS_RC_SOURCES_REL` block | kept. The block now holds two lists, one per library (§ 2.2) |
| ANTS-1642 INV-1 | `mcp_dispatch_forward_completeness` pairs each schema entry in `claudeintegration.cpp` with its `registerToolProvider` lambda in `mainwindow.cpp` | re-pointed at both `mcptoolregistry.cpp` and `mainwindow.cpp`; the schema side stays `claudeintegration.cpp`, where the builder still is. Left on the old files it finds no tools and passes |
| ANTS-2132 § 2.10, the factory scrape | `mcp_verb_offthread_guard` finds the one factory body at `MainWindow::rcDelegate(` | re-pointed at `rcDelegate`'s new home in `mcptoolregistry.cpp` (§ 2.3). Still one definition |
| ANTS-2132 INV-13, INV-20 | scrapes pinned to `src/mainwindow.cpp`'s registration table | re-pointed at `mcptoolregistry.cpp`. INV-20's "exactly one `DispatchLane::Bulk`" is preserved, at the new site |
| ANTS-2132 § 2.10 | the `roadmap_busy` hold registry is process-wide and does **not** cover a second process | the hold becomes a per-root lock file both hosts see (§ 2.7); INV-13 locks it |
| ANTS-1401 INV-2 | `resolveCallerCwdRoot(main, {})` → `EmptyFallback` = focused tab | remains true **for the GUI host**. `ants-mcpd` returns the new `Source::ServerCwd`. § 2.4's `RootProvider` is the seam |
| ANTS-1415 § 4 inv. 2, 6, 7 | TabSpecific verbs refuse with no routing key | **unchanged, and explicitly extended to the new host** by INV-7. The gate runs in `ants-mcpd`, before forwarding |

ANTS-1415 § 4 invariant 3 pins the gate's *order* (after the `Required` branch,
before rate-limit and cache). `ants-mcpd` runs the same three checks in the same
order, so `tests/features/mcp_tabspecific_contract/`'s ordering assertion is
re-used against the new host rather than relaxed.

**Two stale status lines, cited by content not by status** (write-spec Step 1's
rule): `docs/specs/ANTS-1401.md` carries **no `Status:` line at all**, and
`docs/specs/ANTS-1415.md` still reads `spec draft (2026-05-20)` although its
gate is shipped and enforced. Both are cited above for what they say, not for
what their headers claim. Fixing them is not this spec's work.

**Client migration.** One command, once per machine:
`claude mcp remove ants && claude mcp add ants -- <prefix>/bin/ants-mcpd`.
`tools/mcp-bridge.py` is kept for one release and still works against a
terminal that has not been rebuilt, so a half-migrated machine degrades to
today's behaviour rather than breaking.

**Rollback.** Re-register the bridge. Nothing on disk changes format; the store
schema is untouched, so no `kSchemaVersion` bump — which matters, because that
is a one-way door across every project on the machine.

## 9. Decisions taken while drafting

1. **`ants-mcpd` ships in the packages.** The bridge's retirement (ANTS-5308)
   leaves a packaged install with no MCP server otherwise. `install(TARGETS)`
   installs it beside `ants-terminal`, which is all the Arch and Debian
   recipes and the AppImage's AppDir need; the RPM spec names it in `%files`.
2. **The two deferrals in § 5 are filed**: ANTS-5308 (retire the bridge) and
   ANTS-5309 (narrow `ants_core_lib`'s Widgets surface).
3. **One `tools/call` pipeline, extracted and shared** (§ 2.3), rather than a
   second one in `ants-mcpd`. Taken at the review gate.
4. **The migration hold becomes a per-root lock file** (§ 2.7), rather than
   routing every roadmap write through the terminal. It keeps § 2.7's direct
   writes. Taken at the review gate.
5. **`token_usage` and `get_session_info` are terminal-scoped; `tool_info` is
   served by both hosts** (§ 2.3). Taken at the review gate.
6. **The `Required` gate stays in `ants-mcpd`** (§ 2.3), so the server-cwd
   fallback reaches only verbs that gate lets through. Relaxing it would make the two hosts
   answer one call differently. Taken at the review gate.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-24 | 3, every lane held Q1–Q4 | 5 | 7 | 3 | 3 | Verified 18, fixed 18, dismissed 2 (INV-12's test directory; the verb-count label). Largest: the `MainWindow` coupling was undercounted (`!m_main` refusals and resolver reads missed), and the library split could not link. Design choices taken at the gate are recorded in § 9. |
| 2 | 2026-09-24 | 3, every lane held Q1–Q4 | 0 | 5 | 6 | 2 | Verified 13, fixed 13, dismissed 0. Cap reached (2 for a spec); shipped to implementation. Twelve of the thirteen anchored on text loop 1 wrote. The document was new, so that share cannot separate a calm cap from a violent one. Read by substance, they were consequences of loop 1's design choices not yet carried through (the shared pipeline against the forwarder, the `Required` gate against the fallback, the terminal-scoped definition), not repairs of repairs. Deferral filed: ANTS-5311. Disclosure: every lane's git snapshot named loop 1's commit and its finding count. |

<!-- one row per review loop; written as the loops happen, never back-filled -->
