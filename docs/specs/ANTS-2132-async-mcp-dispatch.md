# ANTS-2132 — Dispatch MCP verbs off the GUI thread

**Status:** spec draft (2026-09-13). § 1.2, § 2.7 and § 2.8 amend the design
accepted on 2026-08-26 and await review. The rest is implemented
(`ClaudeIntegration::postToolDispatch`, `tests/features/mcp_async_dispatch/`).
**Kind:** perf.
**Source:** ROADMAP.md ANTS-2132 (user report of intermittent whole-window
freeze; diagnosed in-session 2026-08-25). Amended for ANTS-5051, ANTS-5073 and
ANTS-5035 (code-quality-review-2026-09-11 perf pass).
**Pairs with:** ANTS-4681 (`roadmap_log` render + write cost — the largest
repeatable contributor to the per-verb duration this spec stops blocking on).
**Supersedes:** the GUI-responsiveness claim in
`tests/features/mcp_verb_offthread_guard/spec.md` § Background (see § 7).

**Layman:** While Claude runs a command against Ants, the whole Ants window
freezes until that command finishes. This moves the work onto a background
thread so the window keeps painting.

## 1. Problem

`ClaudeIntegration::onMcpConnection()` serves every MCP request on the thread
that owns the widgets. Its `readyRead` handler calls the registered tool
handler inline and then, on the same thread and without returning to the event
loop, runs the response pipeline and writes the reply. Nothing repaints for the
verb's duration.

Both delegate factories in `MainWindow::setupClaudeMcpProviders()` block it:

- `rcDelegate` calls `(m_remoteControl->*fn)(args)` directly on the dispatching
  thread.
- `rcDelegateWorker` (ANTS-2131) runs the same call on a `QThread::create`
  worker and then `worker->wait()`s. **A join blocks the calling thread for the
  full duration.** Its own comment claims it buys GUI-responsiveness; it does
  not. What it buys is the absence of an event pump, which is the ANTS-2131
  use-after-free fix and is unrelated.

Three consequences:

1. **Every verb freezes the window for as long as it runs**, not merely the
   two the ROADMAP headline names. Measured over one ~20-minute window
   (`token_usage`, 2026-08-25): `workspace_search` max 616 ms, `roadmap_log`
   max 256 ms / mean 137 ms over 7 of 7 calls, `roadmap_query` max 37 ms.
2. **Concurrent sessions multiply it.** Every session's verbs serialise
   through the one GUI thread, so a queue of six 200 ms calls is a 1.2 s
   freeze. The user was running eight sessions against one instance on the day
   of the report, against a usual three or four — which is why the freezes had
   no pattern they could identify: they were waiting on another session's call.
3. **The user confirmed other desktop windows stay responsive** during a
   freeze, which rules out machine-level causes and places it on this process's
   own GUI thread.

### 1.1 What protects off-thread `MainWindow` access today

**Not the join.** `RemoteControl::cmdWorkspaceSearch` and
`RemoteControl::cmdCitedBy` already run on a worker and do call
`MainWindow::currentTerminal()` — but only in a `caller_cwd`-absent fallback
their `Required` contract makes unreachable, since the dispatcher refuses a
`Required` verb with no `caller_cwd`. Their registration comments say so.

**Widening the off-thread set breaks that accident.**
`ants::resolveCallerCwdRoot` walks `MainWindow::tabCount()` and
`terminalAtTab()` on the branch where `caller_cwd` *is* supplied, so no contract
makes it unreachable — and `feedback_query` is `Required`, `rcDelegate`, and
reaches it. § 2.4's eligibility rule does not test for `Required`, so § 2.5's
marshalling is what carries the guarantee.

### 1.2 What the first cut left on the GUI thread

*Amendment, 2026-09-13.*

**The remote-control socket.** `RemoteControl::onNewConnection`'s `readyRead`
handler calls `RemoteControl::dispatch` inline and writes its return value.
These routes call the same `RemoteControl` `cmd*` that an off-thread MCP twin
calls: `roadmap-query`, `workspace-search`, `file-outline`, `find-definition`,
`find-caller`, `similar-code`, `git-state` and `subsystem`. That causes two
defects:

- A socket search, tree walk or `git` fork freezes the window for its whole run
  (ANTS-5073).
- `RemoteControl::cmdRoadmapQuery` reaches `RemoteControl::roadmapStoreOrNull`
  and the `m_roadmap*` cache members from both threads (ANTS-5051). The store's
  `QSqlDatabase` may be used only on the thread that opened it. Neither the
  store pointer nor the caches are locked. Once § 2.1 stopped the GUI thread
  waiting on the worker, the two calls could overlap.

**The socket route is the only GUI-thread path to that store.** Checked
2026-09-13 with `find_caller` on `roadmapStoreOrNull`, `roadmapBullets` and
`cmdRoadmapQuery`:

- Every caller sits in a `RemoteControl` `cmd*`, or in a helper only those
  reach.
- Each of those `cmd*` is registered `Required` through `rcDelegate`, so it runs
  off-thread under § 2.4.
- `mainwindow.cpp` calls `m_remoteControl->` directly only for `cmdTabList`,
  `cmdGetText`, `cmdIndieReviewDispatch` and `cmdTokenUsage`. None reaches the
  store.
- `RoadmapDialog` calls its own `roadmapBullets` over its own store.
- `session_orient`'s pre-warm thread runs only `FindSources::prewarm`.

