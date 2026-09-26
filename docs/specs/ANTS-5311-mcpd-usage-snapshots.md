# ANTS-5311 — Count the calls `ants-mcpd` serves in `token_usage` and the savings chip

**Status:** accepted (2026-09-26), review-contract loops 1 + 2 folded, cap reached.
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-5311 (review-contract-ANTS-4932-loop-2; deferred from `docs/specs/ANTS-4932-standalone-mcp-server.md` § 2.3).
**Composes with:** ANTS-1284 (`token_usage`), ANTS-3572 (saved-token aggregate + chip), ANTS-3579 (per-project chip).

**Layman:** The separate MCP helper now answers most requests, so the "tokens saved" numbers must count its work too, not only the few requests the terminal answers itself.

## 1. Problem

`ants-mcpd` (`src/mcpdmain.cpp`, `main`) builds its own `ClaudeIntegration`, so every call it serves in-process reaches `ClaudeIntegration::recordDispatch` and lands in that process's `m_tokenUsage` and `m_sessionSavedBytesByProject`. Nothing reads them:

1. `token_usage` (`RemoteControl::cmdTokenUsage`) reports only the terminal's own tracker and `MainWindow::tokenSavingsSummary()`. The ANTS-4932 spec § 2.3 records this as the deferral this item closes.
2. The chip (`ClaudeStatusBarController::refreshTokensSavedChip`) reads the terminal's `sessionSavedBytesForProject` and `tokenUsageReport` only.
3. Nothing persists the helper's totals. `MainWindow::foldTokenSavingsIntoConfig` folds the terminal's session only, and `ants-mcpd` holds `Config` read-only (`const Config config` in `main`), so its counts vanish on exit.

Forwarded calls are not affected. A successful forward relays the terminal's reply without calling `recordDispatch` in `ants-mcpd`, and the terminal counts it. Each call is counted in exactly one process today, and this spec keeps that.

## 2. Surface

### 2.1 Design choice

Each `ants-mcpd` writes a **snapshot file** of its own counters. The terminal **reads every snapshot** for display, and **folds a snapshot into config once its writer has exited**.

Rejected: a report channel from `ants-mcpd` to the terminal socket. It loses counts whenever no terminal is running. It also adds a terminal-side verb that calls into `MainWindow`, and the project's hot-reload rule asks new verbs not to grow that seam.

Rejected: both processes writing one shared ledger. `JsonlFile::writeLinesAtomic` and `Config::save` rewrite the whole file, so two writers would need a lock around every write. One file per writer needs none.

### 2.2 Location and naming

```
<GenericDataLocation>/ants-terminal/mcpd-usage/        mode 0700
    <pid>-<start_unix_ms>.json                         mode 0600, the snapshot
    <pid>-<start_unix_ms>.lock                         mode 0600, the liveness lock
```

`GenericDataLocation + "/ants-terminal"` is the root `RoadmapStore` and `debuglog.cpp` already use. The start time in the stem stops a reused pid from overwriting an unfolded snapshot left by a dead process.

### 2.3 Snapshot format

```json
{
  "format": 1,
  "pid": 12345,
  "started_unix_ms": 1790000000000,
  "updated_unix_ms": 1790000123456,
  "tools": {
    "<tool name>": {
      "n_calls": 0, "bytes_in": 0, "bytes_out": 0, "wrap_bytes": 0,
      "duration_us_min": 0, "duration_us_max": 0, "duration_us_sum": 0,
      "failed_calls": 0, "failed_bytes_in": 0, "failed_bytes_out": 0
    }
  },
  "saved_bytes_by_project": { "<canonical root>": 0 }
}
```

`tools` is `TokenUsageEngine::ToolCounter` field for field. The total saved is not stored. The reader derives it **per snapshot**, with `Tracker::totalSaved()` over that one file's counters, and never over counters merged across files. `buildReport` floors each tool's saving at zero, so a merged total differs from the sum of per-file totals. The per-file total is the one that can be folded without moving the display (INV-4).

