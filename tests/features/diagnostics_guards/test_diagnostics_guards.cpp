// Debug log rotation while running; read_log refuses non-regular files —
// see spec.md. ANTS-5110.

#include "../../_support/xdg_guard.h"
#include "buildcache.h"
#include "debuglog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/file.h>
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
    ants_test::XdgGuard env;
    ScratchLog() {
        env.setEnv("XDG_DATA_HOME", dir.path().toLocal8Bit());
        DebugLog::setActive(0);
        DebugLog::clear();   // closes any log an earlier test left open
    }
    ~ScratchLog() {
        DebugLog::setActive(0);
        DebugLog::clear();
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

// INV-7 (ANTS-6528) — a build cache over kMaxCacheFileBytes is not loaded.
TEST(DiagnosticsGuards, Ants6528OversizedBuildCacheIsNotLoaded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    BuildCache::ParsedBuild b;
    b.recordedAtMs = 1;
    ASSERT_TRUE(BuildCache::recordBuild(tmp.path(), b));
    ASSERT_TRUE(BuildCache::loadBuild(tmp.path()).has_value());
    QJsonObject o = BuildCache::toJson(b);
    o[QStringLiteral("padding")] =
        QString(int(BuildCache::kMaxCacheFileBytes), QLatin1Char('p'));
    QFile f(BuildCache::cachePath(tmp.path()));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.close();
    EXPECT_FALSE(BuildCache::loadBuild(tmp.path()).has_value())
        << "a cache file over the cap was read and loaded";
}

// INV-8 (ANTS-6528) — fromJson keeps the writer's caps.
TEST(DiagnosticsGuards, Ants6528FromJsonKeepsTheWritersCaps) {
    QJsonArray errs;
    for (int i = 0; i < 80; ++i)
        errs.append(QJsonObject{{"file", "a.cpp"}, {"line", i}, {"severity", "error"},
                                {"message", QString(1000, QLatin1Char('m'))}});
    QJsonObject o = BuildCache::toJson(BuildCache::ParsedBuild{});
    o[QStringLiteral("errors")] = errs;
    bool ok = false;
    const BuildCache::ParsedBuild b = BuildCache::fromJson(o, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(b.errors.size(), 50);
    for (const auto &e : b.errors) EXPECT_LE(e.message.size(), 240);
}

// INV-9 (ANTS-6596) — a `fatal error:` line is an error.
TEST(DiagnosticsGuards, BuildLogFatalErrorIsAnError) {
    const BuildCache::ParsedBuild b = BuildCache::parseBuildOutput(QStringLiteral(
        "a.cpp:1:10: fatal error: x.h: No such file or directory\n"));
    ASSERT_EQ(b.errors.size(), 1) << "a fatal error was not parsed as an error";
    EXPECT_EQ(b.errorsCount, 1);
    EXPECT_EQ(b.errors.front().message, QStringLiteral("x.h: No such file or directory"));
}

// INV-10 — staleness is measured from the build's start when it was given:
// a file edited while the build ran makes the result stale.
TEST(DiagnosticsGuards, StaleIsMeasuredFromTheBuildStart) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(QDir(tmp.path()).mkpath(QStringLiteral("src")));
    QFile f(tmp.path() + QStringLiteral("/src/a.cpp"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("int a;\n");
    f.close();
    const qint64 edited = QFileInfo(f.fileName()).lastModified().toMSecsSinceEpoch();

    BuildCache::ParsedBuild b;
    b.startedAtMs  = edited - 60'000;   // the build started before the edit
    b.recordedAtMs = edited + 60'000;   // and was recorded after it
    EXPECT_EQ(BuildCache::staleSinceMs(b), b.startedAtMs);
    const BuildCache::StaleCheck s =
        BuildCache::checkStale(tmp.path(), BuildCache::staleSinceMs(b));
    EXPECT_TRUE(s.staleKnown);
    EXPECT_TRUE(s.stale) << "an edit made during the build was reported fresh";

    b.startedAtMs = 0;                  // no start time: the record time
    EXPECT_EQ(BuildCache::staleSinceMs(b), b.recordedAtMs);
    EXPECT_FALSE(BuildCache::checkStale(tmp.path(), BuildCache::staleSinceMs(b)).stale);
}

// INV-11 — a cache with no time to compare against is not reported fresh.
TEST(DiagnosticsGuards, StaleIsUnknownWithNoBuildTime) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_FALSE(BuildCache::checkStale(tmp.path(), 0).staleKnown)
        << "a build with no recorded time was reported known-fresh";
}

// INV-12 — the message cap never splits a surrogate pair, in the parser or
// in fromJson.
TEST(DiagnosticsGuards, MessageCapNeverSplitsASurrogatePair) {
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");   // two UTF-16 units
    ASSERT_EQ(emoji.size(), 2);
    const QString msg = QString(BuildCache::kMessageCharCap - 1, QLatin1Char('m'))
                        + emoji + QStringLiteral("tail");
    const BuildCache::ParsedBuild b = BuildCache::parseBuildOutput(
        QStringLiteral("a.cpp:1:2: error: ") + msg + QLatin1Char('\n'));
    ASSERT_EQ(b.errors.size(), 1);
    const QString &m = b.errors.front().message;
    ASSERT_FALSE(m.isEmpty());
    EXPECT_LE(m.size(), BuildCache::kMessageCharCap);
    EXPECT_FALSE(m.back().isHighSurrogate()) << "the cut split a surrogate pair";

    QJsonObject o = BuildCache::toJson(BuildCache::ParsedBuild{});
    o[QStringLiteral("errors")] = QJsonArray{QJsonObject{
        {"file", "a.cpp"}, {"line", 1}, {"severity", "error"}, {"message", msg}}};
    bool ok = false;
    const BuildCache::ParsedBuild r = BuildCache::fromJson(o, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(r.errors.size(), 1);
    ASSERT_FALSE(r.errors.front().message.isEmpty());
    EXPECT_FALSE(r.errors.front().message.back().isHighSurrogate())
        << "fromJson's cut split a surrogate pair";
}

// INV-13 — clearing the log removes the rotated generation too.
TEST(DiagnosticsGuards, ClearRemovesTheRotatedLog) {
    ScratchLog log;
    ASSERT_TRUE(log.dir.isValid());
    const QString rotated = DebugLog::logFilePath() + QStringLiteral(".1");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(rotated).absolutePath()));
    QFile f(rotated);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("older pty output\n");
    f.close();
    DebugLog::clear();
    EXPECT_FALSE(QFileInfo::exists(rotated)) << "Clear Log File kept debug.log.1";
}

// INV-14 — rotation happens under debug.log.lock: while another instance
// holds it, this one does not rotate; once it is free, the next write does.
TEST(DiagnosticsGuards, RotationWaitsForTheRotationLock) {
    ScratchLog log;
    ASSERT_TRUE(log.dir.isValid());
    const QString path = DebugLog::logFilePath();
    const QString rotated = path + QStringLiteral(".1");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    {
        QFile big(path);
        ASSERT_TRUE(big.open(QIODevice::WriteOnly));
        ASSERT_TRUE(big.resize(qint64(10) * 1024 * 1024 + 1));   // past the 10 MiB cap
    }
    const int fd = ::open((path + QStringLiteral(".lock")).toLocal8Bit().constData(),
                          O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX), 0);   // another instance is rotating
    DebugLog::setActive(DebugLog::Events);
    const bool rotatedWhileHeld = QFileInfo::exists(rotated);
    ::close(fd);
    EXPECT_FALSE(rotatedWhileHeld)
        << "the log rotated while another instance held the rotation lock";

    DebugLog::write(DebugLog::Events, QStringLiteral("after the lock"));
    DebugLog::setActive(0);
    EXPECT_TRUE(QFileInfo::exists(rotated)) << "the log never rotated once the lock was free";
}