So moving the socket route closes the race. A future GUI-thread path to the
store would reopen it; § 5 says why no runtime guard is added.

**`audit_run`.** Its synchronous branch starts a `QThread` and calls
`worker->wait()` on the GUI thread for the whole sweep (ANTS-5035). Every
off-thread reply is written from the GUI thread, so MCP traffic stalls with the
window. `indie_review_dispatch` joins the same way and stays out of scope
(ANTS-3515).

## 2. Surface

### 2.1 Shape of the change

The GUI thread stops *executing* verbs and stops *waiting* for them. It accepts
the request, hands the job to one long-lived worker, and returns to the event
loop. When the worker finishes, it posts the result back and the GUI thread
resumes the existing response pipeline unchanged.

**One worker, not one per call.** Off-thread verbs then execute one at a time
in arrival order — exactly today's serialisation — so **no pair of off-thread
verbs begins to overlap**. Client-visible latency is unchanged; only the
window's responsiveness changes. A thread per call would additionally alter
concurrency for every verb at once and is rejected in § 5.

**That guarantee covers the off-thread set and not the whole surface.** A
synchronous inline verb still runs on the GUI thread, so it *can* now overlap
an off-thread verb — a pair that cannot overlap today. § 5 states the hazard
and names the item that closes it; do not read this paragraph as licence to
leave shared state unguarded.

### 2.2 Splitting the request handler

`onMcpConnection()`'s `readyRead` lambda currently runs to completion in one
pass. Everything from the tool-handler call to the socket write becomes a
continuation that can be invoked later.

```cpp
// claudeintegration.h — captured state a deferred reply needs.
struct McpCallContext {
    QPointer<QLocalSocket> socket;
    QJsonValue   requestId;
    QString      toolName;
    QJsonObject  args;
    qint64       requestBytes = 0;
    bool         cachedHit    = false;
    bool         cacheable    = false;
    QString      dispatchResult;
    QElapsedTimer traceTimer;
};
```

The continuation is a private member:

```cpp
void finishToolDispatch(const McpCallContext &ctx, QString responseText);
```

It contains, verbatim and in the current order, the existing post-handler
pipeline: the ANTS-2175 ignored-args advisory, the ANTS-1357 idempotent-read
cache insert, the ANTS-1499 ETag short-circuit, `mcp::projectFields`,
`mcp::compactEnvelope`, `mcp::appendReadHints`, `mcp::tabularize`,
`mcp::offloadBody` with its ANTS-4626 re-apply, the ANTS-1294 wrap, and
`recordDispatch`. It then writes the JSON-RPC envelope with its ANTS-1769
newline terminator and disconnects.

Synchronous dispatch calls it inline. Deferred dispatch calls it from a queued
slot on the GUI thread. **Neither path may have its own copy of the pipeline.**

### 2.3 The worker

One `QThread` owned by `ClaudeIntegration`, started lazily on the first
off-thread dispatch, running a plain event loop. A job is posted with
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; the worker runs the
handler and posts `finishToolDispatch` back the same way.

The worker never touches `QLocalSocket`. The socket is written only from the
GUI thread, in the continuation, guarded by the existing `QPointer`.

### 2.4 Which verbs are eligible

Eligibility is decided at registration, from two facts the registration site
already carries. **No registration's contract argument or handler shape
changes**, so the scrape tests that match `rcDelegate(` keep matching
untouched. Every `rcDelegateWorker(&RemoteControl::…)` site is the one
exception: this section deletes that factory, so each is retyped to
`rcDelegate(`. § 6 names every test that binds to the deleted spelling.

The factory returns a marked type instead of a bare `ToolHandler`. **It is
declared in `claudeintegration.h`, not in `mainwindow.cpp`** — a
`ClaudeIntegration::registerToolProvider` overload takes it, so the type must
be visible in that header — and it is directly constructible, so a test can
register an off-thread handler without going through an rc factory (§ 6):

```cpp
// claudeintegration.h
struct RcHandler {
    ToolHandler fn;
    bool offThreadEligible = true;
};
```

`registerToolProvider` gains an overload taking `RcHandler` and sets:

```
offThread = handler.offThreadEligible && contract != CallerCwdContract::TabSpecific
```

The bare-`ToolHandler` overload sets `offThread = false`. Two facts justify each half:

- **Built by an rc factory** means the handler body is the ANTS-1427
  lambda-enter wrapper around `(m_remoteControl->*fn)(args)`, and nothing else.
  `ClaudeIntegration::registerToolProvider` re-wraps *every* handler in that
  `ANTS_LOG(DebugLog::Claude, "mcp lambda-enter …")` lambda, so the wrapper runs
  on whichever thread dispatches. That is safe off-thread: `DebugLog::write`
  takes a `std::lock_guard` on a static `std::mutex` before touching the file.
  An inline lambda, by contrast, captures `MainWindow` and may touch widgets.
- **Not `TabSpecific`** excludes the verbs that read live terminal state.
  `RemoteControl::cmdLastSelection` resolves a `TerminalWidget` through
  `MainWindow::terminalForCaller()` and reads `selectedText()`; it must stay on
  the GUI thread.

Counted from `src/mainwindow.cpp` with
`awk '/registerToolProvider\("/{getline c1; getline c2; ...}' | sort | uniq -c`
(2026-08-26): 92 registrations — 63 `rcDelegate` non-TabSpecific, 8
`rcDelegateWorker`, 2 `rcDelegate` TabSpecific, 14 inline non-TabSpecific, 5
inline TabSpecific.

