// Debug log rotation while running; read_log refuses non-regular files —
// see spec.md. ANTS-5110.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString body(const char *rel, const QString &signature) {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/") + QString::fromUtf8(rel));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    const QString src = QString::fromUtf8(f.readAll());
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    return end < 0 ? QString() : src.mid(start, end - start);
}

}  // namespace

// INV-1
TEST(DiagnosticsGuards, DebugLogRotatesWhileWriting) {
    const QString b = body("debuglog.cpp", QStringLiteral("void DebugLog::write("));
    ASSERT_FALSE(b.isEmpty());
    EXPECT_TRUE(b.contains(QStringLiteral("s_file.size() > kMaxLogBytes")))
        << "the size cap is checked only when the log opens";
}

// INV-2
TEST(DiagnosticsGuards, ReadLogRefusesNonRegularFiles) {
    const QString b = body("remotecontrol_workspace.cpp",
                           QStringLiteral("QJsonDocument RemoteControl::cmdReadLog("));
    ASSERT_FALSE(b.isEmpty());
    const int guard = b.indexOf(QStringLiteral("!QFileInfo(resolved).isFile()"));
    const int read = b.indexOf(QStringLiteral("ReadLog::filter("));
    ASSERT_GE(read, 0);
    ASSERT_GE(guard, 0) << "read_log opens a FIFO and blocks";
    EXPECT_LT(guard, read);
}
