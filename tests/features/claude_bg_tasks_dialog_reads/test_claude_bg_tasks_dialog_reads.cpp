// Background tasks dialog reads what it checked — see spec.md. ANTS-5092.
// Source-scrape of src/claudebgtasksdialog.cpp.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString dialogSource() {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/claudebgtasksdialog.cpp"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    if (end < 0) return QString();
    return src.mid(start, end - start);
}

}  // namespace

// INV-1
TEST(ClaudeBgTasksDialogReads, TailOpensTheValidatedPath) {
    const QString body = functionBody(dialogSource(), QStringLiteral("QString tailFile("));
    ASSERT_FALSE(body.isEmpty()) << "tailFile not found";
    EXPECT_FALSE(body.contains(QStringLiteral("QFile f(path)")))
        << "tailFile opens the unvalidated path";
    EXPECT_TRUE(body.contains(QStringLiteral("QFile f(safe)")));
}

// INV-2
TEST(ClaudeBgTasksDialogReads, FinishedOutputReadOnce) {
    const QString src = dialogSource();
    const QString rebuild = functionBody(src, QStringLiteral("void ClaudeBgTasksDialog::rebuild()"));
    ASSERT_FALSE(rebuild.isEmpty());
    EXPECT_TRUE(rebuild.contains(QStringLiteral("m_finishedOutput")))
        << "every rebuild re-reads finished tasks' output";
    const QString rewatch = functionBody(src, QStringLiteral("void ClaudeBgTasksDialog::rewatch()"));
    ASSERT_FALSE(rewatch.isEmpty());
    EXPECT_TRUE(rewatch.contains(QStringLiteral("!t.finished")))
        << "finished tasks' output files are still watched";
}