So **every rc-factory verb outside the TabSpecific pair becomes off-thread**,
and that set contains every verb the § 1 measurements name. The TabSpecific
verbs stay synchronous, and so does an inline handler unless it registers
through `RcHandler{` (§ 5).

`rcDelegateWorker` is deleted. Under this design it would be
byte-for-byte `rcDelegate`, and two factories doing one thing is the
duplication `coding.md` § 1.3 forbids. Its verbs move to `rcDelegate`.

### 2.5 MainWindow access from an off-thread verb

These symbols outside `src/remotecontrol_terminal.cpp` reach into `MainWindow`
from a verb body. **Enumerate from source, never from this table** —
`grep -nE '\b(m_main|main)->[A-Za-z_]+' src/remotecontrol*.cpp`, excluding the
terminal file (2026-08-26):

| Symbol | File | What it reads |
|---|---|---|
| `RemoteControl::cmdWorkspaceSearch` | `remotecontrol_workspace.cpp` | `currentTerminal()` |
| `RemoteControl::cmdCitedBy` | `remotecontrol_workspace.cpp` | `currentTerminal()` |
| `RemoteControl::cmdRoadmapQuery` | `remotecontrol_roadmap_query.cpp` | `roadmapPathForRemote()` |
| `RemoteControl::cmdTokenUsage` | `remotecontrol_review.cpp` | `tokenSavingsSummary()` |
| `resolveRootCanonical` | `remotecontrol_feedback.cpp` | `currentTerminal()` |
| `resolveCallerCwdRoot` | `remotecontrol_feedback.cpp` | `focusedTerminal()`, `currentTabIndexForRemote()`, `tabCount()`, `terminalAtTab()` |

A narrower pattern misses sites: one matching only `currentTerminal` and
`terminalAtTab` drops `focusedTerminal()` and `tabCount()`, both of which
`resolveCallerCwdRoot` calls. Match the arrow, not the accessor names.

Each is wrapped in a marshalling helper. **It is a free function template in a
shared header (`namespace ants`, `src/resolvedroot.h` or a new small header),
not a `RemoteControl` member** — two of the six symbols above are free
functions with no `RemoteControl` instance to call a member on:
`resolveRootCanonical` is declared in `src/remotecontrol_internal.h`, and
`ants::resolveCallerCwdRoot` in `src/resolvedroot.h`. Between them they carry most of the reach-back sites, so a member helper
would leave INV-6 unsatisfiable.

```cpp
// namespace ants — runs f on the GUI thread. Direct call when already there;
// a blocking queued invocation otherwise. nullopt means the dispatcher is
// shutting down and refused the marshal (§ 2.6).
template <class F>
auto onGuiThread(F &&f) -> std::optional<std::invoke_result_t<F>>;
```

**The refusal channel is part of the contract, not an implementation detail.**
Nine call sites in four files bind to this signature, and § 2.6 requires a
refusal at shutdown. Without `nullopt` a refused marshal returns a
default-constructed value, so `resolveCallerCwdRoot` yields an empty root and
`cmdRoadmapQuery` an empty path — the verb answers with a silently wrong
project instead of refusing. **On `nullopt` a call site refuses with its
existing anchor-failure code; it never falls back to a default.**

`Qt::BlockingQueuedConnection` cannot deadlock **during dispatch**, because the
GUI thread never waits on the worker while serving a request (§ 2.1) — that is
INV-7, and it is the property the join gave away for free and this design has
to state. Shutdown is the one exception and § 2.6 owns it.

**The exemption is per verb body, never per file.**
`src/remotecontrol_terminal.cpp` holds the TabSpecific bodies *and*
`cmdFindSources` (`Required`, `rcDelegate`) and the
`rcdetail::cmdRoadmapLogPass*` helpers `roadmap_log` calls — all off-thread
under § 2.4. Exempt only bodies that always run on the GUI thread: those
registered `TabSpecific`, plus that file's `--remote` CLI verbs, which are not
MCP-registered at all. **Derive that set from the registration table rather
than hard-coding a list**, or it rots the first time a verb moves.

### 2.6 Socket lifetime, backpressure and shutdown

- The peer may disconnect while a job is queued. The continuation's existing
  `QPointer` guard and `ConnectedState` check already cover this; deferral only
  widens the window.
- The 5 s slow-loris idle timer is already stopped before dispatch, so a
  long-running verb cannot be aborted by it.
- **Queue cap.** The queue holds at most 64 jobs, **counting the one currently
  executing**. So the 65th concurrently-outstanding job is the first refused,
  and it is refused synchronously with `code: "dispatch_queue_full"` and a
  `retry_after_ms` hint, mirroring `audit_run`'s `already_running` shape. The
  cap exists so a wedged verb cannot grow the queue without bound. Counting the
  in-flight job is stated because otherwise the boundary is off by one and
  INV-10's fixture cannot say what it expects.
- **Shutdown — the one place the GUI thread does wait on the worker.**
  `ClaudeIntegration`'s destructor stops accepting jobs, refuses any in-flight
  `onGuiThread` marshal rather than serving it, then quits the worker's event
  loop and joins it. Refusing the marshal first is what keeps the join from
  deadlocking against a worker parked in a `BlockingQueuedConnection`. A result
  arriving after shutdown has begun is dropped, not written. INV-7 is scoped to
  the dispatch path for exactly this reason.

