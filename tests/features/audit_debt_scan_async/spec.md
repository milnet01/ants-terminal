# Feature: the Debt Sweep scan runs off the GUI thread, not synchronously on every click (ANTS-5057)

## Contract

`AuditDialog::debtScan()` calls `DebtSweepEngine::scanAll(m_projectPath, opt)`
synchronously. `scanAll` reads the whole project tree into one string, scans
that string once per comment token, runs one `git blame` per file holding a
TODO (30 s timeout each, no total bound), and runs the packaging script.
`onDebtScanClicked()` calls `debtScan()` directly after one
`QApplication::processEvents()` — the window is frozen for the scan's whole
duration, whatever that duration turns out to be (not measured). Worse: the
`ants-debt-fix` and `ants-debt-allow` branches of `onDebtAnchorClicked()`
each call `debtScan(); renderDebtResults();` again after a single fix or
allowlist click, so the whole sweep re-runs on every one of what could be
many clicks in a session.

The MCP path to the same engine was already moved off the GUI thread; this
one, reachable only from the Audit dialog's Debt Sweep tab, was not.

The fix has one part this spec locks (the threading half; token-lookup
memoisation is a second, independent part the roadmap item also asks for and
this spec does not pin): `DebtSweepEngine::scanAll` runs inside a
`QThread::create` worker — the idiom `MainWindow`, `LuaEngine` and
`cmdSessionOrient` already use (`Qt6::Concurrent` is not linked) — the result
is posted back to the dialog and filtered/rendered there, a scan requested
while one is running is coalesced rather than started in parallel, and
`QApplication::processEvents()` is gone from the scan path.

## Rationale

Found by two lanes (audit, threading). `AuditDialog` is a `QDialog`; this
project's house pattern for its invariants is source-scrape (see
`audit_dialog_render_hardening`, `audit_tool_process_group_kill`,
`audit_blame_bulk_async`), not construction, so every invariant below is a
scrape of `src/auditdialog.cpp` with comments stripped.

## Invariants

- **INV-1** — no GUI-thread path calls `DebtSweepEngine::scanAll(` outside a
  `QThread::create` worker. For every occurrence of `scanAll(` in the
  comment-stripped file, the nearest preceding `QThread::create(` must open
  a brace scope that is still unclosed at the point of the call — i.e. the
  net `{`/`}` balance from that `QThread::create(` up to the `scanAll(` call
  is at least +1, meaning the call sits inside the worker lambda's body
  rather than after it has already closed. This is a brace-balance check
  rather than a fixed-byte window because the lambda body can grow; it
  tolerates any correct async implementation using the project's own
  `QThread::create` idiom without pinning a specific variable name or
  capture list. Today there is no `QThread::create(` in the file at all, so
  this fails outright rather than merely landing outside a lambda.
  *Test:* `test_audit_debt_scan_async.cpp`, `ScanAllOnlyCalledInsideWorker`.

- **INV-2** — `onDebtScanClicked()`'s body contains no `processEvents`. A
  worker-thread scan needs no manual event-pump to keep the window painting;
  its presence is a direct signal that the caller is still stalling the GUI
  thread waiting on the scan to finish before doing anything else.
  *Test:* `test_audit_debt_scan_async.cpp`, `NoProcessEventsInScanClickHandler`.

- **INV-3** — neither `onDebtScanClicked()` nor the `ants-debt-fix` /
  `ants-debt-allow` branches of `onDebtAnchorClicked()` call a synchronous
  `debtScan()` immediately followed by `renderDebtResults()` with no async
  boundary between them. Per region (the click handler's body, and each of
  the two URL-scheme branches inside `onDebtAnchorClicked()`'s body), the
  check finds `debtScan()` and, later in the same region, `renderDebtResults()`,
  then inspects the text *between* the two calls: if neither `QThread` nor
  `connect(` appears in that span, the pairing is treated as the
  scan-then-render-on-the-spot idiom the defect describes and the test
  fails. The two markers are the project's own async idiom (`QThread::create`
  paired with a `connect(...)` to the worker's completion signal) — their
  absence between the calls means nothing interrupts the synchronous
  sequence, whatever the exact statements around it look like (an
  intervening `if`/`else` or closing brace doesn't change that). This
  admits any correct implementation that defers the render to a completion
  callback, however that callback is wired, without requiring a specific
  method name to survive the refactor — the roadmap item's own fix note
  doesn't commit to renaming `debtScan()` or `renderDebtResults()`.
  *Test:* `test_audit_debt_scan_async.cpp`, `NoSyncScanThenRenderInScanClickHandler`,
  `NoSyncScanThenRenderInFixBranch`, `NoSyncScanThenRenderInAllowBranch`.

- **INV-4** (guard, expected green today and after the fix) — the allowlist
  filter still applies to the scan result: `allowlisted(debtToAuditFinding(`
  appears in the file. This pins that the threading fix doesn't drop the
  filtering step described in the roadmap item's own "filtered/rendered
  there" — it is not the defect this spec is about, but a fix that moves
  the filter into the wrong place (or drops it) would be worth catching as
  a side effect of this same scrape.
  *Test:* `test_audit_debt_scan_async.cpp`, `AllowlistFilterStillApplied`.

## Scope

### In scope
- The GUI-thread call sites reachable from the Debt Sweep tab:
  `AuditDialog::debtScan()`, `AuditDialog::onDebtScanClicked()`, and the
  `ants-debt-fix` / `ants-debt-allow` branches of
  `AuditDialog::onDebtAnchorClicked()` — all in `src/auditdialog.cpp`.
- Whether the scan and its re-render run off the GUI thread.

### Out of scope
- **Token-lookup memoisation per run** — the roadmap item's second fix part.
  Nothing here pins whether `DebtSweepEngine::scanAll`'s comment-token scan
  is memoised; that is a performance characteristic of the engine, not a
  threading contract of the dialog, and needs its own spec if it's locked.
- **The `ants-debt-defer` branch** of `onDebtAnchorClicked()` — it never
  calls `debtScan()` synchronously (it removes the deferred finding from
  the in-memory list directly), so it isn't part of the defect.
- **How the async result is delivered** (signal, callback, future) — INV-1
  and INV-3 tolerate any implementation using the project's `QThread::create`
  + `connect(...)` idiom; the exact wiring shape isn't pinned.
- **`DebtSweepEngine::scanAll`'s own internals** (the git-blame-per-file
  timeout, the packaging-script invocation) — unrelated to this defect,
  which is about *where* (which thread) the call runs, not what it does.
- **`AuditDialog`'s behaviour end to end** (a `QDialog`; house pattern is
  source-scrape, not construction).

## Regression history

- **ANTS-5057 (found by two lanes: audit, threading):** `AuditDialog::debtScan()`
  calls `DebtSweepEngine::scanAll()` synchronously on the GUI thread, and
  `onDebtScanClicked()` plus the fix/allow branches of `onDebtAnchorClicked()`
  each re-run the whole sweep after a single click, freezing the window each
  time. This spec locks the contract before the fix lands; all three of
  INV-1 through INV-3 are expected to fail against the current tree — INV-1
  because the file has no `QThread::create(` at all, INV-2 because
  `onDebtScanClicked()` still calls `processEvents()`, and INV-3 because
  `onDebtScanClicked()` calls `debtScan(); renderDebtResults();` back to
  back with nothing async between them (and the fix/allow branches show the
  same pairing across an intervening `if`/`else`). INV-4 is expected to pass
  already — the allowlist filter is present in today's `debtScan()`.
