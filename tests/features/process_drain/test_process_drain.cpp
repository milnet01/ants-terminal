// ProcessDrain keeps a bounded slice of a child's output — see spec.md.
// ANTS-6497.

#include "processdrain.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

namespace {

bool startSh(QProcess &p, const QString &script) {
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
    return p.waitForStarted(5000);
}

// 3 MB of 'a': far past any cap below.
const QString kBigBody = QStringLiteral("head -c 3000000 /dev/zero | tr '\\0' a");

}  // namespace

// INV-1
TEST(ProcessDrain, ShortOutputIsKeptWhole) {
    QProcess p;
    ASSERT_TRUE(startSh(p, QStringLiteral("printf hello")));
    const auto r = ProcessDrain::run(p, 10000, 100, ProcessDrain::Keep::Tail);
    EXPECT_EQ(r.bytes, QByteArray("hello"));
    EXPECT_FALSE(r.truncated);
    EXPECT_FALSE(r.timedOut);
}

// INV-2
TEST(ProcessDrain, TailKeepsTheLastBytes) {
    QProcess p;
    ASSERT_TRUE(startSh(p, kBigBody + QStringLiteral("; printf END")));
    const auto r = ProcessDrain::run(p, 30000, 1000, ProcessDrain::Keep::Tail);
    EXPECT_EQ(r.bytes.size(), 1000);
    EXPECT_TRUE(r.bytes.endsWith("aEND"));
    EXPECT_TRUE(r.truncated);
    EXPECT_FALSE(r.timedOut);
}

// INV-3
TEST(ProcessDrain, HeadKeepsTheFirstBytesAndRunsToTheEnd) {
    QProcess p;
    ASSERT_TRUE(startSh(p, QStringLiteral("printf START; ") + kBigBody + QStringLiteral("; exit 3")));
    const auto r = ProcessDrain::run(p, 30000, 1000, ProcessDrain::Keep::Head);
    EXPECT_EQ(r.bytes.size(), 1000);
    EXPECT_TRUE(r.bytes.startsWith("STARTa"));
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(p.exitCode(), 3) << "the child was stopped before it finished";
}

// INV-4
TEST(ProcessDrain, PastTheDeadlineTheProcessIsKilled) {
    QProcess p;
    ASSERT_TRUE(startSh(p, QStringLiteral("exec sleep 30")));
    QElapsedTimer clock;
    clock.start();
    const auto r = ProcessDrain::run(p, 300, 100, ProcessDrain::Keep::Tail);
    EXPECT_TRUE(r.timedOut);
    EXPECT_LT(clock.elapsed(), 5000);
    EXPECT_EQ(p.state(), QProcess::NotRunning);
}
