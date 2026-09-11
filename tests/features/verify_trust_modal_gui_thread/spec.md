# Feature: VerifyTrust modal body runs on the GUI thread, and a refused marshal skips it

## Problem

`VerifyTrust::ModalClient::prompt()` (`src/verifytrustmodal.cpp`) is the hook
`FilePersistedTrustClient::outcomeForConfig()` calls when neither the SHA nor
the repo is already trusted. `prompt()` calls `showPrompt()` — the method that
builds the `QMessageBox` — directly, on whatever thread called `prompt()`.

`verify_changes` is registered through `rcDelegate` with a `Required`
contract, so ANTS-2132 dispatches it on the shared MCP worker thread, not the
GUI thread (`docs/specs/ANTS-2132-async-mcp-dispatch.md`). An untrusted
`.ants/verify.json` therefore reaches `outcomeForConfig()` — and `prompt()` —
from that worker. `showPrompt()` constructs a `QMessageBox` there. Building a
`QWidget` off the GUI thread is undefined behaviour in Qt (most likely a
crash), and `QMessageBox::exec()` spins its own event loop on the worker,
which can service queued MCP jobs out of order and break ANTS-2132 INV-2.

This also breaches `docs/specs/ANTS-1337.md` INV-8 ("Modal runs on main
thread"), which was written and verified when every caller of `prompt()` ran
on the GUI thread and stopped holding the moment `verify_changes` moved
off it.

Filed as ANTS-5025 (`code-quality-review-2026-09-11` perf pass, found
independently by two lanes). Not reproduced live — a repro would crash or
hang a live instance, and reproducing it needs a throwaway `--e2e` instance,
which this test does not use (see Scope).

## The fix this test locks

`ants::onGuiThread` (`src/guithread.h`) already exists for exactly this
shape: it marshals a callable onto the GUI thread via
`QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` when called
from elsewhere, calls it directly when already on the GUI thread, and returns
`std::nullopt` — without invoking the callable at all — when
`ants::guiMarshalRefused()` is true and the caller is not already on the GUI
thread. The fix is for `ModalClient::prompt()` to run `showPrompt()` through
it and translate a refused marshal into `Decision{Outcome::Headless,
shaHex}`, mirroring how `FilePersistedTrustClient::prompt()` already treats
"no GUI available" as `Headless` (`src/verifytrust.h` § `prompt` doc comment).

This test does not touch `prompt()` or `showPrompt()` — both stay exactly as
written; only their calling convention is checked from outside.

## Invariants

- **INV-1 — the dialog body runs on the GUI thread.** When
  `outcomeForConfig()` is called from a thread other than the GUI thread, and
  reaches `prompt()` (SHA and repo both untrusted, no session-cache hit), the
  code that builds the dialog (`showPrompt()`) executes on the GUI thread —
  never on the calling worker thread. *Test:* `verify_trust_modal_gui_thread`
  (paired test file).
- **INV-2 — a refused GUI marshal yields `Headless` without running the
  body.** When `ants::guiMarshalRefused()` is true and `outcomeForConfig()`
  is called from a thread other than the GUI thread, the call returns
  `Decision{Outcome::Headless, shaHex}` and `showPrompt()` is never invoked.
  *Test:* `verify_trust_modal_gui_thread` (paired test file).

## Scope

In scope: the calling-thread contract between `outcomeForConfig()` /
`prompt()` and `showPrompt()` — where the dialog body runs, and what happens
when the GUI is unreachable. Both invariants are exercised through a
`ModalClient` subclass that overrides `showPrompt()` to record its calling
thread and return a canned `Decision` without ever constructing a
`QMessageBox`, so the test never blocks on a real modal.

Out of scope:
- The dialog's visual content and button wiring (`commandPreview`,
  `shortSha`, the three-button layout) — untouched by this fix, not
  re-tested here.
- The trust-persistence side effects of `addTrustedSha` /
  `addTrustedRepo` — covered by `tests/features/verify_trust_gate/`.
- A live crash/hang repro via a real `--e2e` instance and a real
  `verify_changes` MCP call — the ANTS-5025 roadmap entry says a repro
  would crash or hang that instance; this test reaches the same code path
  synchronously, from a plain `std::thread`, instead.
- `ants::onGuiThread`'s own contract (nullopt on no app, direct call when
  already on the GUI thread, deadlock avoidance while the dispatch worker is
  parked in `wait()`) — owned by `docs/specs/ANTS-2132-async-mcp-dispatch.md`
  and its tests.

## Test

`test_verify_trust_modal_gui_thread.cpp`, in the `test_chrome` bundle (needs
`ants_chrome_lib` for `VerifyTrust::ModalClient`). A `RecordingModalClient`
subclass overrides `showPrompt()` to store `QThread::currentThread()` and a
call count, then returns a canned `Decision` — no `QMessageBox` is ever
constructed, so nothing blocks on user input.

Each invariant spawns a `std::thread` that calls `outcomeForConfig()` on a
fresh `FilePersistedTrustClient` (temp-file backed, so no real
`~/.config/ants-terminal/verify-trust.json` is touched) with a config byte
string unique to that test, while the GUI/main thread pumps
`QCoreApplication::processEvents(QEventLoop::AllEvents, …)` in a loop bounded
at 5 seconds — the same technique `tests/features/mcp_async_dispatch/`'s
`callVerb()` uses to service a cross-thread `BlockingQueuedConnection` reply,
because the `test_chrome` bundle's `QApplication` never runs `exec()`
(`tests/bundle_main_gui.cpp`). If the worker has not finished inside the
bound, the test fails with a diagnosable message instead of joining
unboundedly — a hang must not join the test process to page-out.

INV-2's `ants::guiMarshalRefused()` flag is process-global
(`src/guithread.h`); the test sets it through an RAII guard that resets it to
`false` on every exit path, including an early `ASSERT_*` return.

## Regression history

- **ANTS-1337 (design, INV-8):** every caller of `prompt()` ran on the GUI
  thread by construction — the QLocalServer's `newConnection` handler and
  every per-request handler ran there. INV-8 held trivially.
- **ANTS-2132:** moved `Required`-contract verbs, including `verify_changes`,
  onto a shared worker thread, without touching `VerifyTrust::ModalClient`.
  INV-8 silently stopped holding; nothing caught it because no test called
  `outcomeForConfig()` from a non-GUI thread.
- **ANTS-5025 (2026-09-11, still open at authoring time):** found by review,
  not by a live crash. `ModalClient::prompt()` still calls `showPrompt()`
  directly as of this writing — this test is written to fail against that
  code and pass once `prompt()` marshals through `ants::onGuiThread`.
