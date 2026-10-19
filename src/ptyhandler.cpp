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
#include <termios.h>
#include <unistd.h>
#include <pwd.h>
#include <iterator>
#include <vector>

extern char **environ;

namespace {

// The TERM family every shell gets, native or under Flatpak. Written once,
// in flatpak-spawn's `--env=` form; the native envp entry is the same text
// with that prefix skipped.
constexpr const char *kTermOverrides[] = {
    "--env=TERM=xterm-256color",
    "--env=COLORTERM=truecolor",
    "--env=TERM_PROGRAM=AntsTerminal",
    "--env=COLORFGBG=15;0",
};
constexpr size_t kEnvFlagLen = sizeof("--env=") - 1;

// ANTS-4541 — a Claude Code session's identity, inherited when the
// terminal was launched from inside one. The session it names is gone,
// and a claude started in the tab reads CLAUDE_CODE_CHILD_SESSION as a
// live parent and turns transcript saving off. Only the identity keys:
// configuration such as CLAUDE_CODE_USE_BEDROCK arrives the same way and
// must still reach the shell. A claude started in the tab sets its own.
constexpr const char *kClaudeIdentityKeys[] = {
    "CLAUDECODE=",
    "CLAUDE_CODE_CHILD_SESSION=",
    "CLAUDE_CODE_SESSION_ID=",
    "CLAUDE_PID=",
    "CLAUDE_CODE_MESSAGING_SOCKET=",
    "CLAUDE_CODE_EXECPATH=",
    "AI_AGENT=",
};

// True when the KEY=VALUE string `entry` sets the same key as `assignment`
// (a KEY= prefix or a whole KEY=VALUE).
bool sameKey(const char *entry, const char *assignment) {
    for (size_t i = 0;; ++i) {
        if (entry[i] != assignment[i]) return false;
        if (assignment[i] == '=') return true;
    }
}

// The caller's choice, else $SHELL, else the passwd entry, else /bin/bash.
QString resolveShell(const QString &shell) {
    if (!shell.isEmpty()) return shell;
    const char *envShell = ::getenv("SHELL");
    if (envShell && envShell[0] != '\0') return QString::fromLocal8Bit(envShell);
    struct passwd *pw = ::getpwuid(::getuid());
    if (pw && pw->pw_shell && pw->pw_shell[0] != '\0')
        return QString::fromLocal8Bit(pw->pw_shell);
    return QStringLiteral("/bin/bash");
}

struct winsize makeWinsize(int rows, int cols) {
    struct winsize ws = {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    return ws;
}

// The child's envp: this process's environment minus the keys the shell must
// not inherit, then the TERM overrides. `termProgramVersion` is the
// TERM_PROGRAM_VERSION=... assignment. ANTS-5075 — sized from environ, so no
// entry is dropped; variables set at run time (ANTS_MCP_SOCKET, ANTS-1897
// INV-14) are the ones a fixed table lost. The pointers are environ's own
// strings and string literals, read-only in the child.
std::vector<const char *> buildChildEnvp(const char *termProgramVersion) {
    size_t environCount = 0;
    for (char **e = environ; *e != nullptr; ++e) ++environCount;
    std::vector<const char *> envp;
    envp.reserve(environCount + std::size(kTermOverrides) + 2);  // + version, NUL
    for (char **e = environ; *e != nullptr; ++e) {
        const char *entry = *e;
        bool drop = sameKey(entry, termProgramVersion);
        for (const char *o : kTermOverrides) drop = drop || sameKey(entry, o + kEnvFlagLen);
        for (const char *k : kClaudeIdentityKeys) drop = drop || sameKey(entry, k);
        if (!drop) envp.push_back(entry);
    }
    for (const char *o : kTermOverrides) envp.push_back(o + kEnvFlagLen);
    envp.push_back(termProgramVersion);
    envp.push_back(nullptr);
    return envp;
}

} // namespace

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
    // Disable notifiers before closing FD to prevent reads/writes on a
    // closed/reused FD. Both are children of this Pty, so ~QObject deletes
    // them after this body (qt.md: never delete a parented child, ANTS-5075).
    if (m_readNotifier) m_readNotifier->setEnabled(false);
    if (m_writeNotifier) m_writeNotifier->setEnabled(false);

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
            // → SIGKILL escalation on the calling thread. Never a
            // detached worker: one that outlives the destructor races
            // Qt teardown (ANTS-1189). Blocking is safe because ~Pty
            // runs on the parse-worker thread (VtStream::~VtStream),
            // and the GUI caps its own wait on that thread at 2 s.
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
                // Bounded reap, never a blocking waitpid: a process in
                // uninterruptible sleep can defer SIGKILL, and an
                // unbounded wait outlasts the GUI's 2 s wait on this
                // thread, which aborts the program. The 1 s cap keeps
                // the destructor within 1.5 s. On timeout we leak a
                // zombie for init to reap.
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
    const QString shellPath = resolveShell(shell);
    struct winsize ws = makeWinsize(rows, cols);

