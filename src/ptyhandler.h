#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QSize>
#include <sys/types.h>

class Pty : public QObject {
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    bool start(const QString &shell = QString());
    void write(const QByteArray &data);
    void resize(int rows, int cols);
    pid_t childPid() const { return m_childPid; }

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);

private slots:
    void onReadReady();

private:
    int m_masterFd = -1;
    pid_t m_childPid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
};
