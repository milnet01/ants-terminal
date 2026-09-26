// ANTS-5311 — ants-mcpd usage snapshots. Contract:
// docs/specs/ANTS-5311-mcpd-usage-snapshots.md § 3 (see spec.md beside this).
//
// Every case works in a QTemporaryDir. Liveness is flock(2) on a lock file, and
// flock treats two open() calls in one process as two holders, so a test
// "holding" a writer's lock is an honest stand-in for a live ants-mcpd.

#include "../../_support/xdg_guard.h"

#include "claudeintegration.h"
#include "remotecontrol.h"
#include "tokenusageengine.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using namespace TokenUsageEngine;

namespace {

ToolCounter counter(int n, qint64 in, qint64 out) {
    ToolCounter c;
    c.nCalls = n;
    c.bytesIn = in;
    c.bytesOut = out;
    return c;
}

// Writes <stem>.json from the real serialiser, and <stem>.lock when asked.
void writeSnapshot(const QString &dir, const QString &stem,
                   const QHash<QString, ToolCounter> &tools,
                   const QHash<QString, qint64> &bytesByRoot, bool withLock = true) {
    QDir().mkpath(dir);
    QFile f(dir + QLatin1Char('/') + stem + QStringLiteral(".json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(snapshotToJson(tools, bytesByRoot, 1, 1, 2)).toJson());
    f.close();
    if (withLock) {
        QFile l(dir + QLatin1Char('/') + stem + QStringLiteral(".lock"));
        ASSERT_TRUE(l.open(QIODevice::WriteOnly));
    }
}

void writeRaw(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

// Holds <stem>.lock the way a live writer does. Returns the fd (close it to
// "exit").
int holdLock(const QString &dir, const QString &stem) {
    const QByteArray p = (dir + QLatin1Char('/') + stem + QStringLiteral(".lock")).toLocal8Bit();
    const int fd = ::open(p.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    EXPECT_GE(fd, 0);
    EXPECT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
    return fd;
}

bool exists(const QString &dir, const QString &name) {
    return QFileInfo::exists(dir + QLatin1Char('/') + name);
}

}  // namespace

// INV-1
TEST(McpdUsageSnapshots, RoundTrip) {
    ToolCounter a;
    a.nCalls = 3; a.bytesIn = 11; a.bytesOut = 12; a.wrapBytes = 13;
    a.durationUsMin = 14; a.durationUsMax = 15; a.durationUsSum = 16;
    a.failedCalls = 17; a.failedBytesIn = 18; a.failedBytesOut = 19;
    const QHash<QString, ToolCounter> tools{{QStringLiteral("read_region"), a},
                                            {QStringLiteral("file_outline"), counter(1, 2, 3)}};
    const QHash<QString, qint64> roots{{QStringLiteral("/p/a"), 40}, {QStringLiteral("/p/b"), 7}};

    QHash<QString, ToolCounter> t2;
    QHash<QString, qint64> r2;
    QString why;
    ASSERT_TRUE(snapshotFromJson(snapshotToJson(tools, roots, 9, 10, 11), &t2, &r2, &why))
        << why.toStdString();
    ASSERT_EQ(t2.size(), 2);
    const ToolCounter b = t2.value(QStringLiteral("read_region"));
    EXPECT_EQ(b.nCalls, 3);        EXPECT_EQ(b.bytesIn, 11);
    EXPECT_EQ(b.bytesOut, 12);     EXPECT_EQ(b.wrapBytes, 13);
    EXPECT_EQ(b.durationUsMin, 14); EXPECT_EQ(b.durationUsMax, 15);
    EXPECT_EQ(b.durationUsSum, 16); EXPECT_EQ(b.failedCalls, 17);
    EXPECT_EQ(b.failedBytesIn, 18); EXPECT_EQ(b.failedBytesOut, 19);
    EXPECT_EQ(r2, roots);
}

// INV-2
TEST(McpdUsageSnapshots, LiveSnapshotNotClaimed) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    writeSnapshot(dir, QStringLiteral("13-13"), {{QStringLiteral("read_region"), counter(1, 10, 10)}}, {});
    const int fd = holdLock(dir, QStringLiteral("13-13"));

    PeerUsage dead;
    QList<int> locks;
    const PeerUsage all = readPeerSnapshots(dir, true, &dead, &locks);
    EXPECT_EQ(all.sessions, 1);
    EXPECT_EQ(dead.sessions, 0) << "a live writer's snapshot was claimed";
    releaseClaimed(dir, dead, locks);
    EXPECT_TRUE(exists(dir, QStringLiteral("13-13.json")));
    EXPECT_TRUE(exists(dir, QStringLiteral("13-13.lock")));
    ::close(fd);
}

// INV-3
TEST(McpdUsageSnapshots, DeadSnapshotFoldsOnce) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    writeSnapshot(dir, QStringLiteral("14-14"), {{QStringLiteral("read_region"), counter(2, 100, 0)}},
                  {{QStringLiteral("/r"), 40}});

    QJsonObject monthly, byProject;
    qint64 lifetime = 0;
    for (int round = 0; round < 2; ++round) {
        PeerUsage dead;
        QList<int> locks;
        readPeerSnapshots(dir, true, &dead, &locks);
        const bool changed = foldPeerUsage(dead, monthly, lifetime, byProject,
                                           QStringLiteral("2026-09"),
                                           QStringLiteral("2026-09-26T10:00:00"));
        releaseClaimed(dir, dead, locks);
        if (round == 0) {
            EXPECT_EQ(dead.sessions, 1);
            EXPECT_TRUE(changed);
        } else {
            EXPECT_EQ(dead.sessions, 0) << "a folded snapshot was claimed again";
            EXPECT_FALSE(changed);
        }
    }
    EXPECT_EQ(lifetime, (2 * 8192 - 100) / kCharsPerToken);
    EXPECT_EQ(static_cast<qint64>(byProject.value(QStringLiteral("/r")).toObject()
                                      .value(QStringLiteral("lifetime")).toDouble()),
              40 / kCharsPerToken);
    EXPECT_FALSE(exists(dir, QStringLiteral("14-14.json")));
    EXPECT_FALSE(exists(dir, QStringLiteral("14-14.lock")));
}

// INV-4 — a live net-negative and a dead net-positive snapshot on one tool, and
// one root whose byte counts are not multiples of kCharsPerToken. A figure
// derived over merged counters, or bytes divided after summing, moves here.
TEST(McpdUsageSnapshots, FoldPreservesDisplayedTotals) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    writeSnapshot(dir, QStringLiteral("1-1"), {{QStringLiteral("read_region"), counter(1, 20000, 0)}},
                  {{QStringLiteral("/r"), 10}});
    const int fd = holdLock(dir, QStringLiteral("1-1"));
    writeSnapshot(dir, QStringLiteral("2-2"), {{QStringLiteral("read_region"), counter(2, 100, 0)}},
                  {{QStringLiteral("/r"), 7}});

    QJsonObject monthly, byProject;
    qint64 lifetime = 0;
    const auto rootLife = [&] {
        return static_cast<qint64>(byProject.value(QStringLiteral("/r")).toObject()
                                       .value(QStringLiteral("lifetime")).toDouble());
    };
    const PeerUsage before = readPeerSnapshots(dir, false, nullptr, nullptr);
    const qint64 globalBefore = lifetime + before.savedTokens;
    const qint64 rootBefore = rootLife() + before.savedTokensByProject.value(QStringLiteral("/r"));

    PeerUsage dead;
    QList<int> locks;
    readPeerSnapshots(dir, true, &dead, &locks);
    ASSERT_EQ(dead.sessions, 1);
    foldPeerUsage(dead, monthly, lifetime, byProject, QStringLiteral("2026-09"),
                  QStringLiteral("2026-09-26T10:00:00"));
    releaseClaimed(dir, dead, locks);

    const PeerUsage after = readPeerSnapshots(dir, false, nullptr, nullptr);
    EXPECT_EQ(lifetime + after.savedTokens, globalBefore)
        << "the global displayed total moved at the fold";
    EXPECT_EQ(rootLife() + after.savedTokensByProject.value(QStringLiteral("/r")), rootBefore)
        << "the per-project displayed total moved at the fold";
    EXPECT_EQ(globalBefore, (2 * 8192 - 100) / kCharsPerToken);
    ::close(fd);
}

// INV-5
TEST(McpdUsageSnapshots, LoneLockAge) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    writeRaw(dir + QStringLiteral("/3-3.lock"), QByteArray());
    writeRaw(dir + QStringLiteral("/4-4.lock"), QByteArray());
    {
        QFile old(dir + QStringLiteral("/4-4.lock"));
        ASSERT_TRUE(old.open(QIODevice::ReadWrite));
        ASSERT_TRUE(old.setFileTime(QDateTime::currentDateTime().addSecs(-120),
                                    QFileDevice::FileModificationTime));
    }
    PeerUsage dead;
    QList<int> locks;
    const PeerUsage all = readPeerSnapshots(dir, true, &dead, &locks);
    releaseClaimed(dir, dead, locks);
    EXPECT_EQ(all.sessions, 0);
    EXPECT_EQ(dead.sessions, 0);
    EXPECT_TRUE(exists(dir, QStringLiteral("3-3.lock"))) << "a young lone lock was swept";
    EXPECT_FALSE(exists(dir, QStringLiteral("4-4.lock"))) << "an old lone lock was left";
}

