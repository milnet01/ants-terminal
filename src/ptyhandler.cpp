#include "ptyhandler.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
    // Disable notifier before closing FD to prevent reads on a closed/reused FD
    delete m_readNotifier;
    m_readNotifier = nullptr;

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

bool Pty::start(const QString &shell, const QString &workDir, int rows, int cols) {
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
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    m_childPid = forkpty(&m_masterFd, nullptr, nullptr, &ws);

    if (m_childPid < 0) {
        return false;
    }

    if (m_childPid == 0) {
        // Child process — close inherited file descriptors (CWE-403)
        // Prevents leaking parent's sockets/FDs (AI network, plugins, etc.)
        for (int fd = 3; fd < 1024; ++fd)
            ::close(fd);

        // Exec the shell
        QByteArray shellBytes = shellPath.toLocal8Bit();
        const char *shellCStr = shellBytes.constData();

        // Change to requested working directory before exec
        if (!workDir.isEmpty()) {
            QByteArray dirBytes = workDir.toLocal8Bit();
            if (::chdir(dirBytes.constData()) != 0) {
                // Fall back to home directory
                const char *home = ::getenv("HOME");
                if (home && ::chdir(home) != 0) { /* last-resort fallback, ignore failure */ }
            }
        }

        // Set TERM so the shell knows our capabilities
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "AntsTerminal", 1);
        ::setenv("TERM_PROGRAM_VERSION", ANTS_VERSION, 1);
        // Dark background hint (15=white fg, 0=black bg) — used by vim, mutt
        ::setenv("COLORFGBG", "15;0", 1);

        // Get just the shell name for argv[0]
        const char *shellName = ::strrchr(shellCStr, '/');
        shellName = shellName ? shellName + 1 : shellCStr;

        // Login shell (prefix with -). Truncation is acceptable — shellName
        // comes from the ShellEntry we just validated, and argv[0] is a
        // display/login-marker, not a lookup key, so the cast silences the
        // cert-err33-c diagnostic without losing any real guarantee.
        char argv0[256];
        (void)::snprintf(argv0, sizeof(argv0), "-%s", shellName);

        ::execlp(shellCStr, argv0, nullptr);
        ::_exit(127);
    }

    // Parent process — set O_CLOEXEC so master FD doesn't leak to child processes
    ::fcntl(m_masterFd, F_SETFD, FD_CLOEXEC);

    // Set master to non-blocking
    int flags = ::fcntl(m_masterFd, F_GETFL);
    if (flags >= 0)
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

void Pty::setReadEnabled(bool enabled) {
    if (m_readNotifier) {
        m_readNotifier->setEnabled(enabled);
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
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            // n == 0 → EOF (child closed the PTY).
            // n < 0  → read error; EAGAIN/EINTR hit the final else below,
            //          anything else means the child has exited.
            m_readNotifier->setEnabled(false);
            int status = 0;
            int exitCode = -1;
            pid_t w = ::waitpid(m_childPid, &status, WNOHANG);
            if (w > 0) {
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            m_childPid = -1; // Prevent double-reap in destructor
            emit finished(exitCode);
            break;
        } else {
            // EAGAIN — no more data right now
            break;
        }
    }
}
