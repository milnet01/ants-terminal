// Audit auto-fix and the per-check cap respect current state — see spec.md.
// ANTS-5083. Source-scrape of the AuditDialog sources.

#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {

QString auditSource() {
    return QString::fromStdString(ants_test::slurpAuditDialog());
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

// INV-1 and INV-2
TEST(AuditAutofixGuards, AutoFixUsesCurrentStateAndReadsLittle) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::runAutoFix()"));
    ASSERT_FALSE(body.isEmpty()) << "runAutoFix not found";
    EXPECT_TRUE(body.contains(QStringLiteral("isSuppressed(f)")));
    EXPECT_FALSE(body.contains(QStringLiteral("f.suppressed")))
        << "auto-fix trusts the suppression flag cached at parse time";
    const int lineGuard = body.indexOf(QStringLiteral("f.line < 1"));
    const int read = body.indexOf(QStringLiteral("readAll()"));
    ASSERT_GE(lineGuard, 0) << "a finding with no line still triggers a file read";
    ASSERT_GE(read, 0);
    EXPECT_LT(lineGuard, read);
    EXPECT_TRUE(body.contains(QStringLiteral("kMaxAutoFixFileBytes")))
        << "auto-fix reads a flagged file of any size";
}

// INV-3
TEST(AuditAutofixGuards, UncappedLaneKeepsEveryFinding) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::handleCheckOutput("));
    ASSERT_FALSE(body.isEmpty()) << "handleCheckOutput not found";
    const int guard = body.indexOf(QStringLiteral("check.filter.maxLines > 0"));
    const int cap = body.indexOf(QStringLiteral("capFindings("));
    ASSERT_GE(cap, 0);
    ASSERT_GE(guard, 0) << "every check's findings are capped, uncapped lanes included";
    EXPECT_LT(guard, cap);
}