// INV-6
TEST(McpdUsageSnapshots, TokenUsageIncludesPeers) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    guard.setEnv("XDG_DATA_HOME", tmp.path().toUtf8());
    const QString dir = peerSnapshotDir();
    ASSERT_TRUE(dir.startsWith(tmp.path())) << "refusing to write outside the sandbox: "
                                            << dir.toStdString();
    writeSnapshot(dir, QStringLiteral("15-15"), {{QStringLiteral("read_region"), counter(3, 30, 30)}}, {});

    ClaudeIntegration ci;
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdTokenUsage(QJsonObject{}, &ci).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonObject mcpd = resp.value(QStringLiteral("mcpd")).toObject();
    EXPECT_EQ(mcpd.value(QStringLiteral("calls")).toInt(), 3);
    EXPECT_EQ(mcpd.value(QStringLiteral("sessions")).toInt(), 1);
    EXPECT_EQ(mcpd.value(QStringLiteral("total_saved")).toInteger(),
              (3 * 8192 - 60) / kCharsPerToken);
    // The terminal's own session is untouched by peers (ANTS-1284's identity).
    EXPECT_EQ(resp.value(QStringLiteral("total_saved")).toInteger(), 0);
    EXPECT_TRUE(resp.value(QStringLiteral("calls")).toArray().isEmpty());
}