### 2.7 Socket routes on the dispatch worker

*Amendment, 2026-09-13 — ANTS-5051, ANTS-5073.*

**Which routes.** A `RemoteControl::dispatch` route runs on the worker when its
MCP twin is off-thread under § 2.4: a handler over the same `cmd*`, registered
through the `RcHandler` overload with a contract other than `TabSpecific`. That
is § 1.2's list. These stay inline:

- `get-text`, whose twin is `TabSpecific`.
- `tab-list`, whose twin registers through the bare `ToolHandler` overload.
- Every route with no MCP twin.

`RemoteControl` cannot read `ClaudeIntegration`'s registry, because
`ants_core_lib` sits below the Claude library. So the set is written out once,
in `static bool RemoteControl::routeRunsOnDispatchWorker(const QString &cmd)`,
and INV-13 keeps it equal to the rule.

**The worker entry.** `ClaudeIntegration` gains a public method:

```cpp
// Runs job on the § 2.3 worker, behind any queued MCP job. Counts against the
// § 2.6 cap until job returns. Returns false, and job never runs, when the
// queue is full or shutdown has begun.
bool postWorkerJob(std::function<void()> job);
```

MCP and socket jobs share one queue and one cap, so INV-2 and INV-10 hold across
both.

**The hook.** `RemoteControl` gains
`void setDispatchWorkerPoster(std::function<bool(std::function<void()>)> poster)`.
`MainWindow` installs it after `m_remoteControl = new RemoteControl(` and before
`m_remoteControl->start()`, forwarding to `m_claudeIntegration->postWorkerJob`.
With no poster installed, every route runs inline as it does today.

**The reply.** The `readyRead` handler keeps its parse, its `_handled` latch and
its idle-timer stop. For a worker route with a poster installed it then:

1. Posts a job that runs `dispatch()` on the worker and queues the write back
   with `QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)`.
2. On the GUI thread, runs the existing `QPointer` and `ConnectedState` check,
   then writes, flushes and disconnects, as the inline path does.

The job carries the `QPointer` but never tests or dereferences it on the worker.
If the poster returns false, the handler replies at once with
`{ok:false, code:"dispatch_queue_full", error, retry_after_ms}`: § 2.6's refusal
without the JSON-RPC envelope.

**Lifetime.** `MainWindow`'s constructor calls `setupStatusBarChrome`, which
creates `ClaudeIntegration`, before it creates `RemoteControl`. Both are children
of `MainWindow`, and `QObject` deletes children in the order they were added. So
§ 2.6's join finishes before `RemoteControl` is freed. A write still queued to a
freed `RemoteControl` is discarded by `~QObject`.

**What moves with it.** A socket request may omit `caller_cwd`, because the
`Required` refusal is an MCP-dispatcher step. So the `currentTerminal()`
fallbacks § 1.1 calls unreachable are reachable from the socket, and now run on
the worker. INV-6 already requires them marshalled, reachable or not.

### 2.8 `audit_run` replies later

*Amendment, 2026-09-13 — ANTS-5035.*

A sweep runs for minutes. On the shared worker it would hold every session's MCP
traffic for its whole run. So `audit_run` keeps its own `QThread`, and the GUI
thread stops joining it.

**Handler type.** `claudeintegration.h` gains:

```cpp
// reply must be called exactly once, on the GUI thread, on every path.
using DeferredToolHandler =
    std::function<void(const QJsonObject &args,
                       std::function<void(QString)> reply)>;
void registerToolProvider(const QString &name,
                          CallerCwdContract contract,
                          DeferredToolHandler handler);
```

A deferred handler runs on the GUI thread, like a bare `ToolHandler`, and must
return promptly. The dispatcher builds the `McpCallContext` as its off-thread
branch does, `toolHandled` included, and passes a `reply` that calls
`finishToolDispatch` with it. A second call to `reply` writes
nothing. A deferred verb does not count against the § 2.6 cap.

**`audit_run`.** It registers through that overload. Its refusals and its
`async:true` branch call `reply` before returning. The synchronous branch starts
the sweep's `QThread`, connects `QThread::finished` to a queued slot whose
context object is `ClaudeIntegration`, and returns. That slot builds the
envelope the synchronous branch builds today, releases the in-flight slot and
calls `reply`. The response shape does not change. What changes is timing: a
second synchronous call for the same root now arrives while the first runs, and
is refused `already_running`, as a call during an `async:true` job already is.

**Shutdown.** A sweep still running when `ClaudeIntegration` is destroyed gets
no reply, because the context object severs the connection. The `async:true`
branch already works this way.

## 3. Invariants

- **INV-1** — For a verb registered off-thread, the GUI thread processes at
  least one event between the request arriving and the reply being written.
  *Test:* `tests/features/mcp_async_dispatch/` — register a test verb that
  sleeps, assert a GUI-thread timer fires during it.
- **INV-2** — Off-thread verbs execute one at a time, in arrival order.
  *Test:* `tests/features/mcp_async_dispatch/` — dispatch three verbs that
  record entry and exit timestamps; assert no two intervals overlap and the
  order matches arrival.
- **INV-3** — The **dispatch path** runs no nested `QEventLoop` on the GUI
  thread (ANTS-2131 preserved). *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape of
  `onMcpConnection()` and the `rcDelegate` factory, which § 2.4 leaves as the
  only one. The two verbs that ever spun such a loop, `audit_run` and
  `indie_review_dispatch`, stay inline (§ 5) and are locked by
  `tests/features/socket_readyread_uaf_guard/` (ANTS-2102), not by this one —
  do not word this as covering them.
