// Review dispatcher reply deadline and size cap — see spec.md. ANTS-5101.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString dispatcherSource() {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/indiereviewdispatcher.cpp"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1
TEST(ReviewDispatchBounds, LaneHasWallClockDeadline) {
    const QString s = dispatcherSource();
    ASSERT_FALSE(s.isEmpty());
    EXPECT_TRUE(s.contains(QStringLiteral("QTimer::singleShot(req.perLaneTimeoutMs, reply,")))
        << "only the transfer (inactivity) timeout bounds a lane";
}

// INV-2
TEST(ReviewDispatchBounds, ReplyIsCapped) {
    const QString s = dispatcherSource();
    ASSERT_FALSE(s.isEmpty());
    EXPECT_TRUE(s.contains(QStringLiteral("received > LlmClient::kMaxBytes")))
        << "an LLM reply is read whole with no size cap";
    EXPECT_TRUE(s.contains(QStringLiteral("reply->property(\"antsTooLarge\")")));
}
