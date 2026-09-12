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

    // ANTS-5135 — does the pty's foreground process group show that a
    // foreground program has just EXITED? `fg` is tcgetpgrp()'s answer,
    // `shellPgid` the forkpty child's pgid (that child calls setsid(), so its
    // pgid is its pid), and `lastSeen` the previous sample, updated in place.
    //
    // True only on the TRANSITION back to the shell: something else held the
    // foreground and now the shell does. Steady state is false either way, so
    // an idle shell does not reset on every read, and a program still running
    // does not reset mid-output. The first sample can never fire it, because
    // `lastSeen` starts -1 — otherwise a terminal opening with the shell
    // already idle would reset on its very first read.
    //
    // Static and free of member state so the state machine is testable
    // without a pty: tests/features/pty_foreground_attr_reset/spec.md.
    static bool foregroundReturnedToShell(pid_t fg, pid_t shellPgid,
                                          pid_t &lastSeen);

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
    // ANTS-5135 — previous tcgetpgrp() sample. -1 until the first read, and
    // reset to -1 whenever the syscall cannot answer, which costs at most one
    // missed transition and can never invent one.
    pid_t m_lastForegroundPgid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    // ANTS-5026 — set when onReadReady handles EOF. No read, reap or
    // re-enable happens after it.
    bool m_readEof = false;
    QSocketNotifier *m_writeNotifier = nullptr;
    // Bytes accepted by ::write() but not yet flushed to the kernel's
    // PTY buffer (master side returned EAGAIN). Drained by onWriteReady
    // when the kernel signals writability. See
    // tests/features/pty_write_eagain_queue/spec.md.
    QByteArray m_pendingWrite;
    static constexpr qsizetype MAX_PENDING_WRITE_BYTES = 4 * 1024 * 1024;
};