- **INV-4** — A handler registered with `CallerCwdContract::TabSpecific` is
  never dispatched off the GUI thread. *Test:*
  `tests/features/mcp_async_dispatch/` — assert `offThread == false` for every
  TabSpecific registration.
- **INV-5** — A handler registered through the bare-`ToolHandler` overload is
  never dispatched off the GUI thread. *Test:* as INV-4, over the inline
  registrations in `mainwindow.cpp`. Worded on the overload, not on factory
  provenance, because § 2.4 lets a test construct an `RcHandler` directly.
- **INV-6** — Outside `src/remotecontrol_terminal.cpp`, no verb body reaches
  `MainWindow` except through `onGuiThread`. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape matching
  `\b(m_main|main)->[A-Za-z_]+` across `src/remotecontrol*.cpp` **including the
  terminal file**; every hit must be inside an `onGuiThread(` call or inside a
  body § 2.5 exempts. The scrape matches the arrow, not a list of accessor
  names, so a new `MainWindow` accessor cannot be added without the test
  noticing.
- **INV-7** — The GUI thread never blocks on the worker **while serving a
  request**. The destructor's shutdown join (§ 2.6) is the sole exception and
  runs only after job acceptance has stopped. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape asserting that the
  only `wait()` on the worker in `claudeintegration.cpp` is lexically inside
  `~ClaudeIntegration`, and that `finishToolDispatch` and the dispatch path
  contain none.
- **INV-8** — Exactly one JSON-RPC reply is written per request carrying an
  `id`, and none is written after the socket is gone or after shutdown has
  begun. *Test:* `tests/features/mcp_async_dispatch/`, two clauses — (a)
  disconnect mid-verb, assert no write and no crash under the `debug` (ASan)
  preset; (b) destroy `ClaudeIntegration` with a job in flight, assert the
  destructor returns within a bounded time and wrote no reply. Clause (b) is
  the only thing that can catch § 2.6's refuse-then-join ordering: INV-7's
  scrape sees where `wait()` sits, never whether it deadlocks.
- **INV-9** — The response pipeline of § 2.2 exists in exactly one place and
  runs identically on the synchronous and deferred paths. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape asserting one
  definition of `finishToolDispatch` and no second `wrapMcpData(` call site in
  the dispatch path.
- **INV-10** — A job arriving at a full queue is refused with
  `dispatch_queue_full` and no job is dropped silently. *Test:*
  `tests/features/mcp_async_dispatch/` — hold the worker on job 1, post jobs
  until one is refused, and assert the refusal is the 65th (the cap counts the
  in-flight job, § 2.6) and that the first 64 all complete.
- **INV-11** — an inline handler registered off-thread reads no `MainWindow`
  member except through `ants::onGuiThread` (ANTS-4682). Owned by
  `tests/features/mcp_verb_offthread_guard/spec.md`; recorded here so the number
  is not reused. *Test:* `tests/features/mcp_verb_offthread_guard/`.

*Amendment, 2026-09-13:*

- **INV-12** — With a poster installed, a socket route for which
  `routeRunsOnDispatchWorker` is true runs on the dispatch worker, and the GUI
  thread processes events while it runs. *Test:*
  `tests/features/mcp_async_dispatch/` — a bare `RemoteControl` listening on an
  `ANTS_REMOTE_SOCKET` path in a temporary directory, whose poster wraps
  `postWorkerJob`, recording the job's thread and sleeping before running it.
  Send `git-state` over the socket. Assert a reply, a job thread other than
  the GUI thread, and heartbeat ticks during the sleep. Breaks if the route runs
  inline: the wrapper never runs and no tick lands.
- **INV-13** — `routeRunsOnDispatchWorker` is true exactly for the `dispatch()`
  routes whose `cmd*` is called by a handler registered in `mainwindow.cpp`
  through the `RcHandler` overload — `rcDelegate(&RemoteControl::…)` or a
  `ClaudeIntegration::RcHandler{` lambda — with a contract other than
  `TabSpecific`.
  *Test:* `tests/features/mcp_verb_offthread_guard/` — source scrape of
  `RemoteControl::dispatch`, `routeRunsOnDispatchWorker` and the registration
  table. Breaks when a route or a registration changes on one side only.
- **INV-14** — A socket request to a worker route that finds the queue full is
  refused at once with `dispatch_queue_full`. *Test:*
  `tests/features/mcp_async_dispatch/` — hold the worker and fill the cap with
  MCP jobs, send `git-state` over the socket, and assert the refusal arrives
  before the held job is released. Breaks if the poster's `false` is ignored:
  no reply arrives.
- **INV-15** — A `DeferredToolHandler`'s first `reply` writes exactly one
  JSON-RPC reply through `finishToolDispatch`, and the GUI thread is not blocked
  before it. *Test:* `tests/features/mcp_async_dispatch/` — register a deferred
  verb that calls `reply` twice from a single-shot timer. Assert heartbeat ticks
  during the wait, one reply line, and `mcpTraceSizeForTest()` grown by exactly
  one. Breaks if the dispatcher waits for the handler, or if `reply` is not
  latched: the reply-line count cannot show that, because the first reply
  disconnects, but a second `recordDispatch` adds a second trace entry.
