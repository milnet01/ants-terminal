# ANTS-2132 — Dispatch MCP verbs off the GUI thread

**Status:** accepted (2026-09-13). Cold-eyes loops 3 + 4 folded; cap reached.
§ 1.2, § 2.7 and § 2.8 amend the design accepted on 2026-08-26, and were built
on 2026-09-13 (`ClaudeIntegration::postWorkerJob`,
`RemoteControl::routeRunsOnDispatchWorker`, the `audit_run` registration). The
rest is implemented
(`ClaudeIntegration::postToolDispatch`, `tests/features/mcp_async_dispatch/`).
**Amendment (2026-09-13, ANTS-5072):** § 1.3 and § 2.9 run an off-thread verb's
reply transforms on the worker; § 2.2, INV-7 and INV-9 change with them, and
INV-18 is added. Gated by review-contract at its cap (loops 5 and 6). Built
2026-09-13.
**Amendment (2026-09-17, ANTS-5086):** § 2.10 runs `roadmap_migrate` on a
second worker; § 2.1, § 2.6, INV-2, INV-7, INV-8, § 4, § 5, § 6 and § 7
change with it, and INV-19 to INV-21 are added. Not yet built.
**Kind:** perf.
**Source:** ROADMAP.md ANTS-2132 (user report of intermittent whole-window
freeze; diagnosed in-session 2026-08-25). Amended for ANTS-5051, ANTS-5073,
ANTS-5035 and ANTS-5072 (code-quality-review-2026-09-11 perf pass).
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

### 1.3 What the second cut left on the GUI thread

*Amendment, 2026-09-13 — ANTS-5072.*

An off-thread verb runs only its handler on the worker. `postToolDispatch`
queues the handler's body back, and `finishToolDispatch` runs every reply
transform on the GUI thread. Measured with `tests/perf/bench_mcp_reply_tail` on a
4298134-byte `read_region` reply, 5 iterations, load average 0.56:

| Phase | ms |
|---|---|
| `applyEtagPattern` | 68.5 |
| `mcp::compactEnvelope`, when compaction applies | 56.5 |
| `mcp::offloadBody` | 23.2 |
| `wrapMcpData` on the whole body | 13.1 |
| the JSON-RPC envelope on the whole body | 12.3 |
| `mcp::appendReadHints` | 0.39 |

Without compaction the tail is 92.4 ms when the reply is offloaded and 94.5 ms
when it is sent whole. The window paints nothing for that long.

## 2. Surface

### 2.1 Shape of the change

The GUI thread stops *executing* verbs and stops *waiting* for them. It accepts
the request, hands the job to one long-lived worker, and returns to the event
loop. When the worker finishes, it posts the result back and the GUI thread
resumes the existing response pipeline unchanged.

**One worker, not one per call.** Off-thread verbs then execute one at a time
in arrival order — exactly today's serialisation — so **no pair of off-thread
verbs on the same lane begins to overlap** (§ 2.10 adds a second lane). Client-visible latency is unchanged; only the
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
    bool         toolHandled  = false;
    QString      dispatchResult;
    QElapsedTimer traceTimer;
    QStringList  ignoredArgKeys;  // amendment, ANTS-5072 — § 2.9
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

*Amendment, 2026-09-13 — ANTS-5072:* § 2.9 splits this pipeline into
`transformReply` and `finishToolDispatch`, keeps its order, and replaces the
signature above.

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
// a blocking queued invocation otherwise. nullopt means the calling worker's
// own dispatcher is shutting down and refused the marshal (§ 2.6).
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
  `ClaudeIntegration`'s destructor stops accepting jobs, refuses in-flight
  `onGuiThread` marshals from its own workers rather than serving them, then
  quits each worker's event loop and joins it. **Both lanes' marshals are
  refused before either worker is joined** (§ 2.10): `joinRefusingMarshals`
  serves a marshal from any thread not refused, so a bulk-lane marshal
  delivered while the shared worker is joined would otherwise run its callable
  during teardown. Both joins sit in `shutdownDispatchWorker`. **Only its own worker's:** File →
  New Window builds a second `MainWindow`, which deletes itself on close along
  with its own `ClaudeIntegration`, and the first window's marshals must still
  be served afterwards (INV-17). Refusing the marshal first is what keeps the join from
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

`RemoteControl` is built and tested without a `ClaudeIntegration`, and
`ants_core_lib` does not link the Claude library, so it does not ask the
registry. The set is written out once, in
`static bool RemoteControl::routeRunsOnDispatchWorker(const QString &cmd)`, and
INV-13 keeps it equal to the rule.

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

### 2.9 Reply transforms on the worker

*Amendment, 2026-09-13 — ANTS-5072.*

**The split.** § 2.2's pipeline becomes two functions. The transforms depend
only on the handler's body, the call's context and process-wide settings, so
they move into a static function. The steps that touch `ClaudeIntegration`
state stay in `finishToolDispatch`.

