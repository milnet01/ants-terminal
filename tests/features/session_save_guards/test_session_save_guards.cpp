// Session saves use per-process temp files and check writes — see spec.md.
// ANTS-5106. Source-scrape of src/sessionmanager.cpp.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source() {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/sessionmanager.cpp"));
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
    const QString b = body(source(), QStringLiteral("void SessionManager::saveTabOrder("));
    ASSERT_FALSE(b.isEmpty());
    const int check = b.indexOf(QStringLiteral("file.write(body) != body.size() || !file.flush()"));
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
