# ANTS-4932 — Serve the project-scoped MCP verbs from a standalone process

**Status:** spec draft (2026-09-22).
**Kind:** refactor.
**Source:** ROADMAP.md ANTS-4932 (user-request-2026-09-07; measurements and
four design decisions recorded on the item 2026-09-21; two further forks
settled by the user 2026-09-22).
**Blocked by:** none.
**Pairs with:** ANTS-1253 (tool-provider registry), ANTS-2132 (off-GUI-thread
dispatch), ANTS-5144 (shared socket listener).
**Supersedes:** no document. It *amends* six clauses named in § 8.

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
   (`grep -c 'rcDelegate(&RemoteControl::' src/mainwindow.cpp` → 74;
   `grep -c registerToolProvider src/mainwindow.cpp` → 94, the difference being
   inline handlers.)
3. **The library carries the GUI.** `ants_core_lib` links `Qt6::Gui`,
   `Qt6::Widgets`, `Qt6::Network`, `Qt6::DBus` and `util` **PUBLIC**, and every
   `remotecontrol_*.cpp` compiles into it.

**What makes this tractable.** The GUI dependency of the whole
`remotecontrol_*` surface is exactly eleven `MainWindow` methods — verified
today, not recalled:

```
grep -rhoE 'm_main->[A-Za-z_]+' src/remotecontrol*.cpp | sort -u | wc -l   → 11
```

`currentTabIndexForRemote`, `currentTerminal`, `newTabForRemote`,
`roadmapPathForRemote`, `selectTabForRemote`, `setTabTitleForRemote`,
`tabListForRemote`, `tabsAsJson`, `terminalAtTab`, `terminalForCaller`,
`tokenSavingsSummary`. All tab or terminal state.

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
                                                              (all ~109 verbs)

SPECIFIED
  Claude Code --stdio--> ants-mcpd --unix socket--> ants-terminal
                         (project-scoped verbs)     (the 11 GUI verbs only)
```

`ants-mcpd` is a new executable. It takes `mcp-bridge.py`'s place in the
client registration; the Python bridge is retained unchanged for one release as
the fallback (§ 8).

Three properties fall out of the client launching it, rather than being
designed:

- **Its process cwd is the project directory**, because that is where the
  client runs. This is what decision 2 below rests on.
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

```cmake
add_library(ants_mcpcore_lib STATIC
    src/remotecontrol_state.cpp     src/remotecontrol_docs.cpp
    src/remotecontrol_review.cpp    src/remotecontrol_coldeyes.cpp
    src/remotecontrol_workspace.cpp src/remotecontrol_feedback.cpp
    src/remotecontrol_roadmap_*.cpp src/remotecontrol_changelog.cpp
    src/mcptoolregistry.cpp)        # § 2.3
target_link_libraries(ants_mcpcore_lib
    PUBLIC  Qt6::Core Qt6::Network ants_roadmapparse_lib
    PRIVATE ants_roadmapstore_lib ants_warnings)

add_executable(ants-mcpd src/mcpdmain.cpp)
target_link_libraries(ants-mcpd PRIVATE ants_mcpcore_lib ants_warnings)
```

`Qt6::Network` stays — it is the socket. `Qt6::Gui`, `Qt6::Widgets` and
`Qt6::DBus` are what must not appear.

`src/remotecontrol_terminal.cpp` stays in `ants_core_lib`: it is the tab-verb
file and remains GUI-side.

**The coupling to sever is the resolver, not the verb bodies.**
`resolveRootCanonical` has 72 call sites across 15 files
(`grep -rho resolveRootCanonical src/ | wc -l` → 80, less 8
declarations/definitions from `grep -rn 'QString resolveRootCanonical' src/`).
*The roadmap item records 63; that figure is stale and this one supersedes it.*
Most references are a `MainWindow *` **type** mention, satisfiable by a forward
declaration — the link edge comes from dereferencing. § 2.4 puts a provider
behind them.

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
void registerProjectScopedVerbs(ToolSink &sink, RemoteControl *rc);

// The verbs that DO require a terminal. The GUI host registers these
// directly; ants-mcpd registers a forwarder for each (§ 2.5).
const QVector<QString> &terminalScopedVerbNames();
}
```

`ToolSink` is an interface with `registerToolProvider()`'s signature, so
`ClaudeIntegration` implements it unchanged and `ants-mcpd` implements its own.
`MainWindow::setupClaudeMcpProviders()` keeps its name and its tab-verb
registrations, and calls `mcp::registerProjectScopedVerbs()` for the rest.

