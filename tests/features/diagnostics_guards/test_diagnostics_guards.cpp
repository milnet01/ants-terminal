// Debug log rotation while running; read_log refuses non-regular files —
// see spec.md. ANTS-5110.

#include "buildcache.h"
#include "debuglog.h"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>

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

// INV-3
TEST(DiagnosticsGuards, BuildLogNoteAfterBlankLineStandsAlone) {
    const BuildCache::ParsedBuild b = BuildCache::parseBuildOutput(QStringLiteral(
        "a.cpp:1:2: error: boom\n\nb.cpp:3:4: note: elsewhere\n"));
    ASSERT_EQ(b.errors.size(), 1);
    EXPECT_EQ(b.errors.front().message, QStringLiteral("boom"))
        << "a note after a blank line folded into the error";
    EXPECT_EQ(b.warningsCount, 1);
}

// INV-4
TEST(DiagnosticsGuards, BuildLogNotesOfADroppedErrorAreDropped) {
    QString out;
    for (int i = 0; i < 51; ++i) {
        out += QStringLiteral("e.cpp:%1:1: error: err%1\n"
                              "e.cpp:%1:1: note: note%1\n").arg(i);
    }
    const BuildCache::ParsedBuild b = BuildCache::parseBuildOutput(out);
    ASSERT_EQ(b.errors.size(), 50);
    EXPECT_EQ(b.errorsCount, 51);
    EXPECT_EQ(b.errors.back().message, QStringLiteral("err49 / note49"))
        << "the 51st error's note was appended to the 50th";
    EXPECT_EQ(b.warningsCount, 0);
}

namespace {

// Point the debug log at a fresh directory for one test; restore on exit.
struct ScratchLog {
    QTemporaryDir dir;
    QByteArray prior = qgetenv("XDG_DATA_HOME");
    bool hadPrior = qEnvironmentVariableIsSet("XDG_DATA_HOME");
    ScratchLog() {
        qputenv("XDG_DATA_HOME", dir.path().toLocal8Bit());
        DebugLog::setActive(0);
        DebugLog::clear();   // closes any log an earlier test left open
    }
    ~ScratchLog() {
        DebugLog::setActive(0);
        DebugLog::clear();
        if (hadPrior) qputenv("XDG_DATA_HOME", prior);
        else qunsetenv("XDG_DATA_HOME");
    }
    static QByteArray read(const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
};

}  // namespace

// INV-5
TEST(DiagnosticsGuards, AlwaysLineReachesStderrWithNoCategoryOn) {
    ScratchLog log;
    ASSERT_TRUE(log.dir.isValid());
    const QString capture = log.dir.path() + "/stderr.txt";
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    const int fd = ::open(capture.toLocal8Bit().constData(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
    ANTS_LOG_ALWAYS("ants-5110 always line %d", 7);
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    EXPECT_TRUE(ScratchLog::read(capture).contains("ants-5110 always line 7"))
        << "an ANTS_LOG_ALWAYS line with no category on went nowhere";
}

// INV-6
TEST(DiagnosticsGuards, DebugLogReopensWhenRotatedAway) {
    ScratchLog log;
    ASSERT_TRUE(log.dir.isValid());
    DebugLog::setActive(DebugLog::Events);
    DebugLog::write(DebugLog::Events, QStringLiteral("before rotation"));
    const QString path = DebugLog::logFilePath();
    const QString rotated = path + QStringLiteral(".1");
    ASSERT_TRUE(QFile::rename(path, rotated));   // another instance rotates
    DebugLog::write(DebugLog::Events, QStringLiteral("after rotation"));
    DebugLog::setActive(0);
    EXPECT_TRUE(ScratchLog::read(path).contains("after rotation"))
        << "the writer kept appending to the rotated-away file";
    EXPECT_FALSE(ScratchLog::read(rotated).contains("after rotation"));
}