- **INV-16** — `audit_run`'s synchronous branch never joins its worker on the
  GUI thread. *Test:* `tests/features/mcp_audit_run_async/` — `Inv1SyncPathUnchanged`
  rewritten to scrape the `audit_run` registration body, asserting no `wait()`
  and a `QThread::finished` connection in each of its two branches. Breaks if
  the join returns, or if the synchronous branch connects no completion slot.
  The
  whole-file scrape it replaces stays green on `indie_review_dispatch`'s join
  whatever `audit_run` does.

## 4. RAM / build cost

One additional `QThread` for the process lifetime — a default OS stack
reservation (8 MiB virtual on Linux, resident far lower), started lazily so an
instance that never serves an MCP call never pays it.

The pending-job queue is capped at 64 (§ 2.6). Each entry holds an
`McpCallContext` and the call's `QJsonObject` args — the args are already
capped at the existing 256 KiB request limit, so the queue's worst case is
bounded at roughly 16 MiB and its realistic case is a few kilobytes.

No new build target, no new external library. `finishToolDispatch` is moved
code, not added code.

The amendment adds no thread and no queue. A queued socket job holds one parsed
request, already bounded by the socket's receive cap. A synchronous `audit_run`
holds its `RunResult` until the reply, as the `async:true` branch does.

## 5. Out of scope

- **Some inline verbs stay synchronous.** ANTS-4682 audited the inline handlers
  after the first cut and moved the GUI-free ones off-thread: the `test_audit_*`
  verbs, `project_query` and `caller_cwd_info`. The rest stay, each verdict
  recorded at its registration.

  **`indie_review_dispatch` still joins its worker on the GUI thread**, so the
  window freezes for a review (ANTS-3515). `get_git_status` still blocks it,
  because it has no refusal channel (ANTS-4686). § 2.8 fixes `audit_run` only.

  **The residual hazard is stated rather than hidden:** a synchronous inline
  verb that reads project files can run on the GUI thread while an off-thread
  verb writes the same tree. ANTS-4686 carries it for `get_git_status`.
- **A second worker for socket requests** — rejected, for the reason a thread
  per verb is. The socket's worker routes call the same `cmd*` as their MCP
  twins, so they must serialise with them.
- **One store connection per thread, with locked caches** — rejected. It
  changes ANTS-3809 § 4's connection rule and still needs every cache guarded.
  Routing keeps `RemoteControl`'s store on one thread.
- **A runtime thread check in `roadmapStoreOrNull`** — not added. The store
  pointer it would read is itself unguarded, so the check races with what it
  checks. § 1.2's census is the evidence instead.
- **`audit_run` on the shared worker** — rejected. A sweep would hold every
  session's MCP traffic for its whole run.
- **`async:true` by default for `audit_run`** — rejected (ANTS-5035). Every
  caller would get a job id to poll instead of results.
- **The `--remote` client's read wait.** `RemoteControl::runClient` gives up
  when the first byte of a reply is later than its per-read wait, and queue
  time now counts toward that. Tracked by ANTS-5138.
- **Per-verb cost.** Making `roadmap_log` cheaper is ANTS-4681. This spec stops
  the GUI *waiting* for a verb; it makes no verb faster.
- **A thread per verb** — rejected. It would let verbs that cannot currently
  overlap run concurrently against the roadmap store, the caches and the
  project tree, which is a much larger change than the freeze warrants. The
  serialised worker delivers the whole of the reported symptom.
- **Moving the MCP server itself onto a worker thread** — rejected. It would
  require marshalling every `ClaudeIntegration` state read, where this design
  marshals only the reach-back sites § 2.5 enumerates.
- **Pumping the event loop during a verb** — forbidden, permanently. That is
  the nested-loop socket use-after-free class ANTS-2131 closed. Not deferred
  work; a boundary. No id.

## 6. Tests

New feature test `tests/features/mcp_async_dispatch/`, label `features;fast`,
covering INV-1, INV-2, INV-4, INV-5, INV-8 and INV-10. INV-8's disconnect case
also runs under `ctest --preset=debug` (ASan) because its failure mode is a
use-after-free rather than a wrong value.

**Those six invariants need a headless `ClaudeIntegration` harness, and none
exists.** Every current test naming that class is a source scrape over
`SRC_*_PATH` via `slurpFile`; not one constructs the object. So building the
harness — construct a `ClaudeIntegration` with no `MainWindow`, register a
directly-constructed `RcHandler` (§ 2.4), drive a request through the
dispatcher — is part of this work rather than a pattern to copy.

**If the harness proves infeasible, INV-4 and INV-5 may be re-cut as source
scrapes — both are structural. INV-1, INV-2, INV-8 and INV-10 may not.** Each
is a runtime observation no scrape can falsify: a grep for
`Qt::QueuedConnection` passes against code that still joins. Record those as
unverified rather than scraping them, which would leave them reading as covered
while testing nothing.

`tests/features/mcp_verb_offthread_guard/` is rewritten for INV-3, INV-6, INV-7
and INV-9. Every one of its current invariants asserts that the `rcDelegateWorker`
factory exists and that named verbs register through it; § 2.4 deletes that
factory, so they are replaced rather than amended, and its `spec.md` loses the
GUI-responsiveness claim § 1 disproves.

**Three other tests bind to the deleted spelling and must move in the same
change** (`grep -rln rcDelegateWorker tests/`):

