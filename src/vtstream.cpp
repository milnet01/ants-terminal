#include "vtstream.h"
#include "ptyhandler.h"

#include <QTimer>

VtStream::VtStream(QObject *parent)
    : QObject(parent) {
    // One-time metatype registration for queued cross-thread signal args.
    // Safe to call repeatedly; Qt guards internally.
    qRegisterMetaType<VtBatchPtr>("VtBatchPtr");
}

VtStream::~VtStream() {
    // m_pty is parented to this; Qt's parent/child tree destroys it.
    // m_parser is a unique_ptr. Both destructors run on whichever thread
    // executes ~VtStream — which is the worker thread if the containing
    // QThread emitted finished() and we were deleteLater'd. This matches
    // the existing Pty dtor's SIGHUP/SIGTERM/SIGKILL ladder, which has
    // no thread affinity (it just closes an FD and waitpid's a child).
}

bool VtStream::start(const QString &shell, const QString &workDir, int rows, int cols) {
    // Constructed here (on worker) so the QSocketNotifier Pty creates
    // lives on the thread that will service the event loop. Creating it
    // on another thread triggers a Qt warning on first activation.
    m_pty = new Pty(this);
    m_parser = std::make_unique<VtParser>([this](const VtAction &a) {
        // Coalesced Print runs (printRun pointing into the feed buffer)
        // do NOT outlive this callback — `data` in onPtyData is freed
        // when that frame returns, so any pointer into it is dangling
        // by the time `flushBatch` ships the batch to the GUI thread.
        // Expand runs into per-byte Print actions here; the cost is
        // one VtAction per byte on the worker thread but that's the
        // same shape as the pre-0.7.17 parser and keeps the GUI-side
        // batch consumer dangling-pointer-free. The direct (non-
        // threaded) path in processAction still benefits from
        // coalescing because there the VtAction lives on the stack
        // during the callback only.
        if (a.type == VtAction::Print && a.printRun != nullptr) {
            for (int i = 0; i < a.printRunLen; ++i) {
                VtAction b;
                b.type = VtAction::Print;
                b.codepoint = static_cast<uint8_t>(a.printRun[i]);
                m_pending.push_back(std::move(b));
            }
            return;
        }
        m_pending.push_back(a);
    });
    connect(m_pty, &Pty::dataReceived, this, &VtStream::onPtyData);
    connect(m_pty, &Pty::finished, this, &VtStream::onPtyFinished);

    m_wallClock.start();

    if (!m_pty->start(shell, workDir, rows, cols)) {
        return false;
    }
    return true;
}

pid_t VtStream::childPid() const {
    return m_pty ? m_pty->childPid() : -1;
}

void VtStream::write(const QByteArray &data) {
    if (m_pty) m_pty->write(data);
}

void VtStream::resize(int rows, int cols) {
    if (m_pty) m_pty->resize(rows, cols);
}

void VtStream::drainAck() {
    if (m_inFlightBatches > 0) {
        --m_inFlightBatches;
    }
    // If reads were paused by back-pressure, flush anything still buffered
    // and re-enable the notifier.
    if (m_readPaused && m_inFlightBatches < kMaxInFlight) {
        m_readPaused = false;
        if (m_pty) m_pty->setReadEnabled(true);
        // Drain any bytes that arrived while paused: onPtyData runs again
        // when the notifier wakes, so no explicit poll needed here. But
        // if m_pending already held bytes we hadn't emitted, flush now.
        if (!m_pending.empty() || !m_pendingRaw.isEmpty()) {
            flushBatch();
        }
    }
}

void VtStream::onPtyData(const QByteArray &data) {

    // Selection-clear hint: single-pass byte scan, matching the pre-thread
    // logic in TerminalWidget::onPtyData. Delivered with the batch so GUI
    // can clear selection without re-scanning.
    if (!m_pendingClearSelection) {
        if (data.contains("\x1B[2J") || data.contains("\x1B[3J") || data.contains("\x0C")) {
            m_pendingClearSelection = true;
        }
    }

    m_pendingRaw.append(data);
    // Feed the parser. Its action-callback appends to m_pending (set up in
    // start()). This runs synchronously on the worker; no cross-thread hop.
    m_parser->feed(data.constData(), static_cast<int>(data.size()));

    // Fast path: if we're over either threshold, flush immediately so
    // the GUI sees output with sub-frame latency on heavy streams.
    if (static_cast<int>(m_pending.size()) >= kFlushActions ||
        m_pendingRaw.size() >= kFlushBytes) {
        flushBatch();
    } else {
        // Otherwise coalesce — next event-loop turn fires a single flush
        // for multiple small reads (typical of a shell prompt echoing
        // keystrokes). sub-ms on an idle worker; naturally deferred under
        // heavy load, which is the batching we want.
        scheduleFlush();
    }
}

void VtStream::scheduleFlush() {
    if (m_flushScheduled) return;
    m_flushScheduled = true;
    QTimer::singleShot(0, this, &VtStream::onFlushTimer);
}

void VtStream::onFlushTimer() {
    m_flushScheduled = false;
    if (!m_pending.empty() || !m_pendingRaw.isEmpty()) {
        flushBatch();
    }
}

void VtStream::flushBatch() {
    // Back-pressure: if GUI hasn't caught up, pause reads and hold the
    // buffered input. Next drainAck() will trigger another flush attempt.
    if (m_inFlightBatches >= kMaxInFlight) {
        if (!m_readPaused && m_pty) {
            m_readPaused = true;
            m_pty->setReadEnabled(false);
        }
        return;
    }

    auto b = std::make_shared<VtBatch>();
    b->actions = std::move(m_pending);
    b->rawBytes = std::move(m_pendingRaw);
    b->clearSelectionHint = m_pendingClearSelection;
    b->wallClockMs = m_wallClock.elapsed();

    m_pending.clear();       // ensure moved-from vector is empty
    m_pendingRaw.clear();
    m_pendingClearSelection = false;

    ++m_inFlightBatches;
    // Implicit shared_ptr<VtBatch> → shared_ptr<const VtBatch> on the
    // signal carrier. Qt copies the smart-pointer across the queued
    // connection, not the underlying VtBatch — so `actions` and
    // `rawBytes` are NOT deep-copied on the worker→GUI hop.
    emit batchReady(b);
}

void VtStream::onPtyFinished(int exitCode) {
    // Flush anything still buffered so the shell's final output reaches
    // the GUI before the finished signal.
    if (!m_pending.empty() || !m_pendingRaw.isEmpty()) {
        // Force a flush even if over the back-pressure cap — the PTY is
        // done, there's no more input coming.
        auto b = std::make_shared<VtBatch>();
        b->actions = std::move(m_pending);
        b->rawBytes = std::move(m_pendingRaw);
        b->clearSelectionHint = m_pendingClearSelection;
        b->wallClockMs = m_wallClock.elapsed();
        m_pending.clear();
        m_pendingRaw.clear();
        m_pendingClearSelection = false;
        emit batchReady(b);
    }
    emit finished(exitCode);
}
