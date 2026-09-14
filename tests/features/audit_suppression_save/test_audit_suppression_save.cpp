// Audit suppressions take effect only when saved, and read fresh lines —
// see spec.md. ANTS-5083. Source-scrape of src/auditdialog.cpp.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString auditSource() {
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
        + QStringLiteral("/../../../src/auditdialog.cpp");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int brace = src.indexOf(QChar('{'), start);
    if (brace < 0) return QString();
    int depth = 1;
    int i = brace + 1;
    while (i < src.size() && depth > 0) {
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(brace, i - brace);
}

}  // namespace

// INV-1
TEST(AuditSuppressionSave, RunClearsTheLineCache) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::runAudit()"));
    ASSERT_FALSE(body.isEmpty()) << "runAudit not found";
    EXPECT_TRUE(body.contains(QStringLiteral("m_fileLineCache.clear()")))
        << "a run reuses the last run's cached lines, missing new inline suppressions";
}

// INV-2
TEST(AuditSuppressionSave, UnsavedSuppressionDoesNotHide) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::saveSuppression("));
    ASSERT_FALSE(body.isEmpty()) << "saveSuppression not found";
    EXPECT_TRUE(body.contains(QStringLiteral("&& sf.commit()")))
        << "the rewrite ignores QSaveFile::commit()";
    const int guard = body.indexOf(QStringLiteral("if (!saved)"));
    const int insert = body.indexOf(QStringLiteral("m_suppressedKeys.insert(dedupKey)"));
    ASSERT_GE(guard, 0) << "saveSuppression never checks whether the save worked";
    ASSERT_GE(insert, 0);
    EXPECT_LT(guard, insert)
        << "the key is marked suppressed before checking the save";
}