```cpp
// claudeintegration.h
struct ReplyTransform {
    QString cacheBody;      // a handled, cacheable, uncached call only: the body
                            // after the ignored-args advisory, before the ETag step
    QString wrapped;        // the text the JSON-RPC result carries
    bool    etagUnchanged = false;
    QString refusalCode;    // handlerRefusalCode of the transformed body; "" when none
    qint64  argBytes = 0;
    qint64  outBytes = 0;
    qint64  wrapBytes = 0;
};
static ReplyTransform transformReply(const McpCallContext &ctx,
                                     QString responseText);
void finishToolDispatch(McpCallContext ctx, ReplyTransform reply);
```

`transformReply` runs, in § 2.2's order: the ignored-args advisory, the ETag
step, `mcp::projectFields`, `mcp::compactEnvelope`, `mcp::appendReadHints`,
`mcp::tabularize`, the offload with its ANTS-4626 re-apply, the wrap, and the
byte counts. It reads no `ClaudeIntegration` member. When `ctx.toolHandled` is
false it returns an empty `ReplyTransform`; `finishToolDispatch` branches on
`ctx.toolHandled`, never on an empty field.

`finishToolDispatch` runs the ANTS-1357 cache insert from `cacheBody`, so the
cache still stores the body as it stood before the ETag step. It then sets
`dispatchResult` to `etag_unchanged` or to the refusal code, as today, calls
`recordDispatch`, and writes the reply. Writing it serialises the JSON-RPC
envelope around `wrapped` on the GUI thread: 12.3 ms for a 4 MiB reply sent
whole, next to nothing for an offloaded one (§ 1.3).

**The ignored-args keys.** The advisory reads `m_toolParamKeys`, which
`tools/list` writes on the GUI thread. The dispatcher computes the keys on the
GUI thread, stores them in `McpCallContext::ignoredArgKeys` before it chooses a
path, and `transformReply` reads only the context.

**Which thread.** An off-thread verb (§ 2.4) calls `transformReply` inside its
worker job, straight after the handler, and queues `finishToolDispatch` with the
result. A synchronous verb, a cache hit and a deferred verb's `reply` call
`transformReply` on the GUI thread, then `finishToolDispatch`.

**What the transforms read off the GUI thread.** `mcp::terseDefault` and the
offload settings are `std::atomic`; the hint latch is guarded by a `QMutex`.
`isEtagSupportedTool`, `mcp::isOffloadEligible` and `mcp::isRawEligible`
compare the tool name only; `mcp::isCompactArgTool` and
`mcp::isDefaultCompactTool` look it up in the `const` table
`kDispatchProjection`.

**The spill directory.** An off-thread verb can now offload while a GUI-thread
verb offloads too. Each writes its own content-addressed file through
`QSaveFile`. `evict` spares only the file its own call wrote and moves on when a
removal fails, so at the eviction cap one thread can remove the file the other
has just written; a later `read_spill` of that handle refuses `not_found`. No
lock is added around the write and the eviction: the GUI thread would wait out
a whole spill write on the worker. The hint latch's `QMutex` is held only for
one set lookup and insert (`claimHint`).

**Test seam.** `ClaudeIntegration` gains
`static QThread *lastReplyTransformThreadForTest()` and
`static void resetReplyTransformThreadForTest()`, over a
`std::atomic<QThread *>` that `transformReply` sets on entry.

### 2.10 The bulk lane

*Amendment, 2026-09-17 — ANTS-5086.*

**Why.** `roadmap_migrate` holds the dispatch worker for a whole migration. A
dry run of this project's roadmap held it about 4.5 s (2026-09-17). When
another connection holds the store's write lock, the migration also waits up
to `RoadmapStore::kBulkBusyTimeoutMs` (30 s) before it starts. Every other
off-thread verb from every session queues behind it, `roadmap_query` and
`roadmap_log` included.

**The lane.** `ClaudeIntegration` owns a second worker, `ants-mcp-bulk`, built
like § 2.3's: one `QThread`, started lazily, running a plain event loop.

```cpp
// claudeintegration.h
enum class DispatchLane { Shared, Bulk };
struct RcHandler {
    ToolHandler fn;
    bool offThreadEligible = true;
    DispatchLane lane = DispatchLane::Shared;
};
bool postWorkerJob(std::function<void()> job,
                   DispatchLane lane = DispatchLane::Shared);
```

The registration names the lane through `rcDelegate`'s one definition, which
gains a defaulted parameter:

```cpp
// mainwindow.cpp
ClaudeIntegration::RcHandler MainWindow::rcDelegate(
        QJsonDocument (RemoteControl::*fn)(const QJsonObject &),
        ClaudeIntegration::DispatchLane lane = ClaudeIntegration::DispatchLane::Shared);
// the roadmap_migrate registration
rcDelegate(&RemoteControl::cmdRoadmapMigrate, ClaudeIntegration::DispatchLane::Bulk)
```

