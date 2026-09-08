#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QSize>
#include <sys/types.h>

#include <atomic>

class Pty : public QObject {
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    bool start(const QString &shell = QString(), const QString &workDir = QString(),
               int rows = 24, int cols = 80);
    // ANTS-4456 — read on the GUI thread (TerminalWidget::ptyChildPid),
    // written on this object's own worker thread. See the member.
    pid_t childPid() const { return m_childPid.load(std::memory_order_relaxed); }
    // Toggle the read notifier without tearing it down. Used by VtStream
    // for back-pressure: when the parse-batch queue is full, reads pause
    // so the kernel buffer applies natural flow control to the child
    // process; when the GUI drains a batch, reads re-enable.
    void setReadEnabled(bool enabled);

public slots:
    // `write` and `resize` are slots so they can be invoked cross-thread
    // via QMetaObject::invokeMethod with Qt::QueuedConnection (for writes)
    // or Qt::BlockingQueuedConnection (for resize, which must synchronise
    // with the next paint on the GUI side).
    void write(const QByteArray &data);
    void resize(int rows, int cols);

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);
    // Emitted when write() drops data due to pending-write queue overflow
    // (> MAX_PENDING_WRITE_BYTES). Callers can use this to implement back-pressure
    // or alert the user (ANTS-1349).
    void writeLost(qint64 byteCount);

private slots:
    void onReadReady();
    void onWriteReady();

private:
    int m_masterFd = -1;
    // ANTS-4456 — atomic because the two threads genuinely disagree about
    // this. terminalwidget.h used to justify the cross-thread read with
    // "written once during forkpty() and never changes afterwards", which
    // is false: onReadReady is the read-notifier slot, runs on the parse
    // worker, and clears this to -1 when the child is reaped at EOF. The
    // GUI-side readers open /proc/<pid>, so a read that misses the clear
    // consults a PID the kernel may already have recycled. Relaxed
    // ordering is enough — the value stands alone and guards no other
    // state. Contract: tests/features/pty_childpid_atomic/spec.md.
    std::atomic<pid_t> m_childPid{-1};
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    // Bytes accepted by ::write() but not yet flushed to the kernel's
    // PTY buffer (master side returned EAGAIN). Drained by onWriteReady
    // when the kernel signals writability. See
    // tests/features/pty_write_eagain_queue/spec.md.
    QByteArray m_pendingWrite;
    static constexpr qsizetype MAX_PENDING_WRITE_BYTES = 4 * 1024 * 1024;
};