    // The rule for everything up to the fork (ANTS-1046, ANTS-1135):
    // the child must not allocate between forkpty and exec. This process
    // is multithreaded, and malloc after fork can deadlock on an arena
    // lock another thread held. So every buffer the child reads — argv,
    // envp, paths — is built here, in the parent, and the child reads
    // them through fork's copy-on-write until exec replaces it.
    const bool inFlatpak =
        ::getenv("FLATPAK_ID") != nullptr ||
        ::access("/.flatpak-info", F_OK) == 0;
    char flatpakDirArg[PATH_MAX + 16];
    char flatpakMcpSocketArg[PATH_MAX + 32];
    if (inFlatpak) {
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

    // One buffer for both launches: flatpak-spawn takes it whole, the
    // native envp from kEnvFlagLen on.
    char termProgramVerArg[128];
    (void)::snprintf(termProgramVerArg, sizeof(termProgramVerArg),
                     "--env=TERM_PROGRAM_VERSION=%s", ANTS_VERSION);
    // ANTS-1135 — the child's envp, built here rather than by setenv() in
    // the child, because setenv can realloc `environ`.
    const std::vector<const char *> childEnvp =
        buildChildEnvp(termProgramVerArg + kEnvFlagLen);

    // ANTS-1135 follow-up (indie-review-2026-05-21): pre-fork the shell-path
    // and workdir byte conversions. toLocal8Bit() heap-allocates, which is
    // NOT on POSIX's async-signal-safe list — doing it in the child between
    // forkpty and execle can deadlock on the malloc arena lock if another
    // thread held it at fork. Compute here in the parent; the child reads
    // .constData() via fork's COW semantics with no child-side allocation.
    const QByteArray shellBytesPre   = shellPath.toLocal8Bit();
    const QByteArray workDirBytesPre = workDir.toLocal8Bit();
    // ANTS-2204 — capture HOME pre-fork too. The child's chdir-fallback branch
    // (below) read ::getenv("HOME") between fork and exec, which is not on the
    // POSIX async-signal-safe list (glibc's is lock-free, hence low real risk);
    // resolving it here in the parent matches the shell/workdir/setenv pre-fork
    // discipline so the child only reads a pre-built buffer.
    const char *homeEnvPre = ::getenv("HOME");
    const QByteArray homeBytesPre =
        homeEnvPre ? QByteArray(homeEnvPre) : QByteArray();

    // ANTS-5075 — argv[0] is built here too, so the child does no snprintf
    // between fork and exec. Login shell: the basename prefixed with "-".
    // Truncation is acceptable — argv[0] is a display/login marker, not a
    // lookup key (same-UID trust, ADR-0004), so the cast silences
    // cert-err33-c without losing a real guarantee.
    char argv0[256];
    {
        const char *shellName = ::strrchr(shellBytesPre.constData(), '/');
        shellName = shellName ? shellName + 1 : shellBytesPre.constData();
        (void)::snprintf(argv0, sizeof(argv0), "-%s", shellName);
    }

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
    // the child/parent setup below) shrinks to a few
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
                // Never below 1024: an fd inherited from a parent with a
                // higher limit can sit above this process's rlim_cur.
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
                // Fall back to home directory — resolved pre-fork (ANTS-2204) so
                // no async-signal-unsafe getenv() runs between fork and exec.
                // Last resort; if this fails too, the shell starts in the
                // inherited cwd.
                if (!homeBytesPre.isEmpty()) {
                    [[maybe_unused]] const int ignored =
                        ::chdir(homeBytesPre.constData());
                }
            }
        }

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
        // failure shape. See packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml.
        if (inFlatpak) {
            // Stack-allocated argv pointer table. Slots filled
            // unconditionally with stable string literals or with
            // pre-built buffers from the pre-fork prep. No heap
            // touch.
            const char *argv[std::size(kTermOverrides) + 8];
            size_t i = 0;
            argv[i++] = "flatpak-spawn";
            argv[i++] = "--host";
            for (const char *o : kTermOverrides) argv[i++] = o;
            argv[i++] = termProgramVerArg;
            if (flatpakDirArg[0] != '\0') argv[i++] = flatpakDirArg;
            if (flatpakMcpSocketArg[0] != '\0') argv[i++] = flatpakMcpSocketArg;
            argv[i++] = "--";
            argv[i++] = shellCStr;
            argv[i++] = nullptr;
            // ANTS-1994 — run flatpak-spawn itself with the pre-fork
            // sanitised childEnvp (built unconditionally above), not the
            // parent's raw `environ` that plain execvp() would inherit. This
            // matches the direct-exec path's execle(childEnvp) so the TERM*
            // normalisation is consistent across both branches and the
            // ANTS-1135 "child only execs a prepared envp" contract holds on
            // the flatpak path too. execvpe keeps execvp's PATH search (PATH is
            // preserved in childEnvp) and is async-signal-safe. The host shell
            // still receives only the explicit --env= values above; flatpak-
            // spawn does not forward its own env across the sandbox boundary.
            ::execvpe("flatpak-spawn",
                      const_cast<char *const *>(argv),
                      const_cast<char *const *>(childEnvp.data()));
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
                 const_cast<char *const *>(childEnvp.data()));
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

    const qsizetype written = writeSome(data.constData(), data.size());
    const qsizetype remaining = data.size() - written;
    if (remaining == 0) return;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Kernel PTY buffer full — queue the remainder and let
        // onWriteReady drain it when the kernel signals writability.
        if (remaining > MAX_PENDING_WRITE_BYTES) {
            ANTS_LOG(DebugLog::Pty,
                     "EAGAIN remainder %lld bytes exceeds queue cap "
                     "(%lld); dropping",
                     (long long)remaining,
                     (long long)MAX_PENDING_WRITE_BYTES);
            emit writeLost(remaining);  // ANTS-1994(3)
            return;
        }
        m_pendingWrite = QByteArray(data.constData() + written, remaining);
        if (m_writeNotifier) m_writeNotifier->setEnabled(true);
        return;
    }
    // Fatal error (EIO, EPIPE on a torn-down master, etc.). Don't queue —
    // the FD is gone or the slave reaped.
    ANTS_LOG(DebugLog::Pty,
             "write failed errno=%d; dropping %lld bytes",
             errno, (long long)remaining);
    emit writeLost(remaining);  // ANTS-4456
}