### 2.4 Writer (`ants-mcpd`)

- At start, `main` creates the directory (mode 0700), opens `<stem>.lock` with `O_CLOEXEC`, and takes `flock(LOCK_EX)`. It keeps that descriptor open until exit. `O_CLOEXEC` stops a child process (`rg`, `git`) from inheriting the lock and making a dead helper look live. It then writes an empty snapshot at once, so a lock with no json exists only between two syscalls.
- On `ClaudeIntegration::tokensSavedUpdated`, a single-shot 2 s `QTimer` is started if it is not already active. It is never restarted, so a busy helper still writes every 2 s. On timeout and on `QCoreApplication::aboutToQuit`, `main` writes the snapshot with `QSaveFile` (write, then atomic rename).
- `main` turns SIGTERM and SIGINT into `QCoreApplication::quit()` through a self-pipe and a `QSocketNotifier`, so a client that stops the server by signal still gets the final write.
- `ClaudeIntegration::initialize` calls `endTokenSession`, which resets the tracker. On `tokenSessionEnding`, emitted before that reset, the writer adds the ending session's counters and project bytes into a process-lifetime accumulator. The snapshot is always accumulator plus live, so a second `initialize` in one process loses nothing. The accumulator holds at most `kMaxTokenProjects` roots in total: to admit a new root when full, it drops the root with the fewest bytes.
- New engine API, in `src/tokenusageengine.{h,cpp}` (`ants_mcpcore_lib`, Qt6::Core only):

```cpp
namespace TokenUsageEngine {
QString peerSnapshotDir();   // GenericDataLocation + "/ants-terminal/mcpd-usage"
QJsonObject snapshotToJson(const QHash<QString, ToolCounter> &tools,
                           const QHash<QString, qint64> &savedBytesByProject,
                           qint64 pid, qint64 startedMs, qint64 updatedMs);
// The inverse, with § 2.6's checks. False and `why` set on any rejection.
bool snapshotFromJson(const QJsonObject &o, QHash<QString, ToolCounter> *tools,
                      QHash<QString, qint64> *savedBytesByProject, QString *why);
// Field-wise sum; min/max combine. Used only by the writer's accumulator.
void addCounters(QHash<QString, ToolCounter> &into,
                 const QHash<QString, ToolCounter> &from);
class Tracker {
public:
    const QHash<QString, ToolCounter> &counters() const { return m_counters; }
    // …existing members unchanged
};
}
```

- The project bytes come from the existing `ClaudeIntegration::sessionSavedBytesByProject()`, and the tools from a new `ClaudeIntegration::tokenUsageCounters()` returning `m_tokenUsage.counters()`.

### 2.5 Reader (terminal)

```cpp
namespace TokenUsageEngine {
// The PEER TOTAL. Every figure is a sum of per-snapshot figures, each derived
// from that snapshot alone (§ 2.3), so removing one snapshot removes exactly
// its own addends.
struct PeerUsage {
    qint64 savedTokens = 0;                    // Σ each snapshot's totalSaved()
    QHash<QString, qint64> savedTokensByProject; // Σ each snapshot's bytes / kCharsPerToken, per root
    qint64 calls = 0;                          // Σ n_calls
    qint64 failedCalls = 0;                    // Σ failed_calls
    int         sessions = 0;                  // snapshots read
    QStringList stems;                         // the stems summed here
    QStringList skipped;                       // file names not read, with the reason
};
// Read every snapshot. When `claimDead` is set, each snapshot whose lock can be
// taken (its writer has exited) is ALSO summed into `dead`, with its lock held
// in `heldLocks` until the caller has folded it and calls releaseClaimed().
PeerUsage readPeerSnapshots(const QString &dir, bool claimDead,
                            PeerUsage *dead, QList<int> *heldLocks);
void releaseClaimed(const QString &dir, const PeerUsage &dead,
                    const QList<int> &heldLocks);   // unlink json + lock, close fds
// Fold `dead` into the three config values, through the existing
// foldMonthlyBucket / foldProjectBucket / pruneProjectBuckets. Pure: no Config,
// no I/O, so the fold is testable without a MainWindow. Returns whether any
// value changed.
bool foldPeerUsage(const PeerUsage &dead, QJsonObject &monthly, qint64 &lifetime,
                   QJsonObject &byProject, const QString &month,
                   const QString &nowIso);
}
```