Every other registration keeps `rcDelegate(&RemoteControl::…)` unchanged. One
definition, not an overload, so INV-3's factory scrape still reads the only
factory body.

`RegisteredTool` stores the lane beside `offThread`. The lane applies only
where `offThread` is true, so a `TabSpecific` verb stays on the GUI thread
whichever lane it names. `postToolDispatch` posts to the entry's lane. Socket
worker routes (§ 2.7) stay on the shared lane.

**Which verbs.** `roadmap_migrate` only. Its handler opens its own
`Access::Bulk` store connection (ANTS-3855 § 2.2) and reads no `RemoteControl`
cache, so it shares no in-process state with the shared worker. Its one
`MainWindow` read, `ants::resolveCallerCwdRoot`, marshals through `onGuiThread`
as it does on the shared lane (§ 2.5).

**What now overlaps, and the busy guard.** A roadmap write sent during a
migration no longer waits in the queue behind it. Unguarded, it could land
between the migration reading `ROADMAP.md` and committing its plan, and a
re-run migration would then revert it silently. So a write to a project whose
migration is running is refused.

- **The registry is process-wide**: a mutex-guarded map from project root to
  its holds, in `RemoteControl` as `static` members, because File → New Window
  builds a second `RemoteControl` beside a second `ClaudeIntegration`. A root
  carries either one exclusive hold or any number of shared holds, never both.
- **A writer takes a shared hold for its whole call**, as the first thing its
  handler does, before any other refusal, and releases it on every return path.
  The writers are `cmdRoadmapLog` (every op) and the four fold-in writers:
  `cmdColdEyesFoldIn`, `cmdIndieReviewFoldIn`, `cmdDebtSweepDefer` and the
  `test_audit_fold_in` registration. A writer keys on `rcProjectRootFor()` of
  the canonical `caller_cwd`. It is refused while the root is held exclusively.
- **`cmdRoadmapMigrate` takes the exclusive hold** for the whole call. Its key
  is the root it registers or deletes: `rr.cwd` for the plain migrate and
  `op:"init"`, and for `op:"deregister"` the root of the row it deletes, which
  keying on `export_slug` can make a different project. A `dry_run` takes no
  hold, since it writes nothing. The migration waits up to 5 s for live shared
  holds to be released, then is refused.
- **The refusal**, on either side, is `code: "roadmap_busy"` with
  `retry_after_ms`, and writes nothing.
- **Writes to other projects still meet the migration.** The store is one
  machine-global SQLite file, and a migration holds its write lock for a whole
  project's load. A write to any other project then waits for that lock on the
  shared lane, under the 5 s interactive deadline, and fails and reports if the
  load outlasts it. What the lane removes is the queueing behind the whole
  migration, its reading and planning included.

The guard covers this process. A second Ants process sharing the
machine-global store is not covered, as it was not before this amendment.
Reads take no hold and do not wait: the store runs in WAL. Two migrations on the bulk lane
still run one at a time, in arrival order.

**Cap and shutdown.** Jobs on both lanes count against the one 64-job cap.
§ 2.6 owns the shutdown order across both workers.

## 3. Invariants

- **INV-1** — For a verb registered off-thread, the GUI thread processes at
  least one event between the request arriving and the reply being written.
  *Test:* `tests/features/mcp_async_dispatch/` — register a test verb that
  sleeps, assert a GUI-thread timer fires during it.
- **INV-2** — Off-thread verbs on the same lane (§ 2.10) execute one at a
  time, in arrival order.
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
  `tests/features/mcp_verb_offthread_guard/` — source scrape asserting that
  every join of a dispatch worker (§ 2.10: both lanes) in
  `claudeintegration.cpp` is in
  `shutdownDispatchWorker`, reached only from `~ClaudeIntegration`, and that
  `finishToolDispatch`, `transformReply` (§ 2.9) and the dispatch path contain
  none.
- **INV-8** — Exactly one JSON-RPC reply is written per request carrying an
  `id`, and none is written after the socket is gone or after shutdown has
  begun. *Test:* `tests/features/mcp_async_dispatch/`, two clauses — (a)
  disconnect mid-verb, assert no write and no crash under the `debug` (ASan)
  preset; (b) destroy `ClaudeIntegration` with a job in flight on each lane,
  both parked in `onGuiThread`, so either join order that refuses one lane late
  is caught, assert the destructor returns
  within a bounded time, wrote no reply, and ran neither marshal's callable. Clause (b) is
  the only thing that can catch § 2.6's refuse-then-join ordering: INV-7's
  scrape sees where `wait()` sits, never whether it deadlocks.