qsizetype Pty::writeSome(const char *buf, qsizetype len) {
    qsizetype written = 0;
    while (written < len) {
        const ssize_t n = ::write(m_masterFd, buf + written, len - written);
        if (n > 0) {
            written += n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            // A write that makes no progress can never finish: report it as
            // an I/O error so the caller treats it as fatal.
            if (n == 0) errno = EIO;
            break;
        }
    }
    return written;
}

void Pty::onWriteReady() {
    if (m_masterFd < 0 || m_pendingWrite.isEmpty()) {
        if (m_writeNotifier) m_writeNotifier->setEnabled(false);
        return;
    }
    const qsizetype written =
        writeSome(m_pendingWrite.constData(), m_pendingWrite.size());
    // Stopped early: a kernel buffer full again is retried on the next fire;
    // anything else is fatal. ANTS-4456: the two used to share one break,
    // and since the notifier is disarmed only when the queue empties and a
    // dead master keeps it firing, a fatal error spun the event loop at
    // 100% CPU for the life of the process.
    const bool fatal = written < m_pendingWrite.size() &&
                       !(errno == EAGAIN || errno == EWOULDBLOCK);
    const int fatalErrno = errno;
    if (written > 0) {
        m_pendingWrite.remove(0, written);
    }
    qint64 lost = 0;
    if (fatal) {
        lost = m_pendingWrite.size();
        ANTS_LOG(DebugLog::Pty,
                 "drain failed errno=%d; dropping %lld queued bytes",
                 fatalErrno, (long long)lost);
        // Dropping the queue is what lets the tail below disarm the
        // notifier — the bytes can never be written, so holding them
        // only keeps the spin alive.
        m_pendingWrite.clear();
    }
    const bool drained = m_pendingWrite.isEmpty();
    if (drained) {
        if (m_writeNotifier) m_writeNotifier->setEnabled(false);
    }
    // Emitted last, after every member is settled, so a slot that calls
    // back into this object cannot observe a half-updated queue.
    if (lost > 0) emit writeLost(lost);
    else if (drained) emit writeDrained();  // ANTS-5075 — resumes a paste feed
}

