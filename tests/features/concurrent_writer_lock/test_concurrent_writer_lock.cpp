// Feature-conformance test for spec.md —
//
// Two halves:
//   1. Runtime: ConfigWriteLock acquires once, blocks the second
//      acquire (verified via fork(2) — flock semantics distinguish
//      processes, not threads), and releases on destruction.
//   2. Source-grep: every save site (Config::save,
//      ClaudeAllowlistDialog::saveSettings, SettingsDialog hook
//      installers) constructs a ConfigWriteLock and checks acquired().
//
// No Qt event loop required — ConfigWriteLock is plain POSIX flock(2)
// wrapped in RAII.

#include "../../_support/expect.h"
#include "configbackup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <QString>
#include <QTemporaryDir>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#ifndef SRC_CONFIG_CPP_PATH
#  error "SRC_CONFIG_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDEALLOWLIST_CPP_PATH
#  error "SRC_CLAUDEALLOWLIST_CPP_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#  error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




// Fork a child that tries to acquire ConfigWriteLock on `path` and
// exits with code 0 if it acquired, 1 if it did not. The child uses
// a SHORT-DEADLINE wrapper because the production helper waits 5 s,
// which is too long for tests. We instead reach for a private
// fast-path: directly call flock(LOCK_EX | LOCK_NB) once and report.
//
// This isolates "can a sibling process acquire?" from "the helper's
// 5-second deadline" — both invariants matter, but the deadline is
// not the test we want to wait on.
int childTryLock(const QString &path) {
    pid_t pid = ::fork();
    if (pid == 0) {
        // Child.
        const std::string lockPath =
            (path + QStringLiteral(".lock")).toLocal8Bit().toStdString();
        // O_NOFOLLOW: refuse to open through a symlink (defence-in-depth;
        // the lock file lives in a private QTemporaryDir so no attacker can
        // pre-place one, but the flag costs nothing). NOT O_EXCL: the parent
        // ConfigWriteLock legitimately pre-creates this file and the child
        // must open the *existing* file to contend on the flock (ANTS-1380).
        int fd = ::open(lockPath.c_str(),
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) std::exit(2);
        int rc = ::flock(fd, LOCK_EX | LOCK_NB);
        std::exit(rc == 0 ? 0 : 1);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

void runtimeTests() {
    // Private 0700 dir with a random name, auto-removed at scope exit —
    // no predictable path in world-writable /tmp, so no symlink-attack
    // window on the lock file (ANTS-1380). Parallel ctest runs each get
    // their own dir, so the old pid/time uniqueness is no longer needed.
    QTemporaryDir tmp;
    expect(tmp.isValid(), "I-A/tempdir-created",
           tmp.errorString().toStdString());
    const QString path = tmp.path() + QStringLiteral("/ants-cwl.dat");

    {
        ConfigWriteLock A(path);
        expect(A.acquired(), "I-A/first-lock-acquires",
               path.toStdString());

        // I1: a sibling process MUST NOT be able to acquire while A holds.
        int childRc = childTryLock(path);
        expect(childRc == 1,
               "I1/sibling-process-blocked-while-locked",
               "expected child to fail to acquire (rc=1); got rc=" +
                   std::to_string(childRc));
    }

    // I2: after A destructs, a fresh sibling acquire must succeed.
    int childRc = childTryLock(path);
    expect(childRc == 0,
           "I2/sibling-acquires-after-release",
           "expected child to succeed (rc=0); got rc=" +
               std::to_string(childRc));

    // I2 (also): consecutive in-process locks both acquire.
    {
        ConfigWriteLock B(path);
        expect(B.acquired(), "I2/in-process-second-lock-acquires");
    }
    {
        ConfigWriteLock C(path);
        expect(C.acquired(), "I2/in-process-third-lock-acquires");
    }

    // Cleanup.
    ::unlink((path + QStringLiteral(".lock")).toLocal8Bit().constData());
}

void sourceGrepTests() {
    {
        const std::string src = ants_test::slurpFile(SRC_CONFIG_CPP_PATH);
        expect(src.find("ConfigWriteLock writeLock(path)") != std::string::npos,
               "I3/config-save-constructs-lock");
        // Lock construction must precede the actual write so the
        // lock covers the whole rename window. We anchor on the
        // QFile(tmpPath) construction.
        const size_t lockIdx = src.find("ConfigWriteLock writeLock(path)");
        const size_t writeIdx = src.find("QFile file(tmpPath)");
        expect(lockIdx != std::string::npos &&
                   writeIdx != std::string::npos &&
                   lockIdx < writeIdx,
               "I3/config-lock-before-write");
        expect(src.find("writeLock.acquired()") != std::string::npos,
               "I3/config-checks-acquired");
    }

    {
        const std::string src = ants_test::slurpFile(SRC_CLAUDEALLOWLIST_CPP_PATH);
        expect(src.find("ConfigWriteLock writeLock(m_settingsPath)") !=
                   std::string::npos,
               "I4/allowlist-saveSettings-constructs-lock");
        expect(src.find("writeLock.acquired()") != std::string::npos,
               "I4/allowlist-checks-acquired");
    }

    {
        const std::string src = ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP_PATH);
        // Both installers (installClaudeHooks +
        // installClaudeGitContextHook) target the same settingsPath
        // variable, so the construction text matches both sites; we
        // require ≥2 occurrences.
        size_t pos = 0;
        int count = 0;
        const std::string needle = "ConfigWriteLock writeLock(settingsPath)";
        while ((pos = src.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        expect(count >= 2,
               "I5/settingsdialog-installers-construct-lock-twice",
               "got " + std::to_string(count));
        expect(src.find("writeLock.acquired()") != std::string::npos,
               "I5/settingsdialog-checks-acquired");
    }
}

}  // namespace

static int runMain() {
    expect_reset();
    runtimeTests();
    sourceGrepTests();

    return expect_finish();
}

TEST(ConcurrentWriterLock, Main) {
    ASSERT_EQ(0, runMain());
}