- **INV-9** — The response pipeline of § 2.2 exists in exactly one place and
  runs identically on every path: one definition of `transformReply` and one of
  `finishToolDispatch` (§ 2.9, amended 2026-09-13 for ANTS-5072). *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape asserting one
  definition of each, a `wrapMcpData(` call inside `transformReply`, none in
  `finishToolDispatch`, `postToolDispatch` or the dispatcher, and a
  `transformReply(` call inside `postToolDispatch`.
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
- **INV-17** — Destroying one `ClaudeIntegration` refuses `onGuiThread`
  marshals from its own worker only; another instance's off-thread verbs are
  still served. *Test:* `tests/features/mcp_async_dispatch/` — two harness
  instances, each registering an off-thread verb that returns the result of an
  `onGuiThread` call. Destroy the second, call the first's verb, and assert it
  returns the marshalled value. Breaks if the refusal is one process-wide flag:
  the call is refused.
- **INV-18** *(amendment, 2026-09-13 — ANTS-5072)* — For a call whose handler
  ran on the dispatch worker, the reply transforms run there too; a verb
  registered through the bare
  `ToolHandler` overload runs them on the GUI thread. *Test:*
  `tests/features/mcp_async_dispatch/` — reset the seam, call an `RcHandler`
  verb that records `QThread::currentThread()`, and assert the seam equals that
  thread; reset it again, call a bare `ToolHandler` verb, and assert the seam
  equals the GUI thread. Breaks if `postToolDispatch` queues the handler's body
  back before `transformReply` runs (the first assertion reads the GUI thread),
  or if the worker job transforms the reply without calling `transformReply`
  (the seam stays `nullptr`).
- **INV-19** *(amendment, 2026-09-17 — ANTS-5086)* — A verb on the shared
  lane replies while a verb on the bulk lane is still running. *Test:*
  `tests/features/mcp_async_dispatch/` — register an `RcHandler` on
  `DispatchLane::Bulk` that blocks until the test releases it, call it, then
  call an `RcHandler` on the shared lane and assert its reply arrives while the
  bulk verb has not returned; release it and assert both replies were written.
  Breaks if `postToolDispatch` ignores the entry's lane: the shared call queues
  behind the blocked one and gets no reply.
- **INV-20** *(amendment, 2026-09-17 — ANTS-5086)* — `roadmap_migrate` is
  registered on `DispatchLane::Bulk`, and no other verb is. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — scrape `src/mainwindow.cpp` for
  `DispatchLane::Bulk` and assert exactly one occurrence, inside the
  `roadmap_migrate` registration. Breaks if the registration keeps plain
  `rcDelegate(`: the scrape finds none.
- **INV-21** *(amendment, 2026-09-17 — ANTS-5086)* — A root held exclusively
  refuses every writer `roadmap_busy` and writes nothing; a root with a live
  shared hold refuses a migration `roadmap_busy` once the wait expires. *Test:*
  `tests/features/mcp_async_dispatch/`, on a bare `RemoteControl` over a
  temporary project with one seeded bullet. (a) Take the exclusive hold, call
  `cmdRoadmapLog` `op:"flip"` from the project's subdirectory, assert
  `roadmap_busy` and an unchanged `ROADMAP.md`; release, repeat, assert
  `ok:true`. `op:"flip"` because `op:"append"` refuses `no_main` without a
  `MainWindow`. (b) Take a shared hold, try the exclusive hold with the wait
  shortened, assert it is refused. A source scrape asserts each fold-in writer
  takes its shared hold before its first write. Breaks if the writer checks
  instead of holding, which (b) sees, or if the two sides key differently,
  which (a)'s subdirectory call sees.

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

*Amendment (ANTS-5072):* an off-thread verb's queued continuation carries a
`ReplyTransform` instead of the handler's body: the wrapped text, plus the
pre-ETag body only for a handled, cacheable, uncached call. No transformed body
is carried, so at most two reply-sized strings cross the queue, as
`finishToolDispatch` holds two at its wrap today.

The amendment adds no thread and no queue. A queued socket job holds one parsed
request, already bounded by the socket's receive cap. A synchronous `audit_run`
holds its `RunResult` until the reply, as the `async:true` branch does.

*Amendment (ANTS-5086):* one more `QThread`, the bulk lane's, started lazily
on the first `roadmap_migrate` call. The queue cap is unchanged, because both
lanes share it.

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
  serialised worker delivers the whole of the reported symptom. The bulk lane
  (§ 2.10) is not a thread per verb: it serialises its own verbs and carries
  one verb that shares no in-process state with the shared worker.
- **Other long verbs on the bulk lane.** § 2.10 moves `roadmap_migrate` only.
  A slow `roadmap_log` render still queues on the shared lane; its cost is
  ANTS-4681.
- **Moving the MCP server itself onto a worker thread** — rejected. It would
  require marshalling every `ClaudeIntegration` state read, where this design
  marshals only the reach-back sites § 2.5 enumerates.