- **Display** reads every snapshot, live and dead-unfolded alike (`claimDead = false`). A dead snapshot therefore stays in the numbers until it is folded, and a fold moves it from "session" to "stored" without changing any displayed total.
- **Liveness** is `flock(LOCK_EX | LOCK_NB)` on `<stem>.lock`. Success means the writer is gone. `EWOULDBLOCK` means it is live, and it is never folded or deleted. A json with no lock file is dead. A lock file with no json is touched only when it is over 60 s old and its lock can be taken: then it is a writer that died between its two first syscalls, and the reader removes it. A younger one may be a writer about to write.
- **Fold.** `MainWindow::foldTokenSavingsIntoConfig` calls `readPeerSnapshots(dir, true, &dead, &locks)`. It passes `dead` to `foldPeerUsage` over the values it already folds, and the result shares the same single `m_config.save()`. Then it calls `releaseClaimed`. The terminal's own session is folded exactly as today, separately from the peers. That fold's triggers are `initialize`, quit and `reset`, and `mcpd::Forwarder` sends the terminal only `tools/call` lines, never `initialize`. So a peer-only fold, `MainWindow::foldDeadPeers()`, also runs on every watcher event and once at construction after `m_claudeIntegration` exists: claim, `foldPeerUsage`, `m_config.save()` only when a value changed, `releaseClaimed`. A helper's final write lands as a rename in the directory, so its snapshot is folded moments after it exits, not at the terminal's next quit.
- **Claiming is once-only across processes.** After taking a lock, the reader re-reads the json under it and skips the stem if the json is gone, since another reader released it. A json with no lock file is claimed by an atomic rename to `<stem>.claimed-<pid>`, and only the reader whose rename succeeds folds it.
- **`token_usage`.** `cmdTokenUsage` calls `readPeerSnapshots` **once** and passes that `PeerUsage` to `MainWindow::tokenSavingsSummary(const PeerUsage &)`, so both uses see one read. `calls[]`, `total_saved` and `tools_called` stay the terminal's own session, unchanged, so ANTS-1284's `total_saved` = Σ `calls[].est_tokens_saved` still holds. The envelope gains `mcpd: {sessions, calls, failed_calls, total_saved}` from the `PeerUsage`, plus `snapshots_skipped` when non-empty. The summary's `session` term becomes the terminal's session plus `PeerUsage::savedTokens`.
- **Chip.** `refreshTokensSavedChip` reads once per refresh. It adds `PeerUsage::savedTokensByProject[root]` to the per-project session tokens, after the terminal's own bytes are divided, and `PeerUsage::savedTokens` to `globalSession`. The terminal creates the directory (mode 0700) before watching it, and a `QFileSystemWatcher` on it calls `refreshTokensSavedChip`, because the terminal's own `tokensSavedUpdated` never fires for a call the helper served.

### 2.6 Trust boundary

The directory sits in the user's own data dir, and a snapshot is written by another process of the same user. The reader still treats every file as untrusted input:

- It reads only regular files matching `^[0-9]+-[0-9]+\.json$`, and never follows a symlink (`QFileInfo::isSymLink` is checked first).
- It skips a file over 256 KiB, one that fails to parse, one with `format != 1`, and any counter that is negative. A skipped file is named in `skipped` and is **never folded or deleted**.
- It rejects a file with more than 64 roots or more than 512 tool entries (`TokenUsageEngine::kMaxPeerProjects`, `kMaxPeerTools`) as `skipped`, never truncating it. Both caps are set here, not measured. 64 matches the writer's cap, `ClaudeIntegration::kMaxTokenProjects`, which is private and so cannot be named from the engine, so a well-formed snapshot never reaches it.

