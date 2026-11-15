// Session saves use per-process temp files and check writes — see spec.md.
// ANTS-5106. Source-scrape of src/sessionmanager.cpp.

#include "secureio.h"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel = "sessionmanager.cpp") {
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

}  // namespace

// INV-1
TEST(SessionSaveGuards, TempNamesArePerProcess) {
    const QString s = source();
    ASSERT_FALSE(s.isEmpty());
    for (const char *sig : {"void SessionManager::saveSession(",
                            "void SessionManager::saveTabOrder("}) {
        const QString b = body(s, QString::fromLatin1(sig));
        ASSERT_FALSE(b.isEmpty()) << sig;
        EXPECT_TRUE(b.contains(QStringLiteral("QCoreApplication::applicationPid()")))
            << sig << " writes a temp name another running copy shares";
    }
}

// INV-2
TEST(SessionSaveGuards, TabOrderChecksItsWrite) {
    const QString tabOrder = body(source(), QStringLiteral("void SessionManager::saveTabOrder("));
    ASSERT_FALSE(tabOrder.isEmpty());
    EXPECT_TRUE(tabOrder.contains(QStringLiteral("writeFileAtomicOwnerOnly(")));
    const QString b = body(source("secureio.h"),
                           QStringLiteral("inline bool writeFileAtomicOwnerOnly("));
    ASSERT_FALSE(b.isEmpty());
    const int check = b.indexOf(QStringLiteral("file.write(data) != data.size() || !file.flush()"));
    const int rename = b.indexOf(QStringLiteral("std::rename("));
    ASSERT_GE(rename, 0);
    ASSERT_GE(check, 0) << "a short write is renamed over the tab order";
    EXPECT_LT(check, rename);
}

// INV-3
TEST(SessionSaveGuards, SweepMatchesPerProcessNames) {
    const QString s = source();
    EXPECT_TRUE(s.contains(QStringLiteral("\"tab_order.txt.*.tmp\"")));
    EXPECT_TRUE(s.contains(QStringLiteral("\"session_*.dat.*.tmp\"")));
}

// INV-4 (ANTS-6472) — a failed flush to disk keeps the previous file and
// removes the temp file.
TEST(SessionSaveGuards, FailedFsyncKeepsThePreviousFile) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("tab_order.txt"));
    const QString tmpPath = path + QStringLiteral(".tmp");
    ASSERT_TRUE(writeFileAtomicOwnerOnly(path, tmpPath, "old\n", "test",
                                         TempPerms::Refuse));
    g_fsyncTempForTest = [](int) { errno = EIO; return -1; };
    QString why;
    const bool ok = writeFileAtomicOwnerOnly(path, tmpPath, "new\n", "test",
                                             TempPerms::Refuse, &why);
    g_fsyncTempForTest = nullptr;
    EXPECT_FALSE(ok) << "a failed fsync must not be renamed into place";
    EXPECT_FALSE(why.isEmpty());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("old\n"));
    EXPECT_FALSE(QFile::exists(tmpPath)) << "the temp file must be removed";
}
