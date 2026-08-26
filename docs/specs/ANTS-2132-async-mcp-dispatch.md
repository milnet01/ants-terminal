# ANTS-2132 — Dispatch MCP verbs off the GUI thread

**Status:** spec draft (2026-08-26).
**Kind:** perf.
**Source:** ROADMAP.md ANTS-2132 (user report of intermittent whole-window
freeze; diagnosed in-session 2026-08-25).
**Pairs with:** ANTS-4681 (`roadmap_log` render + write cost — the largest
repeatable contributor to the per-verb duration this spec stops blocking on).
**Supersedes:** the GUI-responsiveness claim in
`tests/features/mcp_verb_offthread_guard/spec.md` § Mechanism (see § 7).

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

### 1.1 The join is currently load-bearing for safety

`RemoteControl::cmdWorkspaceSearch` and `RemoteControl::cmdCitedBy` — both
already dispatched through `rcDelegateWorker` — call
`MainWindow::currentTerminal()` on the worker to resolve a `caller_cwd`
fallback. That is safe **only because the GUI thread is parked in
`QThread::wait()`** and therefore cannot touch the same widgets concurrently.
Removing the join removes that guarantee. Any design that drops the join owes a
marshalling rule (§ 2.5); this is not an incidental detail.

## 2. Surface

### 2.1 Shape of the change

The GUI thread stops *executing* verbs and stops *waiting* for them. It accepts
the request, hands the job to one long-lived worker, and returns to the event
loop. When the worker finishes, it posts the result back and the GUI thread
resumes the existing response pipeline unchanged.

**One worker, not one per call.** Verbs then execute one at a time in arrival
order — exactly today's serialisation — so no pair of verbs that cannot
currently overlap begins to. Client-visible latency is unchanged; only the
window's responsiveness changes. A thread per call would additionally alter
concurrency for every verb at once and is rejected in § 5.

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
already carries — **no call site changes text**, so the source-scrape tests
that match `rcDelegate(` / `rcDelegateWorker(` keep matching.

The two factories return a marked type instead of a bare `ToolHandler`:

```cpp
// mainwindow.cpp — both factories return this; implicitly convertible.
struct RcHandler {
    ClaudeIntegration::ToolHandler fn;
    bool builtByRcFactory = true;
};
```

`registerToolProvider` gains an overload taking `RcHandler` and sets:

```
offThread = builtByRcFactory && contract != CallerCwdContract::TabSpecific
```

The bare-`ToolHandler` overload — every hand-written inline lambda — sets
`offThread = false`. Two facts justify each half:

- **Built by an rc factory** means the handler body is `(m_remoteControl->*fn)(args)`
  and nothing else. An inline lambda captures `MainWindow` and may touch widgets
  directly.
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
and that set contains every verb the § 1 measurements name. The inline
handlers and the TabSpecific verbs stay synchronous.

`rcDelegateWorker` is deleted. Under this design it would be
byte-for-byte `rcDelegate`, and two factories doing one thing is the
duplication `coding.md` § 1.3 forbids. Its verbs move to `rcDelegate`.

### 2.5 MainWindow access from an off-thread verb

These symbols outside `src/remotecontrol_terminal.cpp` reach into `MainWindow`
from a verb body — enumerated from source, not from this list
(`grep -n "m_main->\|main->terminal\|main->current" src/remotecontrol*.cpp`,
2026-08-26):

| Symbol | File | What it reads |
|---|---|---|
| `RemoteControl::cmdWorkspaceSearch` | `remotecontrol_workspace.cpp` | `currentTerminal()` |
| `RemoteControl::cmdCitedBy` | `remotecontrol_workspace.cpp` | `currentTerminal()->shellCwd()` |
| `RemoteControl::cmdRoadmapQuery` | `remotecontrol_roadmap_query.cpp` | `roadmapPathForRemote()` |
| `RemoteControl::cmdTokenUsage` | `remotecontrol_review.cpp` | `tokenSavingsSummary()` |
| `resolveRootCanonical` | `remotecontrol_feedback.cpp` | `currentTerminal()` |
| `resolveCallerCwdRoot` | `remotecontrol_feedback.cpp` | `currentTabIndexForRemote()`, `terminalAtTab()` — two sites |

Each is wrapped in a marshalling helper on `RemoteControl`:

```cpp
// Runs f on the GUI thread and returns its result. Direct call when already
// there; a blocking queued invocation otherwise.
template <class F>
auto onGuiThread(F &&f) -> std::invoke_result_t<F>;
```

`Qt::BlockingQueuedConnection` cannot deadlock here because the GUI thread
never waits on the worker (§ 2.1) — that is INV-7, and it is the property the
join gave away for free and this design has to state.

`src/remotecontrol_terminal.cpp` is exempt: its verbs are all `TabSpecific` and
therefore always run on the GUI thread already.

### 2.6 Socket lifetime, backpressure and shutdown

- The peer may disconnect while a job is queued. The continuation's existing
  `QPointer` guard and `ConnectedState` check already cover this; deferral only
  widens the window.
- The 5 s slow-loris idle timer is already stopped before dispatch, so a
  long-running verb cannot be aborted by it.
- **Queue cap.** The pending-job queue is capped at 64. A job arriving at a
  full queue is refused synchronously with `code: "dispatch_queue_full"` and a
  `retry_after_ms` hint, mirroring `audit_run`'s `already_running` shape. The
  cap exists so a wedged verb cannot grow the queue without bound.
- **Shutdown.** `ClaudeIntegration`'s destructor stops accepting jobs, quits
  the worker's event loop and joins it. A result arriving after shutdown has
  begun is dropped, not written.

## 3. Invariants

