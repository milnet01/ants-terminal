#pragma once

// VtStream — worker-thread wrapper around Pty + VtParser. Phase 1 of the
// 0.7.0 read/parse/render thread decoupling (ROADMAP ⚡).
//
// Ownership / threading model
// ---------------------------
// - The TerminalWidget creates a QThread, constructs a VtStream without a
//   parent, moves it to that thread, and keeps a pointer for queued-slot
//   invocations.
// - VtStream owns the Pty and the VtParser. Both live on the worker thread;
//   the PTY read loop (QSocketNotifier) services the forkpty master FD
//   there, never touching the GUI thread.
// - Parsed VtActions accumulate inside a VtBatch and are shipped to the
//   GUI thread via the `batchReady` signal over a Qt::QueuedConnection.
//   The GUI thread applies the batch to TerminalGrid and repaints.
// - Back-pressure: at most kMaxInFlight batches may be awaiting GUI
//   acknowledgement at once. Exceed that and the worker disables the
//   PTY read notifier until the GUI calls `drainAck()` on an applied
//   batch, which re-enables reads.
//
// Contract
// --------
// - Action-stream identity: for any byte sequence fed through the worker,
//   the sequence of VtActions delivered to `batchReady.actions` — in order,
//   concatenated across batches — is identical to what VtParser emits
//   when driven single-threaded. The `threaded_parse_equivalence` feature
//   test pins this.
// - Write ordering: every call to `write()` (user keystrokes, grid
//   response callback like DA1/CPR) serializes onto the worker's event
//   queue and writes to the PTY master in arrival order. Cross-thread
//   writes from GUI → worker preserve FIFO order via Qt's queued-connection
//   semantics.
// - Resize atomicity: `resize()` is invoked with Qt::BlockingQueuedConnection
//   so the PTY's winsize is updated before `resizeEvent` returns on the
//   GUI side. The next paint always sees consistent geometry.

#include "vtparser.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QElapsedTimer>
#include <QMetaType>

#include <memory>
#include <vector>

class Pty;

// One quantum of parse output shipped from worker → GUI. Carries both
// the structured action stream and the raw bytes (session logging and
// asciicast recording stay on GUI so file ordering remains trivially
// linear; the worker just hands over the bytes it already read).
struct VtBatch {
    std::vector<VtAction> actions;
    QByteArray rawBytes;
    // Set when the raw stream contained a full-screen clear sequence
    // (ESC[2J / ESC[3J / 0x0C). GUI uses this to clear the selection,
    // matching the pre-thread byte scan in TerminalWidget::onPtyData.
    bool clearSelectionHint = false;
    // Milliseconds elapsed since the worker started — sampled at flush
    // time. GUI uses this as the event timestamp for asciicast recording
    // (more accurate than sampling on GUI where paint latency skews it).
    qint64 wallClockMs = 0;
};
Q_DECLARE_METATYPE(VtBatch)

class VtStream : public QObject {
    Q_OBJECT

public:
    explicit VtStream(QObject *parent = nullptr);
    ~VtStream() override;

    // Access to the owned Pty is intentionally not exposed. GUI code must
    // go through invokeMethod → write/resize slots so the FD is only
    // touched on the worker thread.

signals:
    // Emitted from the worker thread whenever a parse batch is ready.
    // Connect with Qt::QueuedConnection to marshal onto GUI.
    void batchReady(const VtBatch &batch);
    // Mirror of Pty::finished, forwarded so TerminalWidget can keep its
    // existing onPtyFinished slot. Queued to GUI.
    void finished(int exitCode);

public slots:
    // Spawn the shell. Runs on the worker thread — invoke via
    // Qt::BlockingQueuedConnection from GUI so startup is deterministic.
    bool start(const QString &shell, const QString &workDir, int rows, int cols);
    // Write bytes to the PTY master. Thread-safe entry point for GUI:
    // invoke via Qt::QueuedConnection. Ordering preserved by Qt's per-
    // receiver FIFO event queue.
    void write(const QByteArray &data);
    // Resize the PTY winsize. Invoke via Qt::BlockingQueuedConnection
    // from GUI so the next paint reflects new geometry.
    void resize(int rows, int cols);
    // GUI acknowledges it has processed one batch. Decrements in-flight
    // counter and re-enables the read notifier if it was paused.
    void drainAck();

private slots:
    void onPtyData(const QByteArray &data);
    void onPtyFinished(int exitCode);
    void onFlushTimer();

private:
    void scheduleFlush();
    void flushBatch();

    Pty *m_pty = nullptr;                    // heap-allocated, owned, worker-thread
    std::unique_ptr<VtParser> m_parser;

    std::vector<VtAction> m_pending;
    QByteArray m_pendingRaw;
    bool m_pendingClearSelection = false;

    QElapsedTimer m_wallClock;               // started in start()
    bool m_flushScheduled = false;
    int m_inFlightBatches = 0;
    bool m_readPaused = false;               // true if we turned off QSocketNotifier

    // Back-pressure cap. Each batch is bounded by action+byte flush
    // thresholds, so in-flight bytes is ≤ kMaxInFlight × kFlushBytes
    // (~128 KB of raw input). Picked to cover a screen of noisy output
    // (find /, yes) with room for one more batch in the pipe. If throughput
    // profiling in phase 2 shows we're spending significant time paused,
    // raise this — the invariant the tests lock is correctness, not cap
    // value.
    static constexpr int kMaxInFlight = 8;
    static constexpr int kFlushActions = 4096;
    static constexpr int kFlushBytes = 16 * 1024;
};
