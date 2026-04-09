#include "ptyhandler.h"

#include <QCoreApplication>
#include <QStandardPaths>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmp.h>
#include <pwd.h>

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
    if (m_childPid > 0) {
        ::kill(m_childPid, SIGHUP);
    }
    if (m_masterFd >= 0) {
        ::close(m_masterFd);
        m_masterFd = -1;
    }
    if (m_childPid > 0) {
        // Reap child — try non-blocking first, then escalate
        int status = 0;
        if (::waitpid(m_childPid, &status, WNOHANG) == 0) {
            // Child still running after SIGHUP — send SIGTERM and wait briefly
            ::kill(m_childPid, SIGTERM);
            for (int i = 0; i < 50; ++i) {
                if (::waitpid(m_childPid, &status, WNOHANG) != 0) break;
                ::usleep(10000); // 10ms
            }
            // Last resort — SIGKILL
            if (::waitpid(m_childPid, &status, WNOHANG) == 0) {
                ::kill(m_childPid, SIGKILL);
                ::waitpid(m_childPid, &status, 0);
            }
        }
        m_childPid = -1;
    }
}

bool Pty::start(const QString &shell) {
    // Determine which shell to use
    QString shellPath = shell;
    if (shellPath.isEmpty()) {
        const char *envShell = ::getenv("SHELL");
        if (envShell && envShell[0] != '\0') {
            shellPath = QString::fromLocal8Bit(envShell);
        } else {
            // Fall back to passwd entry
            struct passwd *pw = ::getpwuid(::getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0] != '\0') {
                shellPath = QString::fromLocal8Bit(pw->pw_shell);
            } else {
                shellPath = QStringLiteral("/bin/bash");
            }
        }
    }

    struct winsize ws = {};
    ws.ws_row = 24;
    ws.ws_col = 80;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    m_childPid = forkpty(&m_masterFd, nullptr, nullptr, &ws);

    if (m_childPid < 0) {
        return false;
    }

    if (m_childPid == 0) {
        // Child process — exec the shell
        QByteArray shellBytes = shellPath.toLocal8Bit();
        const char *shellCStr = shellBytes.constData();

        // Set TERM so the shell knows our capabilities
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);

        // Get just the shell name for argv[0]
        const char *shellName = ::strrchr(shellCStr, '/');
        shellName = shellName ? shellName + 1 : shellCStr;

        // Login shell (prefix with -)
        char argv0[256];
        ::snprintf(argv0, sizeof(argv0), "-%s", shellName);

        ::execlp(shellCStr, argv0, nullptr);
        ::_exit(127);
    }

    // Parent process — set master to non-blocking
    int flags = ::fcntl(m_masterFd, F_GETFL);
    ::fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    m_readNotifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &Pty::onReadReady);

    return true;
}

void Pty::write(const QByteArray &data) {
    if (m_masterFd < 0) return;
    const char *buf = data.constData();
    qsizetype remaining = data.size();
    while (remaining > 0) {
        ssize_t n = ::write(m_masterFd, buf, remaining);
        if (n > 0) {
            buf += n;
            remaining -= n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break; // EAGAIN or fatal error
        }
    }
}

void Pty::resize(int rows, int cols) {
    if (m_masterFd >= 0) {
        struct winsize ws = {};
        ws.ws_row = static_cast<unsigned short>(rows);
        ws.ws_col = static_cast<unsigned short>(cols);
        ::ioctl(m_masterFd, TIOCSWINSZ, &ws);
    }
}

void Pty::onReadReady() {
    char buf[16384];
    while (true) {
        ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
        if (n > 0) {
            emit dataReceived(QByteArray(buf, static_cast<int>(n)));
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            // EOF or error — child exited
            m_readNotifier->setEnabled(false);
            int status = 0;
            ::waitpid(m_childPid, &status, 0);
            emit finished(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            break;
        } else {
            // EAGAIN — no more data right now
            break;
        }
    }
}