- **Pumping the event loop during a verb** — forbidden, permanently. That is
  the nested-loop socket use-after-free class ANTS-2131 closed. Not deferred
  work; a boundary. No id.
- **Reply transforms for synchronous, cached and deferred verbs** stay on the
  GUI thread (§ 2.9, ANTS-5072). Moving them would put a reply that is ready now
  behind the worker queue.
- **Serialising the JSON-RPC envelope** stays on the GUI thread for every verb
  (§ 2.9, ANTS-5072). It costs 12.3 ms only for a 4 MiB reply sent whole.

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
INV-14, INV-15 and INV-17, driving a bare `RemoteControl` beside the existing
`ClaudeIntegration` harness. `tests/features/mcp_verb_offthread_guard/` gains
INV-13, beside the INV-11 it already carries. `tests/features/mcp_audit_run_async/`'s `Inv1SyncPathUnchanged` is
rewritten for INV-16, and that test's `spec.md` INV-1 row changes with it.

**Amendment (2026-09-13, ANTS-5072).** `tests/features/mcp_async_dispatch/`
gains INV-18. `tests/features/mcp_verb_offthread_guard/`'s INV-7 and INV-9
scrapes change with § 2.9. The pipeline's output is held by its existing tests,
which must pass unmodified: `mcp_projection`, `mcp_etag_refusal`,
`mcp_etag_tip_memo`, `mcp_idempotent_read_cache`, `mcp_ignored_args`,
`mcp_offload_keeps_advisory` and `mcp_result_offload`.

**Amendment (2026-09-17, ANTS-5086).** `tests/features/mcp_async_dispatch/`
gains INV-19, and INV-8(b)'s clause gains the bulk-lane job;
`tests/features/mcp_verb_offthread_guard/` gains INV-20, and INV-7's scrape
covers both joins. INV-2's existing test runs on the shared lane and is
unchanged. That guard's INV-6 registration regex,
`rcDelegate\(&RemoteControl::(\w+)\)`, must also accept the optional lane
argument, or `cmdRoadmapMigrate` silently leaves INV-6's off-thread set.
`mcp_call_site_contract` and `mcp_dispatch_forward_completeness` match on
`rcDelegate(` and need no change. INV-21 joins `mcp_async_dispatch`.

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
  - `tests/features/guithread_join_parked_marshal/spec.md` and
    `tests/features/verify_trust_modal_gui_thread/spec.md` — both bind to
    `ants::setGuiMarshalRefused` as one process-wide flag, which § 2.6 now
    scopes to the shutting-down instance's own worker.
  - `CHANGELOG.md` — the window no longer freezes during a `--remote` search or
    a synchronous `audit_run`. It must not claim more: `indie_review_dispatch`
    still freezes it (§ 5).
  - ROADMAP.md — ANTS-5051, ANTS-5073 and ANTS-5035 close with the build.
- **Amendment (2026-09-13, ANTS-5072):**
  - `tests/features/mcp_verb_offthread_guard/spec.md` — its INV-7 and INV-9
    rows name `transformReply`.
  - `CHANGELOG.md` — a large reply from an off-thread verb no longer stalls the
    window for its ETag, compaction, offload and wrap steps. It must not claim
    more: the envelope is still serialised on the GUI thread (§ 2.9), and
    synchronous, cached and deferred verbs still prepare theirs there (§ 5).
  - ROADMAP.md — ANTS-5072 closes with the build.
