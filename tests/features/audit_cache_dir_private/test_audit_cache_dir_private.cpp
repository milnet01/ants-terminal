// The audit cache directory is created private — see spec.md.
// ANTS-5085. Source-scrape of src/auditdialog.cpp and src/auditrunner.cpp.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel) {
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
        + QStringLiteral("/../../../") + QString::fromUtf8(rel);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

TEST(AuditCacheDirPrivate, ExportButtonsUseEnsurePrivateDir) {
    const QString src = source("src/auditdialog.cpp");
    ASSERT_FALSE(src.isEmpty());
    EXPECT_FALSE(src.contains(QStringLiteral("mkpath(m_projectPath + \"/.audit_cache\")")))
        << "an export creates .audit_cache at umask permissions";
}

TEST(AuditCacheDirPrivate, GitleaksConfigUsesEnsurePrivateDir) {
    const QString src = source("src/auditrunner.cpp");
    ASSERT_FALSE(src.isEmpty());
    const int fn = src.indexOf(QStringLiteral("writeGitleaksExcludeConfig("));
    ASSERT_GE(fn, 0);
    const int end = src.indexOf(QStringLiteral("\n}\n"), fn);
    ASSERT_GT(end, fn);
    const QString body = src.mid(fn, end - fn);
    EXPECT_TRUE(body.contains(QStringLiteral("ensurePrivateDir(dir)")));
    EXPECT_FALSE(body.contains(QStringLiteral("mkpath(dir)")));
}
