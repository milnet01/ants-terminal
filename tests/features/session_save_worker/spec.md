# Feature: a changed tab's session is compressed, hashed and written off the GUI thread

## Invariants

**INV-1 — an async save round-trips.** `SessionManager::saveSessionAsync`
followed by `waitForPendingSaves` leaves a blob that `loadSession` restores
to the same scrollback, cwd and pinned title.

**INV-2 — saves of one tab land in call order.** A synchronous
`saveSession` issued straight after an async save of the same tab wins:
the synchronous save waits for the async one first.

**INV-3 — a removed session stays removed.** `removeSession` waits for an
async save in flight, so that save cannot put the file back.

**INV-4 — an overshoot writes nothing and is reported once.** When the
compressed blob is over its file cap and scrollback was kept, the async
save leaves the prior blob in place. `takeOvershoot` returns true for that
tab once, then false.

**INV-5 — the blob is private without touching the umask.** The written
blob is mode 0600, and the process umask is unchanged by a save.

**INV-6 — the durability order is unchanged.** Source-scrape of
`SessionManager::writeBlob`: `fsync` of the temp file, then `std::rename`,
then the post-rename `setOwnerOnlyPerms(path)`, then `fsyncParentDir`.
`saveSessionAsync` writes through `writeBlob` and `seal`, and never calls
`umask`.

**INV-7 — the forced save stays synchronous.** Source-scrape of
`MainWindow::saveAllSessions`: a forced save, and a tab whose async save
overshot, go through `SessionManager::saveSession`.

## Rationale

ANTS-5131. Measured on `tests/perf/bench_session_save`, compression was
about half of a changed tab's save, all of it on the GUI thread. The grid
walk still runs on the GUI thread, because it reads the live grid.
The overshoot fallback and the synchronous close were decided by the user
on 2026-09-14.

## Test surface

`test_session_save_worker.cpp`: behavioural against `SessionManager` and
`TerminalGrid` under `XdgGuard`, plus source-scrapes of
`src/sessionmanager.cpp` and `src/mainwindow.cpp`.

## Regression history

- **ANTS-5131:** the save compressed, hashed and wrote on the GUI thread.