- **Amendment (2026-09-17, ANTS-5086):**
  - `docs/standards/mcp-tools.md` — a verb runs on the shared lane unless its
    registration names `DispatchLane::Bulk` (§ 2.10), and step 2a's "so no two
    of them overlap" is scoped to verbs on the same lane.
  - `docs/specs/ANTS-3855-roadmap-migrate-verb.md` § 2.5 — the verb refuses
    `roadmap_busy` while a write to the same root is in flight (§ 2.10).
  - `docs/standards/mcp-error-codes.md` — `roadmap_busy` joins the taxonomy.
  - `CHANGELOG.md` — other sessions' MCP calls no longer queue behind a whole
    roadmap migration. It must not claim more: a write to the project being
    migrated is refused until it ends, and a write to any project can still
    wait up to 5 s for the store while the migration's load runs (§ 2.10).
  - ROADMAP.md — ANTS-5086's busy-deadline finding closes with the build.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-26 | 3, cold — genre pinned `spec`; one byte-stable shared packet carrying the two delegate factories, the dispatch site, the whole post-dispatch tail, every `MainWindow` reach-back window, and the `mcp_verb_offthread_guard` spec + test | **Q1 3 · Q2 6 · Q3 0 · Q4 3** (12 verified / 12 fixed / 1 dismissed) | **Twelve verified, twelve fixed.** **All three lanes independently found the same defect**, and it is the run's worst: § 2.6 had the destructor join the worker while INV-7 forbade the GUI thread ever blocking on it — in the same file INV-7's scrape covers — so an implementer's own test would red their own shutdown code, and a worker parked in a `BlockingQueuedConnection` would deadlock against that join. INV-7 is now scoped to the dispatch path and § 2.6 owns the exception, refusing in-flight marshals before joining. **Two lanes each found two more.** § 2.1 promised "no pair of verbs that cannot currently overlap begins to" while § 5 said an inline verb can now overlap an off-thread one — narrowed to the off-thread set, with § 5 cross-referenced where the guarantee is stated. And INV-1/2/8/10 were unreachable: `RcHandler` was declared in `mainwindow.cpp` and only an rc factory could produce an off-thread handler, so a test verb would always dispatch synchronously and the tests would pass for the wrong reason — the type moves to `claudeintegration.h` and is directly constructible. **Three Q1s, all false claims about existing code.** `onGuiThread` was specified as a `RemoteControl` member, but `resolveRootCanonical` (`remotecontrol_internal.h`) and `ants::resolveCallerCwdRoot` (`resolvedroot.h`) are free functions carrying five of the nine reach-backs — a member helper leaves INV-6 unsatisfiable. The superseded GUI-responsiveness claim is in the guard spec's § Background, not § Mechanism, cited wrongly twice. And "the handler body is `(m_remoteControl->*fn)(args)` and nothing else" is false — `registerToolProvider` re-wraps every handler in the ANTS-1427 `ANTS_LOG` lambda; verified safe off-thread because `DebugLog::write` takes a `std::lock_guard` on a static mutex, and the spec now says so rather than assuming it. **The sharpest scope finding came from two lanes:** `audit_run` and `indie_review_dispatch` are the two verbs ANTS-2132's headline names and the two this spec defers — confirmed by reading their sync path, which is `QThread::create` + `worker->wait()` on the GUI thread, so they genuinely still freeze for a whole sweep. § 5 now says so outright and § 7 requires the ROADMAP headline be reworded rather than letting the item close on work it left undone. **One lane found a third test nobody had counted:** `tests/features/mutation_probe/` asserts `rcDelegateWorker(&RemoteControl::cmdMutationProbe)`, so deleting the factory reds a suite named in none of the guard test's invariants; § 6 now carries a table of all three binding tests, found by a whole-tree scrape. **Two orchestrator findings, both caught building the packet rather than by a lane:** the reach-back enumeration used a grep pattern too narrow to see `focusedTerminal()` and `tabCount()`, and the guard spec has four invariants where the draft said three. **Dismissed:** "this design marshals seven call sites" — true of the distinct-accessor count, contradicted by six table rows and nine sites; all three lanes raised it and all three judged it immaterial, since § 2.5 orders enumeration from source. Removed anyway under the census-count rule rather than corrected to a number that rots. **Open questions resolved clean:** `rawRequested` is recomputed inside the tail from `toolName` + args, so `McpCallContext` need not carry it; `DebugLog` is mutex-guarded. **Resolved into a finding:** whether the queue cap counts the in-flight job — it does, stated, because otherwise INV-10's fixture cannot say what it expects. |