// INV-7
TEST(McpdUsageSnapshots, UntrustedFilesSkipped) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    writeRaw(dir + QStringLiteral("/5-5.json"), "{");
    writeRaw(dir + QStringLiteral("/6-6.json"), QByteArray(qsizetype{300} * 1024, ' '));
    QTemporaryDir elsewhere;
    writeSnapshot(elsewhere.path(), QStringLiteral("x"),
                  {{QStringLiteral("read_region"), counter(1, 1, 1)}}, {}, false);
    ASSERT_TRUE(QFile::link(elsewhere.path() + QStringLiteral("/x.json"),
                            dir + QStringLiteral("/7-7.json")));
    QJsonObject wrongFormat = snapshotToJson({}, {}, 1, 1, 1);
    wrongFormat[QStringLiteral("format")] = 2;
    writeRaw(dir + QStringLiteral("/8-8.json"), QJsonDocument(wrongFormat).toJson());
    writeSnapshot(dir, QStringLiteral("9-9"), {{QStringLiteral("read_region"), counter(-1, 1, 1)}}, {},
                  false);
    QHash<QString, qint64> tooMany;
    for (int i = 0; i < 65; ++i) tooMany.insert(QStringLiteral("/r%1").arg(i), 1);
    writeSnapshot(dir, QStringLiteral("10-10"), {}, tooMany, false);

    PeerUsage dead;
    QList<int> locks;
    const PeerUsage all = readPeerSnapshots(dir, true, &dead, &locks);
    releaseClaimed(dir, dead, locks);
    EXPECT_EQ(all.sessions, 0);
    EXPECT_EQ(all.savedTokens, 0);
    EXPECT_EQ(dead.sessions, 0);
    EXPECT_EQ(all.skipped.size(), 6) << all.skipped.join(QLatin1Char('\n')).toStdString();
    for (const char *f : {"5-5.json", "6-6.json", "7-7.json", "8-8.json", "9-9.json", "10-10.json"})
        EXPECT_TRUE(exists(dir, QLatin1String(f))) << f << " was removed";
}

