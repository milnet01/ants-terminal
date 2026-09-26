# ANTS-5311 — Count the calls `ants-mcpd` serves in `token_usage` and the savings chip

**Status:** spec draft (2026-09-26).
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

`tools` is `TokenUsageEngine::ToolCounter` field for field. The total saved is not stored: the reader derives it with `Tracker::totalSaved()` over the merged counters, so it is stated once.

### 2.4 Writer (`ants-mcpd`)

- At start, `main` creates the directory, then opens `<stem>.lock` and takes `flock(LOCK_EX)`. It keeps that descriptor open until exit. **The json is written only after the lock is held.**
- On `ClaudeIntegration::tokensSavedUpdated`, a single-shot 2 s `QTimer` is (re)armed. On timeout and on `QCoreApplication::aboutToQuit`, `main` writes the snapshot with `QSaveFile` (write, then atomic rename).
- New engine API, in `src/tokenusageengine.{h,cpp}` (`ants_mcpcore_lib`, Qt6::Core only):

```cpp
namespace TokenUsageEngine {
QString peerSnapshotDir();   // GenericDataLocation + "/ants-terminal/mcpd-usage"
QJsonObject snapshotToJson(const QHash<QString, ToolCounter> &tools,
                           const QHash<QString, qint64> &savedBytesByProject,
                           qint64 pid, qint64 startedMs, qint64 updatedMs);
class Tracker {
public:
    const QHash<QString, ToolCounter> &counters() const { return m_counters; }
    void merge(const QHash<QString, ToolCounter> &other);  // field-wise sum;
                                                           // min/max combine
    // …existing members unchanged
};
}
```

- The project bytes come from the existing `ClaudeIntegration::sessionSavedBytesByProject()`, and the tools from a new `ClaudeIntegration::tokenUsageCounters()` returning `m_tokenUsage.counters()`.

### 2.5 Reader (terminal)

