// ANTS-5084 — SARIF export writes valid artifact URIs. Contract: spec.md
// beside this file.

#include "auditengine.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QJsonObject>
#include <QString>

// INV-1
TEST(AuditSarifUris, RelativePathIsEncodedAndAnchored) {
    const QJsonObject loc = AuditEngine::sarifArtifactLocation(QStringLiteral("src/a b#1.cpp"));
    EXPECT_EQ(loc.value(QStringLiteral("uri")).toString(), QStringLiteral("src/a%20b%231.cpp"));
    EXPECT_EQ(loc.value(QStringLiteral("uriBaseId")).toString(), QStringLiteral("%SRCROOT%"));
}

// INV-2
TEST(AuditSarifUris, AbsolutePathIsAFileUri) {
    const QJsonObject loc = AuditEngine::sarifArtifactLocation(QStringLiteral("/tmp/x y.cpp"));
    EXPECT_EQ(loc.value(QStringLiteral("uri")).toString(), QStringLiteral("file:///tmp/x%20y.cpp"));
    EXPECT_FALSE(loc.contains(QStringLiteral("uriBaseId")));
}

// INV-3
TEST(AuditSarifUris, SourceRootIsADirectoryUri) {
    EXPECT_EQ(AuditEngine::sarifSrcRootUri(QStringLiteral("/home/u/my proj")),
              QStringLiteral("file:///home/u/my%20proj/"));
}

// INV-5
TEST(AuditSarifUris, EncodedLocationsDecodeToTheirPath) {
    for (const QString &path : {QStringLiteral("src/a b#1%.cpp"),
                                QStringLiteral("/tmp/x y%20z.cpp"),
                                QStringLiteral("plain/file.cpp")}) {
        EXPECT_EQ(AuditEngine::sarifLocationPath(AuditEngine::sarifArtifactLocation(path)), path)
            << path.toStdString();
    }
}

// INV-6
TEST(AuditSarifUris, UnmarkedUriIsReturnedAsIs) {
    // The headless runner writes raw paths with no uriBaseId; a literal %20
    // in such a file name must not be decoded.
    const QJsonObject raw{{QStringLiteral("uri"), QStringLiteral("src/100%20.cpp")}};
    EXPECT_EQ(AuditEngine::sarifLocationPath(raw), QStringLiteral("src/100%20.cpp"));
}

// INV-7
TEST(AuditSarifUris, SummaryReaderDecodesLocations) {
    const QString src = QString::fromStdString(
        ants_test::slurpFile(SRC_AUDIT_ENGINE_CPP_PATH));
    const int start = src.indexOf(QStringLiteral("summariseSarif("));
    ASSERT_GE(start, 0) << "summariseSarif not found";
    const QString body = src.mid(start, 6000);
    EXPECT_TRUE(body.contains(QStringLiteral("sarifLocationPath(")))
        << "last_audit_summary would return the encoded uri, not the path";
}

// INV-4
TEST(AuditSarifUris, ExportUsesTheHelpers) {
    const QString src = QString::fromStdString(ants_test::slurpAuditDialog());
    const int start = src.indexOf(QStringLiteral("QString AuditDialog::exportSarif() const"));
    ASSERT_GE(start, 0) << "exportSarif not found";
    const QString body = src.mid(start, src.indexOf(QStringLiteral("\n}\n"), start) - start);
    EXPECT_TRUE(body.contains(QStringLiteral("sarifArtifactLocation(")));
    EXPECT_TRUE(body.contains(QStringLiteral("originalUriBaseIds")));
    EXPECT_FALSE(body.contains(QStringLiteral("artLoc[\"uri\"] = f.file")))
        << "the raw file path is still written as the uri";
}