## 3. Invariants

- **INV-1** — A snapshot round-trips: `snapshotToJson` → `snapshotFromJson` returns the same `ToolCounter`s and project bytes that were written. Broken by a field missed in either direction. *Test:* `tests/features/mcpd_usage_snapshots` `RoundTrip`.
- **INV-2** — A snapshot whose lock is held by another open file description is read but never returned in `dead`, and its files survive `releaseClaimed`. Broken by folding without testing the lock. *Test:* `tests/features/mcpd_usage_snapshots` `LiveSnapshotNotClaimed` — the test holds the lock on a second `open()`.
- **INV-3** — A dead snapshot folds exactly once: claim, `foldPeerUsage` and `releaseClaimed` add its totals to the three values and remove both files, and a second claim-and-fold over the same directory changes nothing. Broken by skipping the unlink (double count). *Test:* `tests/features/mcpd_usage_snapshots` `DeadSnapshotFoldsOnce`.
- **INV-4** — A fold moves a dead snapshot from "session" to "stored" without changing the sum, exactly: stored lifetime plus `savedTokens` of a `claimDead = false` read is the same before the fold and after it, and so is stored per-root lifetime plus `savedTokensByProject[root]`. Broken by deriving any figure over counters merged across files, or dividing bytes after summing them across files. *Test:* `tests/features/mcpd_usage_snapshots` `FoldPreservesDisplayedTotals` — a live and a dead snapshot sharing one tool, one net-positive and one net-negative against its baseline, and sharing one root with byte counts that are not multiples of `kCharsPerToken`.
- **INV-5** — A lock file with no json beside it is never locked, unlinked or counted while it is 60 s old or younger. An older one whose lock can be taken is removed. Broken by a reader that sweeps young lone locks, which would race a writer between its lock and its first write, or by one that never removes old ones. *Test:* `tests/features/mcpd_usage_snapshots` `LoneLockAge` — one lone lock with a fresh mtime survives, one set 120 s old is removed.
- **INV-6** — `token_usage` counts the helper's calls in its own block: with one snapshot holding `n_calls: 3`, `mcpd.calls` is 3 and `mcpd.sessions` is 1. `calls[]` and `total_saved` equal a run with no snapshot at all. Broken by reading only the terminal tracker, or by merging peer counters into `calls[]`. *Test:* `tests/features/mcpd_usage_snapshots` `TokenUsageIncludesPeers`.
- **INV-7** — A malformed, oversized, symlinked, wrong-format or negative-counter file is named in `skipped`, contributes nothing, and survives a fold. Broken by trusting the file. *Test:* `tests/features/mcpd_usage_snapshots` `UntrustedFilesSkipped`, one fixture per case.
- **INV-8** — A forward the terminal answered is counted in neither the helper's tracker nor its snapshot; the terminal counts it. A forward that failed (`no_terminal`) is counted by the helper as a failed call, since the terminal never saw it. Broken by recording before the forward decision. *Test:* `tests/features/mcpd_usage_snapshots` `ForwardedCallNotInSnapshot` — a `ClaudeIntegration` with a forwarder that replies, one call to a forwarded verb, then `tokenUsageReport(true).toolsCalled == 0` and `snapshotToJson` of its counters holds an empty `tools`; a second forwarder that fails gives one `failed_calls`.
- **INV-9** — The peer fold shares the existing single `m_config.save()` (ANTS-3572 INV-6): one fold with both a terminal session and a dead snapshot saves config once. Broken by a second save call. *Test:* source scrape in `tests/features/mcpd_usage_snapshots` `SingleConfigSave`: `foldTokenSavingsIntoConfig`'s body holds exactly one `m_config.save()`.
- **INV-10** — Two readers claiming one directory fold each dead snapshot once: while one claim holds a stem's lock, a second `readPeerSnapshots(dir, true, …)` returns nothing for it, and after `releaseClaimed` a claim over the same directory finds nothing. A lone json renamed by one claimant is not claimed by the other. Broken by claiming from the first read without re-reading under the lock. *Test:* `tests/features/mcpd_usage_snapshots` `ConcurrentClaimOnce` — two claims in one process, each on its own `open()`, which `flock` treats as two holders.

