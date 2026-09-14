// Verify log splitting and test-audit report lifetime — see spec.md. ANTS-5102.

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
TEST(VerifyAndAuditGuards, LinesSplitBeforeSingleLineCap) {
    const QString b = body("verifyengine.cpp", QStringLiteral("GateResult runOneGate("));
    ASSERT_FALSE(b.isEmpty());
    const int split = b.indexOf(QStringLiteral("carry.indexOf(QChar('\\n'))"));
    const int cap = b.indexOf(QStringLiteral("carry.toUtf8().size() > kSingleLineCap"));
    ASSERT_GE(split, 0);
    ASSERT_GE(cap, 0);
    EXPECT_LT(split, cap) << "the single-line cap runs before complete lines are split out";
}
