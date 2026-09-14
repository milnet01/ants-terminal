// Content verb argument bounds — see spec.md. ANTS-5098.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel) {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/") + QString::fromUtf8(rel));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1
TEST(ContentVerbBounds, InboxPageIsBounded) {
    const QString s = source("remotecontrol_session_message.cpp");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_TRUE(s.contains(QStringLiteral("const int limit  = qBound(1,")))
        << "a limit of 0 or less returns the whole mailbox";
    EXPECT_TRUE(s.contains(QStringLiteral("const int offset = qMax(0,")));
}

// INV-2
TEST(ContentVerbBounds, MessageIdIsRangeChecked) {
    const QString s = source("remotecontrol_session_message.cpp");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_FALSE(s.contains(QStringLiteral(
        "qint64(req.value(QStringLiteral(\"message_id\")).toDouble())")))
        << "message_id is cast from a double with no range check";
    EXPECT_TRUE(s.contains(QStringLiteral("must be a \"\n")))
        << "no refusal for an out-of-range message_id";
}

// INV-3
TEST(ContentVerbBounds, AddBatchIsCapped) {
    const QString s = source("remotecontrol_changelog.cpp");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_TRUE(s.contains(QStringLiteral("entries.size() > kMaxBatchEntries")))
        << "add_batch's entries array has no cap";
}