## 4. RAM / build cost

- **Writer:** one snapshot rewrite per 2 s of activity. A snapshot holds at most `kMaxTokenProjects` roots (§ 2.4's accumulator cap) and one entry per tool the tracker has seen, so it is small and bounded.
- **Reader:** display reads run on a watcher event or a `token_usage` call. They hold one `PeerUsage` briefly, bounded per file the same way.
- **Files:** one pair per helper session, from start until the next terminal fold. With no terminal running, pairs accumulate one per helper session. That is the accepted cost of not losing the counts, and the next terminal start folds them.
- No new build target. The new code joins `ants_mcpcore_lib` (engine) and existing GUI TUs (fold, chip).

## 5. Out of scope

- `token_usage reset` does not reset a live helper's counters. The terminal cannot reach them, and a helper session ends when its client disconnects. Permanent: resetting another process's counters would need the channel § 2.1 rejects.
- A crash between the config save and `releaseClaimed` folds that snapshot again on the next fold. Permanent: the window is two syscalls, and closing it would need a folded-id ledger in config.
- Up to 2 s of counts is lost if a helper is killed with SIGKILL. Permanent: SIGKILL cannot be caught. Stdin EOF, SIGTERM and SIGINT all reach `aboutToQuit` (§ 2.4).
- The token_usage `since` stays the terminal's session start. Deferred: nothing reads a per-snapshot start today.

## 6. Tests

Feature test: `tests/features/mcpd_usage_snapshots/`, with a `spec.md` pointing here, built into the `test_claude` bundle, because INV-6 drives `RemoteControl::cmdTokenUsage` with a null `MainWindow` and `test_claude` is the bundle that links `RemoteControl`. It covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8, INV-9 and INV-10. Label `features;fast`. Each test runs against a directory under the test's sandboxed `QStandardPaths`, never the real data dir. Verify each test fails against pre-fix source before the fix is restored, except INV-8 and INV-9, which hold of today's code and are regression guards. Prove each of those red by mutation instead: INV-8 by moving `recordDispatch` ahead of the forward branch, INV-9 by adding a second `m_config.save()`.

Manual recipe: run one Claude Code session through `ants-mcpd` and make a few `read_region` calls. The chip's session figure rises within about 2 s. `/exit` the session, relaunch the terminal, and the chip's all-time figure keeps the same total.

## 7. Cross-doc impact

- `docs/specs/ANTS-4932-standalone-mcp-server.md` § 2.3: the "Deferred — tracked by ANTS-5311" sentence becomes a pointer to this spec.
- `docs/specs/ANTS-3572.md` INV-8 forbids a new file watcher. § 2.5's watcher amends that clause: annotate it `amended by ANTS-5311` when this ships. Its other invariants, and `docs/specs/ANTS-3579.md`'s, stand; this spec adds a term to their session figures.
- `docs/specs/ANTS-1284.md` INV-6 describes a response schema with `additionalProperties: false`. None exists in source: no `outputSchema` is registered for `token_usage`, and ANTS-3572 INV-10 already added envelope fields without one. So `mcpd` and `snapshots_skipped` need no schema change. Annotate INV-6 as superseded by ANTS-3572 INV-10 when this ships.
- CHANGELOG bullet when it ships.

**Hot reload.** A change to the writer reaches users on an MCP reconnect (`/mcp`). The reader is compiled into the terminal, so it takes one relaunch to land. Until then, snapshots accumulate harmlessly and are folded on that start.

## Cold-eyes loop log

Moved to [`docs/reviews/ANTS-5311-mcpd-usage-snapshots-loop-log.md`](../reviews/ANTS-5311-mcpd-usage-snapshots-loop-log.md).