void Pty::setReadEnabled(bool enabled) {
    // ANTS-5026 — after EOF there is nothing left to read or reap, and
    // VtStream::drainAck re-enables on every ack, including one after EOF.
    if (m_readNotifier && !m_readEof) {
        m_readNotifier->setEnabled(enabled);
    }
}

void Pty::resize(int rows, int cols) {
    if (m_masterFd >= 0) {
        const struct winsize ws = makeWinsize(rows, cols);
        ::ioctl(m_masterFd, TIOCSWINSZ, &ws);
        ANTS_LOG(DebugLog::Pty, "resize rows=%d cols=%d", rows, cols);
    }
}

// ANTS-5135 — the header carries the state machine; the full table is in
// tests/features/pty_foreground_attr_reset/spec.md.
bool Pty::foregroundReturnedToShell(pid_t fg, pid_t shellPgid,
                                    pid_t &lastSeen) {
    const pid_t previous = lastSeen;
    // Record EVERY sample, the unusable ones included. Leaving lastSeen alone
    // when tcgetpgrp() cannot answer would make the next sample read as a
    // transition out of a stale value — inventing a reset rather than missing
    // one, which is the wrong direction for a check that strips styling.
    lastSeen = fg;

    // No readable foreground, or the child has been reaped: nothing to
    // conclude, and no shell left to return to.
    if (fg <= 0 || shellPgid <= 0)
        return false;
    // First sample, or the one after an unreadable sample. Cannot distinguish
    // "was a program" from "was always the shell", so stay silent.
    if (previous <= 0)
        return false;

    return previous != shellPgid && fg == shellPgid;
}

bool Pty::keysShareable(int masterFd) {
    struct termios t {};
    if (masterFd < 0 || ::tcgetattr(masterFd, &t) != 0) return false;
    return !((t.c_lflag & ICANON) && !(t.c_lflag & ECHO));
}

void Pty::onReadReady() {
    // ANTS-2119 — defensive guard: a QSocketNotifier can fire once more after
    // the fd was closed (destructor / start-failure / write-error paths set
    // m_masterFd = -1). Reading a closed fd returns EBADF, which would fall
    // through to the child-exit branch and spuriously emit finished(); bail.
    // ANTS-5026 — EOF is handled once. A second pass would reap with
    // m_childPid already -1, i.e. waitpid(-1): any child of the process.
    if (m_masterFd < 0 || m_readEof)
        return;

    // ANTS-5135 — a foreground program that has exited may have left
    // character attributes set: Claude Code emits ESC[2m around its dim
    // status lines, and killing it before the matching ESC[22m leaves dim on.
    // That is correct VT behaviour, so no parser fix reaches it, and this
    // host's prompt ("${USER}@${HOST}:${PWD}> ") carries no SGR reset of its
    // own — so every later prompt and everything typed renders dim.
    //
    // Sampled once per wake, BEFORE draining: the bytes waiting are usually
    // the prompt the shell is about to write, and the reset has to reach the
    // parser ahead of them. Emitted as DATA rather than as its own signal for
    // that same reason — a signal can overtake queued parse batches, and this
    // ordering is the whole point. Safe to inject here because the verdict is
    // only true once the foreground is the shell again, so no program is
    // mid-sequence.
    if (foregroundReturnedToShell(::tcgetpgrp(m_masterFd), childPid(),
                                  m_lastForegroundPgid)) {
        ANTS_LOG(DebugLog::Pty, "foreground back to shell - clearing SGR");
        emit dataReceived(QByteArrayLiteral("\033[0m"));
    }

    // ANTS-4272 — sampled on the worker, beside tcgetpgrp. A password prompt
    // is output, so the read carrying it samples the prompt's termios.
    if (m_keysShareableSink)
        m_keysShareableSink->store(keysShareable(m_masterFd), std::memory_order_relaxed);

    char buf[16384];
    while (true) {
        ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
        if (n > 0) {
            ANTS_LOG(DebugLog::Pty, "read %zd bytes", n);
            emit dataReceived(QByteArray(buf, static_cast<int>(n)));
            // ANTS-5026 — a dataReceived handler may have paused reads
            // (VtStream's back-pressure). Stop now; the level-triggered
            // notifier fires again once reads are re-enabled.
            if (!m_readNotifier->isEnabled())
                break;
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            // n == 0 → EOF (child closed the PTY).
            // n < 0  → read error other than EAGAIN/EINTR: the child has
            //          exited.
            m_readEof = true;
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
            // EAGAIN or EINTR — stop for now. Unlike the write loops, an
            // interrupted read needs no retry: the level-triggered read
            // notifier fires again while data is waiting.
            break;
        }
    }
}
