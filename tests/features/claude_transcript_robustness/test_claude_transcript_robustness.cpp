// Feature-conformance test for spec.md — asserts that the Claude Code
// transcript parser handles events larger than 32 KB without losing state
// and that decodeProjectPath preserves hyphens in directory names when a
// filesystem probe can disambiguate them.
//
// Links against src/claudeintegration.cpp. GUI-free (QCoreApplication only).
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "claudeintegration.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int inv1TailGrowsPastLargeEvent() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "[inv1] temp dir unusable\n");
        return 1;
    }

    // Construct a transcript whose FINAL line is a user tool_result whose
    // inline content exceeds the historical 32 KB tail window. Ordering
    // matters: the fat event has to be the last line for the old firstLine-
    // skip path to actually eat it. If we put a small event after it, the
    // old code gets away with reading the small event unharmed.
    //
    // Under the old window:
    //   seek(size - 32KB) lands inside the fat event's line.
    //   readLine() consumes up to the newline after the fat event.
    //   firstLine=true → that content is skipped.
    //   atEnd → loop exits with zero events → state unchanged.
    //
    // Under the growing window:
    //   32 KB → no newline in buffer → double → eventually finds the
    //   newline preceding the fat event → trim → parse the fat event.
    //   type=user with a tool_result → newState=Thinking, detail="processing result".
    const QString blob(40 * 1024, QLatin1Char('x'));
    const QByteArray fatTrailingEvent =
        QStringLiteral("{\"type\":\"user\",\"message\":"
                       "{\"content\":[{\"type\":\"tool_result\","
                       "\"tool_use_id\":\"t1\",\"content\":\"%1\"}]}}")
            .arg(blob)
            .toUtf8();

    const QString path = tmp.path() + "/large.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return 1;
    f.write(R"({"type":"user","message":{"content":"do a thing"}})"
            "\n");
    f.write(R"({"type":"assistant","message":{"stop_reason":"tool_use",)"
            R"("content":[{"type":"tool_use","name":"Read","input":{}}]}})"
            "\n");
    f.write(fatTrailingEvent);
    f.write("\n");
    f.close();

    ClaudeIntegration ci;
    QSignalSpy stateSpy(&ci, &ClaudeIntegration::stateChanged);
    ci.parseTranscriptForState(path);
    QCoreApplication::processEvents();

    const ClaudeState state = ci.currentState();
    const bool ok = (state == ClaudeState::Thinking) && stateSpy.count() >= 1;
    std::fprintf(stderr,
                 "[inv1 tail-grows-past-large-event] state=%d signals=%lld  %s\n",
                 static_cast<int>(state),
                 static_cast<long long>(stateSpy.count()),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv2TailGrowthBounded() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    // Pathological: 6 MiB of bytes with ZERO newlines. The decoder must
    // give up at the 4 MiB cap and return cleanly rather than spin.
    const QString path = tmp.path() + "/pathological.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return 1;
    const QByteArray junk(6 * 1024 * 1024, 'x');
    f.write(junk);
    f.close();

    ClaudeIntegration ci;
    ci.parseTranscriptForState(path);  // must not hang / OOM
    QCoreApplication::processEvents();

    std::fprintf(stderr, "[inv2 tail-growth-bounded]  PASS (returned)\n");
    return 0;
}