- **INV-1** — For a verb registered off-thread, the GUI thread processes at
  least one event between the request arriving and the reply being written.
  *Test:* `tests/features/mcp_async_dispatch/` — register a test verb that
  sleeps, assert a GUI-thread timer fires during it.
- **INV-2** — Off-thread verbs execute one at a time, in arrival order.
  *Test:* `tests/features/mcp_async_dispatch/` — dispatch three verbs that
  record entry and exit timestamps; assert no two intervals overlap and the
  order matches arrival.
- **INV-3** — No MCP verb dispatch runs a nested `QEventLoop` on the GUI
  thread (ANTS-2131 preserved). *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape of
  `onMcpConnection()` and both factories.
- **INV-4** — A handler registered with `CallerCwdContract::TabSpecific` is
  never dispatched off the GUI thread. *Test:*
  `tests/features/mcp_async_dispatch/` — assert `offThread == false` for every
  TabSpecific registration.
- **INV-5** — A handler not built by an rc factory is never dispatched off the
  GUI thread. *Test:* as INV-4, over the inline registrations.
- **INV-6** — Outside `src/remotecontrol_terminal.cpp`, no verb body reaches
  `MainWindow` except through `onGuiThread`. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape for `m_main->`,
  `main->terminal`, `main->current` in `src/remotecontrol*.cpp` excluding the
  terminal file; every hit must be lexically inside an `onGuiThread(` call.
- **INV-7** — The GUI thread never blocks on the worker. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape asserting no
  `wait()` or `BlockingQueuedConnection` targeting the worker from
  `claudeintegration.cpp` or `mainwindow.cpp`.
- **INV-8** — Exactly one JSON-RPC reply is written per request carrying an
  `id`, and none is written after the socket is gone or after shutdown has
  begun. *Test:* `tests/features/mcp_async_dispatch/` — disconnect mid-verb and
  assert no write and no crash under the `debug` (ASan) preset.
- **INV-9** — The response pipeline of § 2.2 exists in exactly one place and
  runs identically on the synchronous and deferred paths. *Test:*
  `tests/features/mcp_verb_offthread_guard/` — source scrape asserting one
  definition of `finishToolDispatch` and no second `wrapMcpData(` call site in
  the dispatch path.
- **INV-10** — A job arriving at a full queue is refused with
  `dispatch_queue_full` and no job is dropped silently. *Test:*
  `tests/features/mcp_async_dispatch/` — post 65 jobs against a blocked worker.

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

## 5. Out of scope

- **The inline non-TabSpecific verbs stay synchronous** — `audit_run`,
  `audit_poll`, `indie_review_dispatch`, `project_query`, `get_git_status`, the
  five `test_audit_*`, and the Ants-internal `tab_list` / `token_usage` /
  `mcp_trace` / `caller_cwd_info`. Their handlers capture `MainWindow` and each
  needs its own audit. Tracked by ANTS-4682. **The residual hazard is stated
  rather than hidden:** after this change a synchronous inline verb that
  touches project files (`project_query`, `get_git_status`) can run on the GUI
  thread while an off-thread verb touches the same files, which cannot happen
  today. ANTS-4682 closes it.
- **Per-verb cost.** Making `roadmap_log` cheaper is ANTS-4681. This spec stops
  the GUI *waiting* for a verb; it makes no verb faster.
- **A thread per verb** — rejected. It would let verbs that cannot currently
  overlap run concurrently against the roadmap store, the caches and the
  project tree, which is a much larger change than the freeze warrants. The
  serialised worker delivers the whole of the reported symptom.
- **Moving the MCP server itself onto a worker thread** — rejected. It would
  require marshalling every `ClaudeIntegration` state read, where this design
  marshals seven call sites.
- **Pumping the event loop during a verb** — forbidden, permanently. That is
  the nested-loop socket use-after-free class ANTS-2131 closed. Not deferred
  work; a boundary. No id.

## 6. Tests

New feature test `tests/features/mcp_async_dispatch/`, label `features;fast`,
covering INV-1, INV-2, INV-4, INV-5, INV-8 and INV-10. INV-8's disconnect case
also runs under `ctest --preset=debug` (ASan) because its failure mode is a
use-after-free rather than a wrong value.

`tests/features/mcp_verb_offthread_guard/` is rewritten for INV-3, INV-6, INV-7
and INV-9. Its current INV-1/INV-2/INV-3 assert the `rcDelegateWorker` factory
exists and that named verbs register through it; that factory is deleted by
§ 2.4, so those assertions are replaced rather than amended, and its `spec.md`
loses the GUI-responsiveness claim § 1 disproves.

Per the project test convention, add each source to the owning bundle's
`SOURCES` (ask `build_target_for`, do not guess), verify the ctest count moved
with `ctest -N -R`, and verify each test fails against pre-change source before
the change is restored.

## 7. Cross-doc impact

- `tests/features/mcp_verb_offthread_guard/spec.md` — § Mechanism's
  GUI-responsiveness claim is false and is corrected; INV-1..INV-3 replaced.
- `src/mainwindow.cpp` — the `rcDelegateWorker` comment repeats the same false
  claim and goes with the factory.
- `docs/standards/mcp-tools.md` — the authoring checklist gains the eligibility
  rule of § 2.4, so a new verb's author knows which thread it runs on.
- `docs/standards/mcp-error-codes.md` — `dispatch_queue_full` joins the
  taxonomy.
- `CHANGELOG.md` — user-visible: the window no longer freezes during MCP calls.
- ROADMAP.md — ANTS-2132 to 🚧 on start; ANTS-4682 filed for the deferred
  inline verbs.

## Cold-eyes loop log

<one row per review loop; written as the loops happen, never back-filled>