// INV-15 — a failed rotation turns file logging off and says so once on
// stderr, rather than reopening the log on every write.
TEST(DiagnosticsGuards, FailedRotationStopsFileLogging) {
    ScratchLog log;
    ASSERT_TRUE(log.dir.isValid());
    const QString path = DebugLog::logFilePath();
    const QString rotated = path + QStringLiteral(".1");
    // A non-empty directory at debug.log.1 makes the rotation's remove and
    // rename both fail.
    ASSERT_TRUE(QDir().mkpath(rotated));
    {
        QFile keep(rotated + QStringLiteral("/keep"));
        ASSERT_TRUE(keep.open(QIODevice::WriteOnly));
        QFile big(path);
        ASSERT_TRUE(big.open(QIODevice::WriteOnly));
        ASSERT_TRUE(big.resize(qint64(10) * 1024 * 1024 + 1));   // past the 10 MiB cap
    }
    const QString capture = log.dir.path() + "/stderr.txt";
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    const int fd = ::open(capture.toLocal8Bit().constData(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
    DebugLog::setActive(DebugLog::Events);
    DebugLog::write(DebugLog::Events, QStringLiteral("first line"));
    DebugLog::write(DebugLog::Events, QStringLiteral("second line"));
    DebugLog::setActive(0);
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    EXPECT_FALSE(ScratchLog::read(path).contains("second line"))
        << "file logging went on after the rotation failed";
    EXPECT_EQ(ScratchLog::read(capture).count("could not rotate"), 1)
        << "a failed rotation was not reported on stderr exactly once";
}