| Test | What binds |
|---|---|
| `tests/features/mutation_probe/` | asserts `rcDelegateWorker(&RemoteControl::cmdMutationProbe)` — retype to `rcDelegate(` |
| `tests/features/mcp_dispatch_forward_completeness/` | carries `"rcDelegateWorker("` in its accepted-marker list — drop the entry |
| `tests/features/mcp_call_site_contract/` | its regex is `rcDelegate(?:Worker)?\(`, which still matches; only its comment goes stale |

Leaving `mutation_probe` alone ships a red suite, and it is named in none of
the guard test's own invariants — a scrape of the whole `tests/` tree is the
only thing that finds it.

Before its rewrite, that file's § Out of scope deferred full async dispatch as
a larger follow-up that must keep one-request-at-a-time semantics. This spec is
that follow-up, and § 2.1 keeps those semantics.

**Amendment (2026-09-13).** `tests/features/mcp_async_dispatch/` gains INV-12,
INV-14 and INV-15, driving a bare `RemoteControl` beside the existing
`ClaudeIntegration` harness. `tests/features/mcp_verb_offthread_guard/` gains
INV-13, beside the INV-11 it already carries. `tests/features/mcp_audit_run_async/`'s `Inv1SyncPathUnchanged` is
rewritten for INV-16, and that test's `spec.md` INV-1 row changes with it.

Per the project test convention, add each source to the owning bundle's
`SOURCES` (ask `build_target_for`, do not guess), verify the ctest count moved
with `ctest -N -R`, and verify each test fails against pre-change source before
the change is restored.

## 7. Cross-doc impact

