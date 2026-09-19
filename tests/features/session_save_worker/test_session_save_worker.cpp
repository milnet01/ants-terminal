// ANTS-5131 — feature-conformance test; see spec.md. Behavioural against
// SessionManager and TerminalGrid, plus source-scrapes.

#include "sessionmanager.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/xdg_guard.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <sys/stat.h>

namespace {

void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &act) {
        grid.processAction(act);
    });
    parser.feed(bytes.constData(), bytes.size());
}

QByteArray numberedLines(const QByteArray &tag, int n) {
    QByteArray out;
    for (int i = 0; i < n; ++i)
        out += tag + '-' + QByteArray::number(i) + "\r\n";
    return out;
}

QString rowText(const std::vector<Cell> &cells) {
    QString out;
    for (const Cell &c : cells)
        out += QChar(static_cast<char16_t>(c.codepoint ? c.codepoint : ' '));
    return out.trimmed();
}

QString lastScrollbackRow(const TerminalGrid &g) {
    return g.scrollbackSize() > 0
        ? rowText(g.scrollbackLine(g.scrollbackSize() - 1)) : QString();
}

void removeMatching(QDir dir, const QString &stem) {
    for (const QString &f : dir.entryList({stem + QStringLiteral("*")}, QDir::Files))
        dir.remove(f);
}

QString source(const char *rel) {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/") + QString::fromLatin1(rel));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString body(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    return end < 0 ? QString() : src.mid(start, end - start);
}

// A session id under a private test-mode directory, cleared on both ends.
struct Session {
    explicit Session(const char *id) : id(QString::fromLatin1(id)) {
        xdg.setTestMode(true);
        path = SessionManager::sessionPath(this->id);
        dir = QFileInfo(path).absoluteDir();
        stem = QFileInfo(path).fileName();
        removeMatching(dir, stem);
    }
    ~Session() {
        SessionManager::waitForPendingSaves();
        removeMatching(dir, stem);
    }
    ants_test::XdgGuard xdg;
    QString id, path, stem;
    QDir dir;
};

}  // namespace

// INV-1
TEST(SessionSaveWorker, Inv1AsyncSaveRoundTrips) {
    Session s("ants-5131-roundtrip");
    TerminalGrid grid(10, 80);
    feed(grid, numberedLines("async", 500));
    SessionManager::saveSessionAsync(s.id, &grid, QStringLiteral("/tmp/cwd"),
                                     QStringLiteral("pinned"));
    SessionManager::waitForPendingSaves();

    TerminalGrid back(10, 80);
    QString cwd, pinned;
    ASSERT_TRUE(SessionManager::loadSession(s.id, &back, &cwd, &pinned));
    EXPECT_EQ(back.scrollbackSize(), grid.scrollbackSize());
    EXPECT_EQ(lastScrollbackRow(back), lastScrollbackRow(grid));
    EXPECT_EQ(cwd, QStringLiteral("/tmp/cwd"));
    EXPECT_EQ(pinned, QStringLiteral("pinned"));
}

// INV-2
TEST(SessionSaveWorker, Inv2LaterSyncSaveWins) {
    Session s("ants-5131-order");
    TerminalGrid older(10, 80), newer(10, 80);
    older.setMaxScrollback(50000);
    feed(older, numberedLines("older", 40000));  // slow enough to overlap
    feed(newer, numberedLines("newer", 50));
    SessionManager::saveSessionAsync(s.id, &older);
    SessionManager::saveSession(s.id, &newer);
    SessionManager::waitForPendingSaves();

    TerminalGrid back(10, 80);
    ASSERT_TRUE(SessionManager::loadSession(s.id, &back));
    EXPECT_EQ(lastScrollbackRow(back), lastScrollbackRow(newer))
        << "an earlier async save landed after a later synchronous one";
}

// INV-3
TEST(SessionSaveWorker, Inv3RemovedSessionStaysRemoved) {
    Session s("ants-5131-remove");
    TerminalGrid grid(10, 80);
    grid.setMaxScrollback(50000);
    feed(grid, numberedLines("gone", 40000));
    SessionManager::saveSessionAsync(s.id, &grid);
    SessionManager::removeSession(s.id);
    SessionManager::waitForPendingSaves();
    EXPECT_FALSE(QFile::exists(s.path))
        << "an async save in flight put a removed session back";
}

