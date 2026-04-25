#include "ptyhandler.h"
#include "debuglog.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <pwd.h>

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
    // Disable notifiers before closing FD to prevent reads/writes on a
    // closed/reused FD.
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;

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
        // Prevents leaking parent's sockets/FDs (AI network, plugins,
        // D-Bus, X11/Wayland, remote-control IPC) into the user's shell.
        //
        // Prefer close_range(2) (Linux 5.9+) — single signal-safe syscall,
        // closes every FD in the range atomically, ignores the soft cap.
        // Fall back to a getrlimit-bounded loop when the syscall is
        // unavailable (older kernels report ENOSYS). The previous
        // hard-coded fd<1024 cap silently leaked descriptors above 1023
        // on systemd / container default RLIMIT_NOFILE profiles. See
        // tests/features/pty_closefrom/spec.md.
        bool fdsClosed = false;
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 3, ~0U, 0) == 0)
            fdsClosed = true;
#endif
        if (!fdsClosed) {
            struct rlimit rl{};
            int maxFd = 1024;
            if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
                rl.rlim_cur != RLIM_INFINITY) {
                rlim_t cur = rl.rlim_cur;
                // Cap at 65536 to bound the worst-case syscall storm on
                // hardened-server profiles where rlim_cur is in the
                // hundreds of thousands.
                if (cur > 65536) cur = 65536;
                if (cur > 1024) maxFd = static_cast<int>(cur);
            }
            for (int fd = 3; fd < maxFd; ++fd)
                ::close(fd);
        }

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

        // Flatpak: execute the user's shell on the host via
        // `flatpak-spawn --host` so the sandbox doesn't cut off $PATH,
        // tools, and the real home directory. flatpak-spawn does NOT
        // inherit the calling process's env or cwd, so TERM* and
        // workDir cross the sandbox boundary explicitly via --env= and
        // --directory=. Detection checks both FLATPAK_ID (set by the
        // flatpak launcher) and /.flatpak-info (present in every
        // sandbox regardless of how the app was launched). If
        // flatpak-spawn is missing we _exit(127) the same way a
        // missing shell would, matching the direct-exec fallback's
        // failure shape. See packaging/flatpak/org.ants.Terminal.yml.
        const bool inFlatpak =
            ::getenv("FLATPAK_ID") != nullptr ||
            ::access("/.flatpak-info", F_OK) == 0;
        if (inFlatpak) {
            const QByteArray workDirBytes = workDir.toLocal8Bit();
            std::string verArg = "--env=TERM_PROGRAM_VERSION=";
            verArg += ANTS_VERSION;
            std::string dirArg;
            if (!workDir.isEmpty()) {
                dirArg = "--directory=";
                dirArg.append(workDirBytes.constData(),
                              static_cast<size_t>(workDirBytes.size()));
            }
            std::vector<const char *> argv;
            argv.reserve(12);
            argv.push_back("flatpak-spawn");
            argv.push_back("--host");
            argv.push_back("--env=TERM=xterm-256color");
            argv.push_back("--env=COLORTERM=truecolor");
            argv.push_back("--env=TERM_PROGRAM=AntsTerminal");
            argv.push_back(verArg.c_str());
            argv.push_back("--env=COLORFGBG=15;0");
            if (!workDir.isEmpty()) argv.push_back(dirArg.c_str());
            argv.push_back("--");
            argv.push_back(shellCStr);
            argv.push_back(nullptr);
            ::execvp("flatpak-spawn",
                     const_cast<char *const *>(argv.data()));
            ::_exit(127);
        }

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

    // Write-side notifier — disabled until ::write() encounters EAGAIN
    // and queues a remainder. Leaving it always-enabled would burn CPU
    // because PTY masters are writable nearly continuously.
    m_writeNotifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &Pty::onWriteReady);

    return true;
}

void Pty::write(const QByteArray &data) {
    if (m_masterFd < 0) return;
    ANTS_LOG(DebugLog::Pty, "write %lld bytes: %s", (long long)data.size(),
             data.left(60).toPercentEncoding().constData());

    // FIFO ordering — if a previous call queued bytes on EAGAIN, every
    // fresh write must go behind them, never ahead. Bypass would let
    // newer keystrokes race past older ones.
    if (!m_pendingWrite.isEmpty()) {
        if (m_pendingWrite.size() + data.size() > MAX_PENDING_WRITE_BYTES) {
            ANTS_LOG(DebugLog::Pty,
                     "pending-write queue full (%lld bytes); dropping new "
                     "%lld-byte write",
                     (long long)m_pendingWrite.size(),
                     (long long)data.size());
            return;
        }
        m_pendingWrite.append(data);
        return;
    }

    const char *buf = data.constData();
    qsizetype remaining = data.size();
    while (remaining > 0) {
        ssize_t n = ::write(m_masterFd, buf, remaining);
        if (n > 0) {
            buf += n;
            remaining -= n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Kernel PTY buffer full — queue the remainder and let
            // onWriteReady drain it when the kernel signals writability.
            // Pre-fix this branch broke out of the loop with no buffer,
            // silently dropping `remaining` bytes.
            if (remaining > MAX_PENDING_WRITE_BYTES) {
                ANTS_LOG(DebugLog::Pty,
                         "EAGAIN remainder %lld bytes exceeds queue cap "
                         "(%lld); dropping",
                         (long long)remaining,
                         (long long)MAX_PENDING_WRITE_BYTES);
                return;
            }
            m_pendingWrite = QByteArray(buf, remaining);
            if (m_writeNotifier) m_writeNotifier->setEnabled(true);
            return;
        } else {
            // Fatal error (EIO, EPIPE on a torn-down master, etc.).
            // Don't queue — the FD is gone or the slave reaped.
            ANTS_LOG(DebugLog::Pty,
                     "write failed errno=%d; dropping %lld bytes",
                     errno, (long long)remaining);
            return;
        }
    }
}

void Pty::onWriteReady() {
    if (m_masterFd < 0 || m_pendingWrite.isEmpty()) {
        if (m_writeNotifier) m_writeNotifier->setEnabled(false);
        return;
    }
    const char *buf = m_pendingWrite.constData();
    qsizetype remaining = m_pendingWrite.size();
    qsizetype written = 0;
    while (remaining > 0) {
        ssize_t n = ::write(m_masterFd, buf + written, remaining);
        if (n > 0) {
            written += n;
            remaining -= n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break; // EAGAIN — try again next fire; fatal — give up below.
        }
    }
    if (written > 0) {
        m_pendingWrite.remove(0, written);
    }
    if (m_pendingWrite.isEmpty()) {
        if (m_writeNotifier) m_writeNotifier->setEnabled(false);
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
        ANTS_LOG(DebugLog::Pty, "resize rows=%d cols=%d", rows, cols);
    }
}

void Pty::onReadReady() {
    char buf[16384];
    while (true) {
        ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
        if (n > 0) {
            ANTS_LOG(DebugLog::Pty, "read %zd bytes", n);
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
