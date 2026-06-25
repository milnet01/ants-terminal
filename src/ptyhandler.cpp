#include "ptyhandler.h"
#include "debuglog.h"
#include "secretredact.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
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
        // Quick non-blocking reap — most children exit on SIGHUP
        // (closing the master fd via the close() above raises SIGHUP
        // on the slave session, and the shell almost always honors
        // it within microseconds).
        int status = 0;
        if (::waitpid(m_childPid, &status, WNOHANG) == 0) {
            // Child still running — synchronous SIGTERM → 500 ms wait
            // → SIGKILL escalation on the calling thread.
            //
            // ANTS-1189 (2026-05-08): pre-fix code spawned a detached
            // worker here to keep the GUI responsive when N split
            // panes closed together. That worker outlived the
            // destructor and raced main-thread Qt teardown, producing
            // reproducible `_int_free_chunk` SIGABRT crashes in
            // `~TerminalWidget` on app exit (3 user repros, identical
            // fingerprint).
            //
            // The 0.7.33 "GUI freezes 500 ms × N panes" concern no
            // longer applies: in the threaded VtStream architecture
            // (0.7.0+) `~Pty` runs on the parse-worker thread (see
            // `VtStream::~VtStream`), not the GUI thread. The GUI
            // blocks at most on `m_parseThread->wait(2000)` in
            // `TerminalWidget::~TerminalWidget`, which already caps
            // its own wait at 2 s. The worker thread blocks here,
            // never the GUI thread.
            ::kill(m_childPid, SIGTERM);
            // SIGCONT alongside SIGTERM: if the shell is stopped (Ctrl-Z'd
            // job or its own process group suspended), SIGTERM won't be
            // delivered until it resumes — leading to the full 500 ms +
            // SIGKILL escalation on every tab-close that hit a stop.
            ::kill(m_childPid, SIGCONT);
            for (int i = 0; i < 50; ++i) {
                if (::waitpid(m_childPid, &status, WNOHANG) != 0) break;
                ::usleep(10000);  // 10 ms × 50 = 500 ms total
            }
            if (::waitpid(m_childPid, &status, WNOHANG) == 0) {
                ::kill(m_childPid, SIGKILL);
                // Bounded reap — SIGKILL almost always reaches the
                // process within microseconds, but a process stuck
                // in uninterruptible sleep (broken NFS, wedged kernel
                // I/O) can defer signal delivery briefly. Cap at 1 s
                // so total destructor budget stays ≤ 1.5 s, comfortably
                // within `m_parseThread->wait(2000)` in
                // `~TerminalWidget`. Pre-fix used a blocking
                // `waitpid(pid, &st, 0)` here — unbounded wait could
                // hang the worker thread, time out the GUI's wait(),
                // and trigger Qt's "destroying a running QThread
                // aborts the program" assertion. On bounded-loop
                // timeout we leak a zombie; init reaps it on app
                // exit, which is the acceptable degradation path.
                for (int i = 0; i < 100; ++i) {
                    if (::waitpid(m_childPid, &status, WNOHANG) != 0) break;
                    ::usleep(10000);  // 10 ms × 100 = 1 s total
                }
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

    // ANTS-1046 indie-review-2026-04-27: pre-fork the flatpak-spawn
    // argv and the version/directory string buffers so the child
    // does no `std::string` / `std::vector` allocation between
    // forkpty and execvp. The previous post-fork build relied on
    // the glibc malloc fork-handler — usually safe, not strictly
    // POSIX async-signal-safe in a multithreaded program (the
    // parent has the audit pipeline, AI dialog, and Lua VMs
    // threaded by the time forkpty fires). snprintf is on the
    // POSIX async-signal-safe list (§ 2.4.3) and writes into
    // these stack buffers without touching the heap. The argv
    // pointers reference the parent's stack; forkpty's COW
    // semantics make them readable in the child until execvp
    // replaces the address space.
    const bool inFlatpak =
        ::getenv("FLATPAK_ID") != nullptr ||
        ::access("/.flatpak-info", F_OK) == 0;
    char flatpakVerArg[128];
    char flatpakDirArg[PATH_MAX + 16];
    char flatpakMcpSocketArg[PATH_MAX + 32];
    if (inFlatpak) {
        (void)::snprintf(flatpakVerArg, sizeof(flatpakVerArg),
                         "--env=TERM_PROGRAM_VERSION=%s", ANTS_VERSION);
        if (!workDir.isEmpty()) {
            const QByteArray workDirBytes = workDir.toLocal8Bit();
            (void)::snprintf(flatpakDirArg, sizeof(flatpakDirArg),
                             "--directory=%s", workDirBytes.constData());
        } else {
            flatpakDirArg[0] = '\0';
        }
        // ANTS-1900 — flatpak-spawn does NOT inherit the caller's env
        // (same reason TERM* + workDir are forwarded explicitly above),
        // so the ANTS_MCP_SOCKET path mainwindow.cpp exported via
        // qputenv after startMcpServer (ANTS-1897 INV-14) never reaches
        // the host shell. Without this forward, a flatpak-installed Ants
        // runs MCP fine but the SessionStart orientation hook's
        // `[ -S "$ANTS_MCP_SOCKET" ]` guard fails, so the cheat-sheet —
        // and every socket-gated MCP affordance — stays silent. getenv
        // reads the parent's env here (pre-fork, parent context); the
        // value is copied into a stack buffer so the child reads the
        // buffer, not the environ pointer. Gated on a non-empty value so
        // an MCP-disabled install omits the arg rather than passing a
        // bare "--env=ANTS_MCP_SOCKET=".
        const char *mcpSocketEnv = ::getenv("ANTS_MCP_SOCKET");
        if (mcpSocketEnv != nullptr && mcpSocketEnv[0] != '\0') {
            (void)::snprintf(flatpakMcpSocketArg, sizeof(flatpakMcpSocketArg),
                             "--env=ANTS_MCP_SOCKET=%s", mcpSocketEnv);
        } else {
            flatpakMcpSocketArg[0] = '\0';
        }
    }

    // ANTS-1135 — pre-fork envp build for the non-flatpak shell
    // launch. Pre-fix code called ::setenv() five times in the
    // child, but setenv is NOT on POSIX's async-signal-safe list
    // (§ 2.4.3) — it can call realloc() to grow `environ`. The
    // CLAUDE.md contract claims "child only does execvp" and the
    // flatpak path already pays the engineering cost to prep argv
    // pre-fork; this closes the same hole on the non-flatpak
    // path. Stack-allocated buffers + pointer table; the child
    // reads the parent's stack via fork's COW semantics until
    // execle replaces the address space.
    char termProgramVerArg[128];
    (void)::snprintf(termProgramVerArg, sizeof(termProgramVerArg),
                     "TERM_PROGRAM_VERSION=%s", ANTS_VERSION);
    constexpr size_t kEnvpCap = 512;
    const char *childEnvp[kEnvpCap];
    size_t envpCount = 0;
    // Copy parent's environ entries, skipping the 5 keys we
    // override. const_cast is safe — we only read the pointers,
    // not their contents, and POSIX guarantees `environ` entries
    // are NUL-terminated KEY=VALUE strings.
    extern char **environ;
    bool envpTruncated = false;
    for (char **e = environ; *e != nullptr; ++e) {
        const char *entry = *e;
        const auto startsWith = [entry](const char *prefix) {
            for (size_t i = 0; prefix[i] != '\0'; ++i) {
                if (entry[i] != prefix[i]) return false;
            }
            return true;
        };
        if (startsWith("TERM=") || startsWith("COLORTERM=") ||
            startsWith("TERM_PROGRAM=") ||
            startsWith("TERM_PROGRAM_VERSION=") ||
            startsWith("COLORFGBG=")) {
            continue;
        }
        if (envpCount >= kEnvpCap - 8) {
            // ANTS-1175: surface the silent truncation that produced
            // the user-reported "ants-terminal sometimes opens a
            // shell with no PATH" symptom on environments with 500+
            // entries (modern desktop sessions hit this routinely).
            // Only warn once per fork.
            envpTruncated = true;
            break;
        }
        childEnvp[envpCount++] = entry;
    }
    if (envpTruncated) {
        ANTS_LOG_ALWAYS(
            "Pty: parent environ exceeded %zu entries; "
            "child shell may be missing PATH/HOME/etc.",
            kEnvpCap);
    }
    // Append our 5 overrides (5 string literals + 1 snprintf'd
    // buffer). The string-literal pointers live in the binary's
    // .rodata so they outlive forkpty trivially.
    childEnvp[envpCount++] = "TERM=xterm-256color";
    childEnvp[envpCount++] = "COLORTERM=truecolor";
    childEnvp[envpCount++] = "TERM_PROGRAM=AntsTerminal";
    childEnvp[envpCount++] = termProgramVerArg;
    childEnvp[envpCount++] = "COLORFGBG=15;0";
    childEnvp[envpCount] = nullptr;

    // ANTS-1135 follow-up (indie-review-2026-05-21): pre-fork the shell-path
    // and workdir byte conversions. toLocal8Bit() heap-allocates, which is
    // NOT on POSIX's async-signal-safe list — doing it in the child between
    // forkpty and execle can deadlock on the malloc arena lock if another
    // thread held it at fork. Compute here in the parent; the child reads
    // .constData() via fork's COW semantics with no child-side allocation.
    const QByteArray shellBytesPre   = shellPath.toLocal8Bit();
    const QByteArray workDirBytesPre = workDir.toLocal8Bit();

    // indie-review-2026-05-19 ptyhandler M1 — explicit -1 init.
    // POSIX forkpty does not guarantee `*amaster` is untouched on -1.
    // Pty::start is one-shot today, but the header doesn't enforce
    // single-call; a future "restart shell" path would, without this
    // line, see a stale FD value if forkpty failed.
    m_masterFd = -1;
    m_childPid = forkpty(&m_masterFd, nullptr, nullptr, &ws);

    if (m_childPid < 0) {
        return false;
    }

    // ANTS-1751 — set FD_CLOEXEC on the master as the FIRST parent action
    // after the fork. forkpty has no atomic O_CLOEXEC variant, so the
    // master comes back inheritable; doing this here (rather than after
    // the ~140-line child/parent setup below) shrinks to a few
    // instructions the window in which a concurrent fork on another
    // thread (e.g. an audit QProcess) could inherit the PTY master.
    // The residual window cannot be closed without serialising every
    // fork/QProcess site behind a global lock — a known limitation.
    if (m_childPid > 0) {
        ::fcntl(m_masterFd, F_SETFD, FD_CLOEXEC);
    }

    if (m_childPid == 0) {
        // Reset signal mask + dispositions to defaults. Qt installs
        // handlers for SIGCHLD/SIGPIPE/SIGUSR2/...; glib & dbus add more.
        // POSIX § 2.4 keeps ignored dispositions (e.g. SIGPIPE=SIG_IGN)
        // across exec, which is the classic "pipeline under child shell
        // silently hangs" failure mode. Both calls are async-signal-safe.
        {
            sigset_t empty;
            ::sigemptyset(&empty);
            ::sigprocmask(SIG_SETMASK, &empty, nullptr);
            const int kResetSigs[] = {
                SIGPIPE, SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM,
                SIGTSTP, SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGALRM,
            };
            for (int s : kResetSigs) {
                struct sigaction sa{};
                sa.sa_handler = SIG_DFL;
                ::sigemptyset(&sa.sa_mask);
                ::sigaction(s, &sa, nullptr);
            }
        }
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
            // ANTS-1295 indie-review L2-M5: when rlim_cur == RLIM_INFINITY
            // we still need an effective cap — the previous code fell
            // through to maxFd=1024, silently leaking the FDs ≥ 1024
            // exactly like the regression this code block exists to
            // prevent. Treat RLIM_INFINITY as "use the 65536 ceiling."
            struct rlimit rl{};
            int maxFd = 65536;
            if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
                rl.rlim_cur != RLIM_INFINITY) {
                rlim_t cur = rl.rlim_cur;
                // Cap at 65536 to bound the worst-case syscall storm on
                // hardened-server profiles where rlim_cur is in the
                // hundreds of thousands.
                if (cur > 65536) cur = 65536;
                if (cur > 1024) maxFd = static_cast<int>(cur);
                else            maxFd = 1024;
            }
            for (int fd = 3; fd < maxFd; ++fd)
                ::close(fd);
        }

        // Exec the shell. shellBytesPre/workDirBytesPre were converted
        // pre-fork (above) so the child does NO heap allocation here.
        const char *shellCStr = shellBytesPre.constData();

        // Change to requested working directory before exec
        if (!workDir.isEmpty()) {
            if (::chdir(workDirBytesPre.constData()) != 0) {
                // Fall back to home directory
                const char *home = ::getenv("HOME");
                if (home && ::chdir(home) != 0) { /* last-resort fallback, ignore failure */ }
            }
        }

        // ANTS-1135 — env passed via execle's envp arg below; the
        // 5 setenv() calls that used to live here have been
        // moved to the pre-fork prep block (which is signal-safe
        // because the parent isn't post-fork). Pre-fix: child
        // called setenv() ×5, which can realloc(environ) — not
        // on POSIX's async-signal-safe list. CLAUDE.md asserts
        // "child only does execvp"; this matches the flatpak
        // path's pre-fork-allocation discipline.

        // Get just the shell name for argv[0]
        const char *shellName = ::strrchr(shellCStr, '/');
        shellName = shellName ? shellName + 1 : shellCStr;

        // Login shell (prefix with -). Truncation is acceptable —
        // shellName is the basename of `shellCStr`, which the parent
        // resolved from (in order): caller arg → $SHELL → getpwuid()
        // → "/bin/bash" literal fallback. No separate validation step;
        // the same-UID trust model (ADR-0004) accepts $SHELL as the
        // primary source. argv[0] here is a display/login-marker, not
        // a lookup key, so the cast silences the cert-err33-c
        // diagnostic without losing any real guarantee.
        char argv0[256];
        (void)::snprintf(argv0, sizeof(argv0), "-%s", shellName);

        // Flatpak: execute the user's shell on the host via
        // `flatpak-spawn --host` so the sandbox doesn't cut off $PATH,
        // tools, and the real home directory. flatpak-spawn does NOT
        // inherit the calling process's env or cwd, so TERM* and
        // workDir cross the sandbox boundary explicitly via --env=
        // and --directory=. Detection + argv string prep happens
        // pre-fork (ANTS-1046); the child only assembles pointers to
        // those buffers and calls execvp — no std::string or
        // std::vector allocation between forkpty and execvp.
        // If flatpak-spawn is missing we _exit(127) the same way a
        // missing shell would, matching the direct-exec fallback's
        // failure shape. See packaging/flatpak/org.ants.Terminal.yml.
        if (inFlatpak) {
            // Stack-allocated argv pointer table. Slots filled
            // unconditionally with stable string literals or with
            // pre-built buffers from the pre-fork prep. No heap
            // touch.
            const char *argv[14];
            size_t i = 0;
            argv[i++] = "flatpak-spawn";
            argv[i++] = "--host";
            argv[i++] = "--env=TERM=xterm-256color";
            argv[i++] = "--env=COLORTERM=truecolor";
            argv[i++] = "--env=TERM_PROGRAM=AntsTerminal";
            argv[i++] = flatpakVerArg;
            argv[i++] = "--env=COLORFGBG=15;0";
            if (flatpakDirArg[0] != '\0') argv[i++] = flatpakDirArg;
            if (flatpakMcpSocketArg[0] != '\0') argv[i++] = flatpakMcpSocketArg;
            argv[i++] = "--";
            argv[i++] = shellCStr;
            argv[i++] = nullptr;
            ::execvp("flatpak-spawn",
                     const_cast<char *const *>(argv));
            ::_exit(127);
        }

        // ANTS-1135 — execle (signal-safe) replaces execlp. The
        // shell path is the absolute string resolved in the parent
        // from caller arg → $SHELL → getpwuid() → "/bin/bash" fallback
        // (no separate validation step; same-UID trust per ADR-0004).
        // childEnvp was built pre-fork in the parent; readable
        // here via fork's COW semantics until execle replaces
        // the address space.
        ::execle(shellCStr, argv0,
                 static_cast<const char *>(nullptr),
                 const_cast<char *const *>(childEnvp));
        ::_exit(127);
    }

    // Parent process — FD_CLOEXEC already set immediately after fork
    // (ANTS-1751, above) to minimise the inheritance window.

    // Set master to non-blocking. The QSocketNotifier model below assumes
    // a non-blocking master — a spurious wakeup on a still-blocking fd
    // would freeze the GUI thread inside ::read(). Treat F_GETFL or
    // F_SETFL failure as a hard error rather than falling through.
    int flags = ::fcntl(m_masterFd, F_GETFL);
    if (flags < 0 || ::fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ANTS_LOG_ALWAYS("Pty: failed to set master non-blocking: %s",
                        std::strerror(errno));
        ::close(m_masterFd);
        m_masterFd = -1;
        if (m_childPid > 0) ::kill(m_childPid, SIGHUP);
        return false;
    }

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
    if (DebugLog::enabled(DebugLog::Pty)) {
        // ANTS-1164: scrub well-known secret shapes (ghp_…, AKIA…,
        // Bearer …, sk-ant-…, PEM, etc.) from the debug-log slice so
        // a forgotten ANTS_DEBUG=pty doesn't silently capture pasted
        // tokens in ~/.local/share/ants-terminal/debug.log.
        const QString preview =
            SecretRedact::scrub(QString::fromUtf8(data.left(60))).text;
        ANTS_LOG(DebugLog::Pty, "write %lld bytes: %s",
                 (long long)data.size(),
                 preview.toUtf8().toPercentEncoding().constData());
    }

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
            emit writeLost(data.size());  // ANTS-1349 — signal data loss
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
                // ANTS-1994(3) — signal the drop, same as the
                // pending-queue-full path above (ANTS-1349). Without
                // this, an oversize write past the kernel PTY buffer
                // vanished with no writeLost notification.
                emit writeLost(remaining);
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
                // Child genuinely reaped — record exit + clear PID so
                // the destructor's escalation block doesn't try to
                // double-reap.
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                m_childPid = -1;
            }
            // ANTS-1131 — when waitpid(WNOHANG) returns 0, the child
            // closed the PTY but hasn't been reaped yet (very common:
            // shell calls exec against a binary that segfaults during
            // init). Pre-fix code unconditionally cleared m_childPid,
            // defeating the destructor's SIGTERM/SIGKILL escalation
            // and leaking the child as an init-adopted zombie. Now
            // we leave m_childPid populated so destructor cleanup
            // can escalate.
            // ANTS-1657 — the ANTS-1175 childUnreapedAtEof() signal was
            // emitted here but never connected to any consumer; removed as a
            // zombie. finished(exitCode) is the only EOF signal callers rely
            // on (exitCode == -1 when the child wasn't yet reaped at EOF).
            emit finished(exitCode);
            break;
        } else {
            // EAGAIN — no more data right now
            break;
        }
    }
}
