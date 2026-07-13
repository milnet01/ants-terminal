// Feature-conformance test for spec.md (canonical:
// docs/specs/ANTS-1365.md). Pins the ensureSocketDir helper +
// the defaultSocketPath fallback format.
//
// SD-1..SD-4 exercise ensureSocketDir directly via QTemporaryDir.
// WI-1..WI-3 grep remotecontrol.cpp / secureio.h for the wiring.

#include "../../_support/expect.h"
#include "secureio.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_SECUREIO_H_PATH
#error "SRC_SECUREIO_H_PATH compile definition required"
#endif

// NOTE: this test deliberately exercises only the static-inline helper
// in secureio.h. Calling `RemoteControl::defaultSocketPath()` (defined
// in remotecontrol.cpp) would drag the full remotecontrol.cpp.o into
// the link line, which in turn pulls AuditEngine / FeatureCoverage /
// VtParser symbols that aren't part of ants_core_lib's transitive
// closure. The path-format invariant is covered by the WI-2 source-
// grep assertions below instead.

ANTS_TEST_SCOPE();

namespace {


// Quick check of dir mode bits. Returns -1 on failure, mode-bits
// masked with 0777 on success.
int dirMode(const QString &path) {
    struct stat st{};
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return -1;
    return st.st_mode & 0777;
}

// ---- Engine tests --------------------------------------------------

void testEngine() {
    // SD-1 happy path — call on a non-existent path; helper creates
    // it 0700, owned by us, ISDIR.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString target = tmp.path() + QStringLiteral("/ants-N");
        const bool ok = ensureSocketDir(target);
        expect(ok, "SD-1 ensureSocketDir creates the dir");
        expect(QDir(target).exists(), "SD-1 dir exists post-call");
        expect(dirMode(target) == 0700,
               (std::string("SD-1 dir mode 0700, got 0")
                + std::to_string(dirMode(target))).c_str());
    }

    // SD-2 idempotent — second call on the same path returns true,
    // no recreation needed.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString target = tmp.path() + QStringLiteral("/ants-N");
        expect(ensureSocketDir(target), "SD-2 first call creates");
        // Touch a marker file inside to confirm we don't recreate.
        QFile marker(target + QStringLiteral("/marker"));
        expect(marker.open(QIODevice::WriteOnly),
               "SD-2 marker file writable after first call");
        marker.close();
        expect(ensureSocketDir(target), "SD-2 second call returns true");
        expect(QFile::exists(target + QStringLiteral("/marker")),
               "SD-2 idempotent — marker survives");
    }

    // SD-3 symlink reject — pre-create as a symlink (even one
    // pointing to a valid dir) → helper rejects.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString validDir = tmp.path() + QStringLiteral("/real-dir");
        ASSERT_TRUE(QDir().mkdir(validDir));
        ::chmod(validDir.toLocal8Bit().constData(), 0700);

        const QString symlinkTarget = tmp.path() + QStringLiteral("/ants-N");
        ASSERT_EQ(0, ::symlink(validDir.toLocal8Bit().constData(),
                               symlinkTarget.toLocal8Bit().constData()));

        const bool ok = ensureSocketDir(symlinkTarget);
        expect(!ok, "SD-3 symlink rejected (even pointing to valid dir)");
    }

    // SD-4 wrong-mode reject — pre-create 0755 → reject.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString target = tmp.path() + QStringLiteral("/ants-N");
        ASSERT_TRUE(QDir().mkdir(target));
        ::chmod(target.toLocal8Bit().constData(), 0755);
        const bool ok = ensureSocketDir(target);
        expect(!ok, "SD-4 mode-0755 rejected");
    }

    // SD-5 (skipped — different-UID reject requires CAP_CHOWN to
    //       fixture; covered by code-review of the st_uid check).

    // SD-6 (folded into WI-2 source-grep — the path-format invariant
    //       doesn't justify dragging remotecontrol.cpp.o into the
    //       link line of test_core).
}

// ---- Wiring tests (source-grep) -----------------------------------

void testWiring() {
    const std::string rc = ants_test::slurpFile(SRC_RC_CPP);
    const std::string secio = ants_test::slurpFile(SRC_SECUREIO_H_PATH);

    // WI-1 — ensureSocketDir defined in secureio.h.
    {
        expect(secio.find("ensureSocketDir") != std::string::npos,
               "WI-1 ensureSocketDir defined in secureio.h");
    }

    // WI-2 — defaultSocketPath fallback uses /tmp/ants-<uid>/ form,
    // not the old /tmp/ants-terminal-<uid>.sock form.
    {
        expect(rc.find("/tmp/ants-%1") != std::string::npos,
               "WI-2a new /tmp/ants-<uid>/ subdir format present");
        expect(rc.find("/tmp/ants-terminal-%1.sock") == std::string::npos,
               "WI-2b old /tmp/ants-terminal-<uid>.sock format removed");
    }

    // WI-3 — start() calls ensureSocketDir on the socket-containing
    // directory before listen().
    {
        size_t startPos = rc.find("bool RemoteControl::start()");
        expect(startPos != std::string::npos,
               "WI-3 anchor: start() definition present");
        if (startPos != std::string::npos) {
            // Slurp a generous window of the start() body.
            const std::string body = rc.substr(startPos, 2500);
            expect(body.find("ensureSocketDir(") != std::string::npos,
                   "WI-3 start() calls ensureSocketDir before listen");
        }
    }

    // WI-4 — ::mkdir(..., 0700) appears in secureio.h.
    {
        expect(secio.find("::mkdir(") != std::string::npos,
               "WI-4a ::mkdir syscall present in secureio.h");
        expect(secio.find("0700") != std::string::npos,
               "WI-4b 0700 mode literal present in secureio.h");
    }
}

int runMain() {
    expect_reset();
    testEngine();
    testWiring();
    return expect_finish();
}

}  // namespace

TEST(RcSocketDirHardening, Main) { ASSERT_EQ(0, runMain()); }