int inv3LeafHyphenPreserved() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    const QString leaf = "my-project";
    QDir root(tmp.path());
    if (!root.mkdir(leaf)) {
        std::fprintf(stderr, "[inv3] could not mkdir %s\n",
                     qUtf8Printable(leaf));
        return 1;
    }

    // Encode: tmp path is e.g. /tmp/qt_XXXXXX/my-project.
    // Claude encoding: every `/` → `-`. So for /tmp/qt_XXXXXX/my-project
    // the encoded form is `-tmp-qt_XXXXXX-my-project`.
    const QString realPath = tmp.path() + "/" + leaf;
    const QString encoded = "-" + realPath.mid(1).replace('/', '-');

    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const bool ok = (decoded == realPath);
    std::fprintf(stderr,
                 "[inv3 leaf-hyphen-preserved] encoded=%s decoded=%s want=%s  %s\n",
                 qUtf8Printable(encoded), qUtf8Printable(decoded),
                 qUtf8Printable(realPath), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv4IntermediateHyphenPreserved() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    QDir root(tmp.path());
    if (!root.mkpath("my-project/sub")) return 1;

    const QString realPath = tmp.path() + "/my-project/sub";
    const QString encoded = "-" + realPath.mid(1).replace('/', '-');

    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const bool ok = (decoded == realPath);
    std::fprintf(stderr,
                 "[inv4 intermediate-hyphen-preserved] decoded=%s want=%s  %s\n",
                 qUtf8Printable(decoded), qUtf8Printable(realPath),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv5MissingPathFallsBackToSeparator() {
    // No filesystem entries created: decoder cannot probe, must fall back
    // to the legacy "every dash is a slash" behavior so well-formed cases
    // (no embedded hyphens) don't regress.
    const QString encoded = "-nonexistent-top-level-missing-path-segment";
    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const QString want = "/nonexistent/top/level/missing/path/segment";
    const bool ok = (decoded == want);
    std::fprintf(stderr,
                 "[inv5 missing-path-falls-back-to-separator] decoded=%s  %s\n",
                 qUtf8Printable(decoded), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv6LegacyCaseStillWorks() {
    // Anchor: the path this very checkout lives at.
    // Encoded: all slashes flipped to dashes, leading `/` replaced by `-`.
    const QString encoded = "-mnt-Storage-Scripts-Linux-Ants";
    const QString want = "/mnt/Storage/Scripts/Linux/Ants";
    if (!QDir(want).exists()) {
        std::fprintf(stderr,
                     "[inv6 legacy-case] repo path %s missing on this host — skipping\n",
                     qUtf8Printable(want));
        return 0;
    }
    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const bool ok = (decoded == want);
    std::fprintf(stderr,
                 "[inv6 legacy-case-still-works] decoded=%s  %s\n",
                 qUtf8Printable(decoded), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ANTS-1867 investigation: on a found!=child mismatch, dump enough /proc
// state to decide WHY findClaudeChildPid missed the child — is the child a
// direct child of this process (ppid==self), and which task/<tid>/children
// (if any) list it? Prints only on failure, so it's free on green.
void dumpChildProcDiag(const char *tag, pid_t self, pid_t child) {
    // child ppid + comm
    QFile st(QString("/proc/%1/stat").arg(child));
    QString ppid = "?", comm = "?";
    if (st.open(QIODevice::ReadOnly)) {
        const QString s = QString::fromUtf8(st.readAll());
        const int rp = s.lastIndexOf(')');
        if (rp >= 0) {
            const QStringList f = s.mid(rp + 2).split(' ');
            if (f.size() >= 2) ppid = f[1];
        }
        const int lp = s.indexOf('(');
        if (lp >= 0 && rp > lp) comm = s.mid(lp + 1, rp - lp - 1);
    } else {
        ppid = "<no /proc/child/stat>";
    }
    QFile cl(QString("/proc/%1/cmdline").arg(child));
    QByteArray cmd;
    if (cl.open(QIODevice::ReadOnly)) { cmd = cl.readAll(); cmd.replace('\0', '|'); }
    std::fprintf(stderr,
                 "[%s DIAG] self=%d child=%d child_ppid=%s child_comm=%s "
                 "child_cmdline=[%s]\n",
                 tag, static_cast<int>(self), static_cast<int>(child),
                 ppid.toUtf8().constData(), comm.toUtf8().constData(),
                 cmd.constData());
    QDir td(QString("/proc/%1/task").arg(self));
    for (const QString &tid : td.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile cf(QString("/proc/%1/task/%2/children").arg(self).arg(tid));
        if (!cf.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "[%s DIAG]   task %s children=<no-open>\n",
                         tag, tid.toUtf8().constData());
            continue;
        }
        const QString kids = QString::fromUtf8(cf.readAll()).trimmed();
        std::fprintf(stderr, "[%s DIAG]   task %s children=[%s]\n",
                     tag, tid.toUtf8().constData(), kids.toUtf8().constData());
    }
}

int inv7FindClaudeChildPidDetectsChild() {
    // ANTS-1845: after the early-out refactor of findClaudeChildPid, a
    // direct `claude`-named child of the shell must still be detected via
    // the kernel children file (the fast path). This is the live detection
    // primitive (every 2 s × tab) — a refactor bug here silently breaks
    // every Claude status indicator, so lock the behaviour.
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    const QString sleepBin = QStandardPaths::findExecutable("sleep");
    if (sleepBin.isEmpty()) {
        std::fprintf(stderr, "[inv7] no 'sleep' on PATH — skipping\n");
        return 0;
    }

    // Symlink the real sleep binary under the basename "claude" so the
    // child's argv[0] basename matches isClaudeBin(). exec follows the
    // symlink; QProcess sets argv[0] to the program path we hand it.
    const QString fakeClaude = tmp.path() + "/claude";
    if (!QFile::link(sleepBin, fakeClaude)) {
        std::fprintf(stderr, "[inv7] could not symlink %s -> %s\n",
                     qUtf8Printable(fakeClaude), qUtf8Printable(sleepBin));
        return 1;
    }

    QProcess proc;
    proc.setProgram(fakeClaude);
    proc.setArguments({QStringLiteral("30")});
    proc.start();
    if (!proc.waitForStarted(3000)) {
        std::fprintf(stderr, "[inv7] fake claude failed to start\n");
        return 1;
    }
    const pid_t child = static_cast<pid_t>(proc.processId());

    const pid_t found = ClaudeIntegration::findClaudeChildPid(::getpid());

    const bool ok = (found == child);
    if (!ok) dumpChildProcDiag("inv7", ::getpid(), child);  // before kill()

    proc.kill();
    proc.waitForFinished(3000);

    std::fprintf(stderr,
                 "[inv7 findclaudechildpid-detects-child] child=%d found=%d  %s\n",
                 static_cast<int>(child), static_cast<int>(found),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// A QThread that forks the fake-claude child from its own (non-leader)
// thread context, then keeps that thread alive via an event loop so the
// child stays accounted under the worker tid while the test probes for it.
// QProcess is created/destroyed in the same thread to respect its affinity.
class ClaudeLauncherThread : public QThread {
public:
    QString program;
    std::atomic<pid_t> child{0};

protected:
    void run() override {
        QProcess proc;
        proc.setProgram(program);
        proc.setArguments({QStringLiteral("30")});
        proc.start();
        if (proc.waitForStarted(3000))
            child.store(static_cast<pid_t>(proc.processId()));
        exec();  // keep this tid alive until quit() so the child stays ours
        proc.kill();
        proc.waitForFinished(3000);
    }
};

int inv8FindClaudeChildFromWorkerThread() {
    // ANTS-1867: the kernel `children` file is per-TID. A claude child
    // forked by a NON-leader thread (exactly what Qt's QProcess launcher
    // does on CI's Qt build) is absent from /proc/<pid>/task/<pid>/children.
    // findClaudeChildPid must union across every task/<tid>/children, else
    // it reports "no claude" while one is running — INV-7 passed on dev
    // machines (main-thread fork) yet the same spawn was invisible on CI.
    const QString sleepBin = QStandardPaths::findExecutable("sleep");
    if (sleepBin.isEmpty()) {
        std::fprintf(stderr, "[inv8] no 'sleep' on PATH — skipping\n");
        return 0;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QString fakeClaude = tmp.path() + "/claude";
    if (!QFile::link(sleepBin, fakeClaude)) {
        std::fprintf(stderr, "[inv8] could not symlink %s -> %s\n",
                     qUtf8Printable(fakeClaude), qUtf8Printable(sleepBin));
        return 1;
    }

    ClaudeLauncherThread launcher;
    launcher.program = fakeClaude;
    launcher.start();
    for (int i = 0; i < 300 && launcher.child.load() == 0; ++i)
        QThread::msleep(10);
    const pid_t child = launcher.child.load();

    pid_t found = -1;
    if (child > 0) found = ClaudeIntegration::findClaudeChildPid(::getpid());

    launcher.quit();
    launcher.wait(5000);

    const bool ok = (child > 0 && found == child);
    std::fprintf(stderr,
                 "[inv8 findclaudechildpid-worker-thread-fork] child=%d found=%d  %s\n",
                 static_cast<int>(child), static_cast<int>(found),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv9FindClaudeChildAfterForkingThreadExits() {
    // ANTS-1867: the load-bearing case. A child whose forking thread has
    // EXITED is orphaned from every /proc/<pid>/task/<tid>/children (the
    // tid is gone) but still carries ppid == this process. The per-thread
    // children union therefore misses it; only the /proc ppid scan finds
    // it. This is the exact shape of Qt's QProcess transient launcher
    // thread on CI's Qt build — inv7 was red there because the union
    // short-cut skipped the scan. Reproduced deterministically here by
    // forking from a QThread that then exits before detection runs.
    const QString sleepBin = QStandardPaths::findExecutable("sleep");
    if (sleepBin.isEmpty()) {
        std::fprintf(stderr, "[inv9] no 'sleep' on PATH — skipping\n");
        return 0;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QString fakeClaude = tmp.path() + "/claude";
    if (!QFile::link(sleepBin, fakeClaude)) {
        std::fprintf(stderr, "[inv9] could not symlink %s -> %s\n",
                     qUtf8Printable(fakeClaude), qUtf8Printable(sleepBin));
        return 1;
    }

    // Precompute argv byte buffers BEFORE fork — only async-signal-safe
    // calls (execl/_exit) run in the child between fork and exec.
    const QByteArray prog = fakeClaude.toLocal8Bit();
    const QByteArray arg = QByteArrayLiteral("30");
    std::atomic<pid_t> child{0};

    QThread *forker = QThread::create([&]() {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::execl(prog.constData(), prog.constData(), arg.constData(),
                    static_cast<char *>(nullptr));
            ::_exit(127);
        }
        child.store(pid);  // parent (this thread) records, then returns
    });
    forker->start();
    for (int i = 0; i < 300 && child.load() == 0; ++i) QThread::msleep(10);
    forker->wait(5000);   // thread fully exits → its task/<tid> disappears
    delete forker;
    QThread::msleep(50);  // let the kernel retire the dead tid

    const pid_t c = child.load();
    pid_t found = -1;
    if (c > 0) found = ClaudeIntegration::findClaudeChildPid(::getpid());

    if (c > 0) {  // reap the orphaned child
        ::kill(c, SIGKILL);
        int status = 0;
        ::waitpid(c, &status, 0);
    }

    const bool ok = (c > 0 && found == c);
    std::fprintf(stderr,
                 "[inv9 findclaudechildpid-orphaned-after-thread-exit] "
                 "child=%d found=%d  %s\n",
                 static_cast<int>(c), static_cast<int>(found),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

TEST(ClaudeTranscriptRobustness, Main) {
    int failures = 0;
    failures += inv1TailGrowsPastLargeEvent();
    failures += inv2TailGrowthBounded();
    failures += inv3LeafHyphenPreserved();
    failures += inv4IntermediateHyphenPreserved();
    failures += inv5MissingPathFallsBackToSeparator();
    failures += inv6LegacyCaseStillWorks();
    failures += inv7FindClaudeChildPidDetectsChild();
    failures += inv8FindClaudeChildFromWorkerThread();
    failures += inv9FindClaudeChildAfterForkingThreadExits();
    if (failures) FAIL();
}

