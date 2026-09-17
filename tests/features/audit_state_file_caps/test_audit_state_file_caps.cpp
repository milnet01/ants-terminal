// ANTS-5083 — the audit dialog skips oversized project files. Contract:
// spec.md beside this file.

#include "auditdialog.h"
#include "config.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>

namespace {

// Well past the cap, whatever it is retuned to (spec.md § Out of scope).
constexpr qint64 kPadBytes = 40LL * 1024 * 1024;

constexpr char kRulePack[] = R"({"rules":[{"id":"cap_probe","command":"true"}]})";

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

bool hasCheck(const AuditDialog &dlg, const QString &id) {
    for (const AuditCheck &c : dlg.checksForTest())
        if (c.id == id) return true;
    return false;
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int brace = src.indexOf(QChar('{'), start);
    if (brace < 0) return QString();
    int depth = 1;
    int i = brace + 1;
    while (i < src.size() && depth > 0) {
        // Step over a character literal such as '{', which is not a brace.
        if (src.at(i) == QChar('\'') && i + 2 < src.size() && src.at(i + 2) == QChar('\'')) {
            i += 3;
            continue;
        }
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(brace, i - brace);
}

class AuditStateFileCaps : public ::testing::Test {
protected:
    void SetUp() override { qputenv("ANTS_AUDIT_TRUST_UNSAFE", "1"); }
    void TearDown() override { qunsetenv("ANTS_AUDIT_TRUST_UNSAFE"); }
};

}  // namespace

// INV-1
TEST_F(AuditStateFileCaps, OversizedRulePackIsSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QByteArray padded = kRulePack;
    padded.append(QByteArray(kPadBytes, ' '));
    ASSERT_TRUE(writeFile(tmp.filePath(QStringLiteral("audit_rules.json")), padded));

    Config cfg;
    AuditDialog dlg(tmp.path(), nullptr, &cfg);
    EXPECT_FALSE(hasCheck(dlg, QStringLiteral("cap_probe")))
        << "an oversized audit_rules.json was read and loaded";
}

// INV-2
TEST_F(AuditStateFileCaps, NormalRulePackLoads) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.filePath(QStringLiteral("audit_rules.json")), kRulePack));

    Config cfg;
    AuditDialog dlg(tmp.path(), nullptr, &cfg);
    EXPECT_TRUE(hasCheck(dlg, QStringLiteral("cap_probe")))
        << "a normal audit_rules.json no longer loads";
}

// INV-3
TEST_F(AuditStateFileCaps, EveryReaderUsesTheCappedRead) {
    const QString src = QString::fromStdString(ants_test::slurpAuditDialog());
    ASSERT_FALSE(src.isEmpty()) << "AuditDialog sources not readable";
    const char *const readers[] = {
        "int AuditDialog::loadUserRules()",
        "void AuditDialog::loadSuppressions()",
        "void AuditDialog::saveSuppression(",
        "void AuditDialog::loadBaseline()",
        "AuditDialog::TrendSnapshot AuditDialog::loadLastSnapshot() const",
        "void AuditDialog::appendSnapshot(",
    };
    for (const char *sig : readers) {
        const QString body = functionBody(src, QString::fromLatin1(sig));
        ASSERT_FALSE(body.isEmpty()) << sig << " not found";
        EXPECT_TRUE(body.contains(QStringLiteral("readAuditStateFile(")))
            << sig << " does not use the capped read";
        EXPECT_FALSE(body.contains(QStringLiteral("readAll()")))
            << sig << " still reads the file whole";
    }
}