| 2 | 2026-08-26 | 3, cold — identical brief, scrubbed copy and packet rebuilt from disk | **Q1 2 · Q2 1 · Q3 1 · Q4 3** (7 verified / 7 fixed / 0 dismissed) | **Seven verified, seven fixed. Cap reached (2 for a spec); shipped to implementation.** **The run's best finding overturned § 1.1's premise.** It said the `QThread::wait()` join is what makes today's off-thread `MainWindow` access safe. False: `workspace_search` and `cited_by` are `Required`, the dispatcher refuses a `Required` verb with no `caller_cwd`, and their registration comments say the fallback is therefore unreachable. The hazard is real but arrives elsewhere — `ants::resolveCallerCwdRoot` walks the tab list on the branch where `caller_cwd` IS supplied, and `feedback_query` is `Required`, `rcDelegate`, and reaches it. § 1.1 rewritten around what actually holds. **All three lanes found the second:** § 2.5 exempted `remotecontrol_terminal.cpp` as a file, and that file holds `cmdFindSources` (`Required`, `rcDelegate`) and the `cmdRoadmapLogPass*` helpers `roadmap_log` calls — all off-thread. INV-6's scrape would have skipped exactly the bodies it exists to watch. Exemption is now per body, derived from the registration table. No live race today: none of that file's current reach-backs sit in those bodies. **Five of the seven landed on loop 1's own fixes** — a high share, so the run is oscillating rather than converging, and the cap is the right exit. INV-5 still keyed on factory provenance after loop 1 made `RcHandler` directly constructible for tests; `onGuiThread` had no refusal channel though loop 1's shutdown rule requires one, so nine call sites would each have invented an answer; loop 1's harness fallback sanctioned re-cutting runtime invariants as scrapes that cannot falsify them; and INV-3's reworded test no longer reached the only two verbs that spin a `QEventLoop`. INV-8 gained the clause that can catch § 2.6's refuse-then-join ordering, which INV-7's scrape cannot see. **Disclosure:** two lanes reported that an unscoped `workspace_search` returned a truncated headline from the unscrubbed loop-log table; both say they read no further and used nothing from it. Scrubbing the copy does not stop a lane's search reaching the original. |
| 3 | 2026-09-13 | 3, cold — genre pinned `spec`; first loop of the gate on the 2026-09-13 amendment (§ 1.2, § 2.7, § 2.8, INV-11..16) | **Q1 1 · Q2 2 · Q3 1 · Q4 2** (6 verified / 6 fixed / 1 dismissed) | **Six verified, six fixed; loop 2 of this run dispatched.** Two lanes found INV-15 could not catch an unlatched `reply`: the first reply disconnects, so a second writes nothing either way. It now counts trace entries. INV-16's `QThread::finished` check passed on the `async:true` branch alone; it now wants one per branch. § 2.7's route rule and INV-13 keyed on factory spelling, which misses inline `RcHandler{` twins that ANTS-4682 moved off-thread; both now key on the `RcHandler` overload, and § 2.4's inline-lambda sentence was corrected with them. § 2.8 now builds the context as the off-thread branch does, because `finishToolDispatch` skips its transforms when `toolHandled` is false. § 2.8 now states that a second same-root synchronous `audit_run` is refused `already_running`. § 7 now lists the guard spec and the `audit_run` comment that still say it freezes. **Dismissed:** § 1.2's census, raised by two lanes because the packet omitted `roadmapWriteTarget`. Its callers are all `roadmap_log` handlers, so the claim holds; the gap was the packet's. Out of scope, filed separately: three pre-existing copies elsewhere of "every hand-written inline lambda" runs on the GUI thread. |
| 4 | 2026-09-13 | 3, cold — identical brief; scrubbed copy, packet and source facts rebuilt from disk | **Q1 1 · Q2 0 · Q3 1 · Q4 0** (2 verified / 2 fixed / 1 dismissed) | **Two verified, two fixed. Cap reached (2 for a spec); shipped to implementation.** § 2.7 blamed library layering for the static route list; `RemoteControl` files already include `claudeintegration.h`, so the true reason is that `RemoteControl` is built and tested without a `ClaudeIntegration`. § 2.6's shutdown refusal was unscoped, and the shipped flag is process-wide: closing a second window refuses every later marshal in the first (filed ANTS-5142). The refusal is now scoped to the instance's own worker, INV-17 tests it, and § 7 names the two test contracts bound to the global flag. **Dismissed:** INV-7's test clause named a `wait()` in the destructor where the built join is in `shutdownDispatchWorker`. Two lanes raised it; nothing built changes, so it was corrected as a record of the code. **Calm cap:** 0 of this loop's 2 verified findings landed on text a loop-1 fix wrote. **Gate against audit:** 7 of this run's 8 verified findings anchor in the amendment draft (`b198f557..f8ece108`); the § 2.6 refusal scope predates it. Out of scope, filed: ANTS-5141, the store destroyed on the GUI thread. |
| 5 | 2026-09-13 | 3, cold — genre pinned `spec`; first loop of the gate on the ANTS-5072 amendment (§ 1.3, § 2.9, INV-7, INV-9, INV-18) | **Q1 1 · Q2 3 · Q3 0 · Q4 0** (4 verified / 4 fixed / 0 dismissed) | **Four verified, four fixed; loop 2 of this run dispatched.** All three lanes found § 2.2's struct had lost `toolHandled`, which § 2.8 and § 2.9 rely on, so `finishToolDispatch` would have read an empty control-plane body as an unknown tool; the field is back and `finishToolDispatch` branches on it. All three lanes found § 4's peak-memory sentence false: `ReplyTransform` carried the pre-ETag body on every call, beside the transformed body and the wrapped text. The transformed body is no longer carried, and `cacheBody` is filled only for a handled, cacheable, uncached call. Two lanes found INV-18 claimed worker transforms for every off-thread verb, while a cache hit or a queue-full refusal finishes on the GUI thread; it is now scoped to a call whose handler ran on the worker. One lane found the spill directory's no-lock reason contradicted the hint latch's `QMutex`; the reason is now the length of the wait. **Open questions resolved clean:** the scrape tests § 6 requires to pass unmodified (`mcp_projection`, `mcp_rate_limit`, `mcp_ignored_args`) key on spellings a split can keep; whether `read_region` is cacheable stops mattering once `cacheBody` is conditional. |
| 6 | 2026-09-13 | 3, cold — identical brief; scrubbed copy, packet and source facts rebuilt from disk | **Q1 0 · Q2 1 · Q3 0 · Q4 2** (3 verified / 3 fixed / 1 dismissed) | **Three verified, three fixed. Cap reached (2 for a spec); shipped to implementation.** Two lanes found § 7's CHANGELOG line promised a large off-thread reply no longer stalls the window, while § 2.9 still serialises the JSON-RPC envelope on the GUI thread, which § 1.3 measures at 12.3 ms for a reply sent whole. § 2.9 and § 5 now say the envelope stays there, and the CHANGELOG line names only the steps that move. One lane found INV-18's test would pass with its seam never set, since `nullptr` is not the GUI thread; the seam is now atomic and reset before each call, and the test asserts it equals the thread the verb recorded. The same lane found INV-9's scrape never looked at `postToolDispatch`, where the off-thread transforms now run, so a second pipeline there would pass; the scrape now covers it. **Dismissed:** § 2.1's "client-visible latency is unchanged" no longer holds for a queued off-thread verb, which now waits behind the previous job's transforms; nothing built differs. **Checked by the orchestrator:** every function-local static in `claudeintegration.cpp`, `mcpprojection.cpp` and `mcpspill.cpp` is `const` or `constexpr`, so § 2.9's list of what the transforms read off the GUI thread holds. **Calm cap:** 0 of this loop's 3 verified findings landed on text loop 5 wrote. **Gate against audit:** 6 of this run's 7 verified findings anchor in the amendment commit `f60c24e0`; the missing `toolHandled` predates it. Tail filed: none. |
| 7 | 2026-09-17 | 3, cold — genre pinned `spec`; first loop of the gate on the ANTS-5086 amendment (§ 2.10, INV-2, INV-19, INV-20) | **Q1 1 · Q2 1 · Q3 2 · Q4 1** (5 verified / 5 fixed / 0 dismissed) | **Five verified, five fixed; loop 2 of this run dispatched.** All three lanes found § 2.10's shutdown sentence read two ways, and the per-lane reading runs a bulk-lane `onGuiThread` marshal while the shared worker is joined, because `joinRefusingMarshals` serves other threads' marshals; § 2.6 now refuses both lanes before either join. All three found the registration's lane spelling unpinned while INV-20 and INV-6's `rcDelegate\(&RemoteControl::(\w+)\)` scrape bind to it; `rcDelegate` gains a defaulted lane parameter and § 6 widens that regex. All three found § 2.1 and `mcp-tools.md` step 2a still promising no two off-thread verbs overlap; both now say per lane. One lane found INV-7 and INV-8(b) could not fail on the bulk worker's join; both now cover each lane. One lane found § 2.10's "only overlap is a lock wait" false: a roadmap write for the project being migrated could land between its read and its commit, and a re-run would revert it silently. **Surfaced to the user, who chose a busy guard**: a process-wide registry refuses `migration_in_progress`, added as INV-21. **Open questions resolved clean:** `deregister` opens `Access::Bulk`; `rcdetail::findRoadmapUnder` and `ants::resolveCallerCwdRoot` hold no shared static state; a first store open racing creation is INV-15's existing case. |
| 8 | 2026-09-17 | 3, cold — identical brief; scrubbed copy, packet and source facts rebuilt from disk, windows added for the busy guard's writers and `rcProjectRootFor` | **Q1 1 · Q2 0 · Q3 2 · Q4 2** (5 verified / 5 fixed / 0 dismissed) | **Five verified, five fixed. Cap reached (2 for a spec); shipped to implementation.** All three lanes found the busy guard was check-then-act: a writer that passed its check before the migration took the root could still be writing when the migration read `ROADMAP.md`. Writers now take a shared hold for the whole call, the migration an exclusive one that waits 5 s for writers, and the code is `roadmap_busy`. All three found INV-21's release leg unreachable: `op:"append"` refuses `no_main` on the bare `RemoteControl` the harness builds; it uses `op:"flip"`, called from a subdirectory so a key mismatch shows. One lane found INV-8(b) parked only the bulk-lane marshal, so one wrong join order passed; both lanes' jobs are now parked. One lane found the CHANGELOG line and "reads are not refused" hid that a write to any other project still waits up to 5 s for the machine-global store's lock during the load; § 2.10 and § 7 now say so. Resolving an open question found `op:"deregister"` can key on `export_slug` and delete another project's rows while holding the caller's root; it now holds the deleted row's root. **Open questions resolved clean:** a first store open racing creation is INV-15's case; `op:"init"` keys on `rr.cwd`, the root it registers, so a parent project's roadmap does not move the key. **Violent cap:** 5 of this loop's 5 verified findings landed on text loop 7 wrote, so the review of this amendment ends here and it routes to implementation. **Gate against audit:** 3 of this run's 10 verified findings anchor in the gated span `2ca70c5c`; 5 landed on the run's own fixes and 2 on pre-existing text (§ 2.1, INV-7/INV-8). Out of scope, not filed: the GUI-thread fold-in dialogs can overlap a migration, as they could overlap the shared worker before this amendment (§ 5). |