// INV-8 — a regression guard: holds before ANTS-5311 (prove red by moving
// recordDispatch ahead of the forward branch).
TEST(McpdUsageSnapshots, ForwardedCallNotInSnapshot) {
    const QByteArray call =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"tab_list","arguments":{}}})";
    {
        ClaudeIntegration ci;
        ci.setForwarder({QStringLiteral("tab_list")},
            [](const QByteArray &, const ClaudeIntegration::ForwardReplyFn &onReply,
               const ClaudeIntegration::ForwardFailFn &) {
                onReply(R"({"jsonrpc":"2.0","id":7,"result":{}})");
            });
        QByteArray line;
        McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                            [] { return true; });
        ci.handleMcpLine(call, &out);
        EXPECT_EQ(ci.tokenUsageReport(true).toolsCalled, 0)
            << "an answered forward was counted by the helper";
        EXPECT_TRUE(snapshotToJson(ci.tokenUsageCounters(), {}, 1, 1, 1)
                        .value(QStringLiteral("tools")).toObject().isEmpty());
    }
    {
        ClaudeIntegration ci;
        ci.setForwarder({QStringLiteral("tab_list")},
            [](const QByteArray &, const ClaudeIntegration::ForwardReplyFn &,
               const ClaudeIntegration::ForwardFailFn &onFail) {
                onFail(QStringLiteral(R"({"ok":false,"code":"no_terminal","error":"x"})"));
            });
        QByteArray line;
        McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                            [] { return true; });
        ci.handleMcpLine(call, &out);
        EXPECT_EQ(ci.tokenUsageCounters().value(QStringLiteral("tab_list")).failedCalls, 1)
            << "a failed forward was not counted as a failed call";
    }
}

// INV-9 — a regression guard: holds before ANTS-5311 (prove red by adding a
// second m_config.save()).
TEST(McpdUsageSnapshots, SingleConfigSave) {
    QFile mw(QStringLiteral(ANTS_SOURCE_DIR "/src/mainwindow.cpp"));
    ASSERT_TRUE(mw.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(mw.readAll());
    const qsizetype at = src.indexOf(QStringLiteral("void MainWindow::foldTokenSavingsIntoConfig() {"));
    ASSERT_GE(at, 0);
    const qsizetype end = src.indexOf(QStringLiteral("\n}\n"), at);
    ASSERT_GT(end, at);
    EXPECT_EQ(src.mid(at, end - at).count(QStringLiteral("m_config.save()")), 1);
}

// INV-10
TEST(McpdUsageSnapshots, ConcurrentClaimOnce) {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    writeSnapshot(dir, QStringLiteral("11-11"), {{QStringLiteral("read_region"), counter(1, 1, 1)}}, {});
    writeSnapshot(dir, QStringLiteral("12-12"), {{QStringLiteral("read_region"), counter(1, 1, 1)}}, {},
                  /*withLock=*/false);

    PeerUsage deadA, deadB, deadC;
    QList<int> locksA, locksB, locksC;
    readPeerSnapshots(dir, true, &deadA, &locksA);
    readPeerSnapshots(dir, true, &deadB, &locksB);
    EXPECT_EQ(deadA.sessions, 2);
    EXPECT_EQ(deadB.sessions, 0) << "a second reader claimed what the first holds";
    releaseClaimed(dir, deadB, locksB);
    releaseClaimed(dir, deadA, locksA);
    readPeerSnapshots(dir, true, &deadC, &locksC);
    EXPECT_EQ(deadC.sessions, 0) << "a released snapshot was claimed again";
    releaseClaimed(dir, deadC, locksC);
    EXPECT_TRUE(QDir(dir).entryList(QDir::Files).isEmpty())
        << QDir(dir).entryList(QDir::Files).join(QLatin1Char(' ')).toStdString();
}
