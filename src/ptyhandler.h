#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QSize>
#include <sys/types.h>

class Pty : public QObject {
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    bool start(const QString &shell = QString(), const QString &workDir = QString(),
               int rows = 24, int cols = 80);
    pid_t childPid() const { return m_childPid; }
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

private slots:
    void onReadReady();
    void onWriteReady();

private:
    int m_masterFd = -1;
    pid_t m_childPid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    // Bytes accepted by ::write() but not yet flushed to the kernel's
    // PTY buffer (master side returned EAGAIN). Drained by onWriteReady
    // when the kernel signals writability. See
    // tests/features/pty_write_eagain_queue/spec.md.
    QByteArray m_pendingWrite;
    static constexpr qsizetype MAX_PENDING_WRITE_BYTES = 4 * 1024 * 1024;
};