**The schema builder moves with the list**, into `mcp::toolDescriptors()`, so
one function answers `tools/list` in both hosts. This takes up ANTS-1253 § 9's
explicitly deferred "auto-generation of `tools/list` from registry" only so far
as *sharing* it; the schema text stays hand-written, not generated from the
registry. Without this move `ants-mcpd` cannot answer `tools/list` at all.

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
    virtual std::optional<int> tabForCwd(const QString &canonical) const = 0;
};
}
```

- **GUI host:** `fallbackRoot()` returns today's focused-tab answer. Behaviour
  unchanged.
- **`ants-mcpd`:** `fallbackRoot()` returns `QDir::currentPath()` — the
  server's own process cwd, which is the project the client is working in.

A new `Source::ServerCwd` is added to the `ResolvedRoot` enum so the two are
distinguishable in diagnostics; `caller_cwd_info` reports it.

**This is strictly better than today's behaviour for the standalone host.** The
focused-tab guess can silently answer with a *different project*; the process
cwd cannot.

**The hard boundary, and it is a security one.** This fallback applies to
**project-scoped verbs only**. The seven `CallerCwdContract::TabSpecific` verbs
(`get_text`, `recent_errors`, `last_selection`, `get_scrollback`,
`get_last_command`, `get_environment`, `get_cwd`) **still refuse** with `tab_or_cwd_required` on an
absent `caller_cwd`, exactly as ANTS-1415 § 4 invariants 2/5/7 require.
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

**Peer credentials on the second hop.** ANTS-5144 INV-7 requires the
accept-time checks — `SO_PEERCRED` same-uid failing closed, the 5 s idle timer,
the buffer caps — ahead of reading any request. `ants-mcpd` reproduces all
three on its own accept path. On the forward hop the peer is `ants-mcpd`, not
the client; since both run as the same user the uid check still holds, and the
check is not weakened. Stated because it is the obvious place to assume it is.

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

- A project's roadmap writes are serialised **within** a host, not across the
  two.
- The protection that does span both is SQLite's own: `busy_timeout` plus the
  single-writer WAL lock. A concurrent write is delayed, then applied — not
  interleaved.
- The exposure is therefore **lost-update on a read-modify-write**, not
  corruption. `roadmap_log`'s verbs read, mutate and render inside one
  transaction (ANTS-3809), so the window is the transaction, not the session.

INV-9 asserts the outcome that matters: concurrent writes from both hosts
neither corrupt the store nor lose a committed item.

## 3. Invariants

- **INV-1** — `ants-mcpd` links no GUI library. Its transitive link closure
  contains none of `Qt6::Gui`, `Qt6::Widgets`, `Qt6::DBus`. *Test:*
  `tests/features/standalone_mcp_server/` greps the generated
  `build/CMakeFiles/ants-mcpd.dir/link.txt` for those three names → 0 hits.
  Broken by: adding a source to `ants_mcpcore_lib` that pulls in a widget.
- **INV-2** — There is exactly ONE enumeration of the project-scoped verb set.
  `registerToolProvider` is called for a non-terminal verb from
  `src/mcptoolregistry.cpp` and from no other translation unit. *Test:*
  `tests/features/standalone_mcp_server/` asserts
  `grep -rl 'registerToolProvider' src/` lists only `mcptoolregistry.cpp`,
  `claudeintegration.{h,cpp}`, `mainwindow.cpp` — and that every name in
  `mainwindow.cpp`'s remaining calls is in `mcp::terminalScopedVerbNames()`.
  Broken by: registering a project-scoped verb in `mainwindow.cpp` again.
- **INV-3** — Both hosts answer `tools/list` from `mcp::toolDescriptors()`, and
  the two lists agree on every shared verb's name and input schema. *Test:*
  `tests/features/standalone_mcp_server/` builds both descriptor sets in-process
  and compares them field-by-field for the names in
  `mcp::registerProjectScopedVerbs`. Broken by: a second schema builder.
- **INV-4** — `mcp::terminalScopedVerbNames()` is exactly the set of verbs whose
  handler reaches a `MainWindow` method. *Test:* the existing
  `tests/features/mcp_dispatch_forward_completeness/` is extended to assert the
  name set equals the handlers registered in `mainwindow.cpp` outside
  `mcp::registerProjectScopedVerbs`. Broken by: moving a verb between halves and
  updating one side only.
- **INV-5** — With no terminal socket reachable, every verb in
  `mcp::terminalScopedVerbNames()` returns
  `{ok:false, code:"no_terminal"}`, and every project-scoped verb still
  succeeds. *Test:* `tests/features/standalone_mcp_server/` runs `ants-mcpd`
  with `ANTS_MCP_SOCKET` pointed at a path with no listener; asserts
  `tab_list` → `no_terminal` and `spec_lint` → `ok:true`. Broken by: a
  forwarder that hangs or returns a transport error instead of the code.
- **INV-6** — On a project-scoped verb with no `caller_cwd`, `ants-mcpd`
  resolves the root to its own process cwd and reports
  `Source::ServerCwd`. *Test:* same directory; run `ants-mcpd` with cwd set to
  a fixture project, call `roadmap_query` with no `caller_cwd`, assert the
  answer is the fixture's roadmap. Broken by: reinstating a focused-tab guess.
- **INV-7** — On a `CallerCwdContract::TabSpecific` verb with no `caller_cwd`,
  `ants-mcpd` refuses `tab_or_cwd_required` and forwards nothing. *Test:*
  `tests/features/mcp_tabspecific_contract/` gains an `ants-mcpd` case for each
  of the seven verbs. Broken by: applying § 2.4's fallback to a TabSpecific verb.
- **INV-8** — `ants-mcpd` never adds, rewrites or synthesises a `caller_cwd`
  key in a request it forwards. *Test:* a recording stub listener captures the
  forwarded bytes; assert the `caller_cwd` key is byte-identical to the one
  received, and absent when it was absent. Broken by: "helpfully" filling it in.
- **INV-9** — Concurrent `roadmap_log` writes from `ants-mcpd` and from a
  terminal against one project neither corrupt the store nor lose a committed
  item. *Test:* `tests/features/standalone_mcp_server/` forks N appenders across
  both hosts against a temp store, then asserts the item count equals the number
  of calls that reported `ok:true`, and `PRAGMA integrity_check` → `ok`.
  Broken by: dropping `busy_timeout`, or a write path that reads outside its
  transaction.
- **INV-10** — Rebuilding `ants-mcpd` changes a verb's answer with no terminal
  restart. *Test:* manual recipe, § 6.2 — this is the item's whole purpose and
  the one clause a unit test cannot hold.
- **INV-11** — `ants-mcpd`'s connection to the terminal is refused when the
  peer uid differs. *Test:* `tests/features/standalone_mcp_server/` asserts the
  `SO_PEERCRED` check is present on the client path by source-grep of
  `src/mcpdmain.cpp` for `SO_PEERCRED` → at least 1 hit, and the existing
  `tests/features/rc_socket_dir_hardening/` continues to pass unmodified.
  Broken by: reimplementing the picker without ANTS-1322's uid check.
- **INV-12** — `mainwindow.cpp`'s `startMcpServer` call site and its
  `qputenv("ANTS_MCP_SOCKET", …)` export are unchanged by this work. *Test:*
  `McpMasterToggle.INV2_StartupGate` and `McpOrientation_Inv14.MainWindowExportsSocket`
  pass unmodified (ANTS-5144 INV-8's own test surface). Broken by: moving the
  bind to `ants-mcpd`.

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
INV-2, INV-3, INV-5, INV-6, INV-8, INV-9, INV-11.

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
- **`tests/features/ci_workflow_deps`** — only if a packaging carrier ships
  `ants-mcpd`; § 8 says it does.
- **Packaging** — the RPM spec, Arch PKGBUILD and Debian control each gain the
  new binary.
- **CHANGELOG** — one entry stating what shipped, not the defect.

## 8. Migration / compatibility

**The six clauses this spec amends.** Each is an accepted or shipped contract
that the design contradicts as written. None is silently overridden:

| Clause | What it says | What changes |
|---|---|---|
| ANTS-1253 INV-1 | every tool is registered exactly once **in `MainWindow::setupClaudeMcpProviders`** | the project-scoped verbs move to `mcp::registerProjectScopedVerbs`; "exactly once" is preserved and strengthened by INV-2 |
| ANTS-1253 INV-8 | cross-grep binds the `tools/list` builder to registrations **in `mainwindow.cpp`** | the grep's registration side becomes `mcptoolregistry.cpp`; the binding itself is kept, by INV-3 |
| ANTS-2132 INV-13, INV-20 | scrapes pinned to `src/mainwindow.cpp`'s registration table | re-pointed at `mcptoolregistry.cpp`. INV-20's "exactly one `DispatchLane::Bulk`" is preserved, at the new site |
| ANTS-2132 § 2.10 | the `roadmap_busy` hold registry is process-wide and does **not** cover a second process | unchanged and now load-bearing; § 2.7 states the residual exposure and INV-9 bounds it |
| ANTS-1401 INV-2 | `resolveCallerCwdRoot(main, {})` → `EmptyFallback` = focused tab | remains true **for the GUI host**. `ants-mcpd` returns the new `Source::ServerCwd`. § 2.4's `RootProvider` is the seam |
| ANTS-1415 § 4 inv. 2/5/7 | TabSpecific verbs refuse on absent `caller_cwd` | **unchanged, and explicitly extended to the new host** by INV-7. The gate runs in `ants-mcpd`, before forwarding |

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
   leaves a packaged install with no MCP server otherwise. So the AppImage and
   the three distro recipes each gain the binary, and
   `tests/features/ci_workflow_deps` learns about it where a carrier runs it.
2. **The two deferrals in § 5 are filed**: ANTS-5308 (retire the bridge) and
   ANTS-5309 (narrow `ants_core_lib`'s Widgets surface).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|

<!-- one row per review loop; written as the loops happen, never back-filled -->
