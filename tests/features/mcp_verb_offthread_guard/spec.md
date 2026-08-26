# ANTS-2132 — the async-dispatch structure holds

## Background

MCP requests used to run on the GUI thread, so the window stopped painting
for the duration of every verb. ANTS-2132 dispatches the eligible ones on one
shared worker; `docs/specs/ANTS-2132-async-mcp-dispatch.md` is the contract.

This file supersedes the ANTS-2131 version of the same test, which asserted
that named verbs registered through an `rcDelegateWorker` factory. That
factory ran the verb on a short-lived thread and then `wait()`-joined it, and
its comment claimed the join bought GUI responsiveness. **It did not** — a
join blocks the calling thread for the whole call. What worker-isolation
bought was the absence of an event pump, which is the ANTS-2131 use-after-free
fix and is unrelated. The factory is deleted: under the new design it would be
byte-for-byte `rcDelegate`.

## Mechanism

`registerToolProvider` marks a verb off-thread when it was built by the
`rcDelegate` factory and its contract is not `TabSpecific`. `onMcpConnection`
hands a marked verb to `postToolDispatch`, which queues it on the worker; the
worker runs the handler and marshals the result back, and
`finishToolDispatch` — the one response pipeline, shared with the synchronous
path — writes the reply from the GUI thread. A verb body that reaches into
`MainWindow` marshals that read through `ants::onGuiThread` (`src/guithread.h`).

## Contract

Four invariants of the spec, each a claim about where code **is**. The runtime
observations — a GUI tick during a verb, arrival order, the queue cap, the
disconnect and shutdown cases — are `tests/features/mcp_async_dispatch/`,
which drives a real `ClaudeIntegration`. A scrape cannot hold those: a grep
for a queued connection passes against code that still joins.

- **INV-3** — the dispatch path spins no nested `QEventLoop` on the GUI thread
  (ANTS-2131 preserved), and the `rcDelegate` factory neither pumps nor joins.
  The `rcDelegateWorker` factory stays deleted.
- **INV-6** — no verb body reaches `MainWindow` except through
  `ants::onGuiThread`, unless it always runs on the GUI thread. The exempt set
  is **derived from the registration table**, never listed here: a body is
  exempt only while its verb is not registered off-thread — `TabSpecific`,
  registered through an inline lambda, or a `--remote` CLI verb that is not
  MCP-registered at all. Move one to `rcDelegate` and its unwrapped reads fail.
  The scrape matches the `->` arrow rather than accessor names, so a new
  `MainWindow` accessor cannot be added without this test noticing.
- **INV-7** — the GUI thread never blocks on the dispatch worker while serving
  a request. `shutdownDispatchWorker`'s join is the sole exception, it is the
  only join in the file, and only the destructor reaches it.
- **INV-9** — one `finishToolDispatch` definition, the wrap lives inside it,
  and the dispatcher keeps no second copy. Two pipelines would let the
  synchronous and deferred replies drift apart.
- **INV-11** — an INLINE handler registered off-thread reads no `MainWindow`
  member except through `ants::onGuiThread`. INV-6 cannot hold this: its
  scrape covers `remotecontrol*.cpp` and matches `m_main->`, while an inline
  handler lives in `mainwindow.cpp` and reaches `MainWindow` through its own
  members. That gap was inert while every inline verb stayed on the GUI
  thread, and stopped being inert when ANTS-4682 moved six of them off it.
  The subject is **derived** the same way INV-6's is — every
  `ClaudeIntegration::RcHandler{` registration in that file — so moving one
  more inline verb off-thread enrols it here rather than needing a list
  edited. Passing a bare `this` to a free function that marshals internally
  (`ants::resolveCallerCwdRoot`) is not a member read and is not matched.

## Out of scope

- `audit_run` / `audit_poll` / `indie_review_dispatch` — still synchronous,
  still freeze the window for a sweep. ANTS-4682 audited all fourteen inline
  handlers and moved the GUI-free ones; these three keep a GUI-thread job
  registry and build their own workers, so they were recorded as staying
  rather than moved, and the § 5 tree-access hazard is **not** closed for
  them. Their nested loops are locked by
  `tests/features/socket_readyread_uaf_guard/` (ANTS-2102).
- `get_git_status` — also stays. It returns a plain text blob with no refusal
  channel, so a refused marshal could not be distinguished from "no
  terminal"; moving it needs an envelope first.
- INV-1, INV-2, INV-4, INV-5, INV-8, INV-10 — runtime, and owned by
  `tests/features/mcp_async_dispatch/`.

## Test

`test_mcp_verb_offthread_guard.cpp` — source scrape over `mainwindow.cpp`,
`claudeintegration.cpp` and every `RemoteControl` TU (`SRC_MAINWINDOW_CPP_PATH`,
`SRC_CLAUDE_INTEGRATION_CPP_PATH`, `ANTS_RC_SOURCES`, supplied by the
`test_claude` bundle).