- `tests/features/mcp_verb_offthread_guard/spec.md` — **§ Background's**
  GUI-responsiveness claim (*"Moving them to a joined worker thread … keeps the
  GUI responsive"*) is false and is corrected; every invariant replaced.
  § Mechanism describes the factories and carries no such claim.
- `src/mainwindow.cpp` — the `rcDelegateWorker` comment repeats the same false
  claim and goes with the factory.
- `tests/features/mutation_probe/` and
  `tests/features/mcp_dispatch_forward_completeness/` — § 6's table.
- `docs/standards/mcp-tools.md` — the authoring checklist gains the eligibility
  rule of § 2.4, so a new verb's author knows which thread it runs on.
- `docs/standards/mcp-error-codes.md` — `dispatch_queue_full` joins the
  taxonomy.
- `CHANGELOG.md` — user-visible, and **scoped to what actually changes**: the
  window no longer freezes during rc-delegate MCP calls. It must not claim the
  freeze is gone outright, because `audit_run` and `indie_review_dispatch` still
  block for a sweep (§ 5).
- ROADMAP.md — ANTS-2132 to 🚧 on start, and its headline reworded off
  `audit_run` / `indie_review_dispatch`, which § 5 defers; ANTS-4682 filed for
  the deferred inline verbs.
- **Amendment (2026-09-13):**
  - `docs/standards/mcp-tools.md` — a verb's socket route, if it has one, runs
    on its MCP twin's thread (§ 2.7).
  - `tests/features/mcp_audit_run_async/spec.md` — its INV-1 row (§ 6).
  - `tests/features/mcp_verb_offthread_guard/spec.md` § Out of scope, and the
    comment above the `audit_run` registration in `src/mainwindow.cpp` — both
    say `audit_run` still freezes the window.
  - `CHANGELOG.md` — the window no longer freezes during a `--remote` search or
    a synchronous `audit_run`. It must not claim more: `indie_review_dispatch`
    still freezes it (§ 5).
  - ROADMAP.md — ANTS-5051, ANTS-5073 and ANTS-5035 close with the build.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-26 | 3, cold — genre pinned `spec`; one byte-stable shared packet carrying the two delegate factories, the dispatch site, the whole post-dispatch tail, every `MainWindow` reach-back window, and the `mcp_verb_offthread_guard` spec + test | **Q1 3 · Q2 6 · Q3 0 · Q4 3** (12 verified / 12 fixed / 1 dismissed) | **Twelve verified, twelve fixed.** **All three lanes independently found the same defect**, and it is the run's worst: § 2.6 had the destructor join the worker while INV-7 forbade the GUI thread ever blocking on it — in the same file INV-7's scrape covers — so an implementer's own test would red their own shutdown code, and a worker parked in a `BlockingQueuedConnection` would deadlock against that join. INV-7 is now scoped to the dispatch path and § 2.6 owns the exception, refusing in-flight marshals before joining. **Two lanes each found two more.** § 2.1 promised "no pair of verbs that cannot currently overlap begins to" while § 5 said an inline verb can now overlap an off-thread one — narrowed to the off-thread set, with § 5 cross-referenced where the guarantee is stated. And INV-1/2/8/10 were unreachable: `RcHandler` was declared in `mainwindow.cpp` and only an rc factory could produce an off-thread handler, so a test verb would always dispatch synchronously and the tests would pass for the wrong reason — the type moves to `claudeintegration.h` and is directly constructible. **Three Q1s, all false claims about existing code.** `onGuiThread` was specified as a `RemoteControl` member, but `resolveRootCanonical` (`remotecontrol_internal.h`) and `ants::resolveCallerCwdRoot` (`resolvedroot.h`) are free functions carrying five of the nine reach-backs — a member helper leaves INV-6 unsatisfiable. The superseded GUI-responsiveness claim is in the guard spec's § Background, not § Mechanism, cited wrongly twice. And "the handler body is `(m_remoteControl->*fn)(args)` and nothing else" is false — `registerToolProvider` re-wraps every handler in the ANTS-1427 `ANTS_LOG` lambda; verified safe off-thread because `DebugLog::write` takes a `std::lock_guard` on a static mutex, and the spec now says so rather than assuming it. **The sharpest scope finding came from two lanes:** `audit_run` and `indie_review_dispatch` are the two verbs ANTS-2132's headline names and the two this spec defers — confirmed by reading their sync path, which is `QThread::create` + `worker->wait()` on the GUI thread, so they genuinely still freeze for a whole sweep. § 5 now says so outright and § 7 requires the ROADMAP headline be reworded rather than letting the item close on work it left undone. **One lane found a third test nobody had counted:** `tests/features/mutation_probe/` asserts `rcDelegateWorker(&RemoteControl::cmdMutationProbe)`, so deleting the factory reds a suite named in none of the guard test's invariants; § 6 now carries a table of all three binding tests, found by a whole-tree scrape. **Two orchestrator findings, both caught building the packet rather than by a lane:** the reach-back enumeration used a grep pattern too narrow to see `focusedTerminal()` and `tabCount()`, and the guard spec has four invariants where the draft said three. **Dismissed:** "this design marshals seven call sites" — true of the distinct-accessor count, contradicted by six table rows and nine sites; all three lanes raised it and all three judged it immaterial, since § 2.5 orders enumeration from source. Removed anyway under the census-count rule rather than corrected to a number that rots. **Open questions resolved clean:** `rawRequested` is recomputed inside the tail from `toolName` + args, so `McpCallContext` need not carry it; `DebugLog` is mutex-guarded. **Resolved into a finding:** whether the queue cap counts the in-flight job — it does, stated, because otherwise INV-10's fixture cannot say what it expects. |
| 2 | 2026-08-26 | 3, cold — identical brief, scrubbed copy and packet rebuilt from disk | **Q1 2 · Q2 1 · Q3 1 · Q4 3** (7 verified / 7 fixed / 0 dismissed) | **Seven verified, seven fixed. Cap reached (2 for a spec); shipped to implementation.** **The run's best finding overturned § 1.1's premise.** It said the `QThread::wait()` join is what makes today's off-thread `MainWindow` access safe. False: `workspace_search` and `cited_by` are `Required`, the dispatcher refuses a `Required` verb with no `caller_cwd`, and their registration comments say the fallback is therefore unreachable. The hazard is real but arrives elsewhere — `ants::resolveCallerCwdRoot` walks the tab list on the branch where `caller_cwd` IS supplied, and `feedback_query` is `Required`, `rcDelegate`, and reaches it. § 1.1 rewritten around what actually holds. **All three lanes found the second:** § 2.5 exempted `remotecontrol_terminal.cpp` as a file, and that file holds `cmdFindSources` (`Required`, `rcDelegate`) and the `cmdRoadmapLogPass*` helpers `roadmap_log` calls — all off-thread. INV-6's scrape would have skipped exactly the bodies it exists to watch. Exemption is now per body, derived from the registration table. No live race today: none of that file's current reach-backs sit in those bodies. **Five of the seven landed on loop 1's own fixes** — a high share, so the run is oscillating rather than converging, and the cap is the right exit. INV-5 still keyed on factory provenance after loop 1 made `RcHandler` directly constructible for tests; `onGuiThread` had no refusal channel though loop 1's shutdown rule requires one, so nine call sites would each have invented an answer; loop 1's harness fallback sanctioned re-cutting runtime invariants as scrapes that cannot falsify them; and INV-3's reworded test no longer reached the only two verbs that spin a `QEventLoop`. INV-8 gained the clause that can catch § 2.6's refuse-then-join ordering, which INV-7's scrape cannot see. **Disclosure:** two lanes reported that an unscoped `workspace_search` returned a truncated headline from the unscrubbed loop-log table; both say they read no further and used nothing from it. Scrubbing the copy does not stop a lane's search reaching the original. |
| 3 | 2026-09-13 | 3, cold — genre pinned `spec`; first loop of the gate on the 2026-09-13 amendment (§ 1.2, § 2.7, § 2.8, INV-11..16) | **Q1 1 · Q2 2 · Q3 1 · Q4 2** (6 verified / 6 fixed / 1 dismissed) | **Six verified, six fixed; loop 2 of this run dispatched.** Two lanes found INV-15 could not catch an unlatched `reply`: the first reply disconnects, so a second writes nothing either way. It now counts trace entries. INV-16's `QThread::finished` check passed on the `async:true` branch alone; it now wants one per branch. § 2.7's route rule and INV-13 keyed on factory spelling, which misses inline `RcHandler{` twins that ANTS-4682 moved off-thread; both now key on the `RcHandler` overload, and § 2.4's inline-lambda sentence was corrected with them. § 2.8 now builds the context as the off-thread branch does, because `finishToolDispatch` skips its transforms when `toolHandled` is false. § 2.8 now states that a second same-root synchronous `audit_run` is refused `already_running`. § 7 now lists the guard spec and the `audit_run` comment that still say it freezes. **Dismissed:** § 1.2's census, raised by two lanes because the packet omitted `roadmapWriteTarget`. Its callers are all `roadmap_log` handlers, so the claim holds; the gap was the packet's. Out of scope, filed separately: three pre-existing copies elsewhere of "every hand-written inline lambda" runs on the GUI thread. |