// INV-4
TEST(SessionSaveWorker, Inv4OvershootWritesNothingAndIsReportedOnce) {
    Session s("ants-5131-overshoot");
    TerminalGrid prior(10, 80), big(10, 80);
    feed(prior, numberedLines("prior", 20));
    SessionManager::saveSession(s.id, &prior);
    ASSERT_TRUE(QFile::exists(s.path));

    feed(big, numberedLines("big", 500));
    ASSERT_GT(big.scrollbackSize(), 0);
    SessionManager::saveSessionAsync(s.id, &big, {}, {}, /*maxFileBytes=*/64);
    SessionManager::waitForPendingSaves();

    TerminalGrid back(10, 80);
    ASSERT_TRUE(SessionManager::loadSession(s.id, &back));
    EXPECT_EQ(lastScrollbackRow(back), lastScrollbackRow(prior))
        << "an overshooting async save replaced the prior blob";
    EXPECT_TRUE(SessionManager::takeOvershoot(s.id));
    EXPECT_FALSE(SessionManager::takeOvershoot(s.id)) << "reported twice";
}

// INV-5
TEST(SessionSaveWorker, Inv5BlobIsPrivateAndUmaskUntouched) {
    Session s("ants-5131-perms");
    const mode_t before = ::umask(0022);
    TerminalGrid grid(10, 80);
    feed(grid, numberedLines("perms", 50));
    SessionManager::saveSessionAsync(s.id, &grid);
    SessionManager::waitForPendingSaves();
    const mode_t during = ::umask(before);
    EXPECT_EQ(during, mode_t(0022)) << "a save changed the process umask";

    struct stat st {};
    ASSERT_EQ(::stat(QFile::encodeName(s.path).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, mode_t(0600));
}

// INV-6
TEST(SessionSaveWorker, Inv6DurabilityOrderUnchanged) {
    const QString src = source("sessionmanager.cpp");
    ASSERT_FALSE(src.isEmpty());
    const QString w = body(src, QStringLiteral("bool SessionManager::writeBlob("));
    ASSERT_FALSE(w.isEmpty());
    const int fsyncAt = w.indexOf(QStringLiteral("::fsync(file.handle())"));
    const int renameAt = w.indexOf(QStringLiteral("std::rename("));
    const int chmodAt = w.indexOf(QStringLiteral("setOwnerOnlyPerms(path)"));
    const int dirAt = w.indexOf(QStringLiteral("fsyncParentDir(path)"));
    ASSERT_GE(fsyncAt, 0);
    EXPECT_LT(fsyncAt, renameAt) << "the temp file is renamed before it is fsynced";
    EXPECT_LT(renameAt, chmodAt);
    EXPECT_LT(chmodAt, dirAt);

    const QString a = body(src, QStringLiteral("void SessionManager::saveSessionAsync("));
    ASSERT_FALSE(a.isEmpty());
    EXPECT_TRUE(a.contains(QStringLiteral("writeBlob(")));
    EXPECT_TRUE(a.contains(QStringLiteral("seal(")));
    EXPECT_FALSE(a.contains(QStringLiteral("::umask("))) << "the umask is process-wide";
    EXPECT_FALSE(w.contains(QStringLiteral("::umask("))) << "the umask is process-wide";
}

// INV-7
TEST(SessionSaveWorker, Inv7ForcedSaveStaysSynchronous) {
    const QString b = body(source("mainwindow.cpp"),
                           QStringLiteral("void MainWindow::saveAllSessions("));
    ASSERT_FALSE(b.isEmpty());
    const int branch = b.indexOf(QStringLiteral("if (force || overshot)"));
    const int sync = b.indexOf(QStringLiteral("SessionManager::saveSession("));
    const int async = b.indexOf(QStringLiteral("SessionManager::saveSessionAsync("));
    ASSERT_GE(branch, 0) << "the forced save no longer picks the synchronous path";
    EXPECT_LT(branch, sync);
    EXPECT_LT(sync, async);
}