```cpp
namespace TokenUsageEngine {
struct PeerUsage {
    QHash<QString, ToolCounter> tools;             // summed over every snapshot read
    QHash<QString, qint64>      savedBytesByProject;
    int         sessions = 0;                      // snapshots read
    QStringList skipped;                           // file names not read, with the reason
};
// Read every snapshot. When `claimDead` is set, each snapshot whose lock can be
// taken (its writer has exited) is ALSO returned in `dead`, with its lock held
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
- **Liveness** is `flock(LOCK_EX | LOCK_NB)` on `<stem>.lock`. Success means the writer is gone. `EWOULDBLOCK` means it is live, and it is never folded or deleted. A json with no lock file is dead. A lock file with no json is never touched: that is a writer between taking its lock and its first write.
- **Fold.** `MainWindow::foldTokenSavingsIntoConfig` calls `readPeerSnapshots(dir, true, &dead, &locks)`. It passes `dead` to `foldPeerUsage` over the values it already folds, and the result shares the same single `m_config.save()`. Then it calls `releaseClaimed`. It also runs once at `MainWindow` construction, so snapshots left while no terminal ran are folded on the next start.
- **`token_usage`.** `cmdTokenUsage` merges `PeerUsage::tools` into a copy of the terminal's tracker before `buildReport`, so `calls[]`, `total_saved` and `tools_called` include the helper's calls. `MainWindow::tokenSavingsSummary` adds the peer total to its `session` term. The envelope gains `mcpd_sessions` (`PeerUsage::sessions`) and, when non-empty, `mcpd_snapshots_skipped`.
- **Chip.** `refreshTokensSavedChip` adds `PeerUsage::savedBytesByProject[root]` to the per-project session bytes, and the peer total to `globalSession`. A `QFileSystemWatcher` on the directory calls `refreshTokensSavedChip`, because the terminal's own `tokensSavedUpdated` never fires for a call the helper served.

### 2.6 Trust boundary

The directory sits in the user's own data dir, and a snapshot is written by another process of the same user. The reader still treats every file as untrusted input:

- It reads only regular files matching `^[0-9]+-[0-9]+\.json$`, and never follows a symlink (`QFileInfo::isSymLink` is checked first).
- It skips a file over 256 KiB, one that fails to parse, one with `format != 1`, and any counter that is negative. A skipped file is named in `skipped` and is **never folded or deleted**.
- It caps what one file contributes: the first 64 roots and the first 512 tool entries, as `TokenUsageEngine::kMaxPeerProjects` and `kMaxPeerTools`. Both caps are set here, not measured. 64 matches the writer's own live-map cap, `ClaudeIntegration::kMaxTokenProjects`, which is private and so cannot be named from the engine.

## 3. Invariants

- **INV-1** — A flushed snapshot round-trips: `snapshotToJson` → `readPeerSnapshots` returns the same `ToolCounter`s and project bytes that were written. Broken by a field missed in either direction. *Test:* `tests/features/mcpd_usage_snapshots` `RoundTrip`.
- **INV-2** — A snapshot whose lock is held by another open file description is read but never returned in `dead`, and its files survive `releaseClaimed`. Broken by folding without testing the lock. *Test:* `tests/features/mcpd_usage_snapshots` `LiveSnapshotNotClaimed` — the test holds the lock on a second `open()`.
- **INV-3** — A dead snapshot folds exactly once: claim, `foldPeerUsage` and `releaseClaimed` add its totals to the three values and remove both files, and a second claim-and-fold over the same directory changes nothing. Broken by skipping the unlink (double count). *Test:* `tests/features/mcpd_usage_snapshots` `DeadSnapshotFoldsOnce`.
- **INV-4** — A fold moves a dead snapshot from "session" to "stored" without changing the sum: stored lifetime plus the derived total of a `claimDead = false` read is the same before the fold and after it, globally and for the snapshot's project. Broken by counting a folded snapshot twice, or dropping an unfolded one from the display read. *Test:* `tests/features/mcpd_usage_snapshots` `FoldPreservesDisplayedTotals`.
- **INV-5** — A lock file with no json beside it is never locked, unlinked or counted. Broken by a reader that sweeps lone lock files, which would race a writer that has locked but not yet written. *Test:* `tests/features/mcpd_usage_snapshots` `LoneLockUntouched`.
- **INV-6** — `token_usage` counts the helper's calls: with one snapshot holding `n_calls: 3` for a tool, that tool's `calls[]` row and `total_saved` include them, and `mcpd_sessions` is 1. Broken by reading only the terminal tracker. *Test:* `tests/features/mcpd_usage_snapshots` `TokenUsageIncludesPeers`.
- **INV-7** — A malformed, oversized, symlinked, wrong-format or negative-counter file is named in `skipped`, contributes nothing, and survives a fold. Broken by trusting the file. *Test:* `tests/features/mcpd_usage_snapshots` `UntrustedFilesSkipped`, one fixture per case.
- **INV-8** — A forwarded call is counted in neither the helper's tracker nor its snapshot. Broken by recording before the forward decision. *Test:* `tests/features/mcpd_usage_snapshots` `ForwardedCallNotInSnapshot` — a `ClaudeIntegration` with a forwarder that replies, one call to a forwarded verb, then `tokenUsageReport(true).toolsCalled == 0`.
- **INV-9** — The peer fold shares the existing single `m_config.save()` (ANTS-3572 INV-6): one fold with both a terminal session and a dead snapshot saves config once. Broken by a second save call. *Test:* source scrape in `tests/features/mcpd_usage_snapshots` `SingleConfigSave`: `foldTokenSavingsIntoConfig`'s body holds exactly one `m_config.save()`.

## 4. RAM / build cost

- **Writer:** one snapshot rewrite per 2 s of activity. A snapshot holds at most `kMaxTokenProjects` roots (the live map's own cap) and one entry per tool the tracker has seen, so it is small and bounded.
- **Reader:** display reads run on a watcher event or a `token_usage` call. They hold one `PeerUsage` briefly, bounded per file the same way.
- **Files:** one pair per helper session, from start until the next terminal fold. With no terminal running, pairs accumulate one per helper session. That is the accepted cost of not losing the counts, and the next terminal start folds them.
- No new build target. The new code joins `ants_mcpcore_lib` (engine) and existing GUI TUs (fold, chip).

## 5. Out of scope

- `token_usage reset` does not reset a live helper's counters. The terminal cannot reach them, and a helper session ends when its client disconnects. Permanent: resetting another process's counters would need the channel § 2.1 rejects.
- A crash between the config save and `releaseClaimed` folds that snapshot again on the next fold. Permanent: the window is two syscalls, and closing it would need a folded-id ledger in config.
- Up to 2 s of counts is lost if a helper is killed with SIGKILL. Permanent: `aboutToQuit` covers every orderly exit.
- The token_usage `since` stays the terminal's session start. Deferred: nothing reads a per-snapshot start today.

## 6. Tests

Feature test: `tests/features/mcpd_usage_snapshots/`, with a `spec.md` pointing here, built into the `test_claude` bundle, because INV-6 drives `RemoteControl::cmdTokenUsage` with a null `MainWindow` and `test_claude` is the bundle that links `RemoteControl`. It covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7, INV-8 and INV-9. Label `features;fast`. Each test runs against a directory under the test's sandboxed `QStandardPaths`, never the real data dir. Verify each test fails against pre-fix source before the fix is restored.

Manual recipe: run one Claude Code session through `ants-mcpd` and make a few `read_region` calls. The chip's session figure rises within about 2 s. `/exit` the session, relaunch the terminal, and the chip's all-time figure keeps the same total.

## 7. Cross-doc impact

- `docs/specs/ANTS-4932-standalone-mcp-server.md` § 2.3: the "Deferred — tracked by ANTS-5311" sentence becomes a pointer to this spec.
- `docs/specs/ANTS-3572.md` and `docs/specs/ANTS-3579.md`: no change. This spec adds a term to their session figures and keeps their invariants.
- CHANGELOG bullet when it ships.

**Hot reload.** A change to the writer reaches users on an MCP reconnect (`/mcp`). The reader is compiled into the terminal, so it takes one relaunch to land. Until then, snapshots accumulate harmlessly and are folded on that start.

## Cold-eyes loop log

Moved to [`docs/reviews/ANTS-5311-mcpd-usage-snapshots-loop-log.md`](../reviews/ANTS-5311-mcpd-usage-snapshots-loop-log.md).
